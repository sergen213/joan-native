// Shared setup for the AI / troop / physics tests: GameData + S1L1 navmesh (+ .POR LOS quads).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "formats/pfd_mesh.hpp"
#include "formats/vfs_fs.hpp"
#include "game/nav_mesh.hpp"
#include "game/physics_char.hpp"
#include "game/unitclass_db.hpp"

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

namespace fixture {
using namespace joa::game;
namespace fs = std::filesystem;

struct Env {
    std::unique_ptr<GameData> gd;
    std::unique_ptr<NavMesh> nav;
    std::unique_ptr<NavTerrain> terrain;
};

// nullptr when JOA_GAME_DIR is missing (test returns 77).
inline std::unique_ptr<Env> load() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "joan.exe")) return nullptr;
    auto e = std::make_unique<Env>();
    std::string err;
    e->gd = GameData::load(dir, &err);
    if (!e->gd) { std::fprintf(stderr, "GameData: %s\n", err.c_str()); return nullptr; }
    auto pfd = joa::formats::parse_pfd(*joa::formats::read_file(fs::path(dir) / "map/scenario1/S1L1.pfd"));
    e->nav = std::make_unique<NavMesh>(*pfd);
    auto por = joa::formats::read_file(fs::path(dir) / "map/scenario1/S1L1.por");
    e->nav->add_los_quads_from_por(std::string(por->begin(), por->end()));
    e->terrain = std::make_unique<NavTerrain>(*e->nav);
    return e;
}

// Centre of the first w x h rectangle (axis aligned, sampled every 50 units) that is entirely walkable
// and at most 300 units of height variation; scanned row-major on a 256 grid (deterministic).
inline bool open_area(const NavMesh& m, float w, float h, float& cx, float& cy) {
    for (float y = 1024; y < 15360; y += 256)
        for (float x = 1024; x < 15360; x += 256) {
            bool ok = true;
            float lo = 1e9f, hi = -1e9f;
            for (float dy = -h / 2; dy <= h / 2 && ok; dy += 50)
                for (float dx = -w / 2; dx <= w / 2 && ok; dx += 50) {
                    const float g = m.grid_height(x + dx, y + dy);
                    const int p = m.find_poly_xy(x + dx, y + dy, g);
                    ok = p >= 0 && m.polys()[std::size_t(p)].type == 0;
                    lo = std::min(lo, g), hi = std::max(hi, g);
                }
            if (ok && hi - lo < 300) { cx = x, cy = y; return true; }
        }
    return false;
}
}  // namespace fixture
