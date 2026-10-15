// Mission 7 (S7L1, La Charité) played by the campaign bot (game/autopilot_player + autopilot_plan_S7L1).
//
// Start: the legitimate chain only reaches mission 4, so mission 7 starts from the chain's mission-4 start
// (campaign_checkpoints/bot-S4L1.jsav, written by the game.campaign_legit fixture) with missions 4-6 won through
// Mission::finish_objectives() (the end-condition awards, as a real win gives) and CONTINUE: Joan level 12.
// The whole mission takes the bot ~1 M ticks, longer than a test may take, so it is cut at a self-generated
// checkpoint: the bot plays the mission start up to La Charité's gates opening (event bit 2:26, the trebuchet's first
// stone on "wall2"; Saint-Pierre, Varzy, the gold, the Goughs and Begley done on the way, the Gate Guard bribed without
// the alarm) and saves campaign_checkpoints/bot-S7L1-charite.jsav (game.campaign_S7L1_charite, fixture). The test proper
// plays on from there and must win: La Charité captured (objective 2), Gressart (5) and Hudleg (26) dead.
//
//   game_campaign_S7L1_bot_test [CHARITE.jsav] [TICKS]                    the bot from the La Charité checkpoint: must win
//   game_campaign_S7L1_bot_test --charite OUT [FROM_S4L1.jsav] [D]        play the mission start to the gates, save OUT
//   game_campaign_S7L1_bot_test --checkpoint OUT [D] [FROM.jsav]          write the mission-7 start (default: forced from a
//                                                                         new game) and exit; iterate with
//                                                                         joa-campaign --load OUT --autopilot
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

constexpr int kTicks = 700000;          // to the win from the checkpoint (Normal, seed 1: see SAVES_AND_CAMPAIGN.md §8.1)
constexpr int kChariteTicks = 700000;   // mission start to the gates
constexpr const char* kCharite = "campaign_checkpoints/bot-S7L1-charite.jsav";

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
    // Difficulty lives in the profile and the world (a campaign started on that difficulty carries it).
    run.profile.difficulty = run.mission()->profile().difficulty = run.mission()->world().difficulty = difficulty;
    return true;
}

bool gates_open(Mission& m) { return (m.vm().state().bits[1] >> 25) & 1u; }

int script_failures(Mission& m) {
    int fails = 0;
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("FAIL unimplemented builtin %d %s\n", id, std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
    for (const auto& e : m.vm().errors()) std::printf("FAIL script error: %s\n", e.c_str()), ++fails;
    return fails;
}

// --charite: the bot from the mission-7 start until the gates open (a game over or the tick cap fails).
int make_charite(CampaignRunner& run, const char* out) {
    Mission& m = *run.mission();
    m.autosave = false;
    Autopilot ap(m);
    if (const char* tr = std::getenv("JOA_BOT_TRACE")) ap.trace = std::atoi(tr);
    int t = 0;
    for (; t < kChariteTicks && !gates_open(m) && m.finish() == 0 && !m.state().game_over; ++t) ap.step();
    for (const std::string& l : ap.log) std::printf("  %s\n", l.c_str());
    for (std::size_t i = 0; i < m.state().end.size(); ++i)
        if (m.state().end[i].finish == 2) std::printf("  end condition %zu (type %d) failed\n", i + 1, int(m.state().end[i].type));
    int fails = script_failures(m);
    const auto& bits = m.vm().state().bits;
    if (!((bits[1] >> 13) & 1u) || ((bits[1] >> 12) & 1u)) std::printf("FAIL Gate Guard not bribed / alarm raised\n"), ++fails;
    if (!gates_open(m)) std::printf("FAIL La Charite's gates shut after %d ticks (game over %d)\n", t, int(m.state().game_over)), ++fails;
    std::string err;
    if (!fails && !write_save_atomic(out, encode_save(run.save()), 0, &err)) std::printf("FAIL save: %s\n", err.c_str()), ++fails;
    if (!fails) std::printf("gates open at tick %d: saved %s\n", t, out);
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool checkpoint = argc > 2 && mode == "--checkpoint", charite = argc > 2 && mode == "--charite";
    const char* from = charite ? (argc > 3 ? argv[3] : "campaign_checkpoints/bot-S4L1.jsav") : checkpoint && argc > 4 ? argv[4] : nullptr;
    const char* start = checkpoint || charite ? nullptr : argc > 1 ? argv[1] : kCharite;
    if (const char* need = from ? from : start; need && !read_file(need))
        return std::printf("skip: no %s (game.campaign_legit / game.campaign_S7L1_charite write it)\n", need), 77;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    if (start) {
        auto f = decode_save(*read_file(start), &err);
        if (!f || !migrate_save(*f, &err) || !run.init(&err) || !run.load(*f, &err)) return std::printf("FAIL load: %s\n", err.c_str()), 1;
    } else {
        const int d = checkpoint ? (argc > 3 ? std::atoi(argv[3]) : 1) : argc > 4 ? std::atoi(argv[4]) : 1;
        if (!forced_to_7(run, d, from, err)) return std::printf("FAIL chain: %s\n", err.c_str()), 1;
    }
    if (checkpoint) {
        if (!write_save_atomic(argv[2], encode_save(run.save()), 0, &err)) return std::printf("FAIL save: %s\n", err.c_str()), 1;
        return std::printf("saved %s\n", argv[2]), 0;
    }
    if (charite) return make_charite(run, argv[2]);
    const MissionReport r = play_mission(run, argc > 2 ? std::atoi(argv[2]) : kTicks, 0);
    Mission& m = *run.mission();
    int fails = script_failures(m);
    for (const std::string& l : r.log) std::printf("  %s\n", l.c_str());
    std::printf("%s %s ticks %d attempts %d deaths %d kills %d objectives %d/%d%s%s\n", r.stem.c_str(),
                r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills, r.objectives_done,
                r.objectives_total, r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    const auto& end = m.state().end;
    for (int n : {2, 5, 26})
        if (end.size() < std::size_t(n) || end[std::size_t(n - 1)].finish != 1) std::printf("FAIL objective %d open\n", n), ++fails;
    if (r.result != 1) std::printf("FAIL S7L1 not won\n"), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
