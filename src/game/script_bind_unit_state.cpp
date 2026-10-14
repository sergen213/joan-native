// Unit state setters (SCRIPT_VM.md §8 "unit-state" ids owned by the units_state slice). Each handler
// follows its executor case (VA in the comment): same validity checks and failure values. "live" =
// UnitArray_is_deleted(recno) == 0. Unit+0x33 Team* exists only on a leader (team_of_leader), its
// member list is the leader's Unit::members. Unit+0x1e (desired heading, degrees) is Unit::yaw_goal;
// Unit+0x1a (facing) is Unit::yaw_deg.
//
// Not modelled (render/physics only, no gameplay state in the port): hand-held weapon item instances
// (+0x1bd/+0x1bf, FUN_0050a480), unit-type resource refcounts, actor fade parameters, physics bodies.
#include "game/script_bind.hpp"

#include <cmath>
#include <string_view>

#include "formats/vfs_fs.hpp"
#include "game/mission.hpp"
#include "game/mount_ride.hpp"

namespace joa::game {
namespace {

constexpr float kDeg = 0.017453292f;
constexpr float kHalfPi = 1.5707964f;  // _DAT_00a3cbb4 / 2 (HYPOTHESIS: DAT_00a3cbb4 = pi)

Value ok(bool b) { return Value::integer(b ? 1 : 0); }

Unit* live(Mission& m, int recno) {
    Unit* u = m.world().unit(recno);
    return u && !u->deleted ? u : nullptr;
}

// Unit+0x33 Team (leader only) -> member recnos in insertion order; nullptr when no team.
const std::vector<int>* team(Mission& m, const Unit& u) {
    return m.ai().team_of_leader(u.id) ? &u.members : nullptr;
}

// Unit_set_yaw_deg 0x50abd0 (+0x1e) / Unit_set_yaw_rad 0x4ff870 (+0x1a).
void set_desired(Mission& m, Unit& u, float deg) { m.world().set_yaw(u, deg); }
void set_heading(Mission& m, Unit& u, float rad) { m.world().set_yaw_now(u, rad / kDeg); }

// AnimVariant_from_name 0x416aa0 (strcmp, -1 if unknown).
int anim_variant(std::string_view s) {
    static constexpr std::string_view kNames[] = {"DEFAULT", "TIRED",  "HAPPY",  "POISON", "CAMP",   "CANNON",
                                                  "FAUCONN", "BOMBARD", "CROSSBOW", "SIEGET", "TREB_L", "TREB_R",
                                                  "TREB_F",  "BATTER", "BATTER_L", "BATTER_R"};
    for (int i = 0; i < 16; ++i)
        if (s == kNames[i]) return i;
    return -1;
}

// UnitRes_parse_ranking 0x4064c9 (stricmp, unknown -> 0).
int parse_ranking(std::string_view s) {
    static constexpr std::pair<std::string_view, int> kRanks[] = {
        {"KING", 5}, {"GENERAL", 4}, {"CAPTAIN", 3}, {"ARCHER", 1}, {"INFANTRY", 0}, {"CAVALRY", 2}};
    for (auto [name, r] : kRanks)
        if (formats::iequals(s, name)) return r;
    return 0;
}

// Unit_set_rank 0x5089e0 (nation leader/free-unit lists are not modelled).
void set_rank(Mission& m, Unit& u, int rank) {
    if (rank == u.rank) return;
    if (rank > 2 && u.leader != 0) m.ai().leave_team(u);
    u.rank = rank;
    if (rank > 2 && !m.ai().is_leader(u)) m.ai().create_team(u);
}

// Unit_set_training_mode 0x510030.
void set_training_mode(World& w, Unit& u, int mode) {
    if (u.m.training == mode) return;
    u.m.training = mode;
    if (mode == 0) {
        u.lives = u.m.saved_lives;
    } else {
        u.m.saved_lives = u.lives;
        u.lives = 1;
    }
    u.hp = w.max_hp(u);
    u.ep = w.max_ep(u);
}

// Free-space probe FUN_00693170 (physics overlap of the actor at a candidate position) is not
// modelled: candidate positions always count as free (HYPOTHESIS).

// FUN_0051a8a0 on a cart: link / unlink the driver (+0x1f9). 1 ok, 0 refused.
int cart_set_driver(Mission& m, Unit& c, int driver) {
    World& w = m.world();
    if (driver == 0 && c.m.cart_driver != 0) {
        c.m.cart_driver = 0;
        if (c.m.active) {
            w.stop(c);
            c.m.bounding = 1;
        }
        c.ai_controlled = true;
        return 1;
    }
    Unit* d = live(m, driver);
    if (!d || c.m.cart_driver != 0) return 0;
    c.m.cart_driver = driver;
    w.stop(c);
    c.ai_controlled = d->ai_controlled;
    return 1;
}

// FUN_0051a250 (ECX = driver): board `cart`, or get off when 0. Returns 0 refused, 1 ok, 2 bad cart. HIGH.
int ride_cart(Mission& m, Unit& u, int cart) {
    if (cart == u.id) return 0;
    if (cart == u.m.cart) return 1;
    if (cart == 0 && u.m.cart != 0) {
        // Side probe at pos + side*80 always free here; the driver's position is restored after it.
        u.m.bounding = 0;
        set_desired(m, u, u.yaw_deg);
        if (Unit* c = live(m, u.m.cart)) {
            cart_set_driver(m, *c, 0);
            set_desired(m, *c, c->yaw_deg);
        }
        u.m.cart = 0;
        return 1;
    }
    Unit* c = live(m, cart);
    if (!c) return 2;
    if (!cart_set_driver(m, *c, u.id)) return 0;
    const float yaw = c->yaw_rad();
    const float fx = std::cos(yaw), fy = std::sin(yaw);
    const float heading = std::atan2(fy, fx);
    if (m.world().rel_angle(*c, u.pos) >= 0) {
        u.pos = {c->pos.x + fx * 80, c->pos.y + fy * 80, c->pos.z};
        set_heading(m, u, heading + kHalfPi);
    } else {
        u.pos = {c->pos.x - fx * 80, c->pos.y - fy * 80, c->pos.z};
        set_heading(m, u, heading - kHalfPi);
    }
    u.m.cart = cart;
    return 1;
}

// Shared body of attach_unit 0x657670 (range + facing test) and move_and_attach_unit 0x65f160
// (teleport): first free vehicle attach point matching the M/F mask. 1 attached, 0 none, -1 bad.
int attach(Mission& m, int rider, int vehicle, std::string_view kind, int radius, bool teleport) {
    World& w = m.world();
    Unit* r = live(m, rider);
    Unit* v = live(m, vehicle);
    if (!r || !v) return -1;
    const int mask = !kind.empty() && kind[0] == 'M' ? 1 : !kind.empty() && kind[0] == 'F' ? 2 : 0;
    if (!mask) return -1;
    if (!is_idle_class(r->action)) return 0;  // HYPOTHESIS: idle-class test is on the rider
    const UnitClass& vc = m.data().units[v->cls];
    const size_t n = vc.attach_points.size();
    if (v->m.crew.size() < n) v->m.crew.resize(n), v->m.reserved.resize(n);
    const float yaw = v->yaw_rad(), cs = std::cos(yaw), sn = std::sin(yaw);
    for (size_t i = 0; i < n; ++i) {
        const UnitClass::AttachPoint& p = vc.attach_points[i];
        if (!(p.usage & mask) || v->m.crew[i] >= 1) continue;
        const Vec3 at{p.x * cs - p.y * sn + v->pos.x, p.x * sn + p.y * cs + v->pos.y, p.z + v->pos.z};
        if (teleport) {
            r->pos = at;
        } else {
            const float dx = r->pos.x - at.x, dy = r->pos.y - at.y;
            if (dx * dx + dy * dy > float(radius * radius)) continue;
            // Rider facing within 2 deg of the slot direction (HYPOTHESIS: FUN_00577770 = |diff| > 2 deg);
            // otherwise request the turn (+0x1e) and keep scanning.
            float diff = std::remainder(r->yaw_rad() - (yaw + p.dir * kDeg), 2 * 3.14159265f);
            if (std::fabs(diff) > 2 * kDeg) {
                set_desired(m, *r, yaw / kDeg + p.dir);
                continue;
            }
        }
        v->m.crew[i] = r->id;
        v->m.reserved[i] = 0;
        r->m.vehicle = v->id;
        r->m.slot = int(i);
        set_desired(m, *r, yaw / kDeg + p.dir);
        switch (vc.special) {
        case 4: r->special = 5; break;
        case 5: r->special = 6; break;
        case 6: r->special = 8; break;
        case 8: r->special = 9; break;
        case 9: r->special = 7; break;
        case 10:
            if (i <= 2) r->special = 10 + int(i);
            break;
        case 11:
            if (mask == 1) r->special = 13;
            else r->special = p.y < 0 ? 15 : 14;
            break;
        }
        w.set_action(*r, r->action);
        return 1;
    }
    return 0;
}

// Detach body of 0x657cc0 for one occupied slot (rider valid).
void detach_rider(Mission& m, Unit& v, Unit& r, size_t slot) {
    set_desired(m, r, r.yaw_deg);
    r.m.vehicle = 0;
    r.m.slot = 0;
    m.ai().st(r).push_slot = -1;  // +0x380
    m.world().stop(r);
    v.m.crew[slot] = 0;
    v.m.reserved[slot] = 0;
    r.special = 0;
    // FUN_0065b9b0 (get_default_weapon) only re-creates hand items: not modelled.
}

// ---------------------------------------------------------------------------------------------
// Handlers

// 47 set_unit_mode 0x6515d0: no deleted check; unknown string ignored. Returns 0.
Value set_unit_mode(Mission& m, const BuiltinCall& c) {
    if (Unit* u = m.world().unit(c.i(0))) {
        std::string_view s = c.s(1);
        if (s == "IDLE") u->mode = 3;
        else if (s == "ATTACK") u->mode = 1;
        else if (s == "ACTIVE") u->mode = 2;
    }
    return Value::integer(0);
}

// 48 set_unit_attack_behaviour 0x651690. Returns 0.
Value set_unit_attack_behaviour(Mission& m, const BuiltinCall& c) {
    static constexpr std::string_view kNames[] = {"NEAREST", "WEAKEST", "IMPORTANT", "NEAREST_ARCHER",
                                                  "NEAREST_HIGH_PRIORITY"};
    if (Unit* u = live(m, c.i(0)))
        for (int i = 0; i < 5; ++i)
            if (c.s(1) == kNames[i]) {
                u->attack_behaviour = i + 1;
                break;
            }
    return Value::integer(0);
}

// 68 set_unit_drop_item_id 0x653210: level = ItemRes +0x36 (MinLevel).
Value set_unit_drop_item_id(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    const int id = short(c.i(1));
    u->drop_item = id;
    if (id > 0 && id < int(m.data().items.size())) u->drop_level = m.data().items[id].min_level;
    return ok(true);
}

// 69 set_unit_drop_item_level 0x6532a0.
Value set_unit_drop_item_level(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->drop_level = c.i(1);
    return ok(u);
}

// 70 set_unit_track 0x653300 -> FUN_00427b50: +0x3c9 = track id by name (FUN_004ec320).
Value set_unit_track(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->m.track = m.places().track_id(c.s(1));
    return ok(u);
}

// 73 set_unit_can_process_in_cut_scene 0x653590: unit 0 = every unit (top down).
Value set_unit_can_process_in_cut_scene(Mission& m, const BuiltinCall& c) {
    const int flag = static_cast<signed char>(c.i(1));
    if (c.i(0) == 0) {
        auto& us = m.world().units();
        for (int r = int(us.size()); r > 0; --r)
            if (Unit* u = live(m, r)) u->m.cutscene_process = flag;
        return ok(true);
    }
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    u->m.cutscene_process = flag;
    return ok(true);
}

// 74 set_unit_team_can_process_in_cut_scene 0x653640: unit (no deleted check) + live team members
// (last to first); unit 0 = every unit. Always 1.
Value set_unit_team_can_process_in_cut_scene(Mission& m, const BuiltinCall& c) {
    const int flag = static_cast<signed char>(c.i(1));
    if (c.i(0) == 0) {
        auto& us = m.world().units();
        for (int r = int(us.size()); r > 0; --r)
            if (Unit* u = live(m, r)) u->m.cutscene_process = flag;
        return ok(true);
    }
    Unit* u = m.world().unit(c.i(0));
    if (!u) return ok(true);
    u->m.cutscene_process = flag;
    if (const auto* mem = team(m, *u))
        for (size_t i = mem->size(); i > 0; --i)
            if (Unit* t = live(m, (*mem)[i - 1])) t->m.cutscene_process = flag;
    return ok(true);
}

// 78 set_unit_level 0x65c250: Unit_set_level 0x523190 then hp = max hp.
Value set_unit_level(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    World& w = m.world();
    w.set_level(*u, c.i(1));
    w.set_hp(*u, w.max_hp(*u));
    return ok(true);
}

// 79 set_unit_id 0x65c2e0: +0x3b = unit type id (script id = cls + 1). The active-unit type refcount
// move is resource bookkeeping only. An out-of-range id is not written (the original would crash later).
Value set_unit_id(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    const int id = short(c.i(1));
    if (id >= 1 && id <= int(m.data().units.size())) u->cls = id - 1;
    return ok(true);
}

// 82 add_gold 0x65c220.
Value add_gold(Mission& m, const BuiltinCall& c) {
    m.state().gold += c.i(0);
    return ok(true);
}

// 83 set_unit_hide_hit_point_bar_flag 0x652de0: 0 without an HP-bar object (+0x31d).
// HYPOTHESIS: the HP-bar object exists while the unit is active (+0x2ed).
Value set_unit_hide_hit_point_bar_flag(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u || !u->m.active) return ok(false);
    u->m.hide_hp_bar = static_cast<signed char>(c.i(1));
    return ok(true);
}

// 84 activate_all_unit 0x652e60: Unit_clear_task on every live unit (top down). Always 1.
Value activate_all_unit(Mission& m, const BuiltinCall&) {
    for (int r = int(m.world().units().size()); r > 0; --r)
        if (Unit* u = live(m, r)) m.ai().clear_task(*u);
    return ok(true);
}

// 88 set_unit_pos 0x6554f0: returns recno (0 missing).
Value set_unit_pos(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(0);
    u->pos = {c.f(1), c.f(2), c.f(3)};
    return Value::integer(short(c.i(0)));
}

// 89 set_unit_dir 0x655590: facing +0x1a and desired heading +0x1e.
Value set_unit_dir(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    const float deg = float(c.i(1));
    m.world().set_yaw(*u, deg);
    set_desired(m, *u, deg);
    return ok(true);
}

// 90 set_unit_degree 0x655620: desired heading +0x1e only.
Value set_unit_degree(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    set_desired(m, *u, float(c.i(1)));
    return ok(true);
}

// 92 set_unit_ranking 0x6556e0: returns recno (0 missing).
Value set_unit_ranking(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(0);
    set_rank(m, *u, parse_ranking(c.s(1)));
    return Value::integer(short(c.i(0)));
}

// 93 set_unit_nation 0x655750: nation name (stricmp) -> id; not found -> +2 = 0, returns 0.
// Unit_set_nation 0x509250's nation unit counts / morale strength are not modelled.
Value set_unit_nation(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(0);
    const int id = m.data().nation_id(c.s(1));
    u->nation = id;
    return Value::integer(id);
}

// 94 deactivate_unit 0x655810 / 95 activate_unit 0x655870: +0x2ed (render/physics objects not modelled).
Value deactivate_unit(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(0);
    u->m.active = 0;
    return Value::integer(short(c.i(0)));
}
Value activate_unit(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(0);
    u->m.active = 1;
    return Value::integer(short(c.i(0)));
}

// 96 set_unit_target_unit_recno 0x6558d0.
Value set_unit_target_unit_recno(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(0);
    m.ai().set_target(*u, short(c.i(1)));
    return Value::integer(short(c.i(0)));
}

// 97 set_unit_ride_horse 0x655930 -> FUN_004fc220. Returns recno (0 missing), whatever the mount did.
Value set_unit_ride_horse(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(0);
    ride_horse(m.world(), *u, short(c.i(1)));
    return Value::integer(short(c.i(0)));
}

// 100 set_unit_ride_cart 0x6559e0 -> FUN_0051a250. Returns recno (0 missing).
Value set_unit_ride_cart(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(0);
    ride_cart(m, *u, short(c.i(1)));
    return Value::integer(short(c.i(0)));
}

// 101 set_unit_ride_off_cart 0x655a40: raw unlink (no repositioning).
Value set_unit_ride_off_cart(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    if (u->m.cart != 0) {
        if (Unit* cart = live(m, u->m.cart)) cart->m.cart_driver = 0;
        u->m.cart = 0;
    }
    return ok(true);
}

// 103 / 105 / 107 / 114: short field writes on a live unit; always 1.
Value set_interact_unit_recno(Mission& m, const BuiltinCall& c) {  // 0x655c20 +0x2e3
    if (Unit* u = live(m, c.i(0))) u->m.interact_unit = short(c.i(1));
    return ok(true);
}
Value set_interact_item_recno(Mission& m, const BuiltinCall& c) {  // 0x655cd0 +0x2e5
    if (Unit* u = live(m, c.i(0))) u->m.interact_item = short(c.i(1));
    return ok(true);
}
Value set_can_interact_flag(Mission& m, const BuiltinCall& c) {  // 0x655e30 +0x2e9
    if (Unit* u = live(m, c.i(0))) u->m.can_interact = short(c.i(1));
    return ok(true);
}
Value set_interact_stage(Mission& m, const BuiltinCall& c) {  // 0x655d80 +0x2e7
    if (Unit* u = live(m, c.i(0))) u->m.interact_stage = short(c.i(1));
    return ok(true);
}

// 108 set_can_bribe_flag 0x655e90: unless already bribed (2). Always 1.
Value set_can_bribe_flag(Mission& m, const BuiltinCall& c) {
    if (Unit* u = live(m, c.i(0)); u && u->m.bribe != 2) u->m.bribe = short(c.i(1));
    return ok(true);
}

// 110 set_units_can_bribe_flag 0x655f80: every unit (1..N) of a unit type id. Always 1.
Value set_units_can_bribe_flag(Mission& m, const BuiltinCall& c) {
    const int type = short(c.i(0));
    for (int r = 1; r <= int(m.world().units().size()); ++r)
        if (Unit* u = live(m, r); u && u->cls + 1 == type && u->m.bribe != 2) u->m.bribe = short(c.i(1));
    return ok(true);
}

// 115 set_unit_retreat 0x656210. Always 1.
Value set_unit_retreat(Mission& m, const BuiltinCall& c) {
    if (Unit* u = live(m, c.i(0))) u->retreat = c.i(1);
    return ok(true);
}

// 117 set_unit_armor_id 0x6562b0. Always 1.
Value set_unit_armor_id(Mission& m, const BuiltinCall& c) {
    if (Unit* u = live(m, c.i(0))) u->armor_item = short(c.i(1));
    return ok(true);
}

// 119 set_unit_name 0x656350: strncpy 64. Always 0.
Value set_unit_name(Mission& m, const BuiltinCall& c) {
    if (Unit* u = live(m, c.i(0)); u && c.args.size() > 1) u->name = std::string(c.s(1).substr(0, 64));
    return Value::integer(0);
}

// 121 set_unit_dead 0x656410: actor collision off (+0xb8 = 0 when it has an actor), then the
// fade-out + die of AiSystem::set_unit_dead. HIGH.
Value set_unit_dead(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    if (u->m.active) u->m.bounding = 0;
    m.ai().set_unit_dead(*u);
    return ok(true);
}

// 122 reset_unit_hp 0x6568d0: hp = base max hp (+0x13d, raw). Always 1.
Value reset_unit_hp(Mission& m, const BuiltinCall& c) {
    if (Unit* u = live(m, c.i(0))) u->hp = u->base_max_hp;
    return ok(true);
}

// 123 set_unit_hp 0x656990: raw +0x135 write.
Value set_unit_hp(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->hp = c.i(1);
    return ok(u);
}

// 127 attach_unit 0x657670 (HIGH). 389 move_and_attach_unit 0x65f160.
Value attach_unit(Mission& m, const BuiltinCall& c) {
    return Value::integer(attach(m, short(c.i(0)), short(c.i(1)), c.s(2), c.i(3), false));
}
Value move_and_attach_unit(Mission& m, const BuiltinCall& c) {
    return Value::integer(attach(m, short(c.i(0)), short(c.i(1)), c.s(2), 0, true));
}

// 129 detach_unit 0x657cc0: slot < 0 = every slot (last first). 1 ok, 0 bad slot, -1 bad vehicle.
Value detach_unit(Mission& m, const BuiltinCall& c) {
    Unit* v = live(m, c.i(0));
    if (!v) return Value::integer(-1);
    const int slot = c.i(1);
    const size_t n = m.data().units[v->cls].attach_points.size();
    if (v->m.crew.size() < n) v->m.crew.resize(n), v->m.reserved.resize(n);
    if (slot < 0) {
        for (size_t i = n; i > 0; --i)
            if (Unit* r = m.world().unit(v->m.crew[i - 1])) detach_rider(m, *v, *r, i - 1);
            else v->m.crew[i - 1] = 0, v->m.reserved[i - 1] = 0;
        return ok(true);
    }
    if (size_t(slot) >= n) return ok(false);
    if (Unit* r = live(m, v->m.crew[slot])) detach_rider(m, *v, *r, slot);
    else v->m.crew[slot] = 0, v->m.reserved[slot] = 0;
    return ok(true);
}

// 136 set_control_flag 0x6574a0: +0x2f = (flag == 0) on the unit and, for a leader, its live members.
// 1 ok, 0 no team, -1 bad unit.
Value set_control_flag(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return Value::integer(-1);
    const int v = c.i(1) == 0;
    u->m.control = v;
    const auto* mem = team(m, *u);
    if (!mem) return Value::integer(0);
    for (int r : *mem)
        if (Unit* t = live(m, r)) t->m.control = v;
    return ok(true);
}

// 157 set_main_character 0x658f70. mode 1: player unit (FUN_0042f9c0), HUD unit, camera 0 target.
// mode 2: player-control +0x04. Always 1.
Value set_main_character(Mission& m, const BuiltinCall& c) {
    World& w = m.world();
    const int id = short(c.i(0));
    const int mode = short(c.i(1));
    if (mode == 1) {
        Unit* u = live(m, id);
        if (u) {
            u->ai_controlled = false;
            w.set_player(id);  // old player +0x102 = 1
            u->combo_lock = 0;
            u->ai_controlled = w.game_mode == 4;
        }
        m.emit(MissionEvent::Kind::Camera, 0, id, "target");
    } else if (mode == 2) {
        w.controller().unit2 = id;
    }
    return ok(true);
}

// 217 set_ride_mode 0x659860: player-control byte +0xc4.
Value set_ride_mode(Mission& m, const BuiltinCall& c) {
    m.world().controller().ride_mode = static_cast<signed char>(c.i(0));
    return ok(true);
}

// 262 quit_rts_mode 0x65a320 -> PlayerCtrl_set_mode 0x42e040(prev_mode, 0). HIGH: the pending-switch
// (+0x81), DAT_00a48cef and lock (+0xcd) gates are not modelled.
Value quit_rts_mode(Mission& m, const BuiltinCall&) {
    World& w = m.world();
    PlayerController& pc = w.controller();
    const int to = pc.prev_mode;
    if (w.game_mode != to) {
        if (w.game_mode == 4)
            if (Unit* p = live(m, w.player_id())) {
                m.ai().rts_exit(*p);  // ViewCtrl_exit_mode -> Rts_deinit
                m.ai().clear_task(*p);
            }
        pc.prev_mode = w.game_mode;
        w.game_mode = to;
    }
    return ok(true);
}

// 311 set_getting_weapon_flag 0x65b8c0.
Value set_getting_weapon_flag(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    u->m.getting_weapon = c.i(1);
    u->special = c.i(1) ? 4 : 0;
    return ok(true);
}

// 313 get_default_weapon 0x65b9b0: re-creates the default hand item instances; hand items are not
// modelled (the inventory is unchanged by it), so only the validity result remains.
Value get_default_weapon(Mission& m, const BuiltinCall& c) { return ok(live(m, c.i(0))); }

// 328 change_animation_special 0x65c050.
Value change_animation_special(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->special = anim_variant(c.s(1));
    return ok(u);
}

// 335 set_unit_training_mode 0x65c870 (HIGH).
Value set_unit_training_mode(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) set_training_mode(m.world(), *u, c.i(1));
    return ok(u);
}

// 339 add_unit_experience 0x65c9e0: Unit_add_exp(exp, 0, 0) (HIGH).
Value add_unit_experience(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) m.world().add_xp(*u, c.i(1), false, false);
    return ok(u);
}

// 347 switch_unit_model 0x65cfd0: mesh swap (0 default, 1 alternate). HIGH: FUN_0050cbc0(1,1) for
// model 1 is a render-side call.
Value switch_unit_model(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    if (c.i(1) == 0 || c.i(1) == 1) u->m.model_alt = c.i(1);
    return ok(true);
}

// 348 enable_unit_physic 0x65d070 -> FUN_005102a0 (+0x3b8; the body itself is not modelled).
Value enable_unit_physic(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->m.physics_disabled = c.i(1) == 0;
    return ok(u);
}

// 349 enable_unit_bounding_volume 0x65d0d0: actor collision volume (+0xb8) off / class volume.
Value enable_unit_bounding_volume(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->m.bounding = c.i(1) != 0;
    return ok(u);
}

// 350 enable_unit_team_bounding_volume 0x65d350: the unit, then live team members (last to first).
Value enable_unit_team_bounding_volume(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    const int on = c.i(1) != 0;
    u->m.bounding = on;
    if (const auto* mem = team(m, *u))
        for (size_t i = mem->size(); i > 0; --i)
            if (Unit* t = live(m, (*mem)[i - 1])) t->m.bounding = on;
    return ok(true);
}

// 358 set_prisoner 0x65daf0 -> Unit_set_prisoner 0x519800(type, 1). Hiding the hand items is not modelled.
Value set_prisoner(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    if (u->no_react && u->action == IDLE_T) return ok(true);
    const int type = c.args.size() > 1 ? c.i(1) : 1;
    World& w = m.world();
    w.stop(*u);
    if (w.can_use_special(*u, IDLE_T)) w.set_action(*u, IDLE_T);  // bound = 1 branch
    u->no_react = static_cast<signed char>(type) != 0;
    u->m.bound = 1;
    return ok(true);
}

// 359 free_prisoner 0x65dba0 -> Unit_free_prisoner 0x5199b0 (HIGH). Hand items not modelled.
Value free_prisoner(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    if (u->no_react) {
        u->no_react = false;
        u->m.bound = 0;
        m.world().stop(*u);
    }
    return ok(true);
}

// 373 set_unit_can_rided_flag 0x65e9a0 / 399 set_unit_vision_range 0x65f980 /
// 401 set_unit_hide_in_command_interface 0x65fb50 / 407 set_prisoner_can_be_attacked_flag 0x65fd10.
Value set_unit_can_rided_flag(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->m.can_rided = static_cast<signed char>(c.i(1));
    return ok(u);
}
Value set_unit_vision_range(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->m.vision = c.i(1);
    return ok(u);
}
Value set_unit_hide_in_command_interface(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->m.hide_cmd = static_cast<signed char>(c.i(1));
    return ok(u);
}
Value set_prisoner_can_be_attacked_flag(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (u) u->m.prisoner_can_be_attacked = static_cast<signed char>(c.i(1));
    return ok(u);
}

// 400 set_unit_team_vision_range 0x65f9e0: leader -> live members (first to last); no team ->
// every live unit with the same team id (+0x31), top down (includes the unit itself).
Value set_unit_team_vision_range(Mission& m, const BuiltinCall& c) {
    Unit* u = live(m, c.i(0));
    if (!u) return ok(false);
    const int range = c.i(1);
    u->m.vision = range;
    if (const auto* mem = team(m, *u)) {
        for (int r : *mem)
            if (Unit* t = live(m, r)) t->m.vision = range;
    } else {
        for (int r = int(m.world().units().size()); r > 0; --r)
            if (Unit* t = live(m, r); t && short(t->team) == short(u->team)) t->m.vision = range;
    }
    return ok(true);
}

}  // namespace

void bind_unit_state(BuiltinTable& t) {
    t.call[47] = &set_unit_mode;
    t.call[48] = &set_unit_attack_behaviour;
    t.call[68] = &set_unit_drop_item_id;
    t.call[69] = &set_unit_drop_item_level;
    t.call[70] = &set_unit_track;
    t.call[73] = &set_unit_can_process_in_cut_scene;
    t.call[74] = &set_unit_team_can_process_in_cut_scene;
    t.call[78] = &set_unit_level;
    t.call[79] = &set_unit_id;
    t.call[82] = &add_gold;
    t.call[83] = &set_unit_hide_hit_point_bar_flag;
    t.call[84] = &activate_all_unit;
    t.call[88] = &set_unit_pos;
    t.call[89] = &set_unit_dir;
    t.call[90] = &set_unit_degree;
    t.call[92] = &set_unit_ranking;
    t.call[93] = &set_unit_nation;
    t.call[94] = &deactivate_unit;
    t.call[95] = &activate_unit;
    t.call[96] = &set_unit_target_unit_recno;
    t.call[97] = &set_unit_ride_horse;
    t.call[100] = &set_unit_ride_cart;
    t.call[101] = &set_unit_ride_off_cart;
    t.call[103] = &set_interact_unit_recno;
    t.call[105] = &set_interact_item_recno;
    t.call[107] = &set_can_interact_flag;
    t.call[108] = &set_can_bribe_flag;
    t.call[110] = &set_units_can_bribe_flag;
    t.call[114] = &set_interact_stage;
    t.call[115] = &set_unit_retreat;
    t.call[117] = &set_unit_armor_id;
    t.call[119] = &set_unit_name;
    t.call[121] = &set_unit_dead;
    t.call[122] = &reset_unit_hp;
    t.call[123] = &set_unit_hp;
    t.call[127] = &attach_unit;
    t.call[129] = &detach_unit;
    t.call[136] = &set_control_flag;
    t.call[157] = &set_main_character;
    t.call[217] = &set_ride_mode;
    t.call[262] = &quit_rts_mode;
    t.call[311] = &set_getting_weapon_flag;
    t.call[313] = &get_default_weapon;
    t.call[328] = &change_animation_special;
    t.call[335] = &set_unit_training_mode;
    t.call[339] = &add_unit_experience;
    t.call[347] = &switch_unit_model;
    t.call[348] = &enable_unit_physic;
    t.call[349] = &enable_unit_bounding_volume;
    t.call[350] = &enable_unit_team_bounding_volume;
    t.call[358] = &set_prisoner;
    t.call[359] = &free_prisoner;
    t.call[373] = &set_unit_can_rided_flag;
    t.call[389] = &move_and_attach_unit;
    t.call[399] = &set_unit_vision_range;
    t.call[400] = &set_unit_team_vision_range;
    t.call[401] = &set_unit_hide_in_command_interface;
    t.call[407] = &set_prisoner_can_be_attacked_flag;
    t.unimplemented[261] = "switch_controllable_unit (unused): ViewCtrl_switch_character 0x432300 hero cycling + "
                           "task parking not modelled";
    t.unimplemented[351] = "mount_siege_tower_to_wall (unused): wall link (wall+0xc6/+0xca, FUN_004633e0) not "
                           "modelled in MapObjects";
}

}  // namespace joa::game
