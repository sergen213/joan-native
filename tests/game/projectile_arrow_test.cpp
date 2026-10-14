// Ranged pipeline (Part D): AI archer fire state machine -> PROJ arrow (ITEMTYPE "Projectile Arrow",
// GL 0.5, ballistic solver, no spread under 300 units) -> unit hit event type 2 -> 0x509e90 damage.
// English archer: ENGLISH_BOW dmg 2, ARROW dmg 4 (its "Inc Damage" key typo is ignored as in the
// original), DEX 4, DEX_DAM atoi 1, morale 70; Joan armour 1, morale 70, player side (F_DEFEND).
#include "sim_fixture.hpp"

using namespace joa::game;

int main() {
    auto gd = load_data_or_skip();
    CHECK(gd->throwi[1].av == 1800 && gd->throwi[1].gl == 0.5f && gd->throwi[0].fc == 200, "THROWI");
    FlatTerrain flat;
    World w(*gd, &flat, 0);
    const int j = w.spawn_unit("JOAN_OF_ARC", gd->nation_id("FRENCH"), {250, 0, 0}, 180);
    const int a = w.spawn_unit("ENGLISH_ARCHER", gd->nation_id("ENGLISH"), {0, 0, 0}, 0);
    w.set_player(j);
    Unit& archer = *w.unit(a);
    // base = (DEX 4*1 + 100) * (2 + 4) / 200 = 3.12 (ammo present -> divisor 200)
    CHECK(NEAR(w.ranged_base_damage(archer), 3.12, 1e-5), "ranged base %f", w.ranged_base_damage(archer));
    w.tick({});   // CanAct needs CLB_IDLE (20 ms) since the spawn state began
    CHECK(w.ai_shoot(archer, j), "archer cannot shoot");
    bool spawned = false, hit = false;
    int hit_value = -1;
    for (int t = 0; t < 120 && !hit; ++t) {
        w.tick({});
        for (const auto& e : w.take_events()) {
            if (e.type == Ev::ProjectileSpawn) {
                spawned = true;
                CHECK(e.value == 1, "projectile type %d", e.value);
            }
            if (e.type == Ev::Hit && e.unit == j) hit = true, hit_value = e.value;
        }
    }
    CHECK(spawned, "no projectile");
    CHECK(hit, "arrow missed");
    // E_ARCUP grants RANGE_ATTACK at level 0 (skill level 1 -> x(1+10)/11 = 1.0):
    // atk = 1.2 * 3.12 = 3.744; def 1.2; dmg = 1.872 + 1.272 = 3.144; no falloff (< 800);
    // x F_DEFEND 0.7 = 2.2008 -> Joan 60 -> trunc(57.7992) = 57.
    CHECK(w.unit(j)->hp == 57, "Joan hp %d (event %d)", w.unit(j)->hp, hit_value);
    CHECK(w.unit(j)->balance == 0 || w.unit(j)->balance < 20, "ARR_FALL not applied: %d", w.unit(j)->balance);
    CHECK(archer.inv.ammo.quantity == gd->items[size_t(archer.inv.ammo.item)].quantity, "AI consumed ammo");
    return finish();
}
