// Mission 8 (S6L1, The Road to Coronation) played by the campaign bot (game/autopilot_player + autopilot_plan_S6L1).
//
// Start checkpoint: the legitimate chain wins missions 1-4 and saves the mission-5 start (campaign_checkpoints/bot-S5L1.jsav,
// written by the game.campaign_legit fixture into this build directory); missions 5..7 are won through
// Mission::finish_objectives() (the end-condition awards) and CONTINUE. Without that file the test is skipped.
//
// The mission spans the whole map (Auxerre, Troyes and its trebuchet hill, the camps on the road north, Reims) and needs
// reputation 80: longer than a test may take, so it is cut at self-generated checkpoints, each segment a test of its own
// (fixtures; each starts a fresh bot from the save, the plan keeps no state across it):
//   game.campaign_S6L1_auxerre  mission start -> Auxerre captured with the king there (objective 1): bot-S6L1-auxerre.jsav
//   game.campaign_S6L1_hill     -> the hill camp's trebuchets broke Troyes' eastern wall (S6L1_3_Wall3): bot-S6L1-hill.jsav
//   game.campaign_S6L1_troyes   -> Troyes captured with the king there (objective 2): bot-S6L1-troyes.jsav
//   game.campaign_S6L1_road     -> the camps and towns on the road north taken (reputation 77): bot-S6L1-road.jsav
//   game.campaign_S6L1_bot      -> the win, then CONTINUE after the last scenario: MissionEvent::Ending.
// Iterate on one segment: ctest -R S6L1 -FS 'campaign_.*'.
//
//   game_campaign_S6L1_bot_test [ROAD.jsav] [TICKS]          the bot from the road checkpoint: must win
//   game_campaign_S6L1_bot_test --SEGMENT OUT [IN] [D]       one segment (auxerre: IN is the chain's mission-5 start, D the
//                                                            difficulty, 1 Normal), save OUT
//   game_campaign_S6L1_bot_test --checkpoint OUT [FROM] [D]  write the mission-8 start and exit; iterate with
//                                                            joa-campaign --load OUT --autopilot
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

bool done(Mission& m, int objective) { return m.state().end.size() >= std::size_t(objective) && m.state().end[std::size_t(objective - 1)].finish == 1; }

// The segments: --name, the save it starts from (the previous segment's), the save it writes, its end. Each takes the bot
// well under 200000 ticks (2-3 ms a tick, machine load: inside a test's 600 s).
struct Segment { const char* name; const char* in; const char* out; bool (*end)(Mission&); const char* what; };
constexpr Segment kSegments[] = {
    {"auxerre", "campaign_checkpoints/bot-S5L1.jsav", "campaign_checkpoints/bot-S6L1-auxerre.jsav", [](Mission& m) { return done(m, 1); },
     "Auxerre captured (objective 1)"},
    {"hill", "campaign_checkpoints/bot-S6L1-auxerre.jsav", "campaign_checkpoints/bot-S6L1-hill.jsav",
     [](Mission& m) {
         const MapItem* wall = m.objects().item(m.objects().find_by_name("S6L1_3_Wall3"));
         return wall && wall->hp <= 0;
     },
     "Troyes' eastern wall down"},
    {"troyes", "campaign_checkpoints/bot-S6L1-hill.jsav", "campaign_checkpoints/bot-S6L1-troyes.jsav", [](Mission& m) { return done(m, 2); },
     "Troyes captured (objective 2)"},
    {"road", "campaign_checkpoints/bot-S6L1-troyes.jsav", "campaign_checkpoints/bot-S6L1-road.jsav",
     [](Mission& m) { return m.state().reputation >= 77; }, "reputation 77"},
};
constexpr int kSegmentTicks = 200000, kTicks = 200000;

bool start_8(CampaignRunner& run, const char* from, int difficulty, std::string& err) {
    if (!run.init(&err)) return false;
    auto bytes = read_file(from);
    auto f = bytes ? decode_save(*bytes, &err) : std::nullopt;
    if (!f || !migrate_save(*f, &err) || !run.load(*f, &err)) return false;
    while (run.mission()->scenario_no() < 8) {
        Mission& m = *run.mission();
        m.autosave = false;
        for (int t = 0; t < 60; ++t) m.tick();
        m.finish_objectives();
        for (int t = 0; m.finish() == 0 && t < 6000; ++t) m.tick();
        if (m.finish() != 1) return err = m.info().stem() + " forced win failed", false;
        if (!run.continue_after_win(&err)) return false;
    }
    run.profile.difficulty = run.mission()->profile().difficulty = run.mission()->world().difficulty = difficulty;
    return run.mission()->info().stem() == "S6L1" || (err = "not at S6L1", false);
}

int script_failures(Mission& m) {
    int fails = 0;
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("FAIL unimplemented builtin %d %s\n", id, std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
    for (const auto& e : m.vm().errors()) std::printf("FAIL script error: %s\n", e.c_str()), ++fails;
    return fails;
}

// One segment: the bot until its end (a game over, a finished mission or the tick cap fail), then OUT is saved.
int segment(CampaignRunner& run, const Segment& s, const char* out) {
    Mission& m = *run.mission();
    m.autosave = false;
    Autopilot ap(m);
    if (const char* tr = std::getenv("JOA_BOT_TRACE")) ap.trace = std::atoi(tr);
    int t = 0;
    for (; t < kSegmentTicks && !s.end(m) && m.finish() == 0 && !m.state().game_over; ++t) ap.step();
    for (const std::string& l : ap.log) std::printf("  %s\n", l.c_str());
    int fails = script_failures(m);
    if (!s.end(m))
        std::printf("FAIL %s not reached after %d ticks (game over %d '%s') | bot %s\n", s.what, t, int(m.state().game_over),
                    m.state().game_over_text.c_str(), ap.goal.c_str()), ++fails;
    std::string err;
    if (!fails && !write_save_atomic(out, encode_save(run.save()), 0, &err)) std::printf("FAIL save: %s\n", err.c_str()), ++fails;
    if (!fails) std::printf("%s at tick %d (mission tick %llu, rep %d): saved %s\n", s.what, t, (unsigned long long)m.ticks(),
                            m.state().reputation, out);
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool checkpoint = argc > 2 && mode == "--checkpoint";
    const Segment* seg = nullptr;
    for (const Segment& s : kSegments)
        if (argc > 2 && mode == std::string("--") + s.name) seg = &s;
    const bool start = checkpoint || seg == &kSegments[0];
    const char* from = argc > 3 && (checkpoint || seg) ? argv[3]
                       : checkpoint                    ? kSegments[0].in
                       : seg                           ? seg->in
                       : argc > 1                      ? argv[1]
                                                       : std::end(kSegments)[-1].out;
    if (!read_file(from)) return std::printf("skip: no %s (game.campaign_legit / the earlier S6L1 segment writes it)\n", from), 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    if (start) {
        if (!start_8(run, from, argc > 4 ? std::atoi(argv[4]) : 1, err)) return std::printf("FAIL start: %s\n", err.c_str()), 1;
    } else {
        auto f = decode_save(*read_file(from), &err);
        if (!f || !migrate_save(*f, &err) || !run.init(&err) || !run.load(*f, &err)) return std::printf("FAIL load: %s\n", err.c_str()), 1;
        if (run.mission()->info().stem() != "S6L1") return std::printf("FAIL %s is not at S6L1\n", from), 1;
    }
    if (checkpoint) {
        if (!write_save_atomic(argv[2], encode_save(run.save()), 0, &err)) return std::printf("FAIL save: %s\n", err.c_str()), 1;
        return std::printf("saved %s\n", argv[2]), 0;
    }
    if (seg) return segment(run, *seg, argv[2]);
    const MissionReport r = play_mission(run, argc > 2 ? std::atoi(argv[2]) : kTicks, 0);
    Mission& m = *run.mission();
    int fails = script_failures(m);
    for (const std::string& l : r.log) std::printf("  %s\n", l.c_str());
    std::printf("%s %s ticks %d attempts %d deaths %d kills %d objectives %d/%d rep %d%s%s\n", r.stem.c_str(),
                r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills, r.objectives_done,
                r.objectives_total, m.state().reputation, r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    if (r.result != 1) {
        std::printf("FAIL S6L1 not won\n%d failures\n", fails + 1);
        return 1;
    }
    m.take_events();
    if (run.continue_after_win(&err) || err != "campaign complete") std::printf("FAIL continue: '%s'\n", err.c_str()), ++fails;
    bool ending = false;
    for (const MissionEvent& e : m.take_events()) ending |= e.kind == MissionEvent::Kind::Ending;
    if (!ending) std::printf("FAIL no Ending event\n"), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
