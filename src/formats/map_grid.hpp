// map/scenarioN/*.GRI : terrain vertex grid (heights, sector ids, ground-type ids).
// Loader in joan.exe: FUN_004f6eb0 (world_s.cpp), called from the world loader FUN_004f2f80.
// See docs/formats/CONTAINERS_WORLD_SAVES.md for the byte layout.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace joa::formats {

struct GriTexture {
    std::string name;  // ground type, e.g. "drygrass_L01" (matches item/DynGrass.res entry names)
    std::string alt;   // second name; often equal or "none" (bytes after its NUL are editor garbage)
};

// Nodes with detail != 0 carry 5 extra RLE planes of 9 (detail 1) or 25 (detail 2) samples.
// Unused by all shipped maps (kept for completeness).
struct GriDetail {
    std::uint32_t node = 0;
    std::uint8_t level = 0;
    std::vector<std::uint8_t> samples;  // samples[i * 5 + plane], the engine's in-memory order
};

struct TerrainGrid {
    static constexpr float kCellSize = 64.0f;  // world units between vertices (0x40 in FUN_004f6eb0)

    std::int32_t cells_x = 0, cells_y = 0;  // vertices = cells + 1 per axis
    std::int32_t header_c = 0;              // always 4 in shipped data; meaning UNKNOWN
    std::vector<std::string> sector_names;  // "scene_sector_N"; node sector ids index this (1-based)
    std::vector<GriTexture> textures;       // node tex1/tex2 index this (1-based)
    // Bytes 0..15 of each engine node, index = x * verts_y() + y.
    std::vector<std::array<std::uint8_t, 16>> nodes;
    std::vector<GriDetail> details;

    int verts_x() const { return cells_x + 1; }
    int verts_y() const { return cells_y + 1; }
    const std::array<std::uint8_t, 16>& node(int x, int y) const { return nodes[std::size_t(x) * verts_y() + y]; }

    bool valid(int x, int y) const { return node(x, y)[0] != 0; }                    // byte 0
    std::uint8_t blend(int x, int y) const { return node(x, y)[4]; }                 // byte 4
    std::int16_t height(int x, int y) const { return std::int16_t(u16(x, y, 7)); }   // bytes 7-8
    std::uint16_t sector(int x, int y) const { return u16(x, y, 9); }                // bytes 9-10
    std::uint16_t tex1(int x, int y) const { return u16(x, y, 11); }                 // bytes 11-12
    std::uint16_t tex2(int x, int y) const { return u16(x, y, 13); }                 // bytes 13-14
    std::uint8_t detail(int x, int y) const { return node(x, y)[15]; }               // byte 15

    // Bilinear height at local world coordinates (x right, y "north"), clamped to the grid.
    float height_at(float wx, float wy) const;

private:
    std::uint16_t u16(int x, int y, int b) const {
        const auto& n = node(x, y);
        return std::uint16_t(n[b] | (n[b + 1] << 8));
    }
};

std::optional<TerrainGrid> parse_gri(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

}  // namespace joa::formats
