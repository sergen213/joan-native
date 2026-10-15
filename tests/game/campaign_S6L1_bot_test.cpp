// Mission 8 (S6L1, The Road to Coronation) played by the campaign bot (game/autopilot_player + autopilot_plan_S6L1).
//
// Start checkpoint: as game.campaign_S5L1_bot, the legitimate chain's mission-4 start (campaign_checkpoints/bot-S4L1.jsav,
// written by the game.campaign_legit fixture into this build directory), missions 4..7 won through
// Mission::finish_objectives() (the end-condition awards) and CONTINUE: Joan level 13. Without that file the test is skipped.
//
// The mission spans the whole map (Auxerre, Troyes and its trebuchet hill, the camps, Chalons, Reims) and needs reputation
// 80; the bot does not win it yet (SAVES_AND_CAMPAIGN.md §8.1). Within kTicks it gets through Auxerre's trap: Glasdale
// (objective 13) killed and the duke's cannons built at the hut for the corner towers (how many towers fall in time varies
// from run to run: 0..4). The test checks that measured progress: Joan, Charles and Alencon alive (no defeat), Glasdale
// dead, a French cannon standing or a tower down. A win also
// checks the ending (CONTINUE after the last scenario: MissionEvent::Ending, no next mission).
//
//   game_campaign_S6L1_bot_test [FROM_S4L1.jsav] [DIFFICULTY] [TICKS]   the bot on the mission-8 start
//   game_campaign_S6L1_bot_test --checkpoint OUT [FROM] [D]             write the mission-8 start and exit; iterate with
//                                                                       joa-campaign --load OUT --autopilot
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

// No tower minimum: the crew can die at a corner tower's gun spot (SAVES_AND_CAMPAIGN.md §8.1 S6L1). The harness and a
// save/load drive loop are deterministic and identical (MissionBot8d); the old "drive took Auxerre, test stuck" gap came
// from a different start checkpoint.
constexpr int kTicks = 120000;
constexpr int kRetries = 0;

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
    if (!start_8(run, from, difficulty, err)) return std::printf("FAIL start: %s\n", err.c_str()), 1;
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
    std::printf("%s %s ticks %d attempts %d deaths %d kills %d objectives %d/%d rep %d%s%s\n", r.stem.c_str(),
                r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills, r.objectives_done,
                r.objectives_total, m.state().reputation, r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    if (r.result == 1) {
        m.take_events();
        if (run.continue_after_win(&err) || err != "campaign complete") std::printf("FAIL continue: '%s'\n", err.c_str()), ++fails;
        bool ending = false;
        for (const MissionEvent& e : m.take_events()) ending |= e.kind == MissionEvent::Kind::Ending;
        if (!ending) std::printf("FAIL no Ending event\n"), ++fails;
        std::printf(fails ? "%d failures\n" : "ok\n", fails);
        return fails ? 1 : 0;
    }
    const auto& end = m.state().end;
    const bool glasdale = end.size() > 12 && end[12].finish == 1;
    int towers = 0, cannons = 0;
    for (const Unit& u : m.world().units())
        cannons += !u.deleted && !m.world().dead(u) && u.nation == m.world().unit(m.world().player_id())->nation &&
                   m.data().units[std::size_t(u.cls)].code == "CANNON";
    for (const char* t : {"S6L1_2_Tower1", "S6L1_2_Tower2", "S6L1_2_Tower3", "S6L1_2_Tower4"})
        if (const MapItem* it = m.objects().item(m.objects().find_by_name(t)); it && it->hp <= 0) ++towers;
    std::printf("progress: Glasdale dead %d, French cannons %d, Auxerre towers down %d/4\n", int(glasdale), cannons, towers);
    if (r.result == 2) std::printf("FAIL S6L1 lost\n"), ++fails;
    if (!glasdale) std::printf("FAIL Glasdale (objective 13) not defeated\n"), ++fails;
    if (cannons == 0 && towers == 0) std::printf("FAIL no cannon built and no Auxerre tower down\n"), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
