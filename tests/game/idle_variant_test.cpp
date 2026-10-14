// Idle-variant picks on the cosmetic stream (port deviation 3):
// - Unit_stop on a ridden horse -> FUN_00429ec0: r <= IDLE2_CH (0.2) IDLE2, r <= IDLE3_CH (0.4) IDLE3,
//   else IDLE, one cosmetic draw per pick (exact replay of the expected action).
// - Unit_updateAction 0x7c7ab0 case IDLE: one draw every update; with <= 100 ms left in the idle loop and
//   r <= IDLE_CC a second draw picks IDLE2 (Joan has no IDLE3); IDLE2 ends in IDLE.
// The gameplay RNG never moves.
#include "game/rng_lcg.hpp"
#include "sim_fixture.hpp"

using namespace joa::game;

namespace {
// Cosmetic draws between `before` and `after` (-1 if more than 4).
int draws(Rng before, std::uint32_t after) {
    for (int k = 0; k <= 4; ++k) {
        if (before.seed == after) return k;
        rand15(before);
    }
    return -1;
}
}  // namespace

int main() {
    auto gd = load_data_or_skip();
    CHECK(NEAR(gd->c.idle2_ch, 0.2, 1e-6) && NEAR(gd->c.idle3_ch, 0.4, 1e-6) && NEAR(gd->c.idle_cc, 0.01, 1e-6),
          "GAMEPLAY.RES idle chances %f %f %f", gd->c.idle2_ch, gd->c.idle3_ch, gd->c.idle_cc);
    FlatTerrain flat;
    World w(*gd, &flat, 11);

    // --- FUN_00429ec0 via Unit_stop (horse with a rider) ------------------------------------------
    const int hid = w.spawn_unit("CAMP_HORSE", 2, {0, 0, 0}, 0);
    Unit& h = *w.unit(hid);
    CHECK(w.find_move(h, IDLE2) && w.find_move(h, IDLE3), "horse IDLE2/IDLE3 records");
    h.m.rider = hid;   // +0x1e6 only needs to be non-zero for Unit_stop's horse branch
    const std::uint32_t g0 = w.rng().seed;   // after spawning (Unit_init draws the idle row selector)
    int n[4] = {};
    for (int i = 0; i < 3000; ++i) {
        w.set_action(h, WALK);
        Rng c = w.cosmetic_rng();
        const float r = float(rand_unit_incl(c));
        const int want = r <= gd->c.idle2_ch ? IDLE2 : r <= gd->c.idle3_ch ? IDLE3 : IDLE;
        w.stop(h);
        CHECK(h.action == want, "pick %d: r %f -> %d, got %d", i, r, want, h.action);
        CHECK(w.cosmetic_rng().seed == c.seed, "one cosmetic draw per pick");
        ++n[h.action == IDLE2 ? 2 : h.action == IDLE3 ? 3 : 1];
    }
    std::printf("horse picks: IDLE %d IDLE2 %d IDLE3 %d\n", n[1], n[2], n[3]);
    CHECK(n[2] > 480 && n[2] < 720 && n[3] > 480 && n[3] < 720 && n[1] > 1620 && n[1] < 1980, "20/20/60 split");
    CHECK(w.rng().seed == g0, "gameplay RNG moved by the horse picks");
    h.m.rider = 0;
    w.delete_unit(hid);

    // --- IDLE_CC at the end of the idle loop -------------------------------------------------------
    const int jid = w.spawn_unit("JOAN_OF_ARC", 2, {500, 0, 0}, 0);
    Unit& j = *w.unit(jid);
    j.ai_controlled = false;   // no think stagger draws (gameplay stream)
    CHECK(w.find_move(j, IDLE2) && !w.find_move(j, IDLE3), "Joan has IDLE2 only");
    const std::uint32_t g1 = w.rng().seed;
    int picks = 0, ends = 0, idle_ticks = 0;
    for (int i = 0; i < 40000; ++i) {
        const int a0 = j.action, start0 = j.anim_start, len0 = w.anim_length(j);
        const Rng c0 = w.cosmetic_rng();
        w.tick({});
        const int k = draws(c0, w.cosmetic_rng().seed);
        if (a0 == IDLE) {
            ++idle_ticks;
            Rng c = c0;
            const float r = float(rand_unit_incl(c));
            const int left = len0 > 0 ? len0 - (w.now() - start0) % len0 : 0;
            const bool roll = left <= 100 && r <= gd->c.idle_cc;
            CHECK(k == (roll ? 2 : 1), "tick %d: %d draws (left %d, r %f)", i, k, left, r);
            CHECK(j.action == (roll ? IDLE2 : IDLE), "tick %d: action %d (roll %d)", i, j.action, int(roll));
            picks += roll;
        } else {
            CHECK(a0 == IDLE2 && k == 0, "tick %d: state %d, %d draws", i, a0, k);
            if (a0 == IDLE2 && j.action == IDLE) {
                ++ends;
                CHECK(w.now() - start0 >= len0, "IDLE2 ended early");
            }
        }
    }
    std::printf("Joan: %d IDLE2 picks, %d ends in %d idle ticks\n", picks, ends, idle_ticks);
    CHECK(picks > 0 && ends >= picks - 1, "IDLE2 picked and finished");
    CHECK(w.rng().seed == g1, "gameplay RNG moved: %u -> %u", g1, w.rng().seed);
    return finish();
}
