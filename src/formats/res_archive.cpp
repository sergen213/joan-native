#include "formats/res_archive.hpp"

#include "formats/vfs_bytes.hpp"
#include "formats/vfs_fs.hpp"

namespace joa::formats {

static bool set_err(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

std::optional<ResArchive> ResArchive::open(const std::filesystem::path& p, std::string* err) {
    auto bytes = read_file(p);
    if (!bytes) {
        set_err(err, "cannot read file");
        return std::nullopt;
    }
    return parse(std::move(*bytes), err);
}

// Returns false if this index layout does not describe the buffer exactly.
static bool try_index(std::span<const std::uint8_t> b, bool wide_count, std::size_t name_len,
                      std::vector<ResEntry>& out, std::uint32_t& end) {
    ByteReader r(b);
    const std::uint64_t count = wide_count ? r.u32() : r.u16();
    const std::uint64_t index_size = (wide_count ? 4 : 2) + (count + 1) * (name_len + 4);
    if (!r.ok() || count == 0 || index_size > b.size()) return false;
    std::vector<ResEntry> entries(count);
    std::uint32_t prev = 0;
    for (std::uint64_t i = 0; i <= count; ++i) {
        std::string name = r.fixed_string(name_len);
        const std::uint32_t off = r.u32();
        if (i == 0 ? off != index_size : off < prev) return false;
        if (off > b.size()) return false;
        if (i > 0) entries[i - 1].size = off - prev;
        if (i < count) entries[i] = ResEntry{std::move(name), off, 0};
        prev = off;
    }
    out = std::move(entries);
    end = prev;
    return r.ok();
}

std::optional<ResArchive> ResArchive::parse(std::vector<std::uint8_t> bytes, std::string* err) {
    ResArchive a;
    // Name34 first: a Name9 reading of a Name34 file fails the first-offset check and vice versa.
    if (try_index(bytes, true, 34, a.entries_, a.end_)) a.format_ = ResIndexFormat::Name34;
    else if (try_index(bytes, false, 9, a.entries_, a.end_)) a.format_ = ResIndexFormat::Name9;
    else {
        set_err(err, "not a resource index container");
        return std::nullopt;
    }
    a.bytes_ = std::move(bytes);
    return a;
}

const ResEntry* ResArchive::find(std::string_view name) const {
    for (const auto& e : entries_)
        if (iequals(e.name, name)) return &e;
    return nullptr;
}

std::optional<std::string_view> ResArchive::text(std::string_view name) const {
    const ResEntry* e = find(name);
    if (!e) return std::nullopt;
    auto d = data(*e);
    return std::string_view(reinterpret_cast<const char*>(d.data()), d.size());
}

// ---- DBF ----

std::optional<DbfTable> DbfTable::parse(std::span<const std::uint8_t> bytes, std::string* err) {
    ByteReader r(bytes);
    DbfTable t;
    t.version_ = r.u8();
    r.skip(3);  // YY MM DD of last update
    t.count_ = r.u32();
    t.header_len_ = r.u16();
    t.record_len_ = r.u16();
    r.seek(32);
    if (!r.ok() || t.version_ != 0x03 || t.record_len_ == 0) {
        set_err(err, "not a dBase III table");
        return std::nullopt;
    }
    std::uint32_t off = 1;  // byte 0 of a record is the delete flag
    while (r.ok() && r.pos() < t.header_len_ && bytes[r.pos()] != 0x0D) {
        DbfField f;
        f.name = r.fixed_string(11);
        f.type = char(r.u8());
        r.skip(4);  // in-memory field address; unreliable in these files, recomputed below
        f.length = r.u8();
        f.decimals = r.u8();
        r.skip(14);
        f.offset = off;
        off += f.length;
        t.fields_.push_back(std::move(f));
    }
    if (!r.ok() || off != t.record_len_ ||
        t.header_len_ + std::uint64_t(t.count_) * t.record_len_ > bytes.size()) {
        set_err(err, "dBase table header/records out of bounds");
        return std::nullopt;
    }
    t.bytes_.assign(bytes.begin(), bytes.begin() + t.header_len_ + t.count_ * t.record_len_);
    return t;
}

int DbfTable::field_index(std::string_view name) const {
    for (std::size_t i = 0; i < fields_.size(); ++i)
        if (iequals(fields_[i].name, name)) return int(i);
    return -1;
}

std::string DbfTable::text(std::size_t rec, std::size_t field) const {
    auto s = raw(rec, field);
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == 0)) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == 0)) --e;
    return std::string(reinterpret_cast<const char*>(s.data() + b), e - b);
}

// ---- misc Resource/ formats ----

std::optional<Palette> parse_palette(std::span<const std::uint8_t> bytes, std::string* err) {
    if (bytes.size() != 8 + 768) {
        set_err(err, "palette must be 776 bytes");
        return std::nullopt;
    }
    ByteReader r(bytes);
    Palette p;
    p.header0 = r.u32();
    p.header1 = r.u32();
    auto rgb = r.bytes(768);
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c) p.rgb[i][c] = rgb[i * 3 + c];
    return p;
}

std::optional<std::vector<IndexedBitmap>> parse_bitmap_db(std::span<const std::uint8_t> bytes, std::string* err) {
    std::vector<IndexedBitmap> out;
    ByteReader r(bytes);
    while (r.ok() && !r.at_end()) {
        IndexedBitmap bm;
        bm.offset = std::uint32_t(r.pos());
        const std::uint32_t size = r.u32();
        bm.width = r.u16();
        bm.height = r.u16();
        if (!r.ok() || size != 4u + std::uint32_t(bm.width) * bm.height) {
            set_err(err, "bitmap record size mismatch");
            return std::nullopt;
        }
        bm.pixels = r.bytes(size - 4);
        out.push_back(bm);
    }
    if (!r.ok() || out.empty()) {
        set_err(err, "truncated bitmap record");
        return std::nullopt;
    }
    return out;
}

}  // namespace joa::formats
