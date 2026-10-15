// Mission 7 (S7L1, La Charité) played by the campaign bot (game/autopilot_player + autopilot_plan_S7L1).
//
// Start checkpoint: the legitimate chain only reaches mission 4, so mission 7 starts from a forced-win chain with
// carried progression: CampaignRunner from a new game (or from FILE.jsav), every mission up to 6 won through
// Mission::finish_objectives() (the end-condition awards: gold and experience to every live hero, as a real win
// gives) and CONTINUE. Heroes therefore carry only the award experience (no kill experience, no bought items): a
// pessimistic start compared with a played campaign.
//
//   game_campaign_S7L1_bot_test                        forced chain from a new game, then the bot on Normal for kTicks:
//                                                      no script error / unimplemented builtin; the Gate Guard bribed
//                                                      without the alarm (event bits 14 / 13). This pessimistic start
//                                                      (Joan level 9) gets no further in kTicks; from the legit-chain
//                                                      checkpoint the bot reaches 5/6 WIN objectives (SAVES_AND_CAMPAIGN.md
//                                                      §8.1). Make `won` a check once the full win is reached.
//   game_campaign_S7L1_bot_test --checkpoint OUT [D] [FROM.jsav]
//                                                      write the mission-7 start (difficulty D, default 1) and exit;
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

constexpr int kTicks = 60000;
constexpr int kRetries = 1;

bool forced_to_7(CampaignRunner& run, int difficulty, const char* from, std::string& err) {
    run.profile.difficulty = difficulty;
    if (!run.init(&err)) return false;
    if (from) {
        auto bytes = read_file(from);
        auto f = bytes ? decode_save(*bytes, &err) : std::nullopt;
        if (!f || !migrate_save(*f, &err) || !run.load(*f, &err)) return false;
    } else if (!run.start(1, &err)) {
        return false;
    }
    while (run.mission()->scenario_no() < 7) {
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
    if (!forced_to_7(run, make && argc > 3 ? std::atoi(argv[3]) : 1, make && argc > 4 ? argv[4] : nullptr, err))
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
    const auto& bits = m.vm().state().bits;
    if (!((bits[1] >> 13) & 1u) || ((bits[1] >> 12) & 1u)) std::printf("FAIL Gate Guard not bribed / alarm raised\n"), ++fails;
    for (const std::string& l : r.log) std::printf("  %s\n", l.c_str());
    std::printf("%s %s ticks %d attempts %d deaths %d kills %d objectives %d/%d%s%s\n", r.stem.c_str(),
                r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills, r.objectives_done,
                r.objectives_total, r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    if (r.result == 1) std::printf("S7L1 won\n");
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
