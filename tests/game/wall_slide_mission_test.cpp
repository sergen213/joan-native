// Mission-1 boundary run (DIFFERENTIAL_VERIFICATION.md, scenario "boundary"): from the start (cut scene skipped)
// Joan runs (W held, camera fed back like GameCamera) 11 s along her initial facing into the battlefield edge.
// Original joan.exe: she slides west turning 115 -> 182 deg (+2 deg per ~19 ms frame), turns the corner at
// (13876, 14081) onto the next edge (245 deg) and ends at (13528, 13021). Before the Round-3 fix the port turned
// +2 deg per 33 ms tick: the heading was still 129 deg when a bend of the edge blocked the slide, and she stopped
// dead at the corner.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    run.seed = 1;
    if (!run.init(&err) || !run.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    Mission& m = *run.mission();
    for (int t = 0; t < 600 && !(m.ui().cs.active || m.ui().game_cut_scene); ++t) m.tick();
    if (m.ui().cs.active || m.ui().game_cut_scene) m.ui().skip_cut_scene(m);
    m.tick();
    World& w = m.world();
    float cam = w.unit(w.player_id())->yaw_goal;
    float max_yaw_before_corner = 0;
    for (int t = 0; t < 30 + 11000 / World::kTickMs; ++t) {   // 1 s settle, then W for 11 s
        InputFrame f;
        if (t >= 30) f.held.set(4);
        f.camera_yaw_deg = cam;
        m.tick(f);
        cam = w.controller().camera_yaw_out;
        const Unit& j = *w.unit(w.player_id());
        if (j.pos.x > 13900.0f) max_yaw_before_corner = std::max(max_yaw_before_corner, j.yaw_deg);
    }
    const Unit& j = *w.unit(w.player_id());
    std::printf("end (%.0f, %.0f) yaw %.0f, max yaw before the corner %.0f (original (13528, 13021), 182)\n", j.pos.x, j.pos.y,
                j.yaw_deg, max_yaw_before_corner);
    int fails = 0;
    if (max_yaw_before_corner < 175.0f || max_yaw_before_corner > 190.0f) std::printf("FAIL heading along the edge\n"), ++fails;
    if (std::hypot(j.pos.x - 13528.0f, j.pos.y - 13021.0f) > 150.0f) std::printf("FAIL end position: corner not turned\n"), ++fails;
    return fails ? 1 : 0;
}
