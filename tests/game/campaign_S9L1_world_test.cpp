// Scenario 5 -> 6 keeps the world (S9L1 continues in S5L1's). GameOver_win_freeze_units 0x6e7900 skips the
// IDLE-task (7, prio 0x7f, p1 1) / order freeze when the won scenario is 5 and only stops the units, so S9L1 starts
// with every unit's own task; the port froze them all, and no enemy of S9L1 ever fought back.
#include <cstdio>
#include <cstdlib>

#include "game/ai_system.hpp"
#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

static int fails = 0;
#define CHECK(c) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++fails; \
    } while (0)

static int frozen(Mission& m) {  // units holding the win/lose freeze task
    int n = 0;
    for (Unit& u : m.world().units())
        if (!u.deleted && m.ai().st(u).task.id == task::IDLE && m.ai().st(u).task.prio == 0x7f) ++n;
    return n;
}

static bool win(Mission& m) {
    m.finish_objectives();
    for (int t = 0; t < 3000 && m.finish() == 0; ++t) m.tick();
    return m.finish() == 1;
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_05.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    CHECK(run.init(&err));

    if (!run.start(5, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    run.mission()->autosave = false;
    for (int t = 0; t < 60; ++t) run.mission()->tick();
    CHECK(win(*run.mission()));
    CHECK(frozen(*run.mission()) == 0);  // scenario 5: stopped, not frozen
    CHECK(run.continue_after_win(&err));
    Mission& m = *run.mission();
    CHECK(m.info().stem() == "S9L1");
    for (int t = 0; t < 30; ++t) m.tick();
    std::printf("S9L1 frozen units %d\n", frozen(m));
    CHECK(frozen(m) == 0);

    // Any other scenario's win still freezes every unit.
    if (!run.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    run.mission()->autosave = false;
    for (int t = 0; t < 60; ++t) run.mission()->tick();
    CHECK(win(*run.mission()));
    CHECK(frozen(*run.mission()) > 0);

    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
