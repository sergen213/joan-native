// Save/*.SAV (+ .BMP thumbnail) and Save/Player.dat (profile).
// SAV writer: header FUN_00478cc0, body FUN_00474ec0; reader: header FUN_00478520.
// Profile writer FUN_004c8810 / reader FUN_004c8bb0 (profile.cpp).
// Layout and confidence: docs/formats/CONTAINERS_WORLD_SAVES.md ("Saves").
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace joa::formats {

// The SAV file is written through a File object with record-size mode on: every File::write
// emits u16 length + data (length 0 when data >= 64 KiB), while put_short/put_long emit raw
// values. The body is therefore not self-describing; it is split here at the 0x2FACEBEE
// section markers that FUN_00474ec0 writes before each of its 20 section writers and at EOF.
struct SaveHeader {
    std::uint8_t flag = 0;                 // record 2, byte 0 (game clears it before saving)
    std::uint64_t saved_utc = 0;           // FILETIME, 100 ns since 1601-01-01 UTC
    std::uint64_t saved_local = 0;         // FILETIME, local time
    std::uint8_t game_mode = 0;            // DAT_00a500c2
    std::uint8_t game_submode = 0;         // DAT_00a500c3
    std::uint32_t value_a = 0;             // DAT_00a47bb0, UNKNOWN
    std::uint32_t value_b = 0;             // DAT_00a47bb4, UNKNOWN
    std::string description;               // char[128], e.g. "Mission 1 Auto Save"
    std::uint32_t value_c = 0;             // save object +0x19a, UNKNOWN
    std::uint8_t value_d = 0;              // save object +0x19e, UNKNOWN
};

struct SaveSection {
    std::uint32_t offset = 0;  // first byte after the marker record
    std::uint32_t size = 0;
};

struct SaveGame {
    static constexpr std::uint32_t kMagic = 0x76615352;   // "RSav"
    static constexpr std::uint32_t kMarker = 0x2FACEBEE;
    static constexpr std::size_t kSections = 20;

    SaveHeader header;
    std::uint32_t body_offset = 0;
    std::vector<SaveSection> sections;  // kSections entries, in writer order (see docs)
};

std::optional<SaveGame> parse_sav(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

// FILETIME -> seconds since the Unix epoch.
inline std::int64_t filetime_to_unix(std::uint64_t ft) { return std::int64_t(ft / 10000000ULL) - 11644473600LL; }

// Player.dat: raw writes (no record-size prefixes).
struct PlayerProfile {
    std::vector<std::uint8_t> header;  // 0x48 bytes; profile name at +0 (char[], "Player")
    std::string name;                  // header name
    std::uint32_t header_38 = 0;       // header +0x38 (100 in the shipped file), UNKNOWN
    std::vector<std::uint8_t> block2;  // 0x20 bytes; starts with the player/character name
    std::string name2;
    std::uint32_t entry_count = 0;     // N; the shipped file has 0. Entries are not decoded.
    std::vector<std::uint16_t> list_a, list_b;
    std::uint16_t trailing = 0;
};

// Only N == 0 profiles are fully decoded; N > 0 fails (per-entry layout not yet confirmed).
std::optional<PlayerProfile> parse_player_dat(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

// Uncompressed bottom-up BMP (the thumbnail written beside each SAV, 512x256x24 in the shipped save).
struct BmpInfo {
    std::int32_t width = 0, height = 0;
    std::uint16_t bpp = 0;
    std::uint32_t pixel_offset = 0;
};
std::optional<BmpInfo> parse_bmp_header(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

}  // namespace joa::formats
