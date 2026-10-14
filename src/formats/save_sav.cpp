#include "formats/save_sav.hpp"

#include <algorithm>
#include <array>

#include "formats/vfs_bytes.hpp"

namespace joa::formats {

template <class T>
static std::optional<T> fail(std::string* err, const char* m) {
    if (err) *err = m;
    return std::nullopt;
}

// One File::write record: u16 length + data. The game's reader (FUN_005873a0) copies
// min(length, wanted) bytes and skips the rest, so longer records are legal.
static std::span<const std::uint8_t> record(ByteReader& r, std::size_t min_len) {
    const std::uint16_t len = r.u16();
    if (len < min_len) r.fail();
    return r.bytes(len);
}

template <class T>
static T as(std::span<const std::uint8_t> s) {
    T v{};
    std::copy_n(s.begin(), std::min(s.size(), sizeof(T)), reinterpret_cast<std::uint8_t*>(&v));
    return v;
}

std::optional<SaveGame> parse_sav(std::span<const std::uint8_t> bytes, std::string* err) {
    ByteReader r(bytes);
    SaveGame s;
    if (as<std::uint32_t>(record(r, 4)) != SaveGame::kMagic || !r.ok())
        return fail<SaveGame>(err, "missing RSav magic record");
    auto& h = s.header;
    h.flag = as<std::uint8_t>(record(r, 1));
    h.saved_utc = as<std::uint64_t>(record(r, 8));
    h.saved_local = as<std::uint64_t>(record(r, 8));
    h.game_mode = as<std::uint8_t>(record(r, 1));
    h.game_submode = as<std::uint8_t>(record(r, 1));
    h.value_a = as<std::uint32_t>(record(r, 4));
    h.value_b = as<std::uint32_t>(record(r, 4));
    h.description = ByteReader::cstr(record(r, 0x80));
    h.value_c = as<std::uint32_t>(record(r, 4));
    h.value_d = as<std::uint8_t>(record(r, 1));
    if (!r.ok()) return fail<SaveGame>(err, "truncated SAV header");
    s.body_offset = std::uint32_t(r.pos());

    // ponytail: byte-pattern scan; would mis-split if section data ever contained the marker
    // record. A schema-driven body parser replaces this once the section writers are mapped.
    static constexpr std::array<std::uint8_t, 6> kMarkerRec{0x04, 0x00, 0xEE, 0xEB, 0xAC, 0x2F};
    std::vector<std::size_t> marks;
    for (auto it = bytes.begin() + s.body_offset;
         (it = std::search(it, bytes.end(), kMarkerRec.begin(), kMarkerRec.end())) != bytes.end(); ++it)
        marks.push_back(std::size_t(it - bytes.begin()));
    if (marks.size() != SaveGame::kSections + 1 || marks.front() != s.body_offset ||
        marks.back() + kMarkerRec.size() != bytes.size())
        return fail<SaveGame>(err, "SAV body does not have 20 marker-delimited sections");
    for (std::size_t i = 0; i < SaveGame::kSections; ++i) {
        const auto off = marks[i] + kMarkerRec.size();
        s.sections.push_back({std::uint32_t(off), std::uint32_t(marks[i + 1] - off)});
    }
    return s;
}

std::optional<PlayerProfile> parse_player_dat(std::span<const std::uint8_t> bytes, std::string* err) {
    ByteReader r(bytes);
    PlayerProfile p;
    auto hdr = r.bytes(0x48);
    auto b2 = r.bytes(0x20);
    p.entry_count = r.u32();
    if (!r.ok()) return fail<PlayerProfile>(err, "truncated Player.dat");
    if (p.entry_count != 0) return fail<PlayerProfile>(err, "Player.dat entries not supported yet");
    p.header.assign(hdr.begin(), hdr.end());
    p.block2.assign(b2.begin(), b2.end());
    p.name = ByteReader::cstr(hdr);
    p.name2 = ByteReader::cstr(b2);
    p.header_38 = ByteReader(hdr, 0x38).u32();
    for (auto* list : {&p.list_a, &p.list_b}) {
        const std::uint16_t n = r.u16();
        for (std::uint16_t i = 0; i < n && r.ok(); ++i) list->push_back(r.u16());
    }
    p.trailing = r.u16();
    if (!r.at_end()) return fail<PlayerProfile>(err, "Player.dat size mismatch");
    return p;
}

std::optional<BmpInfo> parse_bmp_header(std::span<const std::uint8_t> bytes, std::string* err) {
    ByteReader r(bytes);
    if (r.u16() != 0x4D42) return fail<BmpInfo>(err, "not a BMP");
    const std::uint32_t file_size = r.u32();
    r.skip(4);
    BmpInfo b;
    b.pixel_offset = r.u32();
    const std::uint32_t dib = r.u32();
    b.width = r.i32();
    b.height = r.i32();
    r.skip(2);  // planes
    b.bpp = r.u16();
    const std::uint32_t compression = r.u32();
    const std::uint64_t row = (std::uint64_t(b.width) * b.bpp / 8 + 3) & ~3ULL;
    if (!r.ok() || dib < 40 || file_size != bytes.size() || compression != 0 || b.width <= 0 || b.height == 0 ||
        b.pixel_offset + row * std::uint64_t(b.height < 0 ? -std::int64_t(b.height) : b.height) > bytes.size())
        return fail<BmpInfo>(err, "unsupported or truncated BMP");
    return b;
}

}  // namespace joa::formats
