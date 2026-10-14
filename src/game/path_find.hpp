// Polygon A* + funnel smoothing (PathSpace_find_path 0066cb40 -> A* 0066ce30 -> Path_smooth 0066ece0).
// Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md Part C §2-§3.
#pragma once

#include <vector>

#include "game/nav_mesh.hpp"

namespace joa::game {

struct PathResult {
    enum Status { NoStart, Partial, Full } status = NoStart;  // Partial = best node on failure (0066ce30 returns 0)
    std::vector<NavPt> points;   // smoothed waypoints after the start, last = destination (possibly the partial dest)
    int expanded = 0;            // polygons popped (<= max_expand)
};

struct PathQuery {
    NavPt from, to;
    float radius = 64.0f;        // Unit_move_to default BODY_R*2; < 200 treated as 0 for narrow polys
    int start_poly = -1;         // -1 = find_poly / nearest_poly
    int max_expand = 100;        // every game caller passes 100
};

// Deterministic: no RNG, integer A* costs as in the original.
PathResult find_path(const NavMesh& mesh, const PathQuery& q);

}  // namespace joa::game
