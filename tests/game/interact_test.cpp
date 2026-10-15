// Interact key handlers (0x43bef0 free prisoner, 0x43bd40 talk, 0x43be40 bribe, 0x43bc50 item) and the
// interact indicator (FUN_006d3590) on S1L1 units placed in front of the player.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/campaign_runner.hpp"
#include "game/interact_player.hpp"

using namespace joa;
using namespace joa::game;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    if (!run.init(&err) || !run.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    Mission& m = *run.mission();
    m.autosave = false;
    World& w = m.world();
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    };
    Unit& p = *w.unit(w.player_id());
    auto put_front = [&](Unit& u, float d) {
        u.pos = {p.pos.x + std::cos(p.yaw_rad()) * d, p.pos.y + std::sin(p.yaw_rad()) * d, p.pos.z};
        u.vel = {};
    };
    auto park = [&](Unit& u) { u.pos = {p.pos.x - 5000.0f, p.pos.y - 5000.0f, p.pos.z}; };
    auto has_event = [&](MissionEvent::Kind k, const char* text, int b) {
        for (const auto& e : m.state().events)
            if (e.kind == k && e.text == text && e.b == b) return true;
        return false;
    };
    // Park everyone but the player far away so the searches only see the unit under test.
    int enemy = 0, friend_ = 0;
    for (Unit& u : w.units()) {
        if (u.deleted || u.id == p.id || w.dead(u)) continue;
        if (!enemy && w.hostile_to_player(u) && u.rank < 3) enemy = u.id;
        else if (!friend_ && !w.hostile_to_player(u) && u.nation == p.nation && !u.mount && u.rank < 3) friend_ = u.id;
        park(u);
        u.m.bribe = 0, u.m.can_interact = 0, u.no_react = false;
    }
    if (!enemy || !friend_) return std::printf("FAIL no test units\n"), 1;

    // Bribe: not enough gold -> one-shot message; then paid, side switched.
    Unit& e = *w.unit(enemy);
    put_front(e, 80.0f);
    e.m.bribe = 1;
    const int cost = bribe_cost(m, e);
    check(cost == int(double(e.hp * gd->c.bri_comm) * 0.5), "bribe cost = HP x BRI_COMM x max(0.5, (level-21)/10+1)");
    m.state().gold = 0;
    check(find_bribable(m, p, 200.0f) == enemy, "bribe search finds the enemy at 80");
    w.interact(p);
    w.interact(p);
    int no_gold = 0;
    for (const auto& ev : m.state().events) no_gold += ev.kind == MissionEvent::Kind::Interact && ev.text == "bribe_no_gold";
    check(no_gold == 1 && e.nation != p.nation, "no gold: message once, unit not bribed");
    m.state().gold = cost + 7;
    check(w.interact(p) && e.nation == p.nation && e.m.bribe == 2 && m.state().gold == 7 && m.state().enemy_bribed == 1 &&
              m.state().enemy_bribed_cost == cost,
          "bribe: gold paid, nation switched, counters");
    park(e);

    // Free prisoner: a friendly bound unit within 100 in front.
    Unit& f = *w.unit(friend_);
    put_front(f, 60.0f);
    f.no_react = true;
    const int saved = m.state().citizen_saved;
    m.tick();
    check(has_event(MissionEvent::Kind::InteractPrompt, "prisoner", friend_), "indicator shows the prisoner");
    put_front(f, 60.0f);
    check(w.interact(p) && !f.no_react && m.state().citizen_saved == saved + 1 && f.m.vision == 2000, "prisoner freed");
    // Behind the player: not found (bearing outside +-90).
    f.no_react = true;
    put_front(f, -60.0f);
    check(find_prisoner(m, p, 100.0f) == 0, "prisoner behind the player is ignored");
    f.no_react = false;
    park(f);

    // Talk: a can-interact unit whose class trigger mask has 0x200.
    int talker = 0;
    for (Unit& u : w.units())
        if (!u.deleted && u.id != p.id && !w.dead(u) && (gd->units[size_t(u.cls)].trigger_mask & 0x200u)) { talker = u.id; break; }
    if (talker) {
        Unit& t = *w.unit(talker);
        put_front(t, 70.0f);
        t.m.can_interact = 1;
        p.m.interact_unit = 0;
        check(w.interact(p), "talk handler fires");
        put_front(t, 70.0f);
        m.tick();   // Trigger_process delivers TRIGGER_INTERACT; the receiver writes the sender's +0x2e3
        check(p.m.interact_unit == talker, "talk: player +0x2e3 = talker");
        park(t);
        m.tick();   // Unit_process 0x52c490 phase 0: +0x2e3 / +0x2e5 cleared, the script sees the talk once
        check(p.m.interact_unit == 0 && p.m.interact_item == 0, "next tick clears +0x2e3 / +0x2e5");
        t.m.can_interact = 0;
        park(t);
    } else {
        std::printf("note: no S1L1 class with trigger bit 0x200\n");
    }
    // Nothing near: no handler fires, the indicator hides.
    m.tick();
    check(!w.interact(p) || find_interact_item(m, p, 200.0f) || find_recruit_facility(m, p) || find_workshop(m, p),
          "no target -> no handler (unless a map item / facility is in reach)");
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
