// Enlight resource containers (Seven Kingdoms "ResourceIdx" lineage).
//   Name9 : u16 count; (count+1) x {char name[9]; u32 offset}   (RESOURCE, *.SET, GAMEPLAY.RES)
//   Name34: u32 count; (count+1) x {char name[34]; u32 offset}  (WAVE.RES, scenarioN.res, item/unit .res, ...)
// The extra last record holds the end offset; entry size = next offset - offset.
// See docs/formats/CONTAINERS_WORLD_SAVES.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace joa::formats {

enum class ResIndexFormat { Name9, Name34 };

struct ResEntry {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

// The bytes are shared by copies and stay valid (spans from data()/text() included) as long as any copy lives.
class ResArchive {
public:
    // mmap()s the file read-only (falls back to reading it into memory if mapping fails).
    static std::optional<ResArchive> open(const std::filesystem::path& p, std::string* err = nullptr);
    // Auto-detects the index format; fails unless offsets are monotonic, start right after the
    // index and end inside the file.
    static std::optional<ResArchive> parse(std::vector<std::uint8_t> bytes, std::string* err = nullptr);

    ResIndexFormat format() const { return format_; }
    const std::vector<ResEntry>& entries() const { return entries_; }
    const ResEntry* find(std::string_view name) const;  // case-insensitive
    std::span<const std::uint8_t> data(const ResEntry& e) const {
        return bytes_.subspan(e.offset, e.size);
    }
    std::optional<std::string_view> text(std::string_view name) const;
    std::uint32_t end_offset() const { return end_; }   // usually == file size
    std::size_t file_size() const { return bytes_.size(); }

private:
    static std::optional<ResArchive> parse(std::shared_ptr<const void> owner, std::span<const std::uint8_t> bytes,
                                           std::string* err);
    std::shared_ptr<const void> owner_;   // mapping or vector behind bytes_
    std::span<const std::uint8_t> bytes_;
    std::vector<ResEntry> entries_;
    ResIndexFormat format_ = ResIndexFormat::Name34;
    std::uint32_t end_ = 0;
};

// dBase III table (7KAA "Database"); used for CURSOR.RES and every entry of RESOURCE / *.SET.
struct DbfField {
    std::string name;
    char type = 'C';
    std::uint8_t length = 0;
    std::uint8_t decimals = 0;
    std::uint32_t offset = 0;  // byte offset inside a record (record byte 0 = delete flag)
};

class DbfTable {
public:
    static std::optional<DbfTable> parse(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

    std::uint8_t version() const { return version_; }
    const std::vector<DbfField>& fields() const { return fields_; }
    std::size_t record_count() const { return count_; }
    int field_index(std::string_view name) const;  // case-insensitive, -1 if absent
    bool deleted(std::size_t rec) const { return record(rec)[0] == '*'; }
    std::span<const std::uint8_t> raw(std::size_t rec, std::size_t field) const {
        return record(rec).subspan(fields_[field].offset, fields_[field].length);
    }
    std::string text(std::size_t rec, std::size_t field) const;  // trailing/leading blanks and NULs trimmed

private:
    std::span<const std::uint8_t> record(std::size_t rec) const {
        return std::span(bytes_).subspan(header_len_ + rec * record_len_, record_len_);
    }
    std::vector<std::uint8_t> bytes_;
    std::vector<DbfField> fields_;
    std::size_t count_ = 0, header_len_ = 0, record_len_ = 0;
    std::uint8_t version_ = 0;
};

// Resource/pal_std.res: 8-byte header then 256 x RGB (8 bit per channel).
struct Palette {
    std::uint32_t header0 = 0, header1 = 0;  // header0 == file size (776)
    std::uint8_t rgb[256][3]{};
};
std::optional<Palette> parse_palette(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

// Resource/I_CURSOR.RES: concatenated {u32 size; u16 width; u16 height; u8 pixels[w*h]} records,
// addressed by byte offset (CURSOR.RES field BITMAPPTR). Pixels index pal_std.res; 0xFF looks transparent.
struct IndexedBitmap {
    std::uint32_t offset = 0;  // offset of the size field
    std::uint16_t width = 0, height = 0;
    std::span<const std::uint8_t> pixels;
};
std::optional<std::vector<IndexedBitmap>> parse_bitmap_db(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

}  // namespace joa::formats
