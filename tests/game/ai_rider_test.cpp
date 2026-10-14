// AI riders (S6L1 Charles VII on "Charlies Horse"): a seated AI rider follows its path on the horse
// (0x52daa0: run gear FUN_00466d50, rider_stop at the end), the ridden horse does not think (0x49d810
// rider check), MOVE_TO_TARGET ends with Unit_rider_stop (0x4c1670), COMBO_ATTACK swings H_ATTACK 'A'
// p3 times (0x4bbf20 mounted branch: every swing decrements p3, the last one ends the order) and hits,
// a dead horse drops the rider (H_DROP); deterministic.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/ai_system.hpp"
#include "game/mount_ride.hpp"

using namespace joa::game;

static int fails = 0;
#define CHECK(c, m) \
    do { if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, m), ++fails; } while (0)

static std::uint64_t run(GameData& gd, bool verbose) {
    FlatTerrain flat;
    World w(gd, &flat, 3);
    AiSystem ai(w, nullptr);
    InputFrame none{};
    const int king = w.spawn_unit("CHARLIE7", 2, {0, 0, 0}, 0);
    const int horse = w.spawn_unit("CAMP_HORSE", 2, {40, 20, 0}, 0);
    CHECK(king && horse, "spawn CHARLIE7 + CAMP_HORSE");
    if (!king || !horse) return 0;
    w.tick(none);
    CHECK(ride_horse(w, *w.unit(king), horse) == 1, "set_unit_ride_horse");
    for (int i = 0; i < 120 && !w.unit(king)->ride_ready; ++i) w.tick(none);
    CHECK(w.unit(king)->ride_ready && w.unit(king)->action == H_IDLE, "seated");

    // Ride 2000 units east.
    ai.give_order(*w.unit(king), order::MOVE_TO_TARGET, 5, false, 2000, 0, 0, 0, 60000);
    const Vec3 p0 = w.unit(horse)->pos;
    int t = 0, ran = 0;
    for (; t < 600 && (t == 0 || ai.curr_order(*w.unit(king)) == order::MOVE_TO_TARGET); ++t) {
        w.tick(none);
        ran += w.unit(horse)->action == WALK;
        CHECK(ai.st(*w.unit(horse)).orders.empty(), "ridden horse gets no orders (does not think)");
    }
    const Unit& K = *w.unit(king);
    const Unit& H = *w.unit(horse);
    if (verbose)
        std::printf("ride: %d ticks, horse WALK %d ticks, horse at (%.0f,%.0f) action %s, rider %s\n", t, ran, H.pos.x, H.pos.y,
                    action_name(H.action), action_name(K.action));
    CHECK(ai.curr_order(K) == 0, "MOVE_TO_TARGET finished");
    CHECK(std::hypot(H.pos.x - 2000, H.pos.y) <= 2.0f * gd.c.reach_dt + 5, "horse arrived (2 x REACH_DT for riders)");
    CHECK(ran > 30 && std::hypot(H.pos.x - p0.x, H.pos.y - p0.y) > 1500, "horse carried the rider");
    CHECK(std::hypot(K.pos.x - H.pos.x, K.pos.y - H.pos.y) < 1, "rider on the horse");
    CHECK(H.action == STOP || H.action == STOP_B || H.action == IDLE, "rider_stop at the end");
    CHECK(K.mount == horse, "still mounted (the AI never dismounts by itself)");
    for (int i = 0; i < 60; ++i) w.tick(none);

    // Mounted combo: H_ATTACK 'A', re-swung at the window while p3 > 0 (p3 = 2 -> 3 swings).
    const int foe = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {w.unit(king)->pos.x + 60, w.unit(king)->pos.y, 0}, 180);
    w.set_hostile(1, 2, true), w.set_hostile(2, 1, true);
    ai.give_task(*w.unit(foe), task::IDLE, 100, 1, 0, 0, 0);  // the foe stands still
    w.set_yaw_now(*w.unit(king), 0);
    w.take_events();
    ai.give_order(*w.unit(king), order::COMBO_ATTACK, 5, false, AiSystem::combo_para("AAAB", 1), 0, 2, 0, 20000);
    int swings = 0, hits = 0;
    for (t = 0; t < 300 && (t == 0 || ai.curr_order(*w.unit(king)) == order::COMBO_ATTACK); ++t) {
        w.tick(none);
        for (const GameEvent& e : w.take_events()) {
            swings += e.type == Ev::Action && e.unit == king && e.value == H_ATTACK;
            hits += (e.type == Ev::Hit || e.type == Ev::Blocked) && e.other == king;
            if (verbose && e.type != Ev::Sound && e.type != Ev::AnimEvent) std::printf("  ev %s %d %d %d\n", ev_name(e.type), e.unit, e.other, e.value);
        }
    }
    if (verbose) std::printf("mounted combo: %d H_ATTACK, %d hits, %d ticks, rider %s at (%.0f,%.0f,%.0f) yaw %.0f; foe at (%.0f,%.0f,%.0f) hp %d %s\n", swings, hits, t,
                             action_name(w.unit(king)->action), w.unit(king)->pos.x, w.unit(king)->pos.y, w.unit(king)->pos.z, w.unit(king)->yaw_deg,
                             w.unit(foe)->pos.x, w.unit(foe)->pos.y, w.unit(foe)->pos.z, w.unit(foe)->hp, action_name(w.unit(foe)->action));
    CHECK(swings == 2, "p3 = 2 mounted swings (the first one decrements p3 too)");
    CHECK(hits >= 1, "mounted swing hits");
    CHECK(ai.curr_order(*w.unit(king)) == 0 && w.unit(king)->action == H_IDLE, "order ends with Unit_stop (H_IDLE)");

    // Dead horse: the rider drops (0x5174e0).
    w.unit(horse)->lives = 0;
    w.set_hp(*w.unit(horse), 0);
    w.unit_die(*w.unit(horse), foe);
    for (int i = 0; i < 3; ++i) w.tick(none);
    if (verbose) std::printf("dead horse: rider %s mount %d horse %d rider %d\n", action_name(w.unit(king)->action), w.unit(king)->mount,
                             w.unit(horse) ? 1 : 0, w.unit(horse) ? w.unit(horse)->m.rider : -1);
    CHECK(w.unit(king)->mount == 0 && w.unit(king)->action == H_DROP, "dead horse: rider H_DROP, unlinked");
    CHECK(!w.unit(horse) || w.unit(horse)->m.rider == 0, "horse has no rider");

    // Unit_deinit 0x461f10: deleting a ridden horse dismounts its rider.
    const int king2 = w.spawn_unit("CHARLIE7", 2, {-500, 0, 0}, 0);
    const int horse2 = w.spawn_unit("CAMP_HORSE", 2, {-460, 20, 0}, 0);
    w.tick(none);
    ride_horse(w, *w.unit(king2), horse2);
    for (int i = 0; i < 120 && !w.unit(king2)->ride_ready; ++i) w.tick(none);
    if (verbose) std::printf("king2 mount %d ready %d action %s\n", w.unit(king2)->mount, int(w.unit(king2)->ride_ready), action_name(w.unit(king2)->action));
    w.delete_unit(horse2);
    if (verbose) std::printf("king2 after delete: mount %d action %s\n", w.unit(king2)->mount, action_name(w.unit(king2)->action));
    CHECK(w.unit(king2)->mount == 0 && !w.unit(horse2), "deleted horse: rider unlinked (0x4fc220(0); CHARLIE7 has no H_DMNT row)");
    return w.hash() ^ ai.hash();
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return 77;
    const auto a = run(*gd, true), b = run(*gd, false);
    CHECK(a == b && a != 0, "deterministic");
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
