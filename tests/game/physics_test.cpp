// Character physics on S1L1: gravity fall time, ground snap, unit-unit block + push-aside.
#include <cmath>

#include "ai_fixture.hpp"
#include "game/world_sim.hpp"

using namespace joa::game;

int main() {
    auto env = fixture::load();
    if (!env) return 77;
    float cx = 0, cy = 0;
    CHECK(fixture::open_area(*env->nav, 800, 400, cx, cy), "open area on S1L1");
    World w(*env->gd, env->terrain.get(), 7);
    CharPhysics phys(env->nav.get());
    w.set_physics(&phys);

    // Fall 10 m: t = sqrt(2h / g), g = 9.81 m/s^2 * GravityLevel(3) * 46.1 u/m.
    const int a = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx, cy, 0}, 0);
    CHECK(a, "spawn");
    const float ground = env->terrain->height(cx, cy);
    CHECK(std::fabs(w.unit(a)->pos.z - ground) < 1, "spawned on the ground");
    w.unit(a)->pos.z = ground + 461.0f;
    w.unit(a)->airborne = true;
    int ticks = 0;
    while (w.unit(a)->airborne && ticks < 200) { w.tick_physics(); ++ticks; }
    const float g = 9.81f * float(w.unit(a)->gravity_scale) * World::kUnitsPerMetre;
    const float expect = std::sqrt(2 * 461.0f / g) * 1000.0f / World::kTickMs;
    std::printf("fall 461 u: %d ticks (expected %.1f)\n", ticks, expect);
    CHECK(std::fabs(float(ticks) - expect) <= 2.0f, "fall time matches g");
    CHECK(std::fabs(w.unit(a)->pos.z - ground) < 0.01f && w.unit(a)->vel.z == 0, "landed on the ground");

    // Unit-unit block: A runs into a standing B; A never overlaps B and B is pushed aside.
    const int b = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx + 200, cy + 2, 0}, 0);
    Unit* A = w.unit(a);
    A->pos = {cx, cy, env->terrain->height(cx, cy)};
    A->action = RUN;
    const float b_y0 = w.unit(b)->pos.y;
    float min_d = 1e9f;
    for (int i = 0; i < 40; ++i) {
        A = w.unit(a);
        A->vel = {300, 0, 0};
        w.tick_physics();
        const Unit& B = *w.unit(b);
        min_d = std::min(min_d, std::hypot(B.pos.x - w.unit(a)->pos.x, B.pos.y - w.unit(a)->pos.y));
    }
    const float rr = 2 * env->gd->units[std::size_t(w.unit(a)->cls)].cyl_radius;
    std::printf("min distance %.1f (radii %.1f), B moved %.1f sideways\n", min_d, rr, w.unit(b)->pos.y - b_y0);
    CHECK(min_d >= rr - 1.0f, "no overlap");
    CHECK(std::fabs(w.unit(b)->pos.y - b_y0) > 5.0f, "blocker pushed aside");
    return 0;
}
