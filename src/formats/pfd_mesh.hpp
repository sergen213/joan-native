// map/scenarioN/*.pfd : precomputed convex-polygon navmesh + height grid used by pathfinding and
// ground height. Loader in joan.exe: FUN_00670980 (uti3d\pathsp.cpp). Layout and post-load rules:
// docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md Part C §1.1.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace joa::formats {

struct PfdPoint { std::int32_t x = 0, y = 0, z = 0; };

struct PfdPoly {
    std::int32_t type = 0;              // 0 ground (height from grid), 1 walkway on a model (plane height)
    std::int32_t bbox[6] = {};          // as stored; engine recomputes from verts
    std::int32_t model = 0;             // 1-based into models; 0 = none
    std::vector<std::int32_t> verts;    // 0-based point indices (file stores 1-based), ring order
};

struct PfdLink { std::int32_t poly_a = 0, edge_a = 0, poly_b = 0, edge_b = 0; };  // 1-based as stored

struct PfdGrid {
    std::int32_t x0 = 0, y0 = 0, cols = 0, rows = 0, cell_w = 0, cell_h = 0;
    std::vector<std::int16_t> h;  // h[y * cols + x]
};

struct PfdMesh {
    std::vector<std::string> models;  // Object\Models_Unique\... paths
    std::vector<PfdPoint> points;
    std::vector<PfdPoly> polys;
    std::vector<PfdLink> links;
    std::optional<PfdGrid> grid;      // detail sub-grids are parsed and dropped (none shipped)
};

std::optional<PfdMesh> parse_pfd(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

}  // namespace joa::formats
