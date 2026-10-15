// Mission 8 (S6L1, The Road to Coronation) played by the campaign bot (game/autopilot_player + autopilot_plan_S6L1),
// then CONTINUE: the last scenario plays the ending (MissionEvent::Ending) instead of a next mission.
//
// Start checkpoint: self-generated, as game.campaign_S7L1_bot does: CampaignRunner from a new game (or from FILE.jsav),
// every mission up to 7 won through Mission::finish_objectives() (the end-condition awards) and CONTINUE, so the heroes
// carry only the award experience: a pessimistic start compared with a played campaign.
//
//   game_campaign_S6L1_bot_test                        forced chain from a new game, then the bot (must win + ending)
//   game_campaign_S6L1_bot_test --checkpoint OUT [D] [FROM.jsav]
//                                                      write the mission-8 start (difficulty D, default 1) and exit;
//                                                      iterate with joa-campaign --load OUT --autopilot
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

constexpr int kTicks = 200000;
constexpr int kRetries = 0;

bool forced_to_8(CampaignRunner& run, int difficulty, const char* from, std::string& err) {
    run.profile.difficulty = difficulty;
    if (!run.init(&err)) return false;
    if (from) {
        auto bytes = read_file(from);
        auto f = bytes ? decode_save(*bytes, &err) : std::nullopt;
        if (!f || !migrate_save(*f, &err) || !run.load(*f, &err)) return false;
    } else if (!run.start(1, &err)) {
        return false;
    }
    while (run.mission()->scenario_no() < 8) {
        Mission& m = *run.mission();
        m.autosave = false;
        for (int t = 0; t < 60; ++t) m.tick();
        m.finish_objectives();
        for (int t = 0; m.finish() == 0 && t < 6000; ++t) m.tick();
        if (m.finish() != 1) return err = m.info().stem() + " forced win failed", false;
        if (!run.continue_after_win(&err)) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    const bool make = argc > 2 && std::string(argv[1]) == "--checkpoint";
    if (!forced_to_8(run, make && argc > 3 ? std::atoi(argv[3]) : 1, make && argc > 4 ? argv[4] : nullptr, err))
        return std::printf("FAIL chain: %s\n", err.c_str()), 1;
    if (make) {
        if (!write_save_atomic(argv[2], encode_save(run.save()), 0, &err)) return std::printf("FAIL save: %s\n", err.c_str()), 1;
        return std::printf("saved %s\n", argv[2]), 0;
    }
    const MissionReport r = play_mission(run, kTicks, kRetries);
    Mission& m = *run.mission();
    int fails = 0;
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("FAIL unimplemented builtin %d %s\n", id, std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
    for (const auto& e : m.vm().errors()) std::printf("FAIL script error: %s\n", e.c_str()), ++fails;
    for (const std::string& l : r.log) std::printf("  %s\n", l.c_str());
    std::printf("%s %s ticks %d attempts %d deaths %d kills %d objectives %d/%d rep %d%s%s\n", r.stem.c_str(),
                r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills, r.objectives_done,
                r.objectives_total, m.state().reputation, r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    if (r.result != 1) {
        std::printf("FAIL S6L1 not won\n");
        return 1;
    }
    // CONTINUE after the last scenario: the ending (FUN_006d19e0(0)), no next mission.
    m.take_events();
    if (run.continue_after_win(&err) || err != "campaign complete") std::printf("FAIL continue: '%s'\n", err.c_str()), ++fails;
    bool ending = false;
    for (const MissionEvent& e : m.take_events()) ending |= e.kind == MissionEvent::Kind::Ending;
    if (!ending) std::printf("FAIL no Ending event\n"), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
