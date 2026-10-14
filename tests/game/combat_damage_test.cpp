// Melee rules against values computed by hand from GAMEPLAY_CHARACTER_COMBAT.md Part C §4..§5 with
// the shipped data (Joan STR 9, Warbrand dmg 10; English light infantry STR 4, sword dmg 40; both
// morale 70, armour level 1; GAMEPLAY.RES STR_DAM 3, DEF_RAT atoi 0, MOR_NOR 50, F_ATTACK 130,
// F_DEFEND 70, *_D 0.7/1/1.5/2, BLOCKANG 70, T_FALLF 6000, V_FALLF 3000, F_FALLR 250, INVC_DUR 6000).
#include "sim_fixture.hpp"

using namespace joa::game;

namespace {
struct Duo {
    std::unique_ptr<GameData>& gd;
    FlatTerrain flat{0};
    World w;
    int joan, eng;
    explicit Duo(std::unique_ptr<GameData>& g) : gd(g), w(*g, &flat, 0) {
        joan = w.spawn_unit("JOAN_OF_ARC", g->nation_id("FRENCH"), {0, 0, 0}, 0);
        eng = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", g->nation_id("ENGLISH"), {60, 0, 0}, 180);
        w.set_player(joan);
    }
    Unit& J() { return *w.unit(joan); }
    Unit& E() { return *w.unit(eng); }
    // Tick (no input) until the next Hit/Blocked event or the limit; returns that event type.
    Ev run_until_hit(int max_ticks = 40) {
        for (int i = 0; i < max_ticks; ++i) {
            w.tick({});
            for (const auto& e : w.take_events())
                if (e.type == Ev::Hit || e.type == Ev::Blocked) return e.type;
        }
        return Ev::Custom;
    }
};
}  // namespace

int main() {
    auto gd = load_data_or_skip();
    CHECK(gd->c.str_dam == 3 && gd->c.def_rat == 0 && gd->c.dex_dam == 1, "atoi quirks %d %d %d", gd->c.str_dam, gd->c.def_rat, gd->c.dex_dam);

    {   // 1. Joan 'A' on the English soldier (player side attacking).
        Duo d(gd);
        CHECK(d.w.set_action(d.J(), ATTACK, 'A'), "Joan ATTACK A");
        // power = (STR 9*3 + (10 - 0) + 100) * DAMAGE 2 / 100 = 2.74
        CHECK(NEAR(d.w.melee_attack_power(d.J()), 2.74, 1e-5), "power %f", d.w.melee_attack_power(d.J()));
        // atk = skill 1.0 * morale 1.2 * 2.74 = 3.288; def = 1.2 * (0 + armour 1) = 1.2;
        // dmg = 3.288/2 + (3.288-1.2)/2 = 2.688; x F_ATTACK 1.3 = 3.4944; 65 -> trunc(61.5056) = 61.
        const float dmg = d.w.apply_melee_damage(d.E(), d.J(), 0);
        CHECK(NEAR(dmg, 3.4944, 1e-4), "Joan A dmg %f", dmg);
        CHECK(d.E().hp == 61, "English hp %d", d.E().hp);
        // Crit x2 and chain level 2 (player only): 2.688*2*2*1.3 = 13.9776.
        d.J().crit_flag = true;
        d.w.controller().chain = 2;
        const float dmg2 = d.w.apply_melee_damage(d.E(), d.J(), 0);
        CHECK(NEAR(dmg2, 13.9776, 1e-3), "crit+chain dmg %f", dmg2);
        CHECK(d.E().hp == 47, "English hp after crit %d", d.E().hp);
    }
    {   // 2. English 'A' on Joan: player-hostile attacker, difficulty multiplier, F_DEFEND.
        Duo d(gd);
        CHECK(d.w.set_action(d.E(), ATTACK, 'A'), "English ATTACK A");
        // power = (4*3 + 40 + 100) * 3 / 100 = 4.56 (x NORM_D 1.0)
        CHECK(NEAR(d.w.melee_attack_power(d.E()), 4.56, 1e-5), "power %f", d.w.melee_attack_power(d.E()));
        // atk 5.472, def 1.2 -> 2.736 + 2.136 = 4.872; x F_DEFEND 0.7 = 3.4104; 60 -> 56.
        float dmg = d.w.apply_melee_damage(d.J(), d.E(), 0);
        CHECK(NEAR(dmg, 3.4104, 1e-4), "English A dmg %f", dmg);
        CHECK(d.J().hp == 56, "Joan hp %d", d.J().hp);
        d.w.difficulty = 2;   // DIFF_D 1.5: power 6.84 -> atk 8.208 -> 4.104 + 3.504 = 7.608 x 0.7 = 5.3256
        dmg = d.w.apply_melee_damage(d.J(), d.E(), 0);
        CHECK(NEAR(dmg, 5.3256, 1e-4), "hard dmg %f", dmg);
        CHECK(d.J().hp == 50, "Joan hp %d", d.J().hp);
        // DEF_RAT = atoi("0.3") = 0: the DEF stat and the front/side/back ignore % do nothing.
        d.J().def = 200;
        dmg = d.w.apply_melee_damage(d.J(), d.E(), 40);
        CHECK(NEAR(dmg, 5.3256, 1e-4), "DEF ignored dmg %f", dmg);
    }
    {   // 3. Block arc: GUARD1 blocks attackers within +-70 deg of the defender's facing (inclusive).
        Duo d(gd);
        auto try_from = [&](float deg) {
            Unit& e = d.E();
            const float r = deg * 0.017453292f;
            e.pos = {50 * std::cos(r), 50 * std::sin(r), 0};
            d.w.set_yaw(e, deg + 180);           // face Joan
            d.w.set_action(e, IDLE);             // restart the combo string ("A" record, Range 38)
            d.w.set_action(e, ATTACK, 'A');
            d.w.set_action(d.J(), GUARD1);
            d.J().hp = 60;
            d.w.take_events();
            return d.w.try_melee_hit(d.J(), e);
        };
        CHECK(try_from(0) == 0, "front not blocked");
        CHECK(try_from(69) == 0, "69 deg not blocked");
        CHECK(try_from(-69) == 0, "-69 deg not blocked");
        CHECK(try_from(71) > 0, "71 deg blocked");
        CHECK(try_from(180) > 0, "back blocked");
        d.w.set_action(d.J(), IDLE);   // not guarding: front hit lands
        Unit& e = d.E();
        e.pos = {50, 0, 0};
        d.w.set_yaw(e, 180);
        d.w.set_action(e, IDLE);
        d.w.set_action(e, ATTACK, 'A');
        CHECK(d.w.try_melee_hit(d.J(), e) > 0, "unguarded front hit");
        // Out of reach: Range 38 + sword reach 45 = 83.
        e.pos = {84, 0, 0};
        CHECK(d.w.try_melee_hit(d.J(), e) == 0, "out of reach hit");
        e.pos = {82, 0, 0};
        CHECK(d.w.try_melee_hit(d.J(), e) > 0, "in reach miss");
    }
    {   // 4. Balance / knockdown through the trigger path. A: force 3000/0 -> balance -12.
        Duo d(gd);
        CHECK(d.E().balance == 20, "initial balance %d", d.E().balance);
        d.w.set_action(d.J(), ATTACK, 'A');
        CHECK(d.run_until_hit() == Ev::Hit, "A hit");
        CHECK(d.E().balance == 8 && !is_knocked(d.E().action), "after A balance %d action %d", d.E().balance, d.E().action);
        d.w.set_action(d.J(), ATTACK, 'A');   // "AA" 3000/0: 8 - 12 -> 0 -> knockdown
        CHECK(d.run_until_hit() == Ev::Hit, "AA hit");
        CHECK(d.E().balance == 0 && is_knocked(d.E().action), "after AA balance %d action %d", d.E().balance, d.E().action);
        CHECK(d.E().knock_ms >= 500 && d.E().knock_ms < 3000, "lying time %d", d.E().knock_ms);
        // Light hits never touch a downed unit.
        d.w.set_action(d.J(), ATTACK, 'A');
        CHECK(d.w.try_melee_hit(d.E(), d.J()) == 0, "A on downed");
    }
    {   // B: 4500/3000 -> V not < V_FALLF -> balance 0 at once, whatever the gauge.
        Duo d(gd);
        d.E().balance = 100;
        d.w.set_action(d.J(), ATTACK, 'B');
        CHECK(d.run_until_hit() == Ev::Hit, "B hit");
        CHECK(is_knocked(d.E().action), "B knockdown action %d", d.E().action);
    }
    {   // 5. Rank >= 4 knocked down: 6000 ms invulnerability to melee (not to down attacks).
        Duo d(gd);
        d.w.knockdown(d.J());
        CHECK(is_knocked(d.J().action) && d.J().invuln_ms == 6000, "invuln %d", d.J().invuln_ms);
        d.w.set_action(d.E(), ATTACK, 'A');
        d.E().pos = {40, 0, 0};
        CHECK(d.w.try_melee_hit(d.J(), d.E()) == 0, "hit through invulnerability");
        CHECK(d.E().invuln_ms == 0, "soldier invuln %d", d.E().invuln_ms);
    }
    return finish();
}
