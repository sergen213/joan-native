// Scripted win of S1L1 (test hook: objectives finished) -> win delay -> game-end CONTINUE ->
// campaign mission 2 (S3L1) with Joan's progression carried through the profile / load_unit.
#include <cstdio>
#include <cstdlib>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

static int fails = 0;
#define CHECK(c) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++fails; \
    } while (0)

static Unit* find_class(World& w, int cls) {
    for (Unit& u : w.units())
        if (!u.deleted && u.cls == cls) return &u;
    return nullptr;
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    CHECK(run.init(&err) && run.missions.size() == 8);
    const char* order[] = {"S1L1", "S3L1", "S2L1", "S4L1", "S5L1", "S9L1", "S7L1", "S6L1"};
    for (int i = 0; i < 8 && i < int(run.missions.size()); ++i) CHECK(run.missions[i].stem() == order[i]);
    if (!run.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    run.mission()->autosave = false;

    const int joan_cls = gd->unit_index("JOAN_OF_ARC");
    CHECK(joan_cls >= 0);
    CHECK(run.profile.continued == false);
    for (int t = 0; t < 60; ++t) run.mission()->tick();
    Unit* joan = find_class(run.mission()->world(), joan_cls);
    CHECK(joan != nullptr);
    if (!joan) return 1;
    const int raised = joan->level + 3;
    run.mission()->world().set_level(*joan, raised);

    run.mission()->finish_objectives();
    int t = 0;
    while (run.mission()->finish() == 0 && t < 3000) run.mission()->tick(), ++t;
    std::printf("won after %d ticks (finish %d)\n", t, run.mission()->finish());
    CHECK(run.mission()->finish() == 1);
    CHECK(t >= 15000 / 33);  // S1L1 set_delay_before_win(15000)
    const int gold = run.mission()->state().gold;
    joan = find_class(run.mission()->world(), joan_cls);  // progression at CONTINUE time
    CHECK(joan && joan->level >= raised);
    if (!joan) return 1;
    const int level = joan->level, xp = joan->xp, str = joan->str;

    CHECK(run.continue_after_win(&err));
    if (fails) return std::printf("%d failures (%s)\n", fails, err.c_str()), 1;
    Mission& m2 = *run.mission();
    CHECK(m2.scenario_no() == 2 && m2.info().stem() == "S3L1");
    CHECK(run.profile.continued);
    const HeroRecord* rec = run.profile.find("JOAN_OF_ARC");
    CHECK(rec && rec->unit.level == level && rec->unit.xp == xp);
    CHECK(m2.state().gold == gold);  // gold carried through the profile
    bool transition = false;
    for (const auto& e : m2.take_events()) transition |= e.kind == MissionEvent::Kind::Transition;
    CHECK(transition);
    for (int i = 0; i < 30; ++i) m2.tick();
    Unit* joan2 = find_class(m2.world(), joan_cls);
    CHECK(joan2 != nullptr);
    if (joan2) {
        std::printf("S3L1 Joan level %d (carried %d) xp %d str %d\n", joan2->level, level, joan2->xp, joan2->str);
        CHECK(joan2->level == level && joan2->xp == xp && joan2->str == str);
    }
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
