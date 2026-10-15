// Ranged pipeline (Part D): AI archer fire state machine -> PROJ arrow (ITEMTYPE "Projectile Arrow",
// GL 0.5, ballistic solver, no spread under 300 units) -> unit hit event type 2 -> 0x509e90 damage.
// English archer: ENGLISH_BOW dmg 2, ARROW dmg 4 (its "Inc Damage" key typo is ignored as in the
// original), DEX 4, DEX_DAM atoi 1, morale 70; Joan armour 1, morale 70, player side (F_DEFEND).
#include "sim_fixture.hpp"

using namespace joa::game;

// Structure look-ahead sweep 0x693750 with a stand-in structure: the plane x = wall answered through
// World::structure_ray as map item 5 (modify recno 3). Returns Joan's HP; fills what the item received.
struct WallRun { int joan_hp = 0, casts = 0; bool item_hit = false, unit_hit = false; float last_x = -1; std::vector<Trigger> got; };
WallRun wall_run(GameData& gd, float wall) {
    FlatTerrain flat;
    World w(gd, &flat, 0);
    const int j = w.spawn_unit("JOAN_OF_ARC", gd.nation_id("FRENCH"), {250, 0, 0}, 180);
    const int a = w.spawn_unit("ENGLISH_ARCHER", gd.nation_id("ENGLISH"), {0, 0, 0}, 0);
    w.set_player(j);
    WallRun r;
    w.structure_ray = [&](int item, Vec3 from, Vec3 to) {
        StructureHit h;
        if (item) {
            if (item == 5) h = {5, 1, 3, {wall, 0, 0}, 2};
            return h;
        }
        ++r.casts;
        if (from.x != to.x && (from.x - wall) * (to.x - wall) <= 0) h = {5, (wall - from.x) / (to.x - from.x), 3, {wall, 0, 0}, 2};
        return h;
    };
    w.object_trigger = [&](const Trigger& t) { r.got.push_back(t); };
    w.tick({});
    CHECK(w.ai_shoot(*w.unit(a), j), "archer cannot shoot");
    for (int t = 0; t < 120; ++t) {
        w.tick({});
        for (const auto& p : w.snapshot().projectiles) r.last_x = p.pos.x;
        for (const auto& e : w.take_events())
            if (e.type == Ev::ProjectileHit) (e.unit ? r.unit_hit : r.item_hit) = true;
    }
    CHECK(w.snapshot().projectiles.empty(), "projectile still flying");
    r.joan_hp = w.unit(j)->hp;
    for (const Trigger& t : r.got) CHECK(t.sender == a, "sender %d", t.sender);
    return r;
}

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

    // Wall between: the arrow stops at it (one look-ahead cast covers the flight; the hit frame starts
    // before the plane), Joan is untouched, the item gets ARROW at its position (0x4fbc90 kinds 0/1).
    const WallRun s = wall_run(*gd, 150);
    std::printf("wall 150: casts %d, last x %.1f, %zu item events\n", s.casts, s.last_x, s.got.size());
    CHECK(s.item_hit && !s.unit_hit && s.joan_hp == 60, "wall hit %d unit hit %d hp %d", int(s.item_hit), int(s.unit_hit), s.joan_hp);
    CHECK(s.casts == 1, "look-ahead casts %d", s.casts);
    CHECK(s.last_x > 40 && s.last_x <= 150, "stopped at x %.1f", s.last_x);
    CHECK(s.got.size() == 1 && s.got[0].type == 2 && s.got[0].target_kind == 2 && s.got[0].target_id == 3 &&
              s.got[0].pos.x == 150 && s.got[0].radius == gd->throwi[1].tr && s.got[0].param == 1,
          "item event");
    // Wall behind Joan: the closer body hit wins, the item gets nothing.
    const WallRun b = wall_run(*gd, 400);
    CHECK(b.unit_hit && !b.item_hit && b.joan_hp == 57 && b.got.empty(), "behind: unit %d item %d hp %d", int(b.unit_hit),
          int(b.item_hit), b.joan_hp);

    // Draw-end re-aim (0x7c7ab0 case 0x30, Aim_solve_by_type 0x4945c0 to the target's feet): mission-1 geometry,
    // Joan 40 units up the slope at 260. The feet-to-feet direction from ai_shoot has z 0.15 (-> SHTIDLEH); the
    // re-solved aim from the launch point is flat -> SHTIDLE, as the original archer (DIFFERENTIAL_VERIFICATION.md).
    {
        World v(*gd, &flat, 0);
        const int jj = v.spawn_unit("JOAN_OF_ARC", gd->nation_id("FRENCH"), {260, 0, 40}, 180);
        const int aa = v.spawn_unit("ENGLISH_ARCHER", gd->nation_id("ENGLISH"), {0, 0, 0}, 0);
        v.set_player(jj);
        v.tick({});
        v.unit(jj)->pos.z = 40;
        CHECK(v.ai_shoot(*v.unit(aa), jj) && v.unit(aa)->aim_dir.z > 0.1f, "feet-to-feet aim z %f", v.unit(aa)->aim_dir.z);
        int drawn = -1;
        for (int t = 0; t < 120 && drawn < 0; ++t) {
            v.unit(jj)->pos.z = 40;
            v.tick({});
            if (v.unit(aa)->action != SHTLOAD) drawn = v.unit(aa)->action;
        }
        CHECK(drawn == SHTIDLE, "after the draw: action %d aim z %f", drawn, v.unit(aa)->aim_dir.z);
    }
    // Arrow = sphere r 10 (Item_setup_collision 0x41e850) vs the unit cylinder r 15, z 0..80 (0x699a20): hits reach
    // lateral 25 and 90 above the feet (measured in the original: hit at lateral 24.5, miss at 25.0; hit at axis
    // height ~90, miss at ~92; DIFFERENTIAL_VERIFICATION.md round 5). The port used to test a point (lateral 15, top 80).
    auto pass = [&](float lateral, float z) {
        World v(*gd, &flat, 0);
        // Target: an English archer (Height 100; Joan's 70 would fail 0x514c40's head test above 70).
        const int aa = v.spawn_unit("JOAN_OF_ARC", gd->nation_id("FRENCH"), {0, 0, 0}, 0);
        const int jj = v.spawn_unit("ENGLISH_ARCHER", gd->nation_id("ENGLISH"), {300, 0, 0}, 180);
        v.set_player(aa);
        v.tick({});
        Projectile p;
        p.id = 1, p.type = 1, p.shooter = aa, p.item = gd->item_index("ARROW"), p.tl = 7000, p.dl = 3000, p.launch_ms = v.now();
        p.pos_m = {150 / 46.1f, lateral / 46.1f, z / 46.1f};
        p.vel_m = {1800 / 46.1f, 0, 0};   // gl 0: a level flight past Joan's axis
        v.projectiles().push_back(p);
        bool hit = false;
        for (int t = 0; t < 20; ++t) {
            v.tick({});
            for (const auto& e : v.take_events()) hit |= e.type == Ev::ProjectileHit && e.unit == jj;
        }
        return hit;
    };
    CHECK(pass(0, 60) && pass(20, 60) && pass(24, 60), "lateral hits within 15 + 10");
    CHECK(!pass(26, 60) && !pass(-26, 60), "lateral miss beyond 25");
    CHECK(pass(0, 85) && pass(0, 89), "hits up to 10 above the 80-high cylinder");
    CHECK(!pass(0, 92), "misses over the head");
    return finish();
}
