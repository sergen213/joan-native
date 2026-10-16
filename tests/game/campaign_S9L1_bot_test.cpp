// Mission 6 (S9L1, the sieges of Meung and Beaugency) played by the campaign bot (game/autopilot_player +
// autopilot_plan_S9L1), from the legitimate chain's mission-5 start through the S5L1 win into the S9L1 win.
//
// One bot run of S5L1 + S9L1 takes far longer than a test may (SAVES_AND_CAMPAIGN.md §8.1), so it is cut into CTest
// fixture segments of at most kSegmentTicks each (src/game/CMakeLists.txt): game.campaign_S9L1_seg1 starts from
// campaign_checkpoints/bot-S5L1.jsav (written by the game.campaign_legit fixture when the chain wins mission 4), every
// segment saves its end for the next one, a win CONTINUEs into the next mission and ends the segment. The test proper is
// the last segment: S9L1 must be won by its end, and it saves the S7L1 start (campaign_checkpoints/bot-S7L1.jsav, fixture
// of game.campaign_S7L1_charite). The bot's own memory starts fresh at every segment, as a player loading a save.
//
//   game_campaign_S9L1_bot_test --segment IN OUT [D]   one segment (D: set difficulty 0 Easy / 1 Normal first): must not
//                                                      lose; a mission past S9L1 passes through
//   game_campaign_S9L1_bot_test [IN]                   the last segment (default campaign_checkpoints/bot-S9L1-last.jsav):
//                                                      S9L1 must be won by its end; saves campaign_checkpoints/bot-S7L1.jsav
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

constexpr int kSegmentTicks = 60000;  // 2.5-8 ms a tick (machine load): inside a test's 600 s
constexpr int kAfter = 7;             // scenario after S9L1 (S7L1): reached = S9L1 won

int segment(const formats::Vfs& vfs, GameData& gd, const char* in, const char* out, int difficulty, bool last) {
    std::string err;
    auto bytes = read_file(in);
    if (!bytes) return std::printf("skip: no %s (an earlier segment / game.campaign_legit writes it)\n", in), 77;
    CampaignRunner run(gd, vfs);
    auto f = decode_save(*bytes, &err);
    if (!f || !migrate_save(*f, &err) || !run.init(&err) || !run.load(*f, &err)) return std::printf("FAIL load %s: %s\n", in, err.c_str()), 1;
    // Difficulty lives in the profile and the world (a campaign started on that difficulty carries it).
    if (difficulty >= 0) run.profile.difficulty = run.mission()->profile().difficulty = run.mission()->world().difficulty = difficulty;
    int fails = 0;
    if (run.mission()->scenario_no() < kAfter) {
        // No retry: RETRY of mission 6 restarts S9L1 without S5L1's world (Battle_start_scenario keep = 0).
        const MissionReport r = play_mission(run, kSegmentTicks, 0);
        Mission& m = *run.mission();
        for (const auto& [id, n] : m.host().unimplemented_calls())
            std::printf("FAIL unimplemented builtin %d %s\n", id, std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
        for (const auto& e : m.vm().errors()) std::printf("FAIL script error: %s\n", e.c_str()), ++fails;
        for (const std::string& l : r.log) std::printf("  %s\n", l.c_str());
        std::printf("%s %s ticks %d kills %d objectives %d/%d%s%s\ncaptured:", r.stem.c_str(),
                    r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "PLAYING", r.ticks, r.kills, r.objectives_done, r.objectives_total,
                    r.stuck.empty() ? "" : " | ", r.stuck.c_str());
        for (const Place& pl : m.places().places())
            if (pl.captured) std::printf(" %s", pl.name.c_str());
        std::printf("\n");
        if (r.result == 2) std::printf("FAIL %s lost\n", r.stem.c_str()), ++fails;
        if (r.result == 1 && !run.continue_after_win(&err)) std::printf("FAIL continue: %s\n", err.c_str()), ++fails;
    } else {
        std::printf("%s: S9L1 already won\n", in);
    }
    if (last && run.mission()->scenario_no() < kAfter) std::printf("FAIL S9L1 not won by the last segment\n"), ++fails;
    if (!fails && !write_save_atomic(out, encode_save(run.save()), 0, &err)) std::printf("FAIL save %s: %s\n", out, err.c_str()), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    if (argc > 3 && std::string(argv[1]) == "--segment")
        return segment(vfs, *gd, argv[2], argv[3], argc > 4 ? std::atoi(argv[4]) : -1, false);
    return segment(vfs, *gd, argc > 1 ? argv[1] : "campaign_checkpoints/bot-S9L1-last.jsav", "campaign_checkpoints/bot-S7L1.jsav", -1, true);
}
