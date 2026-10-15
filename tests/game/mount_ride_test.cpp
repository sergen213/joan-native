// Horses + turn rate: body turns 20 deg / tick toward the desired heading (0x4f2190) and AI units do
// not translate while turning; R mounts a free horse (0x43c7d0 / 0x4fc220), W runs it (gear 5 m/s,
// 10 m/s after 1 s), W+A steers <= TURN_ANG per frame, no key stops it, R dismounts; deterministic.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/world_sim.hpp"

using namespace joa::game;

static int fails = 0;
#define CHECK(c, m) \
    do { if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, m), ++fails; } while (0)

static std::uint64_t run(GameData& gd, bool verbose) {
    FlatTerrain flat;
    World w(gd, &flat, 7);
    // Turn rate: an AI soldier asked to face 180 from 0.
    const int s = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {-2000, 0, 0}, 0);
    w.set_yaw(*w.unit(s), 180);
    InputFrame none{};
    w.tick(none);
    CHECK(std::fabs(w.unit(s)->yaw_deg - 20) < 0.01f || std::fabs(w.unit(s)->yaw_deg - 340) < 0.01f, "20 deg per tick");
    int t = 1;
    while (std::fabs(w.unit(s)->yaw_deg - 180) > 0.01f && t < 30) w.tick(none), ++t;
    if (verbose) std::printf("180 deg turn: %d ticks\n", t);
    CHECK(t == 9, "180 deg in 9 ticks");

    // Horse.
    int horse_cls = -1;
    for (size_t i = 0; i < gd.units.size(); ++i)
        if (gd.units[i].special == 2 && gd.units[i].code.find("HORSE") != std::string::npos && gd.moves(int(i))) { horse_cls = int(i); break; }
    CHECK(horse_cls >= 0, "horse class");
    if (horse_cls < 0) return 0;
    const int joan = w.spawn_unit("JOAN_OF_ARC", 2, {0, 0, 0}, 0);
    const int horse = w.spawn_unit(gd.units[size_t(horse_cls)].code, 2, {40, 20, 0}, 0);
    w.set_player(joan);
    Unit* J = w.unit(joan);
    J->horse_riding = 1;
    InputFrame r{};
    r.press(IN_INTERACT);
    w.tick(none);   // CLB_IDLE: canAct needs 20 ms in the current state
    w.tick(r);
    if (verbose) std::printf("horse %s: mount %d action %s\n", gd.units[size_t(horse_cls)].code.c_str(), J->mount, action_name(J->action));
    CHECK(J->mount == horse && (J->action == H_MNT_L || J->action == H_MNT_R), "R mounts the horse");
    for (int i = 0; i < 120 && J->action != H_IDLE; ++i) w.tick(none);
    CHECK(J->ride_ready && J->action == H_IDLE, "seated, H_IDLE");
    const Vec3 p0 = w.unit(horse)->pos;
    InputFrame fw{};
    fw.held.set(IN_FORWARD);
    for (int i = 0; i < 60; ++i) w.tick(fw);
    const Vec3 p1 = w.unit(horse)->pos;
    const float d = std::hypot(p1.x - p0.x, p1.y - p0.y);
    if (verbose) std::printf("horse ran %.0f u in 2 s, rider at (%.0f,%.0f) horse (%.0f,%.0f)\n", d, J->pos.x, J->pos.y, p1.x, p1.y);
    CHECK(d > 46.1f * 5 * 1.0f && d < 46.1f * 10 * 2.0f + 1, "run gear 5..10 m/s");
    CHECK(std::hypot(J->pos.x - p1.x, J->pos.y - p1.y) < 1, "rider on the horse");
    const float y0 = J->yaw_deg;
    InputFrame fl{};
    fl.held.set(IN_FORWARD), fl.held.set(IN_STRAFE_LEFT);
    w.tick(fl);
    CHECK(std::fabs(J->yaw_goal - (y0 + 6)) < 0.01f, "W+A steers TURN_ANG per frame");
    CHECK(std::fabs(w.unit(horse)->yaw_deg - J->yaw_deg) < 0.01f, "horse follows the rider heading");
    for (int i = 0; i < 5; ++i) w.tick(none);
    CHECK(w.unit(horse)->action == IDLE || w.unit(horse)->action == STOP || w.unit(horse)->action == STOP_B, "no key: horse stops");
    for (int i = 0; i < 60; ++i) w.tick(none);
    w.tick(r);
    CHECK(J->mount == 0 && (J->action == H_DMNT_L || J->action == H_DMNT_R), "R dismounts");
    for (int i = 0; i < 120 && J->action != IDLE; ++i) w.tick(none);
    CHECK(J->action == IDLE && w.unit(horse)->m.rider == 0, "on foot again");
    return w.hash();
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
