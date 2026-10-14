// Runtime navmesh built from a .PFD (joan.exe uti3d\pathsp.cpp). Map-local world units.
// Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md Part C §1-§2, §4, §6.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "formats/pfd_mesh.hpp"

namespace joa::game {

struct NavPt { float x = 0, y = 0, z = 0; };

struct NavPoly {
    int type = 0;                 // 0 ground (grid height), 1 walkway (plane through v0,v1,v2)
    int state = 0;                // poly+0x20: 0 open, 1 blocked (closed gate), -1 narrow
    int model = 0;
    int bbox[6] = {};             // min xyz, max xyz from verts (00663cb0)
    int cx = 0, cy = 0, cz = 0;   // vertex average (00664920), ints
    int component = -1;           // poly+0x70
    std::vector<int> v;           // point indices
    std::vector<int> nb;          // neighbour poly per edge v[i]->v[i+1 mod n], -1 none
};

// Anti-portal LOS quad (.POR / gate), Level_segment_blocked 00560d80.
struct LosQuad {
    NavPt v[4];
    NavPt n;     // plane normal
    float d = 0; // plane D
    bool enabled = true;
};

class NavMesh {
public:
    static constexpr float kNarrowClearance = 200.0f;  // radius < 200 -> 0 in A* (0066ce30)
    static constexpr int kNarrowEdgeLen = 170;         // post-load narrow test (00671364)

    explicit NavMesh(const formats::PfdMesh& m);

    const std::vector<NavPoly>& polys() const { return polys_; }
    const std::vector<formats::PfdPoint>& points() const { return pts_; }
    const formats::PfdPoint& pt(int poly, int i) const { return pts_[std::size_t(polys_[poly].v[i % polys_[poly].v.size()])]; }

    // Poly_contains 006718c0: convex test (interior cross <= 0) + optional clearance radius against wall edges.
    bool contains(int poly, float x, float y, float radius = 0, bool ignore_blocked = false) const;
    // PathSpace_find_poly 0066c020: first poly containing (x,y) with |height - z| <= 16. -1 if none.
    int find_poly(float x, float y, float z, float radius = 0) const;
    // Any poly containing (x,y), ignoring z; prefers the one whose surface is closest to z. -1 if none.
    int find_poly_xy(float x, float y, float z) const;
    // 0066c710: nearest poly by 3-D bbox distance (ties: centroid distance).
    int nearest_poly(float x, float y, float z) const;
    // Poly_height 00664180 (grid triangulated bilinear 004c7b70, or walkway plane).
    float poly_height(int poly, float x, float y) const;
    float grid_height(float x, float y) const;  // 004c7b70 on the height grid alone; 0 without a grid
    // Height at (x,y): poly under (x,y) closest to z, else grid. Mirrors Phys_ground 006adfd0 (no poly -> 0).
    float ground_height(float x, float y, float z) const;

    // Gate toggle (006af710): set blocked state, recompute components (00675660).
    void set_blocked(int poly, bool blocked);
    void compute_components();
    bool same_component(int a, int b) const;

    // LOS (00560d80): true if segment a->b (a.z raised by eye_z) crosses an enabled quad.
    void add_los_quads_from_por(const std::string& por_text);
    std::vector<LosQuad>& los_quads() { return quads_; }
    bool segment_blocked(NavPt a, NavPt b, float eye_z) const;

    float min_x() const { return border_[0]; }
    float min_y() const { return border_[1]; }
    float max_x() const { return border_[2]; }
    float max_y() const { return border_[3]; }

private:
    bool clearance_ok(int poly, float x, float y, float r, std::vector<int>& seen) const;
    bool is_border_edge(const formats::PfdPoint& a, const formats::PfdPoint& b) const;
    template <class F> void for_candidates(float x, float y, F&& f) const;

    std::vector<formats::PfdPoint> pts_;
    std::vector<NavPoly> polys_;
    std::optional<formats::PfdGrid> grid_;
    std::vector<LosQuad> quads_;
    float border_[4] = {};
    // ponytail: uniform bucket grid instead of the original BVH (006745e0); same answers, simpler.
    static constexpr int kBucket = 512;
    int bcols_ = 0, brows_ = 0;
    std::vector<std::vector<int>> buckets_;
};

}  // namespace joa::game
