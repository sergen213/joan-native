// Point & click controller, mode 2 ("mouse-driven", t_basic POPUP|4): PlayerCtrl_mouse_click 0x438b30 and the mode
// update 0x439090 (Player_autoAttackUpdate 0x43acb0 / item approach 0x43b2d0). docs/GAMEPLAY_IMPLEMENTATION.md
// "Point & click".
#include "game/controller_player.hpp"

#include <cmath>
#include <optional>

#include "game/ai_system.hpp"
#include "game/rng_lcg.hpp"
#include "game/world_sim.hpp"

namespace joa::game {

namespace {

// Target of the auto action: position (Unit_get_position / item 0x499930) and actor bounding radius
// (0x5acc80(.., 1): model bounding sphere x scale). ponytail: game/ has no models; unit = the sphere around its
// class collision cylinder, map item = AiItem::radius (ItemRes footprint). Upgrade: model spheres from the engine.
struct Target {
    Vec3 pos;
    float radius = 0;
};

std::optional<Target> target_of(const World& w, AiSystem* ai, int id, int kind) {
    if (kind == 0) {  // UnitArray_is_deleted / Unit_is_dead
        const Unit* t = w.unit(id);
        if (!t || w.dead(*t)) return std::nullopt;
        if (t->cls < 0 || std::size_t(t->cls) >= w.data().units.size()) return Target{t->pos, 40.0f};
        const UnitClass& c = w.data().units[std::size_t(t->cls)];
        const float h = c.height > 0 ? float(c.height) : c.cyl_height;
        return Target{t->pos, std::sqrt(c.cyl_radius * c.cyl_radius + h * h * 0.25f)};
    }
    if (kind == 1 && ai && ai->item)  // ItemArray_is_deleted
        if (const std::optional<AiItem> it = ai->item(id)) return Target{it->pos, it->radius};
    return std::nullopt;
}

float dist3(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 0x43a580: Unit_clear_path, refused while THROWN (0x3b) / DIZZY (0x3d), else Unit_path_to(p, budget 100, 0, 0).
bool path_to(AiSystem* ai, Unit& u, Vec3 p) {
    if (!ai) return false;
    ai->clear_path(u);
    if (u.action == THROWN || u.action == DIZZY) return false;
    return ai->move_to(u, p, 100, false, 0) != 0;
}

}  // namespace

AiSystem* PlayerController::click_ai(World& w) { return dynamic_cast<AiSystem*>(w.brain_); }

// Player_autoAttackSetTarget 0x43a7d0 (third argument unused, `ret 0xc` reads 2): refused in fury; path to the
// target; a second set while attacking with EP >= EGATK_MP (raw, no EP_CONSUME) pays it and starts the fury round.
bool PlayerController::auto_attack_set(World& w, Unit& u, int id, int kind) {
    if (id == 0 || auto_state == 2) return false;
    AiSystem* ai = click_ai(w);
    const std::optional<Target> t = target_of(w, ai, id, kind);
    if (!t || !path_to(ai, u, t->pos)) return false;
    auto_target = id, auto_kind = kind;
    const int cost = w.data().c.egatk_mp;  // DAT_00a3cd28
    if (auto_state == 1 && u.ep >= cost) {
        auto_state = 2;
        w.set_ep(u, u.ep - cost);
    } else {
        auto_state = 1;
    }
    return true;
}

// 0x43a9e0: path to the target, state 3 (energy attack on arrival).
bool PlayerController::auto_energy_set(World& w, Unit& u, int id, int kind) {
    if (id == 0) return false;
    AiSystem* ai = click_ai(w);
    const std::optional<Target> t = target_of(w, ai, id, kind);
    if (!t || !path_to(ai, u, t->pos)) return false;
    auto_target = id, auto_kind = kind, auto_state = 3;
    return true;
}

// PlayerCtrl_mouse_click 0x438b30 (u = the live controlled unit; the dispatcher head ran).
bool PlayerController::process_click(World& w, Unit& u, const InputFrame& in) {
    // Keys: Action_pressed(0, ..) = the keyboard (K_KEY) column, whatever the mode.
    if (in.pressed[IN_HEAL_ITEM]) return w.use_recovery_item(u, false), true;                 // 0x13 Ctrl_use_hp_item
    if (in.pressed[IN_EP_ITEM] && !in.alt_held) return w.use_recovery_item(u, true), true;    // 0x14, no Alt (0xa51800 & 0x30)
    if (in.pressed[IN_INTERACT]) {                                                           // 0x11: item handler 0x43bc50 only
        if (w.interact_item) w.interact_item(u);
        return true;
    }
    AiSystem* ai = click_ai(w);
    auto clear = [&] { auto_target = auto_kind = auto_state = 0; };  // 0x43ac60
    const bool obj = in.pick_unit || in.pick_item || in.pick_drop;  // 0x439110 hit
    const std::optional<AiItem> item = in.pick_item && ai && ai->item ? ai->item(in.pick_item) : std::nullopt;
    const WorldItem* drop = in.pick_drop ? w.item(in.pick_drop) : nullptr;
    if (drop && drop->deleted) drop = nullptr;
    const Unit* t = in.pick_unit ? w.unit(in.pick_unit) : nullptr;
    const auto hostile_item = [&](int nation) { return nation != 0 && w.hostile(u.nation, nation); };  // 0x509590
    if (in.click_left) {
        if (!obj) {  // terrain 0x43a3b0: clear, path there
            if (!in.pick_ground) return false;
            clear();
            return path_to(ai, u, {in.ground_x, in.ground_y, in.ground_z});
        }
        if (!in.pick_unit) {  // object type 1 (item)
            if (!item && !drop) return false;
            if (drop || item->on_floor) {  // +0xa2 floor marker -> 0x43abd0: clear, walk to it
                clear();
                return path_to(ai, u, drop ? drop->pos : item->pos);
            }
            if (hostile_item(item->nation)) return auto_attack_set(w, u, in.pick_item, 1);
            // 0x509670 own / allied nation -> 0x43a650: approach, state 4. ponytail: allies by relation status 4
            // (mission Places) are not visible to World; only the hero's own nation counts.
            if (item->nation != 0 && item->nation == u.nation) {
                if (!path_to(ai, u, item->pos)) return false;
                auto_state = 4, auto_kind = 1, auto_target = in.pick_item;
                return true;
            }
            // 0x43a740 (item class +0x4e == 0x1e) never matches: ITEMCLAS runs 1..20. Dead branch, not ported.
        } else {  // object type 0xb (unit)
            if (!t) return false;
            if (t->nation != 0 && t->nation == u.nation) return false;  // 0x5095e0 friendly -> 0x43a720 (returns 0)
            if (w.hostile_to_player(*t)) return auto_attack_set(w, u, t->id, 0);
        }
    } else if (in.click_right) {  // RMB: energy attack on arrival
        if (!obj) return false;
        if (!in.pick_unit) {
            if (!item && !drop) return false;
            if (item && !item->on_floor && hostile_item(item->nation)) return auto_energy_set(w, u, in.pick_item, 1);
        } else {
            if (!t) return false;
            if (w.hostile_to_player(*t)) return auto_energy_set(w, u, t->id, 0);
        }
    }
    attack_input(w, u, false, false, false);  // Player_attackInput(0, 0, 0)
    return false;
}

// 0x439090: states 1..3 Player_autoAttackUpdate 0x43acb0, state 4 0x43b2d0, then 0x4384e0.
void PlayerController::update_click(World& w) {
    auto clear = [&] { auto_target = auto_kind = auto_state = 0; };  // 0x43ac60
    Unit* up = w.unit(unit);
    AiSystem* ai = click_ai(w);
    if (!up) {
        if (auto_state > 0) clear();
    } else if (auto_state >= 1 && auto_state <= 3) {
        Unit& u = *up;
        const std::optional<Target> t = target_of(w, ai, auto_target, auto_kind);
        if (!t) {
            clear();
        } else if (t->radius + 10.0f <= dist3(t->pos, u.pos)) {
            // Out of reach: once the hero has no path left (+0x473 / +0x432 empty) the target is set again.
            if (!ai || ai->st(u).path.empty()) {
                const int id = auto_target, kind = auto_kind;
                if (auto_state == 1) clear(), auto_attack_set(w, u, id, kind);
                else if (auto_state == 2) auto_fury = 0, auto_state = 1, auto_attack_set(w, u, id, kind);  // pays again
                else auto_energy_set(w, u, id, kind);
            }
        } else {
            // In reach: face the target (Unit_set_yaw_deg), drop the path, attack.
            w.set_yaw(u, std::atan2(t->pos.y - u.pos.y, t->pos.x - u.pos.x) / 0.017453292f);
            if (ai) ai->clear_path(u);
            const int cost = w.data().c.egatk_mp;
            if (auto_state == 1) {
                if (w.can_act(u)) attack_input(w, u, true, false, false);
            } else if (auto_state == 2) {
                // Fury: outside the combo window (+0x237 open / +0x23b close vs +0x97) one A, then back to state 1;
                // inside it a random follow-up (Misc_rand_unit_incl 0x590660: > .6 A, > .3 B, else none).
                const unsigned since = unsigned(w.now() - attack_ts);
                if (unsigned(u.win_close) < since || since <= unsigned(u.win_open)) {
                    if (u.action != ATTACK) {
                        if (auto_fury == 0) {
                            if (attack_input(w, u, true, false, false)) auto_fury = 1;
                        } else {
                            auto_fury = 0, auto_state = 1;
                        }
                    }
                } else {
                    const double r = double(float(rand_unit_incl(w.rng())));
                    const bool a = r > 0.6, b = !a && r > 0.3;
                    if (attack_input(w, u, a, b, false)) auto_fury = 1;
                }
            } else if (w.can_act(u) && u.ep >= cost) {  // state 3
                energy_attack(w, u, true);
                clear();
            } else if (w.can_act(u)) {
                attack_input(w, u, true, false, false);
                clear();
            }
        }
    } else if (auto_state == 4) {
        // 0x43b2d0: within the item's radius + 15 drop the path; a gate (class 1) gets the GATE_OPEN event 0x20 (the
        // AI's gate-open order route) and the state ends; other items keep state 4. A deleted item changes nothing.
        Unit& u = *up;
        if (const std::optional<AiItem> it = ai && ai->item ? ai->item(auto_target) : std::nullopt;
            it && dist3(it->pos, u.pos) < it->radius + 15.0f) {
            // DEVIATION (HYPOTHESIS): the original only clears the path (0x422f10); the port also stands a walking
            // hero, who would otherwise keep the RUN cycle past the item.
            if (is_locomotion(u.action)) ai->stop(u);
            else ai->clear_path(u);
            if (it->gate) {
                if (ai->on_gate_request) ai->on_gate_request(u.id, auto_target, 1);
                clear();
            }
        }
    }
    center_camera_step();  // 0x4384e0
}

}  // namespace joa::game
