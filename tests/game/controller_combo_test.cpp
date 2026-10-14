// Player controller timing (Part B §3.2, Part C §2.2) with Joan's move table (JOAN.csv):
// combo input window [START_TIME 0, END_TIME 317) for "A", queued follow-up fires at END_TIME,
// 250 ms post-combo lockout, double-tap evade within 300 ms. One frame = one 33 ms tick.
#include "sim_fixture.hpp"

using namespace joa::game;

namespace {
struct Rig {
    FlatTerrain flat{0};
    World w;
    int joan;
    explicit Rig(GameData& gd) : w(gd, &flat, 0) {
        joan = w.spawn_unit("JOAN_OF_ARC", gd.nation_id("FRENCH"), {0, 0, 0}, 0);
        w.set_player(joan);
    }
    Unit& J() { return *w.unit(joan); }
    // One frame; `key` pressed this frame (0 = nothing). Returns the clock at which input ran.
    int frame(int key = 0, bool keyboard = false) {
        InputFrame f;
        if (key) f.press(key), f.key_char_event = keyboard;
        const int t = w.now();
        w.tick(f);
        return t;
    }
    void idle(int ms) { for (int t = 0; t < ms; t += World::kTickMs) frame(); }
    int action_start(int act, std::string_view combo) {
        for (const auto& e : w.events())
            if (e.type == Ev::Action && e.value == act && (combo.empty() || J().combo == combo)) return e.time;
        return -1;
    }
};
}  // namespace

int main() {
    auto gd = load_data_or_skip();
    {   // Queued follow-up: second A pressed inside [0,317) starts "AA" at the first frame >= 317 ms.
        Rig r(*gd);
        r.idle(99);
        const int t0 = r.frame(IN_ATTACK_A);
        CHECK(r.J().action == ATTACK && r.J().combo == "A", "A start: action %d combo %s", r.J().action, r.J().combo.c_str());
        CHECK(r.w.controller().attack_ts == t0, "attack_ts %d vs %d", r.w.controller().attack_ts, t0);
        r.frame();                    // release
        r.frame(IN_ATTACK_A);         // re-press at t0+66: inside the window -> queued
        CHECK(r.w.controller().follow == 1, "follow-up not queued");
        int fired = -1;
        for (int i = 0; i < 20 && fired < 0; ++i) {
            r.w.take_events();
            const int t = r.frame();
            if (r.J().combo == "AA") fired = t;
        }
        // First frame with now - t0 >= 317: frames run at multiples of 33 -> t0 + 330.
        CHECK(fired == t0 + 330, "AA fired at %d (t0 %d)", fired, t0);
    }
    {   // Press outside the window -> no follow-up; combo ends at anim end, then 250 ms lockout.
        Rig r(*gd);
        r.idle(99);
        const int t0 = r.frame(IN_ATTACK_A);
        while (r.w.now() - t0 < 363) r.frame();
        r.frame(IN_ATTACK_A);         // since >= 317: outside [0,317)
        CHECK(r.w.controller().follow == 0, "late press queued");
        while (r.J().action == ATTACK && r.w.now() - t0 < 2000) r.frame();
        const int lock = r.J().combo_lock;
        CHECK(r.J().combo_idx == -1 && lock > 0, "lockout not set (idx %d lock %d)", r.J().combo_idx, lock);
        CHECK(lock - t0 >= 583 && lock - t0 < 583 + 33 + 33, "combo ended at %d (anim 583 ms)", lock - t0);
        while (r.w.now() - lock < 150) r.frame();
        r.frame(IN_ATTACK_A);
        CHECK(r.J().action != ATTACK, "attack during 250 ms lockout");
        while (r.w.now() - lock <= 250) r.frame();
        r.frame(IN_ATTACK_A);
        CHECK(r.J().action == ATTACK && r.J().combo == "A", "attack after lockout: %d %s", r.J().action, r.J().combo.c_str());
    }
    {   // Double-tap back within 300 ms -> evade jump back (JUMP_B).
        Rig r(*gd);
        r.idle(99);
        r.frame(IN_BACKWARD, true);
        r.frame();
        r.frame();
        r.frame(IN_BACKWARD, true);   // 99 ms after the first tap
        CHECK(r.J().action == JUMP_B, "double tap -> action %s", action_name(r.J().action));
    }
    {   // Same taps 330 ms apart: no evade.
        Rig r(*gd);
        r.idle(99);
        r.frame(IN_BACKWARD, true);
        r.idle(330);
        r.frame(IN_BACKWARD, true);
        CHECK(r.J().action != JUMP_B, "slow double tap evaded");
    }
    {   // Double-tap forward = rush; Joan has not learned RUSH_ATTACK at level 1 (JOANUP L3) and the
        // fallback path needs EP >= ENERGY_PT 5: with 30 EP it rushes (fallback ignores learning).
        Rig r(*gd);
        r.idle(99);
        r.frame(IN_FORWARD, true);
        r.frame();
        InputFrame f;
        f.press(IN_FORWARD);
        f.press(IN_ATTACK_A);
        f.key_char_event = true;
        r.w.tick(f);
        CHECK(r.J().action == RUSH && r.J().ep == 25, "W W+A rush: %s ep %d", action_name(r.J().action), r.J().ep);
    }
    return finish();
}
