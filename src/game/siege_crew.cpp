// Siege-weapon crews and carts, World side. Decomp: work/ghidra/export/joan_decomp.c (FUNC addresses
// in the comments); confidence per rule in docs/GAMEPLAY_IMPLEMENTATION.md (siege crews).
#include "game/siege_crew.hpp"

#include <cmath>

#include "game/world_sim.hpp"

namespace joa::game {

namespace {

Unit* live(World& w, int id) {
    Unit* u = id > 0 ? w.unit(id) : nullptr;
    return u && !u->deleted ? u : nullptr;
}

const UnitClass& cls_of(const World& w, const Unit& u) { return const_cast<World&>(w).data().units[std::size_t(u.cls)]; }

// Crew of slot i (0 when empty / out of range).
int crew_at(const Unit& v, std::size_t i) { return i < v.m.crew.size() ? v.m.crew[i] : 0; }

// Actions that knock a crew member off its slot (0x501190 head: flinches, DNATKED, DIE, DIZZY, knockdowns,
// mounted hits).
bool unseats(int a) {
    return (a >= ATTACKED && a <= DIE) || a == DIZZY || is_knocked(a) || (a >= H_HITED && a <= H_RHITED);
}

}  // namespace

bool siege_slots_full(const World& w, const Unit& v, int mask) {   // 0x50d550
    if (!v.m.can_attach) return false;
    const auto& pts = cls_of(w, v).attach_points;
    int n = 0;
    for (std::size_t i = pts.size(); i-- > 0;) {
        if (!(pts[i].usage & mask)) continue;
        ++n;
        if (crew_at(v, i) < 1) return false;
    }
    return n != 0;
}

void siege_on_set_action(World& w, Unit& u, int a) {
    // 0x459970: a cart's locomotion state drives its driver's cart animation.
    if (Unit* d = live(w, u.m.cart_driver)) {
        if (a == WALK) w.set_action(*d, CA_WALK);
        else if (a == RUN || a == T_LEFT || a == T_RIGHT) w.set_action(*d, CA_RUN);
        else if (a == IDLE) w.set_action(*d, CA_IDLE);
    }
    // Crew hit or killed: free the slot (+0x378/+0x37c) and stop the weapon. The AI's +0x380 reserved
    // slot is AiSystem state; LARGE_WEAPON_PUSH re-reserves through get_nearest_attach_point.
    if (unseats(a)) {
        if (Unit* v = live(w, u.m.vehicle)) {
            const auto s = std::size_t(u.m.slot);
            if (s < v->m.crew.size()) v->m.crew[s] = 0;
            if (s < v->m.reserved.size()) v->m.reserved[s] = 0;
            w.stop(*v);
            u.m.vehicle = 0;
            u.m.slot = 0;
        }
    }
    // A manned weapon mirrors its firing / idle states onto the crew (from the last slot).
    const UnitClass& c = cls_of(w, u);
    const auto& pts = c.attach_points;
    auto each = [&](auto f) {
        for (std::size_t i = pts.size(); i-- > 0;)
            if (Unit* m = live(w, crew_at(u, i))) f(*m, pts[i].usage);
    };
    auto to = [&](int want) { return [&w, want](Unit& m, int usage) { if ((usage & 2) && m.action != want) w.set_action(m, want); }; };
    auto settle = [&w](Unit& m, int) { if (!is_idle_class(m.action)) w.stop(m); };
    switch (c.special) {
    case 4: case 5: case 6: case 9: case 10:   // cannon, fauconneau, siege crossbow, bombard, trebuchet
        if (a == SHTLOAD) each(to(SHTLOADC));
        else if (a == SHTIDLE || a == SHTIDLEH) each(to(SHTIDLEC));
        else if (a == SHTOUT || a == SHTOUTH) each(to(SHTOUTC));
        else if (a >= IDLE && a <= IDLE_BAT) each(settle);
        break;
    case 11:                                   // battering ram: the front crew swings with it
        if (a == ATTACK) each([&](Unit& m, int usage) {
            if (usage & 2) { if (m.action != BATTER_C) w.set_action(m, BATTER_C); }
            else settle(m, usage);
        });
        else if (a >= IDLE && a <= IDLE_BAT) each(settle);
        break;
    default: break;
    }
}

void siege_on_death(World& w, Unit& v, int killer) {   // Unit_die 0x7c6240 crew / driver part
    const UnitClass& c = cls_of(w, v);
    for (std::size_t i = c.attach_points.size(); i-- > 0;) {
        Unit* m = live(w, crew_at(v, i));
        if (!m) continue;
        m->m.slot = 0;
        m->m.vehicle = 0;
        m->special = 0;
        w.stop(*m);
        // The ram's front crew stands inside the ram and dies with it.
        if (c.special == 11 && (c.attach_points[i].usage & 2) && !w.dead(*m)) {
            m->lives = 0;
            w.set_hp(*m, 0);
            w.unit_die(*m, killer);
        }
    }
    if (Unit* d = live(w, v.m.cart_driver); d && d->m.cart == v.id) {
        d->m.cart = 0;
        d->lives = 0;
        w.set_hp(*d, 0);
        w.unit_die(*d, killer);
        v.m.cart_driver = 0;
    }
}

void siege_vehicle_move(World& w, Unit& u) {   // 0x52daa0 head
    if (u.m.vehicle >= 1) return;
    const UnitClass& c = cls_of(w, u);
    if (c.attach_points.empty() || (u.action != WALK && u.action != RUN)) return;
    if (!siege_slots_full(w, u, 1) || c.special == 9) {
        w.stop(u);
        return;
    }
    for (std::size_t i = c.attach_points.size(); i-- > 0;) {
        Unit* m = live(w, crew_at(u, i));
        if (!m) continue;
        const bool pusher = c.attach_points[i].usage & 1;
        int want;
        switch (c.special) {
        case 4: want = pusher ? PUSH_L : WALK; break;
        case 5: case 8: case 9: want = pusher ? PUSH : WALK; break;
        case 11: want = pusher ? PUSH : IDLE; break;
        default: continue;
        }
        if (m->action != want) w.set_action(*m, want);
    }
}

void siege_after_physics(World& w) {
    for (Unit& v : w.units()) {
        if (v.deleted) continue;
        // Unit_update_location 0x4fee60 (every position write of the weapon): crew at the attach
        // point rotated by the weapon, facing weapon + point angle, z on the ground under it + point z.
        const auto& pts = cls_of(w, v).attach_points;
        if (!pts.empty()) {
            const float yaw = v.yaw_rad(), cs = std::cos(yaw), sn = std::sin(yaw);
            for (std::size_t i = 0; i < pts.size(); ++i) {
                Unit* m = live(w, crew_at(v, i));
                if (!m) continue;
                const auto& p = pts[i];
                m->pos = {p.x * cs - p.y * sn + v.pos.x, p.x * sn + p.y * cs + v.pos.y, v.pos.z + p.z};
                if (w.terrain()) m->pos.z = w.terrain()->height(m->pos.x, m->pos.y) + p.z;
                w.set_yaw_now(*m, v.yaw_deg + p.dir);
                m->vel = {};
            }
        }
        // Body sync 0x6b1350: the driver sits 80 units behind the cart's origin; 0x454b20: same yaw.
        if (Unit* d = live(w, v.m.cart_driver); d && d->m.cart == v.id) {
            const float yaw = v.yaw_rad();
            d->pos = {v.pos.x - std::cos(yaw) * 80.0f, v.pos.y - std::sin(yaw) * 80.0f, v.pos.z};
            d->vel = {};
            w.set_yaw_now(*d, v.yaw_deg);
            if (v.action == IDLE && d->action != CA_IDLE) w.set_action(*d, CA_IDLE);
        }
    }
}

}  // namespace joa::game
