// Siege crews, carts, battering ram vs gate, ladder climbing (siege_crew.cpp, ladder_climb.cpp).
// World part (S1L1 navmesh, native AI): a crewed cannon turns and kills an enemy (crew mirror the
// firing states), an uncrewed cannon neither turns nor fires, a cart's driver rides on it, the run is
// deterministic. Mission part (S1L1): a crewed ram breaks the Castle Gate and its nav poly opens; the
// player climbs a ladder map item.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "ai_fixture.hpp"
#include "game/ai_system.hpp"
#include "game/campaign_runner.hpp"
#include "game/script_bind.hpp"
#include "game/siege_crew.hpp"

using namespace joa::game;

namespace {

float wrap180(float a) {
    a = std::fmod(a, 360.0f);
    if (a > 180) a -= 360;
    if (a < -180) a += 360;
    return a;
}

struct CannonRun {
    bool fired = false, crew_loadc = false, crew_attached = false, killed = false;
    float yaw_err = 999;
    int shots = 0;
    std::uint64_t hash = 0;
};

// Cannon at the left, enemy 1100 units to the right; the cannon faces +y (90 deg off the target).
CannonRun cannon_run(fixture::Env& env, float cx, float cy, bool crew, int seconds) {
    CannonRun r;
    World w(*env.gd, env.terrain.get(), 21);
    AiSystem ai(w, env.nav.get());
    CharPhysics phys(env.nav.get());
    w.set_physics(&phys);
    w.set_hostile(1, 2, true), w.set_hostile(2, 1, true);
    const int cannon = w.spawn_unit("CANNON", 1, {cx - 500, cy, 0}, 90);
    std::vector<int> men;
    if (crew)
        for (int i = 0; i < 2; ++i) {
            const int c = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx - 560 + 60.0f * i, cy - 150, 0}, 0);
            ai.give_task(*w.unit(c), task::LARGE_WEAPON_PUSH, 10, cannon, i, 0, 0);   // p2 0: 'M' first, 1: 'F' first
            men.push_back(c);
        }
    const int foe = w.spawn_unit("FRENCH_LIGHT_INFANTRY", 2, {cx + 550, cy, 0}, 180);
    w.set_player(foe);   // a standing target; the player also keeps the crew at LOD 1 (REACH not doubled)
    for (int t = 0; t < seconds * 1000 / World::kTickMs && !r.killed; ++t) {
        w.tick({});
        for (const GameEvent& e : w.take_events()) {
            if (e.type == Ev::ProjectileSpawn && e.unit == cannon) r.fired = true, ++r.shots;
            if (e.type == Ev::Action && e.value == SHTLOADC && (e.unit == (men.empty() ? -1 : men[0]) || (men.size() > 1 && e.unit == men[1])))
                r.crew_loadc = true;
        }
        const Unit* C = w.unit(cannon);
        if (crew && C->m.crew.size() == 2 && C->m.crew[0] && C->m.crew[1]) r.crew_attached = true;
        r.killed = w.dead(*w.unit(foe));
        if (std::getenv("SIEGE_DEBUG") && t % 30 == 0)
            for (int c : men) {
                const Unit& u = *w.unit(c);
                std::printf("t=%d crew %d pos %.0f,%.0f act %s veh %d task %d ps %d | cannon %.0f,%.0f yaw %.0f act %s\n", w.now(), c,
                            u.pos.x, u.pos.y, action_name(u.action), u.m.vehicle, ai.curr_task(u), ai.st(u).push_slot, C->pos.x,
                            C->pos.y, C->yaw_deg, action_name(C->action));
            }
    }
    const Unit* C = w.unit(cannon);
    const Unit* F = w.unit(foe);
    r.yaw_err = std::fabs(wrap180(C->yaw_deg - std::atan2(F->pos.y - C->pos.y, F->pos.x - C->pos.x) / 0.017453292f));
    r.hash = w.hash();
    return r;
}

int world_part(fixture::Env& env) {
    float cx = 0, cy = 0;
    CHECK(fixture::open_area(*env.nav, 1200, 600, cx, cy), "open area");

    const CannonRun a = cannon_run(env, cx, cy, true, 90);
    std::printf("crewed cannon: attached %d loadc %d shots %d killed %d yaw err %.1f\n", a.crew_attached, a.crew_loadc,
                a.shots, a.killed, a.yaw_err);
    CHECK(a.crew_attached, "LARGE_WEAPON_PUSH crew manned both slots");
    CHECK(a.fired && a.crew_loadc, "cannon fired, F crew mirrored SHTLOADC");
    CHECK(a.killed, "target killed");
    CHECK(a.yaw_err < 15, "cannon turned to the target");
    const CannonRun b = cannon_run(env, cx, cy, true, 90);
    CHECK(a.hash == b.hash, "same seed -> same world hash");

    const CannonRun n = cannon_run(env, cx, cy, false, 20);
    std::printf("uncrewed cannon: shots %d yaw err %.1f\n", n.shots, n.yaw_err);
    CHECK(!n.fired && !n.killed, "no crew -> no shot");
    CHECK(n.yaw_err > 80, "no crew -> no turn");

    // Cart with a driver (links of FUN_0051a250 / 0051a8a0): the cart moves, the driver rides 80 units
    // behind its origin with the cart's yaw.
    World w(*env.gd, env.terrain.get(), 5);
    AiSystem ai(w, env.nav.get());
    CharPhysics phys(env.nav.get());
    w.set_physics(&phys);
    const int cart = w.spawn_unit("HORSE_CART", 1, {cx - 400, cy, 0}, 0);
    const int drv = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx - 480, cy, 0}, 0);
    w.unit(cart)->m.cart_driver = drv;
    w.unit(drv)->m.cart = cart;
    const Vec3 start = w.unit(cart)->pos;
    ai.give_order(*w.unit(cart), order::MOVE_TO_TARGET, 5, false, int(cx + 400), int(cy + 100), 0, 0, 30000);
    float worst = 0;
    for (int t = 0; t < 15000 / World::kTickMs; ++t) {
        w.tick({});
        const Unit& c = *w.unit(cart);
        const Unit& d = *w.unit(drv);
        const float ex = c.pos.x - std::cos(c.yaw_rad()) * 80 - d.pos.x, ey = c.pos.y - std::sin(c.yaw_rad()) * 80 - d.pos.y;
        worst = std::max({worst, std::sqrt(ex * ex + ey * ey), std::fabs(wrap180(c.yaw_deg - d.yaw_deg))});
    }
    const float moved = std::hypot(w.unit(cart)->pos.x - start.x, w.unit(cart)->pos.y - start.y);
    std::printf("cart moved %.0f, driver max offset/yaw error %.2f\n", moved, worst);
    CHECK(moved > 500, "cart moved");
    CHECK(worst < 0.5f, "driver follows the cart");
    return 0;
}

Value call(Mission& m, int id, std::initializer_list<Value> args) {
    std::vector<Value> v(args);
    BuiltinCall c;
    c.args = v;
    return builtin_table().call[std::size_t(id)](m, c);
}

int mission_part(const char* dir) {
    std::string err;
    auto gd = GameData::load(dir, &err);
    CHECK(gd, err);
    const joa::formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    CHECK(run.init(&err) && run.start(1, &err), err);
    Mission& m = *run.mission();
    m.autosave = false;
    World& w = m.world();
    for (int i = 0; i < 3000 && (w.cut_scene || w.stop_unit_process); ++i) m.tick();
    std::printf("mission t=%d cut_scene %d\n", w.now(), int(w.cut_scene));
    MapObjects& o = m.objects();

    // --- battering ram vs the Castle Gate -------------------------------------------------------
    const int gate = o.find_by_name("Castle Gate");
    CHECK(gate && o.item(gate)->state == 0, "closed Castle Gate");
    const MapItem g0 = *o.item(gate);
    const int poly = m.nav()->find_poly_xy(g0.pos.x, g0.pos.y, g0.pos.z);
    const int english = gd->nation_id("ENGLISH"), french = gd->nation_id("FRENCH");
    w.set_hostile(english, french, true), w.set_hostile(french, english, true);
    // The S1L1 script loses the mission when a castle guard dies (S1L1_finish.sce): the ram's swing hits
    // units too, so the gate's defenders and the crew are made invulnerable for this check.
    for (Unit& u : w.units())
        if (!u.deleted && std::hypot(u.pos.x - g0.pos.x, u.pos.y - g0.pos.y) < 2000) u.invuln_ms = 1 << 30;
    // Approach side: the gate side whose ground is nearest the gate's z.
    const float gx = std::cos(g0.dir), gy = std::sin(g0.dir);
    float side = 1;
    {
        const float za = w.terrain()->height(g0.pos.x + gx * 260, g0.pos.y + gy * 260);
        const float zb = w.terrain()->height(g0.pos.x - gx * 260, g0.pos.y - gy * 260);
        side = std::fabs(za - g0.pos.z) <= std::fabs(zb - g0.pos.z) ? 1.0f : -1.0f;
    }
    const Vec3 rp{g0.pos.x + side * gx * 260, g0.pos.y + side * gy * 260, g0.pos.z};
    const int ram = w.spawn_unit("Batterram", english, rp, std::atan2(-side * gy, -side * gx) / 0.017453292f);
    w.unit(ram)->m.cutscene_process = 1, w.unit(ram)->invuln_ms = 1 << 30;
    int manned = 0;
    for (int i = 0; i < 8; ++i) {
        const int c = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", english, {rp.x, rp.y - 300, rp.z}, 0);
        w.unit(c)->m.cutscene_process = 1, w.unit(c)->invuln_ms = 1 << 30;
        manned += call(m, 389, {Value::integer(c), Value::integer(ram), Value::string(i < 6 ? "F" : "M")}).as_int() == 1;
    }
    CHECK(manned == 8, "8 crew attached (move_and_attach_unit)");
    m.ai().give_task(*w.unit(ram), task::RTS_ATTACK_ITEM, 10, gate, 0, 0, 0);
    std::set<int> batter;
    int hits = 0, t0 = w.now();
    bool blocking = true;
    while (o.item(gate) && o.item(gate)->hp > 0 && w.now() - t0 < 400000) {
        const int hp = o.item(gate)->hp;
        m.tick();
        for (const GameEvent& e : w.take_events()) {
            if (e.type == Ev::Action && e.value == BATTER_C) batter.insert(e.unit);
            if (std::getenv("SIEGE_DEBUG") && e.type == Ev::Action && e.unit == ram)
                std::printf("t=%d ram %s yaw %.0f hp %d task %d finish %d\n", w.now(), e.name.c_str(), w.unit(ram)->yaw_deg,
                            o.item(gate)->hp, m.ai().curr_task(*w.unit(ram)), int(m.finish()));
        }
        if (o.item(gate) && o.item(gate)->hp < hp) ++hits;
        if (o.item(gate)) blocking = o.item(gate)->blocking;
    }
    for (int i = 0; i < 200; ++i) m.tick();   // destroyed effect / collision release
    const MapItem* g1 = o.item(gate);
    std::printf("ram: gate hp %d -> %d in %d hits (%d ms), %zu crew swung BATTER_C, poly %d state %d, ram %.0f from gate\n",
                g0.hp, g1 ? g1->hp : 0, hits, w.now() - t0, batter.size(), poly, poly >= 0 ? m.nav()->poly(poly).state : -9,
                std::hypot(w.unit(ram)->pos.x - g0.pos.x, w.unit(ram)->pos.y - g0.pos.y));
    CHECK(!g1 || g1->hp == 0, "gate destroyed");
    CHECK(batter.size() >= 6, "front crew swung with the ram");
    // Gate_set_open_collision(0): the passage opens (gate_nav clears the poly under the gate, if any).
    CHECK(!blocking && (poly < 0 || m.nav()->poly(poly).state == 0), "gate collision / nav poly open");

    // Ram death takes its front crew (Unit_die crew release).
    Unit& R = *w.unit(ram);
    const int front = R.m.crew[0], back = R.m.crew[6];
    R.lives = 0;
    w.set_hp(R, 0);
    w.unit_die(R, 0);
    CHECK(w.dead(*w.unit(front)) && !w.dead(*w.unit(back)) && w.unit(back)->m.vehicle == 0, "ram death: F crew dies, M crew freed");

    // --- ladder -------------------------------------------------------------------------------
    Unit& P = *w.unit(w.player_id());
    P.m.cutscene_process = 1;
    const int lres = o.res_by_code("Object\\Models_Items\\Accessories\\Ladder\\ClimbLadder\\ladder");
    CHECK(lres, "ladder ItemRes");
    const int lad = o.create(m, lres);
    MapItem& L = *o.item(lad);
    L.dir = P.yaw_rad() + 3.1415927f;   // ladder faces the player
    L.pos = {P.pos.x + std::cos(P.yaw_rad()) * 50, P.pos.y + std::sin(P.yaw_rad()) * 50, P.pos.z};
    InputFrame use;
    use.press(IN_INTERACT);
    m.tick(use);
    std::printf("ladder: climb %d action %s\n", P.m.climb, action_name(P.action));
    CHECK(w.unit(w.player_id())->m.climb == lad, "interact started the climb");
    InputFrame up;
    up.held.set(IN_FORWARD);
    float top = -1e9f;
    std::set<int> states;
    for (int i = 0; i < 600 && w.unit(w.player_id())->m.climb; ++i) {
        m.tick(up);
        const Unit& p = *w.unit(w.player_id());
        top = std::max(top, p.pos.z);
        states.insert(p.action);
    }
    const Unit& p = *w.unit(w.player_id());
    std::printf("ladder: max z %+.1f over the foot, %zu states, climbing %d, action %s\n", top - L.pos.z, states.size(), p.m.climb,
                action_name(p.action));
    CHECK(states.count(C_UP_AB) && states.count(C_UP_BA) && states.count(C_UP_CS), "rung cycle and top exit");
    CHECK(p.m.climb == 0 && top - L.pos.z > 150, "climbed off the top");
    return 0;
}

}  // namespace

int main() {
    auto env = fixture::load();
    if (!env) return 77;
    if (int r = world_part(*env)) return r;
    if (int r = mission_part(std::getenv("JOA_GAME_DIR"))) return r;
    std::puts("ok");
    return 0;
}
