// Navigation of a whole mission world: one PathSpace per level (world node, <node>.pfd, map-local
// coordinates at the level origin) plus the world-level mesh (<world>.pfd, coordinates / 32) that
// routes between levels. Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md Part C §1, §3.1
// ("Cross-map") and Appendix B (levels, streaming, Unit_update_location).
//
// Queries take world coordinates. Poly ids are global: level base + local poly index.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "game/nav_mesh.hpp"
#include "game/path_find.hpp"

namespace joa::game {

class World;

struct NavLevel {
    NavMesh mesh;                // Level +0x54 (map-local)
    float ox = 0, oy = 0;        // Level +0x44/+0x48 origin (ftol of the box min)
    float x1 = 0, y1 = 0;        // Level +0x08 box max x/y (min = origin; z -5000..5000 never limits)
    bool loaded = true;          // Level +0x43 != 0
    int base = 0;                // first global poly id
};

class NavWorld {
public:
    explicit NavWorld(NavMesh single);  // one level at the origin, no world mesh (S1L1, tests)
    NavWorld(std::vector<NavLevel> levels, std::optional<NavMesh> world);

    const std::vector<NavLevel>& levels() const { return levels_; }
    NavLevel& level(int i) { return levels_[std::size_t(i)]; }
    const NavMesh* world_mesh() const { return world_ ? &*world_ : nullptr; }

    // World_locate 0x54a2c0: level whose box contains (x,y) (inclusive), `prefer` first; -1 if none.
    int level_at(float x, float y, int prefer = -1) const;
    int level_of_poly(int poly) const;
    const NavPoly& poly(int poly) const;

    // NavMesh queries in world coordinates. find_poly_xy searches every level whose box holds (x,y)
    // and keeps the poly whose height is closest to z (global search 006aae70).
    int find_poly_xy(float x, float y, float z, bool ignore_blocked = false) const;
    float poly_height(int poly, float x, float y) const;
    float grid_height(float x, float y) const;  // grid of the level at (x,y); 0 outside every level
    void set_blocked(int poly, bool blocked);
    // Level_segment_blocked 00560d80 on the quads of a's level.
    bool segment_blocked(NavPt a, NavPt b, float eye_z) const;

    // Unit_path_to 0x505dd0 path planning: local path when the target lies in the start's (loaded)
    // level, else world-mesh route (/32, +16 cell centre) with the span inside the start level
    // replanned on the local mesh and spliced in front of the remaining world waypoints.
    PathResult find_path(const PathQuery& q) const;

    // World_draw 0x44c800 level streaming around the camera (inner +-8000 loads, outside +-16000
    // unloads) and Unit_update_location 0x4fee60 / level load-unload unit (de)activation (+0x2ed).
    // No-op for single-level worlds. No player -> every level stays loaded.
    void stream(World& w);
    // After a save is loaded: remember every unit's level/load pair without touching +0x2ed.
    void resync(const World& w);
    std::vector<std::uint8_t> save_levels() const;
    bool load_levels(const std::vector<std::uint8_t>& b);

private:
    PathResult local_path(int lv, PathQuery q) const;
    bool in_level(int lv, float x, float y) const;

    std::vector<NavLevel> levels_;
    std::optional<NavMesh> world_;
    std::vector<int> unit_key_;  // per unit id: level * 2 + loaded seen last stream, -1 unseen
};

}  // namespace joa::game
