// Legitimate campaign run: from a new game on Normal the campaign bot (game/autopilot_player, InputFrames
// only + character-screen point allocation) plays mission after mission with carried progression,
// retrying a loss twice like the game-over RETRY. The chain stops at the first mission it cannot win.
// Checks: the run is deterministic (same reports and hashes twice), no script errors / unimplemented
// builtins, and at least kExpectedWins missions are won legitimately. Prints one report line per mission
// (result, ticks, attempts, deaths, kills, objectives, where it got stuck).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"

using namespace joa;
using namespace joa::game;

namespace {

constexpr int kTicks = 150000;     // per attempt (83 min of game time; mission 3 on Normal is won at 81000-125000)
constexpr int kRetries = 2;
constexpr int kExpectedWins = 3;   // missions 1-3 on Normal; raise when the bot (or a port fix) wins more (docs/SAVES_AND_CAMPAIGN.md §8.1)

int fails = 0;

std::vector<MissionReport> run_chain(GameData& gd, const formats::Vfs& vfs) {
    std::vector<MissionReport> out;
    CampaignRunner run(gd, vfs);
    std::string err;
    if (!run.init(&err) || !run.start(1, &err)) return std::printf("FAIL start: %s\n", err.c_str()), ++fails, out;
    for (;;) {
        out.push_back(play_mission(run, kTicks, kRetries));
        Mission& m = *run.mission();
        for (const auto& [id, n] : m.host().unimplemented_calls())
            std::printf("FAIL %s unimplemented builtin %d %s\n", m.info().stem().c_str(), id,
                        std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
        for (const auto& e : m.vm().errors()) std::printf("FAIL %s script error: %s\n", m.info().stem().c_str(), e.c_str()), ++fails;
        if (out.back().result != 1 || !run.continue_after_win(&err)) break;
    }
    return out;
}

}  // namespace

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    const auto a = run_chain(*gd, vfs), b = run_chain(*gd, vfs);
    int wins = 0;
    for (const MissionReport& r : a) {
        wins += r.result == 1;
        std::printf("%d %s %s ticks %d attempts %d deaths %d kills %d objectives %d/%d hash %016llx%s%s\n", r.no, r.stem.c_str(),
                    r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills, r.objectives_done,
                    r.objectives_total, static_cast<unsigned long long>(r.hash), r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    }
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) same = a[i].hash == b[i].hash && a[i].ticks == b[i].ticks && a[i].result == b[i].result;
    if (!same) std::printf("FAIL nondeterministic campaign run\n"), ++fails;
    if (wins < kExpectedWins) std::printf("FAIL %d missions won, expected >= %d\n", wins, kExpectedWins), ++fails;
    std::printf(fails ? "%d failures\n" : "ok (%d won)\n", fails ? fails : wins);
    return fails ? 1 : 0;
}
