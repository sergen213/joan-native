// Mission 8 (S6L1, The Road to Coronation) played by the campaign bot (game/autopilot_player + autopilot_plan_S6L1).
//
// Start checkpoint: as game.campaign_S5L1_bot, the legitimate chain's mission-4 start (campaign_checkpoints/bot-S4L1.jsav,
// written by the game.campaign_legit fixture into this build directory), missions 4..7 won through
// Mission::finish_objectives() (the end-condition awards) and CONTINUE: Joan level 13. Without that file the test is skipped.
//
// The mission spans the whole map (Auxerre, Troyes and its trebuchet hill, the camps, Chalons, Reims) and needs reputation
// 80: longer than a test may take, so it is cut at self-generated checkpoints, each segment a test of its own (fixtures):
//   game.campaign_S6L1_auxerre  mission start -> Auxerre captured with the king there (objective 1): saves
//                               campaign_checkpoints/bot-S6L1-auxerre.jsav (Glasdale, objective 13, dead on the way);
//   game.campaign_S6L1_troyes   from there -> Troyes captured with the king there (objective 2): bot-S6L1-troyes.jsav;
//   game.campaign_S6L1_bot      from there -> the win, then CONTINUE after the last scenario: MissionEvent::Ending.
// Each segment starts a fresh bot from the save (the plan keeps no state across it). Iterate on one segment:
// ctest -R S6L1 -FS 'campaign_.*'.
//
//   game_campaign_S6L1_bot_test [TROYES.jsav] [TICKS]                 the bot from the Troyes checkpoint: must win
//   game_campaign_S6L1_bot_test --auxerre OUT [FROM_S4L1.jsav] [D]    the mission start to objective 1, save OUT
//   game_campaign_S6L1_bot_test --troyes OUT [AUXERRE.jsav]           the Auxerre checkpoint to objective 2, save OUT
//   game_campaign_S6L1_bot_test --checkpoint OUT [FROM] [D]           write the mission-8 start and exit; iterate with
//                                                                     joa-campaign --load OUT --autopilot
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

constexpr int kAuxerreTicks = 200000, kTroyesTicks = 300000, kTicks = 400000;   // segment caps (Normal, seed 1: §8.1)
constexpr const char* kAuxerre = "campaign_checkpoints/bot-S6L1-auxerre.jsav";
constexpr const char* kTroyes = "campaign_checkpoints/bot-S6L1-troyes.jsav";

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

bool done(Mission& m, int objective) { return m.state().end.size() >= std::size_t(objective) && m.state().end[std::size_t(objective - 1)].finish == 1; }

int script_failures(Mission& m) {
    int fails = 0;
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("FAIL unimplemented builtin %d %s\n", id, std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
    for (const auto& e : m.vm().errors()) std::printf("FAIL script error: %s\n", e.c_str()), ++fails;
    return fails;
}

// --auxerre / --troyes: the bot until `objective` is done (a game over, a finished mission or the tick cap fail), then
// OUT is saved.
int segment(CampaignRunner& run, int objective, int cap, const char* out) {
    Mission& m = *run.mission();
    m.autosave = false;
    Autopilot ap(m);
    if (const char* tr = std::getenv("JOA_BOT_TRACE")) ap.trace = std::atoi(tr);
    int t = 0;
    for (; t < cap && !done(m, objective) && m.finish() == 0 && !m.state().game_over; ++t) ap.step();
    for (const std::string& l : ap.log) std::printf("  %s\n", l.c_str());
    int fails = script_failures(m);
    if (!done(m, objective))
        std::printf("FAIL objective %d open after %d ticks (game over %d '%s') | bot %s\n", objective, t, int(m.state().game_over),
                    m.state().game_over_text.c_str(), ap.goal.c_str()), ++fails;
    std::string err;
    if (!fails && !write_save_atomic(out, encode_save(run.save()), 0, &err)) std::printf("FAIL save: %s\n", err.c_str()), ++fails;
    if (!fails) std::printf("objective %d at tick %d (mission tick %llu, rep %d): saved %s\n", objective, t, (unsigned long long)m.ticks(),
                            m.state().reputation, out);
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool checkpoint = argc > 2 && mode == "--checkpoint", auxerre = argc > 2 && mode == "--auxerre",
               troyes = argc > 2 && mode == "--troyes";
    const char* from = checkpoint || auxerre ? (argc > 3 ? argv[3] : "campaign_checkpoints/bot-S4L1.jsav")
                       : troyes             ? (argc > 3 ? argv[3] : kAuxerre)
                       : argc > 1           ? argv[1]
                                            : kTroyes;
    if (!read_file(from)) return std::printf("skip: no %s (game.campaign_legit / the earlier S6L1 segment writes it)\n", from), 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    if (checkpoint || auxerre) {
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
    if (auxerre) return segment(run, 1, kAuxerreTicks, argv[2]);
    if (troyes) return segment(run, 2, kTroyesTicks, argv[2]);
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
