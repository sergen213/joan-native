// Siege-weapon crews and carts, World side: crew animation mirroring and detach (Unit_set_action
// 0x501190 head), the vehicle movement gate (Unit_process_movement 0x52daa0 head) and the crew / cart
// driver placement after the physics step (Unit_update_location 0x4fee60, body sync 0x6b1350,
// Unit_follow_attach_yaw 0x454b20). Crew turning lives in World::turn_step (0x4f2190).
// Spec: GAMEPLAY_CHARACTER_COMBAT.md Part D §7.1; docs/GAMEPLAY_IMPLEMENTATION.md (siege crews).
#pragma once

namespace joa::game {

class World;
struct Unit;

// Unit_push_slots_full 0x50d550: the class has a `mask` slot and every such slot holds a crew recno.
bool siege_slots_full(const World& w, const Unit& v, int mask);
// Head of Unit_set_action 0x501190 (+ cart hook 0x459970); World::set_action calls it first.
void siege_on_set_action(World& w, Unit& u, int action);
// Head of Unit_process_movement 0x52daa0 for vehicles; phase 1, before Unit_move.
void siege_vehicle_move(World& w, Unit& u);
// Crew / driver part of Unit_die 0x7c6240 for a dying weapon or cart; World::unit_die calls it.
void siege_on_death(World& w, Unit& v, int killer);
// After the physics integrator: crews to their attach points, cart drivers onto their carts.
void siege_after_physics(World& w);

}  // namespace joa::game
