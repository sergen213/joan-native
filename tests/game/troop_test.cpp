// Troops: max size formula, formation slots, FOLLOW_FORMATION convergence, auto-absorb, hold, RTS move.
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "ai_fixture.hpp"
#include "game/ai_system.hpp"
#include "game/world_sim.hpp"

using namespace joa::game;

int main() {
    auto env = fixture::load();
    if (!env) return 77;
    float cx = 0, cy = 0;
    CHECK(fixture::open_area(*env->nav, 1200, 600, cx, cy), "open area");
    World w(*env->gd, env->terrain.get(), 3);
    CharPhysics phys(env->nav.get());
    w.set_physics(&phys);
    AiSystem ai(w, env->nav.get());
    // A player nearby keeps everyone at LOD tier 1 (no player = tier 4, which widens arrival radii x5-x10).
    const int obs = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx + 500, cy + 250, 0}, 0);
    w.set_player(obs);

    const int L = w.spawn_unit("ENGLISH_LIGHT_INFANTRY_CAPTAIN", 1, {cx, cy, 0}, 0);
    CHECK(L && w.unit(L)->rank > 2, "captain is a leader rank");
    ai.st(*w.unit(L));
    CHECK(ai.is_leader(*w.unit(L)), "rank > 2 owns a team");
    CHECK(ai.set_unit_team(*w.unit(L), 5), "set_unit_team leader");

    // Max troop size 0x448080: max(0, (lead-20)/5) + 9, >= current size, <= 20.
    {
        Unit& u = *w.unit(L);
        const int item = w.stat_lead(u) - u.lead;
        const std::pair<int, int> table[] = {{0, 9}, {20, 9}, {24, 9}, {25, 10}, {49, 14}, {50, 15}, {74, 19}, {75, 20}, {200, 20}};
        for (auto [lead, size] : table) {
            u.lead = lead - item;
            CHECK(ai.max_troop_size(u) == size, "max_troop_size(lead " + std::to_string(lead) + ")");
        }
        u.lead = 60 - item;
    }

    // Five light infantry join -> 3 columns, W = 200, slots 100 apart starting 100 behind the anchor.
    std::vector<int> mem;
    for (int i = 0; i < 5; ++i) {
        const int m = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx + 300 + 40.0f * i, cy + 200, 0}, 0);
        CHECK(ai.set_unit_team(*w.unit(m), 5), "member joins by team id");
        mem.push_back(m);
    }
    CHECK(w.unit(L)->members.size() == 5 && w.unit(mem[0])->leader == L, "membership");
    std::set<std::pair<int, int>> got, want = {{-100, 100}, {-100, 0}, {-100, -100}, {-200, 100}, {-200, 0}};
    for (int m : mem) got.insert({int(std::lround(ai.st(*w.unit(m)).slot.x)), int(std::lround(ai.st(*w.unit(m)).slot.y))});
    CHECK(got == want, "formation slots at yaw 0");
    CHECK(ai.follow_unit_distance(L) == 200 + 100 * 3, "follow distance 200 + 100*k (5 members -> k=3)");

    // Members walk into formation (FOLLOW_FORMATION issued by the infantry AI).
    InputFrame in{};
    for (int t = 0; t < 400; ++t) w.tick(in);
    float worst = 0;
    for (int m : mem) {
        const Vec3 g = ai.formation_pos(*w.unit(m));
        worst = std::max(worst, std::hypot(g.x - w.unit(m)->pos.x, g.y - w.unit(m)->pos.y));
    }
    std::printf("formation: worst member %.1f units from its slot\n", worst);
    CHECK(worst < 60, "members reached their slots");

    // Auto-absorb (0x4655a0): a free (team 9999) same-nation unit within 300 joins.
    const int f = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx + 150, cy - 150, 0}, 0);
    w.unit(f)->team = 9999;
    const int far = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx + 700, cy - 300, 0}, 0);
    w.unit(far)->team = 9999;
    w.tick(in);
    CHECK(w.unit(f)->leader == L, "absorbed within 300");
    CHECK(w.unit(far)->leader == 0, "not absorbed beyond 300");

    // Hold: leader task {0, p1=1}; members read the leader's para1.
    ai.toggle_hold(*w.unit(L), true);
    CHECK(ai.is_holding(*w.unit(L)) && ai.curr_task_para(*w.unit(mem[0]), 1) == 1, "hold propagates");
    ai.toggle_hold(*w.unit(L), true);
    CHECK(!ai.is_holding(*w.unit(L)), "resume");

    // RTS move: whole troop gets RTS_MOVE_TO and arrives near the destination.
    std::vector<int> sel = ai.rts_select_with_team(L);
    CHECK(sel.size() == 7, "selection = leader + team");
    const Vec3 dest{cx - 600, cy, 0};
    CHECK(ai.rts_move(sel, dest, false), "rts_move accepted");
    CHECK(ai.st(*w.unit(L)).task.id == task::RTS_MOVE_TO, "task 12");
    for (int t = 0; t < 900; ++t) w.tick(in);
    for (int id : sel) {
        const Unit& u = *w.unit(id);
        CHECK(std::hypot(u.pos.x - dest.x, u.pos.y - dest.y) < 450, "unit " + std::to_string(id) + " near destination");
    }
    // Recruit-by-interact (0x43c210 -> 0x48dfb0(100)): nearest eligible unit in front within 100 joins.
    {
        Unit& Lu = *w.unit(L);
        w.set_yaw(Lu, 0);
        const int r = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {Lu.pos.x + 60, Lu.pos.y, 0}, 0);
        const int behind = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {w.unit(L)->pos.x - 40, w.unit(L)->pos.y, 0}, 0);
        CHECK(ai.recruit_nearby(*w.unit(L)) == r && w.unit(r)->leader == L, "recruit in front");
        CHECK(w.unit(behind)->leader == 0, "unit behind not recruited");
    }
    // Persistence round trip.
    std::vector<std::uint8_t> blob;
    ai.serialize(blob);
    const auto h = ai.hash();
    CHECK(ai.deserialize(blob) && ai.hash() == h, "serialize round trip");
    return 0;
}
