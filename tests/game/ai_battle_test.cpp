// 20 vs 20 battle on S1L1 for 3000 ticks: deterministic hash across two runs + ms/tick; then a
// 100 vs 100 profile (goal §16). Native AI (AiSystem) + character physics (CharPhysics) + World.
#include <chrono>

#include "ai_fixture.hpp"
#include "game/ai_system.hpp"
#include "game/world_sim.hpp"

using namespace joa::game;

namespace {
struct Result {
    std::uint64_t hash = 0;
    int dead[3] = {};
    double ms_avg = 0, ms_max = 0, ms_fight = 0;  // whole run / worst tick / first 600 ticks (melee phase)
};

Result battle(fixture::Env& env, float cx, float cy, int per_side, int ticks, std::uint32_t seed) {
    World w(*env.gd, env.terrain.get(), seed);
    CharPhysics phys(env.nav.get());
    w.set_physics(&phys);
    AiSystem ai(w, env.nav.get());
    // Neutral observer as "player" so LOD tiers are those of a player watching the fight.
    w.set_hostile(3, 1, false), w.set_hostile(3, 2, false);
    w.set_player(w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 3, {cx, cy + 600, 0}, 270));
    const char* code[3][2] = {{}, {"ENGLISH_LIGHT_INFANTRY_CAPTAIN", "ENGLISH_LIGHT_INFANTRY"},
                              {"FRENCH_LIGHT_INFANTRY_CAPTAIN", "FRENCH_LIGHT_INFANTRY"}};
    const int squads = per_side / 10;
    for (int side = 1; side <= 2; ++side) {
        const float x0 = side == 1 ? cx - 450 : cx + 450, yaw = side == 1 ? 0.0f : 180.0f, dir = side == 1 ? -1.0f : 1.0f;
        for (int q = 0; q < squads; ++q) {
            const float y0 = cy - 200.0f * float(squads - 1) / 2.0f + 200.0f * float(q);
            const int team = side * 100 + q;
            const int L = w.spawn_unit(code[side][0], side, {x0, y0, 0}, yaw);
            ai.set_unit_team(*w.unit(L), team);
            for (int i = 0; i < 9; ++i) {
                const int m = w.spawn_unit(code[side][1], side, {x0 + dir * (60.0f + 50.0f * float(i / 3)), y0 - 50.0f + 50.0f * float(i % 3), 0}, yaw);
                ai.set_unit_team(*w.unit(m), team);
            }
        }
    }
    Result r;
    InputFrame in{};
    using clk = std::chrono::steady_clock;
    double total = 0;
    for (int t = 0; t < ticks; ++t) {
        const auto t0 = clk::now();
        w.tick(in);
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        total += ms;
        if (t == 599 || (t == ticks - 1 && ticks < 600)) r.ms_fight = total / (t + 1);
        r.ms_max = std::max(r.ms_max, ms);
    }
    r.ms_avg = total / ticks;
    for (const Unit& u : w.units())
        if (u.hp < 1 && u.nation >= 1 && u.nation <= 2) ++r.dead[u.nation];
    r.hash = w.hash() ^ (ai.hash() * 0x9e3779b97f4a7c15ull);
    return r;
}
}  // namespace

int main() {
    auto env = fixture::load();
    if (!env) return 77;
    float cx = 0, cy = 0;
    CHECK(fixture::open_area(*env->nav, 1200, 600, cx, cy), "open area");

    const Result a = battle(*env, cx, cy, 20, 3000, 1234);
    const Result b = battle(*env, cx, cy, 20, 3000, 1234);
    std::printf("20v20 x 3000 ticks: hash %016llx, dead english %d french %d, %.3f ms/tick avg (%.3f first 600), %.2f max\n",
                (unsigned long long)a.hash, a.dead[1], a.dead[2], a.ms_avg, a.ms_fight, a.ms_max);
    CHECK(a.hash == b.hash, "deterministic: same seed -> same world + AI state");
    CHECK(a.dead[1] + a.dead[2] > 0, "the armies fought");
    const Result c = battle(*env, cx, cy, 20, 3000, 99);
    CHECK(c.hash != a.hash, "different seed -> different battle");

    const Result big = battle(*env, cx, cy, 100, 1000, 7);
    std::printf("100v100 x 1000 ticks: dead english %d french %d, %.3f ms/tick avg (%.3f first 600), %.2f max\n", big.dead[1],
                big.dead[2], big.ms_avg, big.ms_fight, big.ms_max);
    CHECK(big.ms_avg < World::kTickMs, "200 units simulate faster than real time");
    return 0;
}
