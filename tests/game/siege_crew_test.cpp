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
#include <map>
#include <string>

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

    // Rts_cmd_assign 0x44f750 branch A: with every free point reserved by men still on their way, a newly assigned
    // crewman takes the point of one that did not think this tick (its task cleared); captains are not crew.
    {
        World w2(*env.gd, env.terrain.get(), 7);
        AiSystem ai2(w2, env.nav.get());
        CharPhysics phys2(env.nav.get());
        w2.set_physics(&phys2);
        const int gun = w2.spawn_unit("CANNON", 1, {cx, cy, 0}, 90);
        int stale[2];
        for (int i = 0; i < 2; ++i) {
            stale[i] = w2.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx - 1150, cy - 250 + 500.0f * i, 0}, 0);
            ai2.give_task(*w2.unit(stale[i]), task::LARGE_WEAPON_PUSH, 10, gun, i, 0, 0);
        }
        auto res = [&]() -> const std::vector<int>& { return w2.unit(gun)->m.reserved; };
        auto thought_now = [&] { return w2.unit(stale[0])->think_ts >= w2.now() || w2.unit(stale[1])->think_ts >= w2.now(); };
        auto ready = [&] { return res().size() == 2 && res()[0] && res()[1] && !thought_now() && w2.now() > 1000; };
        for (int t = 0; t < 600 && !ready(); ++t) w2.tick({});
        CHECK(ready() && !w2.unit(gun)->m.crew[0] && !w2.unit(gun)->m.crew[1], "both points reserved by men on their way");
        const int cap = w2.spawn_unit("ENGLISH_LIGHT_INFANTRY_CAPTAIN", 1, {cx - 100, cy, 0}, 0);
        const int fresh = w2.spawn_unit("ENGLISH_LIGHT_INFANTRY", 1, {cx - 100, cy + 50, 0}, 0);
        const int sel[] = {cap, fresh};
        CHECK(ai2.rts_assign(sel, gun, true), "assign accepted");
        const int dropped = ai2.curr_task(*w2.unit(stale[0])) == task::LARGE_WEAPON_PUSH ? stale[1] : stale[0];
        std::printf("assign over stale reservations: reserved %d %d, dropped #%d task %d\n", res()[0], res()[1], dropped,
                    ai2.curr_task(*w2.unit(dropped)));
        CHECK(ai2.curr_task(*w2.unit(cap)) != task::LARGE_WEAPON_PUSH, "a captain is not crew");
        CHECK(ai2.curr_task(*w2.unit(fresh)) == task::LARGE_WEAPON_PUSH && std::count(res().begin(), res().end(), fresh) == 1,
              "the new crewman reserved a point");
        CHECK(ai2.curr_task(*w2.unit(dropped)) != task::LARGE_WEAPON_PUSH && std::count(res().begin(), res().end(), dropped) == 0,
              "a stale reserver gave his point up");
    }
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
    const int poly = m.nav()->find_poly_xy(g0.pos.x, g0.pos.y, g0.pos.z, true);   // as gate_nav: the blocked poly under it
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
    // A save taken once the destroyed gate is deleted (its fade-out; S7L1's "StPierre gate 2") loads with the passage open:
    // the map load blocks every closed gate's poly before the saved objects replace them.
    if (poly >= 0) {
        if (o.item(gate)) o.del_item(m, gate);
        CampaignRunner run2(*gd, vfs);
        CHECK(run2.init(&err) && run2.load(run.save(), &err), err);
        CHECK(run2.mission()->nav()->poly(poly).state == 0, "destroyed gate's nav poly open after a load");
    }

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

// S4L1: the French Cannon 1 (crewed by the script) TURNs and SHOOTs the Abtower as S4L1_tactic does (SHOOT p0 2 = map
// item). OrderExec_shoot 0x4bd090 stores the item in +0x2dd; the launch 0x5133e0 solves the ballistic aim to it, so
// every cannonball hits the tower mesh and Wall_take_ranged 0x492b10 takes ranged_base_damage / 2. The port used to fire
// item SHOOTs along a stale aim vector (every ball missed; the tower never fell).
int cannon_tower_part(const char* dir) {
    std::string err;
    auto gd = GameData::load(dir, &err);
    CHECK(gd, err);
    const joa::formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    CHECK(run.init(&err) && run.start(4, &err), err);
    Mission& m = *run.mission();
    m.autosave = false;
    World& w = m.world();
    for (int i = 0; i < 6000 && (w.cut_scene || w.stop_unit_process); ++i) m.tick();
    const int tower = m.objects().find_by_name("Abtower");
    int cannon = 0;
    for (const Unit& u : w.units())
        if (!u.deleted && u.name == "Cannon 1") cannon = u.id;
    CHECK(tower && cannon, "Abtower / Cannon 1");
    const int hp0 = m.objects().item(tower)->hp;
    int shots = 0, hits = 0;
    for (int t = 0; t < 60000 / World::kTickMs; ++t) {   // the crew walk to the cannon first (~35 s)
        Unit& u = *w.unit(cannon);
        AiSystem& ai = m.ai();
        const int rot = ai.item_rotation(u, tower);
        if (std::fabs(wrap180(u.yaw_deg - float(rot))) > 5) {
            if (ai.curr_order(u) != order::TURN) ai.stop_all_order(u), ai.give_order(u, order::TURN, 2, false, rot, 0, 0, 0, 2500);
        } else if (ai.curr_order(u) != order::SHOOT) {
            ai.stop_all_order(u), ai.give_order(u, order::SHOOT, 2, false, 2, tower, 0, 0, 3000);
        }
        m.tick();
        for (const GameEvent& e : w.take_events()) {
            shots += e.type == Ev::ProjectileSpawn && e.unit == cannon;
            hits += e.type == Ev::ProjectileHit && e.value == 5 && e.name == "item";
        }
    }
    const int hp1 = m.objects().item(tower)->hp;
    const int per = int(w.ranged_base_damage(*w.unit(cannon)) * 0.5f);
    std::printf("cannon vs Abtower: %d shots, %d item hits, hp %d -> %d (%d per ball)\n", shots, hits, hp0, hp1, per);
    CHECK(shots >= 2 && hits >= shots - 1, "every cannonball hits the tower");   // the last may still fly
    // Direct (0x40, object) and area (0x40, objects) events both reach the tower (0x4fbc90).
    CHECK(per > 0 && hp1 <= hp0 - 2 * hits * per && hp1 >= hp0 - 2 * hits * (per + 1), "tower takes 2 x ranged_base_damage / 2 per ball");
    return 0;
}

// S4L1, as the original measured live (DIFFERENTIAL_VERIFICATION round 6): Joan next to the four French cannons (their
// crews attach once she is near), scenario bit 27 + Abtower HP max - 1 (script bit 31: the cannons SHOOT the Abtower,
// then the Tourelles gate). Cannon 2 and Cannon 3 put every ball on the gate (-40 each: 2 hits per volley); Cannon 1 and
// Cannon 4 lose theirs on the rise in front of them. The ball stops on the gate's CollisionType box (Physics_sweep_actors
// item pass, ThrowItem +0x26 = the gate): its Door.AM1 never meets 0x693750's mesh ray (the shared AM1 vertex buffer
// holds no positions), so the port's structure ray passes the closed gate and the box stops a sphere.
int cannon_gate_part(const char* dir) {
    std::string err;
    auto gd = GameData::load(dir, &err);
    CHECK(gd, err);
    const joa::formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    CHECK(run.init(&err) && run.start(4, &err), err);
    Mission& m = *run.mission();
    m.autosave = false;
    World& w = m.world();
    for (int i = 0; i < 6000 && (w.cut_scene || w.stop_unit_process); ++i) m.tick();
    const int gi = m.objects().find_by_name("Tourelles gate"), tower = m.objects().find_by_name("Abtower");
    const MapItem* g = m.objects().item(gi);
    CHECK(g && g->state == 0 && tower, "Tourelles gate closed / Abtower");
    const float c = std::cos(g->dir), s = std::sin(g->dir);
    float t = 1;
    CHECK(m.objects().structure_ray(0, {g->pos.x - 200 * c, g->pos.y - 200 * s, 190}, {g->pos.x + 200 * c, g->pos.y + 200 * s, 190}).item != gi,
          "the gate's AM1 mesh is no ray target");
    CHECK(m.objects().sweep_items({g->pos.x - 200 * c, g->pos.y - 200 * s, 190}, 10, {400 * c, 400 * s, 0}, t) == gi &&
              std::fabs(t * 400 - (200 - 15 - 10)) < 1,
          "a sphere meets the gate's box face (x -15) 175 along");
    CHECK(m.objects().sweep_items({g->pos.x - 200 * c, g->pos.y - 200 * s, 280}, 10, {400 * c, 400 * s, 0}, t) == 0, "over the box (265 + 10)");
    Unit* p = w.unit(w.player_id());
    CHECK(p, "player");
    p->pos = {21603, 27100, w.terrain() ? w.terrain()->height(21603, 27100) : p->pos.z};
    for (int i = 0; i < 8000 / World::kTickMs; ++i) m.tick();
    m.scenario().bits[0] |= 1u << 26;   // bit 27: Jean freed
    MapItem* tw = m.objects().item(tower);
    tw->hp = tw->max_hp - 1;
    std::map<std::string, int> gate_hits, balls;
    int fell = -1, drops = 0, bad_drop = 0, hp_prev = g->hp;
    for (int i = 0; i < 120000 / World::kTickMs && (fell < 0 || w.now() - fell < 26000); ++i) {
        if (Unit* j = w.unit(w.player_id())) j->hp = w.max_hp(*j);
        if (fell < 0 && tw->hp > 1 && tw->hp < tw->max_hp - 1) tw->hp = 1;   // the first ball fells it
        if (fell < 0 && tw->hp == 0) fell = w.now();
        m.tick();
        for (const GameEvent& e : w.take_events()) {
            if (fell < 0) continue;
            const Unit* u = w.unit(e.unit);
            if (e.type == Ev::ProjectileSpawn && u && u->name.rfind("Cannon ", 0) == 0) ++balls[u->name];
            if (e.type == Ev::ProjectileHit && e.name == "item")
                for (const Projectile& b : w.projectiles())
                    if (b.id == e.other && b.hit_id == gi && w.unit(b.shooter)) ++gate_hits[w.unit(b.shooter)->name];
        }
        if (g->hp != hp_prev) ++drops, bad_drop += hp_prev - g->hp != 40, hp_prev = g->hp;
    }
    std::printf("cannons vs Tourelles gate: balls C1 %d C2 %d C3 %d C4 %d, gate hits C1 %d C2 %d C3 %d C4 %d, hp %d (%d drops)\n",
                balls["Cannon 1"], balls["Cannon 2"], balls["Cannon 3"], balls["Cannon 4"], gate_hits["Cannon 1"], gate_hits["Cannon 2"],
                gate_hits["Cannon 3"], gate_hits["Cannon 4"], g->hp, drops);
    CHECK(fell >= 0, "the Abtower falls");
    CHECK(gate_hits["Cannon 2"] >= std::max(2, balls["Cannon 2"] - 1) && gate_hits["Cannon 3"] >= std::max(2, balls["Cannon 3"] - 1) &&
              gate_hits["Cannon 1"] == 0 && gate_hits["Cannon 4"] == 0,
          "every ball of Cannon 2 and 3 hits the gate (the last may still fly), none of Cannon 1 and 4");
    CHECK(bad_drop == 0 && 1500 - g->hp == 40 * (gate_hits["Cannon 2"] + gate_hits["Cannon 3"]), "the gate takes 40 per ball");
    return 0;
}

// S7L1: Saint-Pierre's arbalests ("spc1".."spc7", CROSSBOW) stand on the wall walkways and the script crews each (LARGE_WEAPON_PUSH
// "spccN"). Unit_update_location 0x4fee60 puts an attached crew on the floor tracked under the attach point
// (Phys_track_poly + PathSpace_poly_height): the walkway beside its weapon. The port used the terrain height map, which
// dropped every crew ~400 below its arbalest, under the wall where nobody could reach it (objective 1 never finished).
int wall_crew_part(const char* dir) {
    std::string err;
    auto gd = GameData::load(dir, &err);
    CHECK(gd, err);
    const joa::formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    CHECK(run.init(&err) && run.start(7, &err), err);
    Mission& m = *run.mission();
    m.autosave = false;
    World& w = m.world();
    for (int t = 0; t < 1200; ++t) m.tick();
    int crews = 0;
    for (const Unit& c : w.units())
        if (const Unit* v = w.unit(c.m.vehicle); !c.deleted && c.name.rfind("spcc", 0) == 0 && v) {
            ++crews;
            std::printf("%s on %s: z %.0f / weapon z %.0f\n", c.name.c_str(), v->name.c_str(), c.pos.z, v->pos.z);
            CHECK(v->pos.z > 900 && std::fabs(c.pos.z - v->pos.z) < 50, "arbalest crew stands on the walkway beside its weapon");
        }
    CHECK(crews >= 5, "Saint-Pierre arbalests crewed");
    return 0;
}

}  // namespace

int main() {
    auto env = fixture::load();
    if (!env) return 77;
    if (int r = world_part(*env)) return r;
    if (int r = mission_part(std::getenv("JOA_GAME_DIR"))) return r;
    if (int r = cannon_tower_part(std::getenv("JOA_GAME_DIR"))) return r;
    if (int r = cannon_gate_part(std::getenv("JOA_GAME_DIR"))) return r;
    if (int r = wall_crew_part(std::getenv("JOA_GAME_DIR"))) return r;
    std::puts("ok");
    return 0;
}
