// Mission 7 (S7L1, La Charité) played by the campaign bot (game/autopilot_player + autopilot_plan_S7L1).
//
// Start: the legitimate chain's S7L1 start (campaign_checkpoints/bot-S7L1.jsav, saved by game.campaign_S9L1_bot when the
// bot wins S9L1 and CONTINUEs).
// The whole mission takes the bot ~470000 ticks (~500 s), longer than a test may take, so it is cut at a self-generated
// checkpoint: the bot plays the mission start up to La Charité's gates opening (event bit 2:26, the trebuchet's first
// stone on "wall2"; Saint-Pierre, Varzy, the gold, the Goughs and Begley done on the way, the Gate Guard bribed without
// the alarm) and saves campaign_checkpoints/bot-S7L1-charite.jsav (game.campaign_S7L1_charite, fixture). The test proper
// plays on from there and must win: La Charité captured (objective 2), Gressart (5) and Hudleg (26) dead; it CONTINUEs
// and saves the S6L1 start (campaign_checkpoints/bot-S6L1.jsav, fixture of game.campaign_S6L1_auxerre).
//
//   game_campaign_S7L1_bot_test [CHARITE.jsav] [TICKS]          the bot from the La Charité checkpoint: must win
//   game_campaign_S7L1_bot_test --charite OUT [FROM_S7L1.jsav]  play the mission start to the gates, save OUT
//                                                               (iterate with joa-campaign --load FROM --autopilot)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

constexpr int kTicks = 500000;          // to the win from the checkpoint (Normal, seed 1: 269068, see SAVES_AND_CAMPAIGN.md §8.1)
constexpr int kChariteTicks = 400000;   // mission start to the gates (197129)
constexpr const char* kCharite = "campaign_checkpoints/bot-S7L1-charite.jsav";
constexpr const char* kS6L1 = "campaign_checkpoints/bot-S6L1.jsav";

bool load(CampaignRunner& run, const char* path, std::string& err) {
    auto f = decode_save(*read_file(path), &err);
    if (!f || !migrate_save(*f, &err) || !run.init(&err) || !run.load(*f, &err)) return false;
    return run.mission()->info().stem() == "S7L1" || (err = std::string(path) + " is not at S7L1", false);
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
    const bool charite = argc > 2 && mode == "--charite";
    const char* from = charite ? (argc > 3 ? argv[3] : "campaign_checkpoints/bot-S7L1.jsav") : argc > 1 ? argv[1] : kCharite;
    if (!read_file(from)) return std::printf("skip: no %s (game.campaign_S9L1_bot / game.campaign_S7L1_charite write it)\n", from), 77;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    if (!load(run, from, err)) return std::printf("FAIL load: %s\n", err.c_str()), 1;
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
    if (!fails && !run.continue_after_win(&err)) std::printf("FAIL continue: %s\n", err.c_str()), ++fails;
    if (!fails && !write_save_atomic(kS6L1, encode_save(run.save()), 0, &err)) std::printf("FAIL save: %s\n", err.c_str()), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
