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

namespace {
constexpr std::size_t kProfile = 0x48, kName = 0x20, kEntryHead = 0x20, kUnit = 0x432, kBlock16f = 0x3b, kInv = 0x1e,
                      kSlot = 0x12, kBagItem = 0x18;
constexpr std::size_t kBagGrid[3] = {1 * 1 * 4, 1 * 8 * 6, 1 * 4 * 6};  // rows * cols * cell (0x457290 args)

std::uint32_t le32(const PlayerBytes& b, std::size_t off) {
    return off + 4 <= b.size() ? ByteReader(b, off).u32() : 0;
}
PlayerBytes take(ByteReader& r, std::size_t n) {
    auto s = r.bytes(n);
    return {s.begin(), s.end()};
}

PlayerItemBag read_bag(ByteReader& r, std::size_t grid) {
    PlayerItemBag b;
    b.grid = take(r, grid);
    const std::uint32_t n = r.u32();
    if (n > r.remaining() / 2) return r.fail(), b;
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) b.items.push_back(r.u16() ? take(r, kBagItem) : PlayerBytes{});
    const std::uint32_t f = r.u32();
    if (f > r.remaining() / 8) return r.fail(), b;
    b.free_list = take(r, std::size_t(f) * 8);
    return b;
}

PlayerUnit read_unit(ByteReader& r) {
    PlayerUnit u;
    u.raw = take(r, kUnit);
    u.block_16f = take(r, kBlock16f);
    u.upgrade_ptr = r.u32();
    if (u.upgrade_ptr) {
        const std::uint32_t n = r.u32();
        if (n > r.remaining() / 8) return r.fail(), u;
        u.upgrades.resize(n);
        for (auto& e : u.upgrades)
            for (auto& v : e) v = r.i16();
    }
    if (le32(u.raw, 0x1c1)) {
        auto& inv = u.inv.emplace();
        inv.raw = take(r, kInv);
        for (std::size_t s = 0; s < 4; ++s)
            if (le32(inv.raw, 2 + 4 * s)) inv.slots[s] = take(r, kSlot);
        for (std::size_t g = 0; g < 3; ++g)
            if (le32(inv.raw, 0x12 + 4 * g)) inv.bags[g] = read_bag(r, kBagGrid[g]);
    }
    return u;
}

struct Out {
    std::vector<std::uint8_t> b;
    void raw(const PlayerBytes& v, std::size_t n) {  // fixed-size piece, zero-padded / truncated to n
        for (std::size_t i = 0; i < n; ++i) b.push_back(i < v.size() ? v[i] : 0);
    }
    void u16(std::uint16_t v) { b.push_back(std::uint8_t(v)), b.push_back(std::uint8_t(v >> 8)); }
    void u32(std::uint32_t v) { u16(std::uint16_t(v)), u16(std::uint16_t(v >> 16)); }

    void unit(const PlayerUnit& u) {
        raw(u.raw, kUnit);
        raw(u.block_16f, kBlock16f);
        u32(u.upgrade_ptr);
        if (u.upgrade_ptr) {
            u32(std::uint32_t(u.upgrades.size()));
            for (const auto& e : u.upgrades)
                for (auto v : e) u16(std::uint16_t(v));
        }
        if (!le32(u.raw, 0x1c1)) return;
        static const PlayerInventory kEmpty{};
        const auto& inv = u.inv ? *u.inv : kEmpty;
        raw(inv.raw, kInv);
        for (std::size_t s = 0; s < 4; ++s)
            if (le32(inv.raw, 2 + 4 * s)) raw(inv.slots[s], kSlot);
        for (std::size_t g = 0; g < 3; ++g) {
            if (!le32(inv.raw, 0x12 + 4 * g)) continue;
            const auto& bag = inv.bags[g];
            raw(bag.grid, kBagGrid[g]);
            u32(std::uint32_t(bag.items.size()));
            for (const auto& it : bag.items) {
                u16(!it.empty());
                if (!it.empty()) raw(it, kBagItem);
            }
            u32(std::uint32_t(bag.free_list.size() / 8));
            raw(bag.free_list, bag.free_list.size() / 8 * 8);
        }
    }
};
}  // namespace

std::string PlayerEntry::code() const { return ByteReader::cstr(head); }
std::string PlayerProfile::name() const { return ByteReader::cstr(std::span(header).first(std::min(header.size(), kName))); }
std::int32_t PlayerProfile::gold() const { return std::int32_t(le32(header, 0x38)); }
std::uint32_t PlayerProfile::play_ms() const { return le32(header, 0x3c); }
std::int32_t PlayerProfile::command_points() const { return std::int32_t(le32(header, 0x40)); }

std::optional<PlayerProfile> parse_player_dat(std::span<const std::uint8_t> bytes, std::string* err) {
    ByteReader r(bytes);
    PlayerProfile p;
    p.header = take(r, kProfile);
    p.block2 = take(r, kName);
    const std::uint32_t n = r.u32();
    if (!r.ok()) return fail<PlayerProfile>(err, "truncated Player.dat");
    if (n > r.remaining() / (2 * kEntryHead + kUnit)) return fail<PlayerProfile>(err, "Player.dat entry count too large");
    p.entries.resize(n);
    for (auto& e : p.entries) e.head = take(r, kEntryHead);
    for (auto& e : p.entries) {
        e.head2 = take(r, kEntryHead);
        e.unit = read_unit(r);
        const std::uint16_t m = r.u16();
        for (std::uint16_t i = 0; i < m && r.ok(); ++i) e.members.push_back(read_unit(r));
        if (!r.ok()) return fail<PlayerProfile>(err, "truncated Player.dat entry");
    }
    for (auto* list : {&p.list_a, &p.list_b}) {
        const std::uint16_t c = r.u16();
        for (std::uint16_t i = 0; i < c && r.ok(); ++i) list->push_back(r.u16());
    }
    p.trailing = r.u16();
    if (!r.at_end()) return fail<PlayerProfile>(err, "Player.dat size mismatch");
    return p;
}

std::vector<std::uint8_t> write_player_dat(const PlayerProfile& p) {
    Out o;
    o.raw(p.header, kProfile);
    o.raw(p.block2, kName);
    o.u32(std::uint32_t(p.entries.size()));
    for (const auto& e : p.entries) o.raw(e.head, kEntryHead);
    for (const auto& e : p.entries) {
        o.raw(e.head2, kEntryHead);
        o.unit(e.unit);
        o.u16(std::uint16_t(e.members.size()));
        for (const auto& m : e.members) o.unit(m);
    }
    for (const auto* list : {&p.list_a, &p.list_b}) {
        o.u16(std::uint16_t(list->size()));
        for (auto v : *list) o.u16(v);
    }
    o.u16(p.trailing);
    return std::move(o.b);
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
