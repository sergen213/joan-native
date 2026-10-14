// Native save-game container (docs/SAVES_AND_CAMPAIGN.md "Native save format").
//
//   "JOANSAVE" magic (8) | u32 version | u32 section count
//   per section: char tag[4] | u32 size | u32 crc32(data) | data[size]
//   u32 crc32 of every preceding byte
//
// Little-endian. Sections are opaque blobs owned by their subsystem (World, VM, mission...).
// Files live under $XDG_DATA_HOME/joan-native/saves/ (never the original Save/ directory) and
// are written atomically (temp file + fsync + rename + directory fsync) after rotating the
// previous file to <name>.1 .. <name>.<backups>.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace joa::game {

// Append-only little-endian writer used by the mission-side serializers.
class ByteWriter {
public:
    std::vector<std::uint8_t>& out;
    explicit ByteWriter(std::vector<std::uint8_t>& o) : out(o) {}
    template <class T>
    void put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void u8(std::uint8_t v) { put(v); }
    void u16(std::uint16_t v) { put(v); }
    void i16(std::int16_t v) { put(v); }
    void u32(std::uint32_t v) { put(v); }
    void i32(std::int32_t v) { put(v); }
    void u64(std::uint64_t v) { put(v); }
    void f32(float v) { put(v); }
    void bytes(std::span<const std::uint8_t> b) { u32(static_cast<std::uint32_t>(b.size())), out.insert(out.end(), b.begin(), b.end()); }
    void str(std::string_view s) { u32(static_cast<std::uint32_t>(s.size())), out.insert(out.end(), s.begin(), s.end()); }
};

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

struct SaveSection {
    std::array<char, 4> tag{};
    std::vector<std::uint8_t> data;
};

struct SaveFile {
    static constexpr std::uint32_t kVersion = 1;
    std::uint32_t version = kVersion;
    std::vector<SaveSection> sections;

    void add(std::string_view tag4, std::vector<std::uint8_t> data);
    const SaveSection* find(std::string_view tag4) const;
};

std::vector<std::uint8_t> encode_save(const SaveFile& f);
// Validates magic, every section CRC and the trailing file CRC. Versions newer than kVersion
// are rejected; older ones are returned as stored and must go through migrate_save().
std::optional<SaveFile> decode_save(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

// Upgrade step from version N to N+1 (index N-1). Empty in version 1: the hook exists so a
// format change adds one function here instead of breaking existing saves.
using SaveMigration = std::function<bool(SaveFile&)>;
const std::vector<SaveMigration>& save_migrations();
bool migrate_save(SaveFile& f, std::string* err = nullptr);

// $XDG_DATA_HOME/joan-native/saves (fallback $HOME/.local/share/joan-native/saves).
std::filesystem::path native_save_dir();
// Rotates path -> path.1 -> ... -> path.<backups> (oldest dropped), then writes bytes to
// path.tmp, fsyncs, renames over path and fsyncs the directory. Refuses to write into the
// original install (a directory holding joan.exe, or its Save/ subdirectory).
bool write_save_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, int backups = 3,
                       std::string* err = nullptr);
std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path);

}  // namespace joa::game
