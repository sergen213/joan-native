// Mission 5 (S5L1, the Loire: Jargeau) played by the campaign bot (game/autopilot_player + autopilot_plan_S5L1).
//
// Start checkpoint: the legitimate chain only reaches mission 4, so mission 5 starts from the chain's mission-4 start
// (campaign_checkpoints/bot-S4L1.jsav, written by the game.campaign_legit fixture into this build directory) with
// mission 4 won through Mission::finish_objectives() (the end-condition awards, as a real win gives) and CONTINUE:
// Joan level 11, heroes as the legit chain carried them. Without that file the test is skipped.
//
// The bot won S5L1 on Easy in 158169 ticks and on Normal in 187953 (SAVES_AND_CAMPAIGN.md §8.1). A full run is longer
// than a test may take (~18000 ticks a minute on an idle machine, a third of that under load), so the test plays kTicks
// and checks a win is the script's (Jargeau captured), short of one the progress measured there: Joan alive, stocked in
// Orleans (at least kFoodBought heal food from the potion seller: one bag cell a loaf, so she drops her spare apples
// first), Dunois and d'Illiers met (objectives 18 / 19) and at most kMaxLeft fightable English left in the two castle
// places (the castle fight at t40000 varies run to run).
//
//   game_campaign_S5L1_bot_test [FROM_S4L1.jsav] [DIFFICULTY] [TICKS]   the bot on the mission-5 start
//   game_campaign_S5L1_bot_test --checkpoint OUT [FROM] [D]             write the mission-5 start and exit; iterate with
//                                                                       joa-campaign --load OUT --autopilot
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

constexpr int kTicks = 40000;  // d'Illiers is met at ~t34000 (Orleans' errands first)
constexpr int kRetries = 1;
constexpr int kMaxLeft = 200;   // 136 English at the start, waves join as the French close in: 122-186 left at t40000
constexpr int kFoodBought = 10;  // Normal, seed 1: 14 (0 with the bag full of apples)

bool start_5(CampaignRunner& run, const char* from, int difficulty, std::string& err) {
    if (!run.init(&err)) return false;
    auto bytes = read_file(from);
    auto f = bytes ? decode_save(*bytes, &err) : std::nullopt;
    if (!f || !migrate_save(*f, &err) || !run.load(*f, &err)) return false;
    while (run.mission()->scenario_no() < 5) {
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
    return run.mission()->info().stem() == "S5L1" || (err = "not at S5L1", false);
}

// number_of_fightable_enemy_in_place (script_bind_places.cpp) over Castle_Jargeau and castleJargeau2: hostile, alive,
// not unit class 0/8/9 nor special 2/3/7.
int castle_left(Mission& m) {
    World& w = m.world();
    const int a = m.places().place_by_name("Castle_Jargeau"), b = m.places().place_by_name("castleJargeau2");
    int n = 0;
    for (const Unit& u : w.units()) {
        if (u.deleted || (u.hp < 1 && u.lives == 0) || w.dead(u) || u.cls < 0 || !w.hostile_to_player(u)) continue;
        const UnitClass& c = m.data().units[std::size_t(u.cls)];
        if (c.unit_class == 0 || c.unit_class == 8 || c.unit_class == 9 || c.special == 2 || c.special == 3 || c.special == 7) continue;
        n += m.places().inside(a, u.pos) || m.places().inside(b, u.pos);
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    const bool make = argc > 2 && std::string(argv[1]) == "--checkpoint";
    const int a = make ? 3 : 1;
    const char* from = argc > a ? argv[a] : "campaign_checkpoints/bot-S4L1.jsav";
    const int difficulty = argc > a + 1 ? std::atoi(argv[a + 1]) : 1;
    const int ticks = !make && argc > a + 2 ? std::atoi(argv[a + 2]) : kTicks;
    if (!read_file(from)) return std::printf("skip: no %s (game.campaign_legit writes it)\n", from), 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    if (!start_5(run, from, difficulty, err)) return std::printf("FAIL start: %s\n", err.c_str()), 1;
    if (make) {
        if (!write_save_atomic(argv[2], encode_save(run.save()), 0, &err)) return std::printf("FAIL save: %s\n", err.c_str()), 1;
        return std::printf("saved %s\n", argv[2]), 0;
    }
    const MissionReport r = play_mission(run, ticks, kRetries);
    Mission& m = *run.mission();
    int fails = 0;
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("FAIL unimplemented builtin %d %s\n", id, std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
    for (const auto& e : m.vm().errors()) std::printf("FAIL script error: %s\n", e.c_str()), ++fails;
    for (const std::string& l : r.log) std::printf("  %s\n", l.c_str());
    std::printf("%s %s ticks %d attempts %d deaths %d kills %d objectives %d/%d%s%s\n", r.stem.c_str(),
                r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills, r.objectives_done,
                r.objectives_total, r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    const Place* j = m.places().place(m.places().place_by_name("Castle_Jargeau"));
    if (r.result == 1) {
        // The win must be the script's: Jargeau captured (S9L1, which continues in this world, needs it).
        if (!(j && j->captured)) std::printf("FAIL Castle_Jargeau not captured\n"), ++fails;
        std::printf(fails ? "%d failures\n" : "ok\n", fails);
        return fails ? 1 : 0;
    }
    const auto& end = m.state().end;
    const auto done = [&](std::size_t i) { return end.size() > i && end[i].finish == 1; };
    const MapItem* tower = m.objects().item(m.objects().find_by_name("JargeauWestLeft"));
    const Unit* p = m.world().unit(m.world().player_id());
    const bool inside = p && (m.places().inside(m.places().place_by_name("Castle_Jargeau"), p->pos) ||
                              m.places().inside(m.places().place_by_name("castleJargeau2"), p->pos));
    const int left = castle_left(m);
    std::printf("progress: met Dunois %d d'Illiers %d, West Left tower hp %d, Joan in the castle %d, %d fightable English left\n",
                int(done(17)), int(done(18)), tower ? tower->hp : -1, int(inside), left);
    if (r.result == 2) std::printf("FAIL S5L1 lost\n"), ++fails;
    if (!done(17) || !done(18)) std::printf("FAIL Dunois / d'Illiers not met\n"), ++fails;
    if (std::count_if(r.log.begin(), r.log.end(), [](const std::string& l) { return l.find(" buy fd_") != std::string::npos; }) < kFoodBought)
        std::printf("FAIL under %d food bought in Orleans\n", kFoodBought), ++fails;
    if (left > kMaxLeft) std::printf("FAIL %d fightable English left in Jargeau (at most %d)\n", left, kMaxLeft), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
