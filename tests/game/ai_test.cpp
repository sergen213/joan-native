// Native AI building blocks: target selection (0x54b480) cases, ranges, aggression x difficulty,
// orders queue rules, give_order team propagation, class mapping.
#include "ai_fixture.hpp"
#include "game/ai_system.hpp"
#include "game/ai_routines.hpp"
#include "game/world_sim.hpp"

using namespace joa::game;

int main() {
    auto env = fixture::load();
    if (!env) return 77;
    float cx = 0, cy = 0;
    CHECK(fixture::open_area(*env->nav, 1200, 600, cx, cy), "open area");
    World w(*env->gd, env->terrain.get(), 11);
    AiSystem ai(w, env->nav.get());
    auto spawn = [&](const char* code, int nation, float dx, float dy, float yaw = 0) {
        return w.spawn_unit(code, nation, {cx + dx, cy + dy, 0}, yaw);
    };

    const int me = spawn("ENGLISH_LIGHT_INFANTRY", 1, 0, 0, 0);
    Unit* M = w.unit(me);
    ai.st(*M);
    CHECK(M->ai_class == 1, "human_light_infantry.scr -> class 1");
    CHECK(ai.unit_range(*M, 1) == 800 && ai.unit_range(*M, 2) == 1600 && ai.unit_range(*M, 3) == 2500, "ranges class 1");
    // Aggression 0x511e70 / 0x511f80: base 50 (light inf) x {0.5, 1, 1.5, 2}.
    const int want_chance[4] = {25, 50, 75, 100};
    for (int d = 0; d < 4; ++d) {
        w.difficulty = d;
        CHECK(ai.attack_chance(*M) == want_chance[d], "attack chance difficulty " + std::to_string(d));
    }
    w.difficulty = 1;

    // 'E': nearest enemy inside the radius; friends ignored; outside radius ignored.
    const int friend_ = spawn("ENGLISH_LIGHT_INFANTRY", 1, 100, 0);
    const int e1 = spawn("FRENCH_LIGHT_INFANTRY", 2, 300, 0);
    M = w.unit(me);
    M->attack_behaviour = ai::NEAREST;
    CHECK(ai.search_for_unit(*M, "E", 1000) == e1, "single enemy found");
    CHECK(ai.search_for_unit(*w.unit(me), "E", 200) == 0, "enemy outside radius");
    CHECK(ai.search_for_unit(*w.unit(me), "F", 200) == friend_, "'F' finds the friend");
    CHECK(ai.search_for_unit(*w.unit(me), "L", 1000) == -1, "'L' without a leader -> -1 (original quirk)");

    // View cone: facing away (yaw 180) the enemy at +x is 180 deg off.
    w.unit(me)->target = 0;
    w.set_yaw(*w.unit(me), 180);
    CHECK(ai.search_for_unit(*w.unit(me), "E", 1000, false, 0, 90, 0) == 0, "outside 90 deg cone");
    w.set_yaw(*w.unit(me), 0);
    CHECK(ai.search_for_unit(*w.unit(me), "E", 1000, false, 0, 90, 0) == e1, "inside cone");

    // NEAREST_HIGH_PRIORITY: for a light-infantry searcher an archer (prio 5) beats a nearer light
    // infantryman (prio 4) unconditionally (0x527790 table).
    const int arch = spawn("ENGLISH_ARCHER", 2, 600, 100);
    w.unit(me)->target = 0;
    w.unit(me)->attack_behaviour = ai::NEAREST_HIGH_PRIORITY;
    CHECK(ai.search_for_unit(*w.unit(me), "E", 1000) == arch, "high-priority archer chosen");

    // Sticky target: a valid current target inside the radius is kept 96 % of the time.
    int kept = 0;
    for (int i = 0; i < 200; ++i) {
        w.unit(me)->target = arch;
        w.unit(me)->attack_behaviour = ai::NEAREST;
        kept += ai.search_for_unit(*w.unit(me), "E", 1000) == arch;
    }
    std::printf("sticky target kept %d / 200\n", kept);
    CHECK(kept >= 175 && kept < 200, "sticky ~96 %");

    // Determinism: same seed, same calls -> same picks.
    auto picks = [&](std::uint32_t seed) {
        World w2(*env->gd, env->terrain.get(), seed);
        AiSystem a2(w2, env->nav.get());
        const int s = w2.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx, cy, 0}, 0);
        for (int i = 0; i < 6; ++i) w2.spawn_unit("FRENCH_LIGHT_INFANTRY", 2, {cx + 100 + 60.0f * i, cy + 40.0f * (i % 3), 0}, 0);
        std::vector<int> r;
        for (int i = 0; i < 50; ++i) r.push_back(a2.search_for_unit(*w2.unit(s), "E", 1000));
        return r;
    };
    CHECK(picks(5) == picks(5), "deterministic selection");

    // LOS: S1L1 anti-portal 0 (x 4244..6609, y ~8925, z -508..288) hides a target behind it.
    {
        World w3(*env->gd, env->terrain.get(), 1);
        AiSystem a3(w3, env->nav.get());
        const int s = w3.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {5400, 8700, 0}, 90);
        const int t = w3.spawn_unit("FRENCH_LIGHT_INFANTRY", 2, {5400, 9150, 0}, 0);
        w3.unit(s)->pos.z = w3.unit(t)->pos.z = 0;  // LOS is independent of ground here (quad spans z -508..288)
        const bool through = env->nav->segment_blocked({5400, 8700, w3.unit(s)->pos.z}, {5400, 9150, w3.unit(t)->pos.z},
                                                      float(int(env->gd->units[std::size_t(w3.unit(s)->cls)].height * 0.8f)));
        CHECK(through, "test geometry crosses the anti-portal");
        CHECK(a3.search_for_unit(*w3.unit(s), "E", 1600) == 0, "target behind anti-portal not seen");
    }

    // Orders: queue sorted by prio, duplicates rejected, BLOCK refresh; give_order team propagation.
    {
        OrderQueue q;
        q.add({order::TURN, 1, {}, 0, 0, 100});
        q.add({order::SHIFT, 3, {}, 0, 0, 100});
        CHECK(q.add({order::TURN, 5, {}, 0, 0, 100}) == 0, "duplicate queued order rejected");
        q.add({order::IDLE, 1, {}, 0, 0, 100});
        CHECK(q.q.size() == 3 && q.q[0].action == order::SHIFT && q.q[1].action == order::TURN && q.q[2].action == order::IDLE,
              "descending prio, FIFO among equals");
        q.pop_front();
        CHECK(q.cur.action == order::SHIFT, "pop_front");
        CHECK(order_id_from_name("approach_enemy") == order::APPROACH_ENEMY && task_id_from_name("SPECIAL3") == 103, "names");
    }
    {
        World w4(*env->gd, env->terrain.get(), 2);
        AiSystem a4(w4, env->nav.get());
        const int L = w4.spawn_unit("ENGLISH_LIGHT_INFANTRY_CAPTAIN", 1, {cx, cy, 0}, 0);
        a4.set_unit_team(*w4.unit(L), 7);
        std::vector<int> mem;
        for (int i = 0; i < 3; ++i) {
            mem.push_back(w4.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx - 100, cy + 100.0f * i - 100, 0}, 0));
            a4.set_unit_team(*w4.unit(mem.back()), 7);
        }
        a4.give_order(*w4.unit(mem[0]), order::MOVE_TO_TARGET, 5, true, int(cx + 300), int(cy), 0, 0, 5000);
        CHECK(a4.has_order(*w4.unit(L), order::MOVE_TO_TARGET), "team order copied to the leader");
        CHECK(a4.has_order(*w4.unit(mem[2]), order::MOVE_TO_TARGET), "and to the other members");
        CHECK(a4.st(*w4.unit(mem[2])).orders.q.front().prio == 4 && a4.st(*w4.unit(mem[2])).orders.q.front().issuer == mem[0],
              "copies get prio-1 and issuer");
    }
    return 0;
}
