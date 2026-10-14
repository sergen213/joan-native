// Save/*.SAV (+ .BMP thumbnail) and Save/Player.dat (profile).
// SAV writer: header FUN_00478cc0, body FUN_00474ec0; reader: header FUN_00478520.
// Profile writer FUN_004c8810 / reader FUN_004c8bb0 (profile.cpp).
// Layout and confidence: docs/formats/CONTAINERS_WORLD_SAVES.md ("Saves").
#pragma once

#include <array>
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

// Player.dat (profile.cpp; writer FUN_004c8810, reader FUN_004c8bb0): raw writes, no record-size
// prefixes. Fixed-size pieces are raw memory images of the original objects (they contain stale heap
// pointers); they are kept verbatim so write_player_dat(parse_player_dat(b)) == b. Field map and
// confidence: docs/formats/CONTAINERS_WORLD_SAVES.md §7.2.
using PlayerBytes = std::vector<std::uint8_t>;

// Item container (0x3e object, FUN_0047ede0 / FUN_0047ef30): grid of rows x cols cells of `cell`
// bytes (each byte = 1-based item slot occupying it), item slots, DynArrayB free list.
struct PlayerItemBag {
    PlayerBytes grid;                        // 4 / 48 / 24 bytes for inventory +0x12 / +0x16 / +0x1a
    std::vector<PlayerBytes> items;          // 0x18-byte records; empty = free slot (flag u16 0)
    PlayerBytes free_list;                   // n x 8 bytes (FUN_00585b40: u32 n + data)
};

// Unit inventory (unit+0x1c1, FUN_0047e850 / FUN_0047e9f0).
struct PlayerInventory {
    PlayerBytes raw;                         // 0x1e; u32 at +0x02/+0x06/+0x0a/+0x0e/+0x12/+0x16/+0x1a != 0 = present
    std::array<PlayerBytes, 4> slots;        // 0x12-byte item instances for +0x02/+0x06/+0x0a/+0x0e
    std::array<PlayerItemBag, 3> bags;       // +0x12 (1x4x1) / +0x16 (1x6x8) / +0x1a (1x6x4)
};

// FUN_004c9880 / FUN_004c9930.
struct PlayerUnit {
    PlayerBytes raw;                         // 0x432 Unit image
    PlayerBytes block_16f;                   // 0x3b, unit+0x16f again (FUN_0047e5c0)
    std::uint32_t upgrade_ptr = 0;           // unit+0x1aa again (FUN_0047e640); != 0 = list follows
    std::vector<std::array<std::int16_t, 4>> upgrades;  // {upitem id (1-based), unlocked, applied, combo level}
    std::optional<PlayerInventory> inv;      // present iff raw u32 at +0x1c1 != 0
};

// Profile entry (0x77 bytes in memory: char code[0x21], Unit* +0x21, short count +0x25, Unit* +0x27[20]);
// only its first 0x20 bytes are written, twice (FUN_004c93e0, then FUN_004c9460).
struct PlayerEntry {
    PlayerBytes head, head2;                 // 0x20 each; code at +0
    PlayerUnit unit;
    std::vector<PlayerUnit> members;         // u16 count (+0x25) + units; the game never fills it
    std::string code() const;
};

struct PlayerProfile {
    PlayerBytes header;                      // 0x48 profile object image; name at +0, see accessors
    PlayerBytes block2;                      // 0x20 = the profile object's first 0x20 bytes again
    std::vector<PlayerEntry> entries;        // u32 N, N heads, N bodies
    std::vector<std::uint16_t> list_a, list_b;  // item-hint lists (profile +0x28/+0x2c, +0x30/+0x34)
    std::uint16_t trailing = 0;              // (short) profile +0x40 = command points

    std::string name() const;                // +0x00 char[0x20]
    std::int32_t gold() const;               // +0x38 (100 in the shipped file)
    std::uint32_t play_ms() const;           // +0x3c
    std::int32_t command_points() const;     // +0x40
};

std::optional<PlayerProfile> parse_player_dat(std::span<const std::uint8_t> bytes, std::string* err = nullptr);
std::vector<std::uint8_t> write_player_dat(const PlayerProfile& p);

// Uncompressed bottom-up BMP (the thumbnail written beside each SAV, 512x256x24 in the shipped save).
struct BmpInfo {
    std::int32_t width = 0, height = 0;
    std::uint16_t bpp = 0;
    std::uint32_t pixel_offset = 0;
};
std::optional<BmpInfo> parse_bmp_header(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

}  // namespace joa::formats
