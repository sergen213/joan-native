// Mission 6 (S9L1, the sieges of Meung and Beaugency) played by the campaign bot (game/autopilot_player +
// autopilot_plan_S9L1).
//
// Start checkpoint: the legitimate chain only reaches mission 4, so missions 1-4 are won through
// Mission::finish_objectives() (the end-condition awards, as a real win gives) and CONTINUE. S9L1 continues in S5L1's
// world, and its whole main thread (Meung, Loches, Montargis; S9L1_tactic l.2292) waits for Castle_Jargeau captured,
// which only S5L1's own capture code sets (S5L1_tactic l.4259): so S5L1 is won by removing every enemy inside
// Castle_Jargeau / castleJargeau2 (test hook) and putting Joan on the castle ground; the script then captures the
// castle, backs up its bit flags and wins objective 1 as in a played game. Joan's own squad is moved with her (a played
// Joan arrives with the team following her). Heroes carry only award experience: a pessimistic start compared with a
// played campaign (no kill experience, the other heroes and their squads left at Orleans).
//
//   game_campaign_S9L1_bot_test                        checkpoint from a new game, then the bot on Normal (see kTicks)
//   game_campaign_S9L1_bot_test --checkpoint OUT [D] [FROM.jsav]
//                                                      write the mission-6 start (difficulty D, default 1; FROM: a save of
//                                                      missions 1-5 to continue from) and exit
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "formats/script_funcs.hpp"
#include "game/autopilot_player.hpp"
#include "game/savegame_container.hpp"

using namespace joa;
using namespace joa::game;

namespace {

// The bot's budget (~5 min wall time) and the progress it must make in it. From this start the bot does not win S9L1:
// the heroes (levels 8-10) are outmatched by the English garrisons (soldiers of level 12+, Talbot / Glasdale / Fastolf /
// Scales at 23-25), and any hero's death ends the mission (imperatives 14 / 17 / 30 / 36). A win still passes; otherwise
// the places the player liberated during S9L1 (search_for_unit_in_place "P" with the box cleared) are the measure.
constexpr int kTicks = 90000;
constexpr int kMinLiberated = 1;

// S5L1 won the scripted way: Jargeau's garrison removed, Joan on the castle ground. The town's patrols within 2500 of
// the castle go too: the capture wins objective 1 but the WIN fires only after the scenario's 15 s win delay, and an
// idle award-only Joan (level 7, 60 HP) left there was killed by two light infantry walking in (LOSE 5, "Joan of Arc
// is defeated") before it ran out.
bool take_jargeau(Mission& m) {
    World& w = m.world();
    const int a = m.places().place_by_name("Castle_Jargeau"), b = m.places().place_by_name("castleJargeau2");
    Unit* p = w.unit(w.player_id());
    if (!p) return false;
    bool any = false;
    Vec3 spot = p->pos;
    for (const Unit& u : w.units())
        if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && (m.places().inside(a, u.pos) || m.places().inside(b, u.pos)))
            if (!any || u.pos.z < spot.z) spot = u.pos, any = true;  // the lowest stand: the castle ground, not a tower room
    for (Unit& u : w.units())
        if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) &&
            (m.places().inside(a, u.pos) || m.places().inside(b, u.pos) || std::hypot(u.pos.x - spot.x, u.pos.y - spot.y) < 2500))
            m.ai().set_unit_dead(u);
    p->pos = spot;
    for (std::size_t i = 0; i < p->members.size(); ++i)  // her squad in a ring around her
        if (Unit* u = w.unit(p->members[i])) {
            const float a = 6.2832f * float(i) / float(p->members.size());
            u->pos = {spot.x + 100 * std::cos(a), spot.y + 100 * std::sin(a), spot.z};
        }
    return any;
}

bool to_6(CampaignRunner& run, int difficulty, const char* from, std::string& err) {
    run.profile.difficulty = difficulty;
    if (!run.init(&err)) return false;
    if (from) {
        auto bytes = read_file(from);
        auto f = bytes ? decode_save(*bytes, &err) : std::nullopt;
        if (!f || !migrate_save(*f, &err) || !run.load(*f, &err)) return false;
    } else if (!run.start(1, &err)) {
        return false;
    }
    while (run.mission()->scenario_no() < 6) {
        Mission& m = *run.mission();
        m.autosave = false;
        for (int t = 0; t < (m.scenario_no() == 5 ? 600 : 60); ++t) m.tick();  // S5L1: past its stage set-up
        if (m.scenario_no() == 5 ? !take_jargeau(m) : (m.finish_objectives(), false)) return err = "no Jargeau garrison", false;
        for (int t = 0; m.finish() == 0 && t < 20000; ++t) m.tick();
        if (m.finish() != 1) {
            const Place* j = m.places().place(m.places().place_by_name("Castle_Jargeau"));
            err = m.info().stem() + " win failed: finish " + std::to_string(m.finish()) + " (" + m.state().game_over_text + "), Jargeau captured " +
                  std::to_string(j ? j->captured : -1) + ", open WIN records:";
            for (std::size_t i = 0; i < m.state().end.size(); ++i)
                if (const EndCondition& c = m.state().end[i]; c.enabled && c.has_des && c.type == EndCondition::Win && c.finish != 1) err += " " + std::to_string(i + 1);
            return false;
        }
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
    if (!to_6(run, make && argc > 3 ? std::atoi(argv[3]) : 1, make && argc > 4 ? argv[4] : nullptr, err))
        return std::printf("FAIL chain: %s\n", err.c_str()), 1;
    int fails = 0;
    const Place* jargeau = run.mission()->places().place(run.mission()->places().place_by_name("Castle_Jargeau"));
    if (!jargeau || !jargeau->captured) std::printf("FAIL Jargeau not captured at the S9L1 start\n"), ++fails;
    if (make) {
        if (!write_save_atomic(argv[2], encode_save(run.save()), 0, &err)) return std::printf("FAIL save: %s\n", err.c_str()), 1;
        return std::printf("saved %s\n", argv[2]), fails;
    }
    std::vector<int> held;  // places French at the S9L1 start (Jargeau, its barracks)
    for (const Place& pl : run.mission()->places().places())
        if (pl.captured) held.push_back(pl.recno);
    // No retry: RETRY of mission 6 restarts S9L1 without S5L1's world (Battle_start_scenario keep = 0).
    const MissionReport r = play_mission(run, kTicks, 0);
    Mission& m = *run.mission();
    int liberated = 0;
    std::string names;
    for (const Place& pl : m.places().places())
        if (pl.captured && std::find(held.begin(), held.end(), pl.recno) == held.end()) ++liberated, names += " " + pl.name;
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("FAIL unimplemented builtin %d %s\n", id, std::string(formats::script_funcs()[id - 1].name).c_str()), ++fails;
    for (const auto& e : m.vm().errors()) std::printf("FAIL script error: %s\n", e.c_str()), ++fails;
    for (const std::string& l : r.log) std::printf("  %s\n", l.c_str());
    std::printf("%s %s ticks %d deaths %d kills %d objectives %d/%d%s%s\n", r.stem.c_str(),
                r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.deaths, r.kills, r.objectives_done,
                r.objectives_total, r.stuck.empty() ? "" : " | ", r.stuck.c_str());
    std::printf("liberated %d:%s\n", liberated, names.c_str());
    if (r.result != 1 && liberated < kMinLiberated)
        std::printf("FAIL S9L1 not won and %d places liberated (< %d)\n", liberated, kMinLiberated), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
