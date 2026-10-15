// Legitimate campaign run: the campaign bot (game/autopilot_player, InputFrames only + character-screen point
// allocation) plays the campaign on Normal with carried progression, retrying a loss twice like the game-over RETRY.
//
// No argument (game.campaign_legit, CTest fixture setup): the chain from a new game, stopping at the first mission it
// cannot win. Writes campaign_checkpoints/bot-<stem>.jsav at every mission start and campaign_checkpoints/chain.txt
// (one report per mission) into the working directory (the build tree), checks no script errors / unimplemented
// builtins and at least kExpectedWins missions won.
// Argument N (game.campaign_legit_mission_N): mission N from the chain's checkpoint, which must replay the chain's
// report exactly (result, ticks, hash: determinism through save / load); SKIP when the chain did not reach it.
// Prints one report line per mission (result, ticks, attempts, deaths, kills, objectives, where it got stuck).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

constexpr int kTicks = 150000;     // per attempt (83 min of game time; mission 3 on Normal is won at 81000-125000)
constexpr int kRetries = 8;       // a lost mission is retried like the game-over RETRY: up to 9 attempts (seed + attempt)
constexpr int kExpectedWins = 3;   // missions won on Normal; raise when the bot (or a port fix) wins more (docs/SAVES_AND_CAMPAIGN.md §8.1)
const std::filesystem::path kDir = "campaign_checkpoints";

int fails = 0;

std::string line(const MissionReport& r) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d %s %s ticks %d attempts %d deaths %d kills %d objectives %d/%d hash %016llx", r.no, r.stem.c_str(),
                  r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills, r.objectives_done,
                  r.objectives_total, static_cast<unsigned long long>(r.hash));
    return buf;
}

MissionReport play(CampaignRunner& run) {
    MissionReport r = play_mission(run, kTicks, kRetries);
    Mission& m = *run.mission();
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("FAIL %s unimplemented builtin %d %s\n", m.info().stem().c_str(), id, std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
    for (const auto& e : m.vm().errors()) std::printf("FAIL %s script error: %s\n", m.info().stem().c_str(), e.c_str()), ++fails;
    for (const std::string& l : r.log) std::printf("  %s\n", l.c_str());
    std::printf("%s%s%s\n", line(r).c_str(), r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    std::fflush(stdout);
    return r;
}

bool checkpoint(CampaignRunner& run) {
    std::string err;
    const auto path = kDir / ("bot-" + run.mission()->info().stem() + ".jsav");
    if (!write_save_atomic(path, encode_save(run.save()), 0, &err)) return std::printf("FAIL save %s: %s\n", path.c_str(), err.c_str()), ++fails, false;
    return true;
}

int chain(GameData& gd, const formats::Vfs& vfs) {
    std::filesystem::remove_all(kDir);
    std::filesystem::create_directories(kDir);
    std::ofstream out(kDir / "chain.txt");
    CampaignRunner run(gd, vfs);
    std::string err;
    if (!run.init(&err) || !run.start(1, &err)) return std::printf("FAIL start: %s\n", err.c_str()), 1;
    int wins = 0;
    while (checkpoint(run)) {
        const MissionReport r = play(run);
        out << line(r) << '\n' << std::flush;
        wins += r.result == 1;
        if (r.result != 1 || !run.continue_after_win(&err)) break;
    }
    if (wins < kExpectedWins) std::printf("FAIL %d missions won, expected >= %d\n", wins, kExpectedWins), ++fails;
    std::printf(fails ? "%d failures\n" : "ok (%d won)\n", fails ? fails : wins);
    return fails ? 1 : 0;
}

int mission(GameData& gd, const formats::Vfs& vfs, int no) {
    std::string want, err;
    for (std::ifstream in(kDir / "chain.txt"); std::getline(in, want);)
        if (std::atoi(want.c_str()) == no) break;
    if (std::atoi(want.c_str()) != no) return std::printf("mission %d not reached by the chain\n", no), 77;
    const std::string stem = want.substr(want.find(' ') + 1, want.find(' ', want.find(' ') + 1) - want.find(' ') - 1);
    auto bytes = read_file(kDir / ("bot-" + stem + ".jsav"));
    auto f = bytes ? decode_save(*bytes, &err) : std::nullopt;
    CampaignRunner run(gd, vfs);
    if (!f || !migrate_save(*f, &err) || !run.init(&err) || !run.load(*f, &err)) return std::printf("FAIL load %s: %s\n", stem.c_str(), err.c_str()), 1;
    const std::string got = line(play(run));
    if (got != want) std::printf("FAIL replay differs from the chain:\n  chain  %s\n  replay %s\n", want.c_str(), got.c_str()), ++fails;
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
    return argc > 1 ? mission(*gd, vfs, std::atoi(argv[1])) : chain(*gd, vfs);
}
