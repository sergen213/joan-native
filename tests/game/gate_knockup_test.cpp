// A body knocked up (airborne) beside a shut gate stays on its side. A closed gate blocks its passage polys, so the nav
// lookup under the passage finds only the gatehouse top (Beaugency's middle gates: 770 over 290 ground). CharPhysics
// exempted airborne bodies from the 80 step-up: a hit's knock-up (combat_melee vel.z) next to BeaugencyMiddleGate2 let
// Joan step into the passage and "land" on the gatehouse top, into the castle the shut gate walls off (S9L1 bot).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_05.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    run.seed = 1;
    if (!run.init(&err) || !run.start(5, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    Mission& m = *run.mission();
    m.autosave = false;
    for (int t = 0; t < 600 && !(m.ui().cs.active || m.ui().game_cut_scene); ++t) m.tick();
    if (m.ui().cs.active || m.ui().game_cut_scene) m.ui().skip_cut_scene(m);
    m.tick();
    const MapItem* g = m.objects().item(m.objects().find_by_name("BeaugencyMiddleGate2"));
    if (!g || g->state != 0 || !g->blocking) return std::printf("FAIL BeaugencyMiddleGate2 not shut\n"), 1;
    World& w = m.world();
    Unit& p = *w.unit(w.player_id());
    // The town side, 25 off the gate line (the S9L1 bot's spot), a knock-up toward the passage (that run's velocity).
    p.pos = {g->pos.x + 4.3f, g->pos.y + 25.3f, g->pos.z};
    p.vel = {};
    m.tick();
    const float z0 = p.pos.z;
    w.set_action(p, ATTACKED);  // a hit: the controller leaves the body alone
    p.airborne = true;
    p.vel = {181, -82, 300};
    float top = z0;
    for (int t = 0; t < 30; ++t) m.tick(), top = std::max(top, p.pos.z);
    std::printf("gate z %.0f, Joan from z %.0f: highest %.0f, at (%.0f,%.0f,%.0f)\n", g->pos.z, z0, top, p.pos.x, p.pos.y, p.pos.z);
    if (top > z0 + 200) return std::printf("FAIL knocked up onto the gatehouse\n"), 1;
    std::printf("ok\n");
    return 0;
}
