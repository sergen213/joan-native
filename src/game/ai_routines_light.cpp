// Native unit AI, classes 1-3 (human_light_infantry / human_light_general / special_general).
// Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md §A5.5, §A5.6, Part B §3A; bodies checked against
// work/ghidra/export/joan_decomp.c and the joan.exe disassembly (search/order push sequences).
#include <cmath>

#include "game/ai_routines.hpp"

namespace joa::game::ai {
namespace {

using Roll = bool (*)(AiSystem&, Unit&);

constexpr int kTurnMs = 2500;

void turn(AiSystem& s, Unit& u, int prio, int heading) {
    s.give_order(u, order::TURN, prio, false, heading, 0, 0, 0, kTurnMs);
}
void approach(AiSystem& s, Unit& u, bool team, int e, int d) {
    s.give_order(u, order::APPROACH_ENEMY, 1, team, e, d, d, 0, 30000);
}
void shift(AiSystem& s, Unit& u, int prio, int dir) { s.give_order(u, order::SHIFT, prio, false, dir, 0, 0, 0, 1500); }
void combo(AiSystem& s, Unit& u, const char* str, int prio, int p3) {
    s.give_order(u, order::COMBO_ATTACK, prio, false, AiSystem::combo_para(str, 1), 0, p3, 0, 1500);
}
void approach_place(AiSystem& s, Unit& u, int prio, bool team, int place) {
    s.give_order(u, order::APPROACH_PLACE, prio, team, place, 0, 0, 0, 30000);
}
void behave_high_priority(Unit& u) { u.attack_behaviour = NEAREST_HIGH_PRIORITY; }

// Search for the unit `recno` (a leader / followed unit); 0 when it no longer exists.
int target_of(AiSystem& s, int recno) {
    Unit* t = s.world().unit(recno);
    return t && !t->deleted ? s.search_for_unit(*t, "T") : 0;
}

// set_place_unit_view_angle(place, 0) (0x6547e0). The original writes the place record; an unknown
// place is left alone instead of inserting an empty AABB into the map.
void clear_view_angle(AiSystem& s, int place) {
    if (auto it = s.places.find(place); it != s.places.end()) it->second.view_angle = 0;
}

// Contact / near-contact enemy check shared by every routine: 150 -> ATTACK, else ACTIVE and a 200
// search that switches to ATTACK on rand(50)==0 (rand is drawn unconditionally).
int engage(AiSystem& s, Unit& u, const char* type = "E", int place = 0) {
    int e = s.search_for_unit(u, type, 150, false, place);
    if (e == 0) {
        s.set_mode(u, ACTIVE);
        e = s.search_for_unit(u, type, 200, false, place);
        const int r = s.rand(50);
        if (e != 0 && r == 0) s.set_mode(u, ATTACK);
    } else {
        s.set_mode(u, ATTACK);
    }
    return e;
}

// Half-range melee skirmish: c = enemy within range1/2 that is not an archer/crossbowman.
bool skirmish(AiSystem& s, Unit& u, int e, int c) {
    if (c <= 0) return false;
    const int cls = s.class_of(c);
    if (cls == 5 || cls == 6) return false;
    const int r = s.rand(10);
    if (r < 1) {
        approach(s, u, false, e, 60);
    } else {
        turn(s, u, 1, s.rotation_to(u, e));
        shift(s, u, 1, r < 5 ? 1 : r < 9 ? 2 : 3);
    }
    return true;
}

// Class-3 attack-mode guard: when e is winding up at us and we are not mid-combo (anim 0x1e..0x21).
bool guard_block(AiSystem& s, Unit& u, int e) {
    if (!s.will_be_attacked(u, e) || (u.action >= 0x1e && u.action <= 0x21)) return false;
    const int r = s.rand(200);
    if (s.attack_chance(u) < r) {
        if (s.rand(100) <= s.block_chance(u)) {
            turn(s, u, 3, s.rotation_to(u, e));
            s.give_order(u, order::BLOCK, 2, false, e, 0, 0, 0, 1000);
            return true;
        }
    } else {
        u.threat = 200;
    }
    return false;
}

// get_item_rotation(u, missile): the original missile is an ItemArray object; here it is a projectile id.
int missile_rotation(AiSystem& s, const Unit& u, int id) {
    for (const auto& p : s.world().projectiles())
        if (p.id == id) {
            const float k = World::kUnitsPerMetre;
            return int(u.yaw_deg - s.world().rel_angle(u, Vec3{p.pos_m.x * k, p.pos_m.y * k, p.pos_m.z * k}));
        }
    return 0;
}

// Class-3 idle-mode missile block (no return: the routine continues).
void missile_block(AiSystem& s, Unit& u) {
    const int m = s.nearest_incoming_missile(u, true, s.unit_range(u, 2));
    if (m == 0 || s.busy_attacking(u)) return;
    if (s.rand(100) <= s.block_chance(u)) {
        turn(s, u, 1, missile_rotation(s, u, m));
        s.give_order(u, order::BLOCK, 1, false, 0, 0, 0, 0, 2000);
    }
}

// 0x535c20 AI_infantry_melee_roll (class 1 attack roll)
bool roll_inf(AiSystem& s, Unit& u) {
    if (s.busy_attacking(u)) return false;
    const int r = s.rand(100);
    if (r < s.attack_chance(u)) {
        u.threat = s.base_attack_chance(u);
        const int c = s.rand(3);
        combo(s, u, c == 0 ? "A" : "AA", 2, 0);
        if (s.rand(100) < 90) shift(s, u, 1, 3);
    } else if (r >= 90) {
        shift(s, u, 2, s.rand(3) + 1);
    }
    return true;
}

// 0x538340 AI_melee_attack_roll_gen (class 2 attack roll)
bool roll_gen(AiSystem& s, Unit& u) {
    int r = s.rand(100);
    const bool down = s.can_attack_down(u);
    const bool fac = down && s.facing_knock_down_enemy(u);
    if (r < 60 && down && fac) {
        s.give_order(u, order::DOWN_ATTACK, 2, false, 0, 0, 0, 0, 1500);
    } else if (r < s.attack_chance(u)) {
        u.threat = s.base_attack_chance(u);
        r = s.rand(14);
        const char* str = r < 3 ? "A" : r < 5 ? "AA" : r < 6 ? "AAA" : r < 13 ? "AAB" : nullptr;
        if (str) {
            combo(s, u, str, 2, r < 3 ? 1 : r < 5 ? 2 : r < 6 ? 3 : 4);
            if (s.rand(100) < 90) shift(s, u, 1, 3);
        } else if (s.can_energy_attack(u)) {
            s.give_order(u, order::ENERGY_ATTACK, 2, false, 0, 0, 0, 0, 1500);
        }
    } else if (r > 89) {
        shift(s, u, 2, s.rand(3) + 1);
    }
    return true;
}

// 0x53abd0 AI_melee_attack_roll_special_gen (class 3 attack roll)
bool roll_special(AiSystem& s, Unit& u) {
    if (s.busy_attacking(u)) return false;
    World& w = s.world();
    int r = s.rand(100);
    const bool down = s.can_attack_down(u);
    const bool fac = down && s.facing_knock_down_enemy(u);
    if (r < 60 && down && fac) {
        s.give_order(u, order::DOWN_ATTACK, 2, false, 0, 0, 0, 0, 1500);
        return true;
    }
    if (r < 60 && s.chapter == 7 && w.data().units[std::size_t(u.cls)].code == "MARTINGOUGH") {
        w.set_ep(u, w.max_ep(u));
        if (s.can_rush_attack(u)) {
            s.give_order(u, order::RUSH_ATTACK, 2, false, 0, 0, 0, 0, 5000);
            return true;
        }
    }
    if (r >= s.attack_chance(u)) return true;
    bool pf = false;  // hostile to the player and not retreating
    const Unit* p = w.unit(w.player_id());
    if (p && !p->deleted && u.id != p->id && w.hostile_to_player(u) && (u.retreat == 0 || u.retreat == -2)) pf = true;
    int prio = 2;
    if (u.threat < (pf ? 100 : 120)) {
        r = s.rand(24);
    } else {
        const float mul = pf ? 1.3f : 1.0f;
        const int r2 = s.rand(200);
        if (float(r2) <= float(s.attack_chance(u)) * mul) r = 21, prio = 3;
    }
    u.threat = s.base_attack_chance(u);
    if (r < 5) combo(s, u, "A", prio, 1);
    else if (r < 9) combo(s, u, "AA", prio, 2);
    else if (r < 13) combo(s, u, "AAA", prio, 3);
    else if (r < 17) combo(s, u, "B", prio, 1);
    else if (r < 19) combo(s, u, "AAAA", prio, 4);
    else if (r < 21) combo(s, u, "AAAB", prio, 4);
    else if (r == 21) {
        if (s.can_energy_attack(u)) s.give_order(u, order::ENERGY_ATTACK, prio, false, 0, 0, 0, 0, 5000);
        else combo(s, u, "B", prio, 1);
    } else if (r < 24) {
        if (s.can_rush_attack(u)) s.give_order(u, order::RUSH_ATTACK, prio, false, 0, 0, 0, 0, 5000);
        else combo(s, u, "A", prio, 1);
    }
    return true;
}

// 0x534090 AI_infantry_default (class 1: tasks 0, 1, 9, 18, 10 with leader, default)
bool inf_default(AiSystem& s, Unit& u) {
    const int tgt = s.search_for_unit(u, "T");
    int lnear = 0, ltgt = 0;
    if (s.leader_recno(u) > 0) {
        lnear = s.search_for_unit(u, "L", 1000);
        if (lnear == 0) {
            s.give_order(u, order::FOLLOW_FORMATION, 2, true, 0, 0, 0, 0, 10000);
            s.set_target(u, 0);
            return true;
        }
        if (tgt == 0) s.give_order(u, order::FOLLOW_FORMATION, -2, true, 0, 0, 0, 0, 10000);
    }
    int e = engage(s, u);
    if (AiSystem::attack_mode(u)) {
        if (e == 0) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll_inf(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    if (lnear > 0) ltgt = target_of(s, lnear);
    behave_high_priority(u);
    const int r1 = s.unit_range(u, 1);
    e = s.search_for_unit(u, "E", s.curr_task_para(u, 1) == 0 ? r1 : r1 / 2, true);
    const int c = s.search_for_unit(u, "E", r1 / 2);
    if (skirmish(s, u, e, c)) return true;
    if (e > 0) return approach(s, u, false, e, 60), true;
    if ((lnear == 0 || ltgt > 0) && s.curr_task_para(u, 1) == 0) {
        behave_high_priority(u);
        e = s.search_for_unit(u, "E", s.unit_range(u, 2), true);
        if (e > 0) return approach(s, u, false, e, 60), true;
    }
    if (s.curr_task_para(u, 1) == 0 && tgt > 0 && s.unit_dist(u, tgt) <= s.unit_range(u, 3))
        return approach(s, u, true, tgt, 60), true;
    s.set_mode(u, IDLE_MODE);
    return true;
}

// 0x534880 AI_infantry_defend_place (class 1 task 6). The leading get_place_unit_view_angle read is
// discarded in the original and omitted here.
bool inf_defend_place(AiSystem& s, Unit& u) {
    const int place = s.curr_task_para(u, 1);
    const int tgt = s.search_for_unit(u, "T");
    int lnear = 0, ltgt = 0;
    if (s.leader_recno(u) > 0) {
        lnear = s.search_for_unit(u, "L", 1000);
        if (tgt == 0) s.give_order(u, order::FOLLOW_FORMATION, -2, true, 0, 0, 0, 0, 10000);
    }
    int e = engage(s, u, "EP", place);
    if (AiSystem::attack_mode(u)) {
        clear_view_angle(s, place);
        if (e == 0) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll_inf(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    if (lnear > 0) ltgt = target_of(s, lnear);
    behave_high_priority(u);
    const int r1 = s.unit_range(u, 1);
    e = s.search_for_unit(u, "EP", r1, true, place);
    const int c = s.search_for_unit(u, "EP", r1 / 2, false, place);
    if (skirmish(s, u, e, c)) return true;
    if (e > 0) return approach(s, u, false, e, 60), true;
    if (lnear == 0 || ltgt > 0) {
        behave_high_priority(u);
        e = s.search_for_unit(u, "EP", s.unit_range(u, 2), true, place);
        if (e > 0) return approach(s, u, false, e, 60), true;
    }
    if (tgt != 0 && s.inside_place(place, tgt) && s.unit_dist(u, tgt) <= s.unit_range(u, 3))
        return approach(s, u, true, tgt, 60), true;
    approach_place(s, u, -3, false, place);
    s.set_mode(u, IDLE_MODE);
    return true;
}

// 0x536390 AI_general_default (class 2: tasks 0, 3, 4)
bool gen_default(AiSystem& s, Unit& u) {
    const int tgt = s.search_for_unit(u, "T");
    int e = engage(s, u);
    if (AiSystem::attack_mode(u)) {
        if (e == 0) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll_gen(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    behave_high_priority(u);
    const int r1 = s.unit_range(u, 1);
    e = s.search_for_unit(u, "E", s.curr_task_para(u, 1) == 0 ? r1 : r1 / 2, true);
    const int c = s.search_for_unit(u, "E", r1 / 2);
    if (skirmish(s, u, e, c)) return true;
    if (e > 0) return approach(s, u, false, e, 60), true;
    if (s.curr_task_para(u, 1) == 0) {
        behave_high_priority(u);
        const int r2 = s.unit_range(u, 2);
        e = tgt == 0 ? s.search_for_unit(u, "E", r2, true, 0, 135) : s.search_for_unit(u, "E", r2, true);
        if (e > 0) return approach(s, u, false, e, 60), true;
        if (tgt != 0 && s.unit_dist(u, tgt) <= s.unit_range(u, 3)) return approach(s, u, true, tgt, 60), true;
    }
    return true;  // no IDLE mode switch in this routine (CONFIRMED disasm 0x536390)
}

// 0x536a40 AI_attack_place_gen (class 2 task 1)
bool gen_attack_place(AiSystem& s, Unit& u) {
    const int tgt = s.search_for_unit(u, "T");
    int e = engage(s, u);
    if (AiSystem::attack_mode(u)) {
        if (e == 0) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll_gen(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    behave_high_priority(u);
    const int r1 = s.unit_range(u, 1);
    e = s.search_for_unit(u, "E", r1, true);
    const int c = s.search_for_unit(u, "E", r1 / 2);
    if (skirmish(s, u, e, c)) return true;
    if (e > 0) return approach(s, u, false, e, 60), true;
    behave_high_priority(u);
    const int r2 = s.unit_range(u, 2);
    e = tgt == 0 ? s.search_for_unit(u, "E", r2, true, 0, 135) : s.search_for_unit(u, "E", r2, true);
    if (e > 0) return approach(s, u, false, e, 60), true;
    if (tgt != 0 && s.unit_dist(u, tgt) <= s.unit_range(u, 3)) return approach(s, u, true, tgt, 60), true;
    s.set_mode(u, IDLE_MODE);
    approach_place(s, u, 0, true, s.curr_task_para(u, 1));
    return true;
}

// 0x5370d0 AI_defend_place_gen (class 2 task 6)
bool gen_defend_place(AiSystem& s, Unit& u) {
    const int place = s.curr_task_para(u, 1);
    const int tgt = s.search_for_unit(u, "T");
    int e = engage(s, u, "EP", place);
    if (AiSystem::attack_mode(u)) {
        clear_view_angle(s, place);
        if (e == 0) return true;
        turn(s, u, 4, s.rotation_to(u, e));
        return roll_gen(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    behave_high_priority(u);
    const int r1 = s.unit_range(u, 1);
    e = s.search_for_unit(u, "EP", r1, true, place);
    const int c = s.search_for_unit(u, "EP", r1 / 2, false, place);
    if (skirmish(s, u, e, c)) return true;
    if (e > 0) return approach(s, u, false, e, 60), true;
    behave_high_priority(u);
    const int r2 = s.unit_range(u, 2);
    e = tgt == 0 ? s.search_for_unit(u, "EP", r2, true, place, 135) : s.search_for_unit(u, "EP", r2, true, place);
    if (e > 0) return approach(s, u, true, e, 60), true;
    if (tgt != 0 && s.inside_place(place, tgt) && s.unit_dist(u, tgt) <= s.unit_range(u, 3))
        return approach(s, u, true, tgt, 60), true;
    s.set_mode(u, IDLE_MODE);
    approach_place(s, u, 0, true, place);
    return true;
}

// FOLLOW_UNIT prelude shared by 0x5377b0 / 0x5392d0: true when the unit is too far and runs FOLLOW_UNIT.
bool follow_prelude(AiSystem& s, Unit& u, int f) {
    int keep = s.curr_task_para(u, 2);
    if (keep == 0) keep = 180;
    int d = 0;
    if (const Unit* t = s.world().unit(f); t && !t->deleted) {
        const float dx = u.pos.x - t->pos.x, dy = u.pos.y - t->pos.y;
        d = int(std::sqrt(double(dy * dy + dx * dx)));
    }
    const int fd = s.follow_unit_distance(f);
    if (d <= fd + 200) return false;
    s.give_order(u, order::FOLLOW_UNIT, 0, true, f, keep, fd, 0, 5000);
    s.set_target(u, 0);
    return true;
}

// 0x5377b0 AI_follow_unit_gen (class 2 tasks 10/18; class 1 task 10 without leader)
bool follow_gen(AiSystem& s, Unit& u) {
    const int f = s.curr_task_para(u, 1);
    if (follow_prelude(s, u, f)) return true;
    const int tgt = s.search_for_unit(u, "T");
    int e = engage(s, u);
    if (AiSystem::attack_mode(u)) {
        if (e == 0) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll_gen(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    const int ftgt = f != 0 ? target_of(s, f) : 0;
    behave_high_priority(u);
    const int r1 = s.unit_range(u, 1);
    e = s.search_for_unit(u, "E", r1, true);
    const int c = s.search_for_unit(u, "E", r1 / 2);
    if (skirmish(s, u, e, c)) return true;
    if (e > 0) return approach(s, u, false, e, 60), true;
    if (ftgt != 0) {
        behave_high_priority(u);
        const int r2 = s.unit_range(u, 2);
        e = tgt == 0 ? s.search_for_unit(u, "E", r2, false, 0, 135) : s.search_for_unit(u, "E", r2);
        if (e > 0) return approach(s, u, true, e, 70), true;
        if (tgt != 0 && s.unit_dist(u, tgt) <= s.unit_range(u, 3)) return approach(s, u, true, tgt, 70), true;
    }
    s.set_mode(u, IDLE_MODE);
    return true;
}

// 0x538c90 special-general default (class 3: tasks 0, 3, 4)
bool special_default(AiSystem& s, Unit& u) {
    const int tgt = s.search_for_unit(u, "T");
    int e = engage(s, u);
    if (AiSystem::attack_mode(u)) {
        if (e == 0) return true;
        if (guard_block(s, u, e)) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll_special(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    const int r1 = s.unit_range(u, 1);
    e = s.search_for_unit(u, "E", s.curr_task_para(u, 1) == 0 ? r1 : r1 / 2, true);
    if (e > 0) return approach(s, u, false, e, 60), true;
    missile_block(s, u);
    if (s.curr_task_para(u, 1) == 0) {
        behave_high_priority(u);
        e = s.search_for_unit(u, "E", s.unit_range(u, 2), true);
        if (e > 0) return approach(s, u, true, e, 60), true;
        if (tgt != 0) return approach(s, u, true, tgt, 60), true;
    }
    return true;
}

// 0x539a40 AI_attack_place_special_gen (class 3 task 1)
bool special_attack_place(AiSystem& s, Unit& u) {
    const int tgt = s.search_for_unit(u, "T");
    int e = engage(s, u);
    if (AiSystem::attack_mode(u)) {
        if (e == 0) return true;
        if (guard_block(s, u, e)) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll_special(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    missile_block(s, u);
    e = s.search_for_unit(u, "E", s.unit_range(u, 1), true);
    if (e > 0) return approach(s, u, false, e, 60), true;
    behave_high_priority(u);
    e = s.search_for_unit(u, "E", s.unit_range(u, 2), true);
    if (e > 0) return approach(s, u, true, e, 60), true;
    if (tgt != 0) return approach(s, u, true, tgt, 60), true;
    s.set_mode(u, IDLE_MODE);
    approach_place(s, u, 0, true, s.curr_task_para(u, 1));
    return true;
}

// 0x53a060 AI_defend_place_special_gen (class 3 task 6)
bool special_defend_place(AiSystem& s, Unit& u) {
    const int place = s.curr_task_para(u, 1);
    const int tgt = s.search_for_unit(u, "T");
    int e = engage(s, u, "EP", place);
    if (AiSystem::attack_mode(u)) {
        clear_view_angle(s, place);
        if (e == 0) return true;
        if (guard_block(s, u, e)) return true;
        turn(s, u, 4, s.rotation_to(u, e));
        return roll_special(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    missile_block(s, u);
    e = s.search_for_unit(u, "EP", s.unit_range(u, 1), true, place);
    if (e > 0) return approach(s, u, false, e, 60), true;
    behave_high_priority(u);
    e = s.search_for_unit(u, "EP", s.unit_range(u, 2), true, place);
    if (e > 0) return approach(s, u, true, e, 60), true;
    if (tgt != 0 && s.inside_place(place, tgt)) return approach(s, u, true, tgt, 60), true;
    s.set_mode(u, IDLE_MODE);
    approach_place(s, u, 0, true, place);
    return true;
}

// 0x5392d0 AI_follow_unit_special_gen (class 3 tasks 10/18)
bool follow_special(AiSystem& s, Unit& u) {
    const int f = s.curr_task_para(u, 1);
    if (follow_prelude(s, u, f)) return true;
    const int tgt = s.search_for_unit(u, "T");
    int e = engage(s, u);
    if (AiSystem::attack_mode(u)) {
        if (e == 0) return true;
        if (guard_block(s, u, e)) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll_special(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    missile_block(s, u);
    const int ftgt = f != 0 ? target_of(s, f) : 0;
    e = s.search_for_unit(u, "E", s.unit_range(u, 1), true);
    if (e != 0) return approach(s, u, false, e, 70), true;
    if (ftgt != 0) {
        behave_high_priority(u);
        e = s.search_for_unit(u, "E", s.unit_range(u, 2));
        if (e > 0) return approach(s, u, true, e, 70), true;
        if (tgt != 0) return approach(s, u, true, tgt, 70), true;
    }
    s.set_mode(u, IDLE_MODE);
    return true;
}

// 0x535870 / 0x537f90 / 0x53a6d0 RTS_ATTACK_UNIT (task 13); class 3 adds the guard block.
bool rts_attack_unit(AiSystem& s, Unit& u, Roll roll, bool guard) {
    const int t = s.curr_task_para(u, 1);
    if (s.is_attacked(u.id) || s.unit_dead(t)) return s.clear_task(u), true;
    if (s.unit_dist(u, t) > 199) return approach(s, u, false, t, 100), true;
    s.set_mode(u, ATTACK);
    if (AiSystem::attack_mode(u)) {
        if (guard && guard_block(s, u, t)) return true;
        turn(s, u, 4, s.rotation_to(u, t));
        return roll(s, u);
    }
    if (AiSystem::idle_mode(u)) approach(s, u, false, t, 100);
    return true;
}

// is_item_box 0x65b6a0: item exists and is a box (item+0x4e == 10).
bool item_box(AiSystem& s, int it) {
    const auto i = s.item ? s.item(it) : std::nullopt;
    return i && i->box;
}

// 0x535a20 / 0x538140 / 0x53a9d0 RTS_ATTACK_ITEM (task 17)
bool rts_attack_item(AiSystem& s, Unit& u, Roll roll) {
    const int it = s.curr_task_para(u, 1);
    if (s.is_attacked(u.id) || !item_box(s, it)) return s.clear_task(u), true;
    if (s.item_dist(u, it) > 149)
        return s.give_order(u, order::APPROACH_ITEM, 1, false, it, 100, 50, 0, 30000), true;
    s.set_mode(u, ATTACK);
    if (AiSystem::attack_mode(u)) {
        if (!item_box(s, it)) return s.clear_task(u), true;
        turn(s, u, 4, s.item_rotation(u, it));
        return roll(s, u);
    }
    if (AiSystem::idle_mode(u)) s.give_order(u, order::APPROACH_ITEM, 1, false, it, 100, 50, 0, 30000);
    return true;
}

// 0x535e30 / 0x538730 / 0x53b1b0 RTS_ATTACK_MOVE (task 19). Class 3 (special) adds the guard block and
// drops the NEAREST_HIGH_PRIORITY switch and the half-range skirmish.
bool rts_attack_move(AiSystem& s, Unit& u, Roll roll, bool special) {
    s.search_for_unit(u, "T");  // result unused in the original
    int e = engage(s, u);
    if (AiSystem::attack_mode(u) && e != 0) {
        if (special && guard_block(s, u, e)) return true;
        turn(s, u, 3, s.rotation_to(u, e));
        return roll(s, u);
    }
    if (!AiSystem::idle_mode(u)) return true;
    if (!special) behave_high_priority(u);
    const int r1 = s.unit_range(u, 1);
    e = s.search_for_unit(u, "E", r1, true);
    if (!special && skirmish(s, u, e, s.search_for_unit(u, "E", r1 / 2))) return true;
    if (e > 0) return approach(s, u, false, e, 60), true;
    const int x = s.curr_task_para(u, 1), y = s.curr_task_para(u, 2), z = s.curr_task_para(u, 3);
    s.give_order(u, order::MOVE_TO_TARGET, 0, false, x, y, z, 0, 30000);
    if (s.distance_to(u, Vec3{float(x), float(y), float(z)}) < 201) s.clear_task(u);
    s.set_mode(u, IDLE_MODE);
    return true;
}

// ---- LARGE_WEAPON_PUSH attach helpers (weapon crew/reserved per class attach point) ------------------

const UnitClass::AttachPoint* attach_point(AiSystem& s, const Unit& w, int i) {
    const auto& pts = s.world().data().units[std::size_t(w.cls)].attach_points;
    return i >= 0 && i < int(pts.size()) ? &pts[std::size_t(i)] : nullptr;
}

// World position of attach point i: weapon rotation (yaw about z) + translation (0x60bc50 + 0x60b...).
Vec3 attach_world(const Unit& w, const UnitClass::AttachPoint& a) {
    const float c = std::cos(w.yaw_rad()), sn = std::sin(w.yaw_rad());
    return {w.pos.x + a.x * c - a.y * sn, w.pos.y + a.x * sn + a.y * c, w.pos.z + a.z};
}

Unit* live(AiSystem& s, int recno) {
    Unit* t = s.world().unit(recno);
    return t && !t->deleted ? t : nullptr;
}

// get_nearest_attach_point 0x656dc0: nearest free point on `side`, reserving it (weapon reserved[i] = u).
int nearest_attach_point(AiSystem& s, Unit& u, int wid, char side) {
    Unit* w = live(s, wid);
    if (!w || w->m.can_attach == 0) return -1;
    const int mask = side == 'M' ? 1 : 2;
    const auto& pts = s.world().data().units[std::size_t(w->cls)].attach_points;
    int best = -1;
    float best_d = -1.0f;
    for (int i = 0; i < int(pts.size()); ++i) {
        const auto ui = std::size_t(i);
        if (!(pts[ui].usage & mask) || (ui < w->m.crew.size() && w->m.crew[ui] >= 1) ||
            (ui < w->m.reserved.size() && w->m.reserved[ui] >= 1))
            continue;
        const Vec3 p = attach_world(*w, pts[ui]);
        // HYPOTHESIS: the four ftol calls are (int)point then (int)(point - unit) on x/y.
        const int dx = int(float(int(p.x)) - u.pos.x), dy = int(float(int(p.y)) - u.pos.y);
        const float d = float(dx * dx + dy * dy);
        if (best_d == -1.0f || d < best_d) {
            if (best != -1 && std::size_t(best) < w->m.reserved.size()) w->m.reserved[std::size_t(best)] = 0;
            best = i;
            if (ui < w->m.reserved.size()) w->m.reserved[ui] = u.id;
            best_d = d;
        }
    }
    return best;
}

// attach_unit 0x657670(u, w, side, tolerance): attach when within tolerance and aligned (2 deg) with the
// point's facing, else snap the facing so the next think attaches.
void attach_unit(AiSystem& s, Unit& u, int wid, char side, int tol) {
    Unit* w = live(s, wid);
    if (!w) return;
    const int mask = side == 'M' ? 1 : 2;
    if (!is_idle_class(u.action)) return;  // Unit_anim_is_idle_class 0x508ab0
    World& world = s.world();
    const auto& pts = world.data().units[std::size_t(w->cls)].attach_points;
    for (int i = 0; i < int(pts.size()); ++i) {
        const auto ui = std::size_t(i);
        if (!(pts[ui].usage & mask) || (ui < w->m.crew.size() && w->m.crew[ui] >= 1)) continue;
        const Vec3 p = attach_world(*w, pts[ui]);
        const float dx = u.pos.x - p.x, dy = u.pos.y - p.y;
        if (dx * dx + dy * dy > float(tol * tol)) continue;
        const float facing = w->yaw_deg + pts[ui].dir;
        float diff = std::fmod(u.yaw_deg - facing, 360.0f);
        if (diff > 180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;
        // HYPOTHESIS: 0x577770 tests |heading difference| > 2 deg (Angle_set_degrees(2.0)).
        if (std::fabs(diff) <= 2.0f) {
            if (ui < w->m.crew.size()) w->m.crew[ui] = u.id;
            if (ui < w->m.reserved.size()) w->m.reserved[ui] = 0;
            u.m.vehicle = w->id;
            u.m.slot = i;
            world.set_yaw(u, facing);
            // HYPOTHESIS: the hand prop (attach point item, 0x50a480(item,2,0,0)) is visual only; skipped.
            switch (world.data().units[std::size_t(w->cls)].special) {
                case 4: u.special = 5; break;
                case 5: u.special = 6; break;
                case 6: u.special = 8; break;
                case 8: u.special = 9; break;
                case 9: u.special = 7; break;
                case 10: if (i <= 2) u.special = 10 + i; break;
                case 0xb: u.special = mask == 1 ? 0xd : pts[ui].y < 0.0f ? 0xf : 0xe; break;
                default: break;
            }
            world.set_action(u, u.action);
            return;
        }
        world.set_yaw(u, facing);
    }
}

// Unit_attach_point_dist 0x65ddb0: 2-D distance from u to its attached weapon point.
float attach_point_dist(AiSystem& s, const Unit& u) {
    const Unit* w = live(s, u.m.vehicle);
    if (!w) return -1.0f;
    const auto* a = attach_point(s, *w, u.m.slot);
    if (!a) return -1.0f;
    const Vec3 p = attach_world(*w, *a);
    const float dx = p.x - u.pos.x, dy = p.y - u.pos.y;
    return std::sqrt(dx * dx + dy * dy);
}

// detach_unit 0x657cc0(w, slot) for slot >= 0 (the only form 0x535020 uses).
void detach_unit(AiSystem& s, int wid, int slot) {
    Unit* w = live(s, wid);
    if (!w || !attach_point(s, *w, slot)) return;
    const auto ui = std::size_t(slot);
    if (Unit* p = ui < w->m.crew.size() ? live(s, w->m.crew[ui]) : nullptr) {
        p->m.vehicle = 0;
        p->m.slot = 0;
        s.st(*p).push_slot = -1;
        s.world().stop(*p);
        p->special = 0;
        // HYPOTHESIS: get_default_weapon (0x65b9b0) only re-attaches weapon props; skipped.
    }
    if (ui < w->m.crew.size()) w->m.crew[ui] = 0;
    if (ui < w->m.reserved.size()) w->m.reserved[ui] = 0;
}

// 0x535020 AI_large_weapon_push_inf (class 1 task 15). p1 = weapon, p2 = 0 'M' first else 'F' first.
bool large_weapon_push(AiSystem& s, Unit& u) {
    const int w = s.curr_task_para(u, 1);
    const int tol = s.world().data().c.reach_dt;  // DAT_00a3cc28 attach tolerance
    int& ps = s.st(u).push_slot;                    // +0x380 reserved attach point
    if (u.m.vehicle < 1) {                          // get_attaching_unit_recno 0x656af0
        const Unit* wu = live(s, w);
        if (!wu || wu->m.can_attach != 1) return true;  // can_attach 0x656b50
        const bool m_first = s.curr_task_para(u, 2) == 0;
        const char first = m_first ? 'M' : 'F', second = m_first ? 'F' : 'M';
        char side = first;
        int slot = ps;
        if (ps == -1) slot = ps = nearest_attach_point(s, u, w, first);
        if (slot < 0) {
            side = second;
            if (ps == -1) {
                slot = ps = nearest_attach_point(s, u, w, second);
                s.st(u).task.p2 = m_first ? 1 : 0;
            } else {
                slot = ps;
            }
            if (slot < 0) return true;
        }
        if (const auto* a = attach_point(s, *wu, slot)) {  // get_nearest_attach_point_x/y/z 0x657110..
            const Vec3 p = attach_world(*wu, *a);
            s.give_order(u, order::MOVE_TO_TARGET, 10, false, int(p.x), int(p.y), int(p.z), 0, 10000);
        } else {
            s.give_order(u, order::MOVE_TO_TARGET, 10, false, -1, -1, -1, 0, 10000);
        }
        attach_unit(s, u, w, side, tol);
        return true;
    }
    if (attach_point_dist(s, u) <= float(tol)) {
        if (s.unit_dead(u.m.vehicle)) {
            u.m.slot = 0;
            u.m.vehicle = 0;
            ps = -1;
            s.clear_task(u);
            // HYPOTHESIS: 0x50a480(0,0,0,0) + get_default_weapon only swap weapon props; skipped.
        }
    } else {
        detach_unit(s, w, u.m.slot);
    }
    s.stop_all_order(u);
    return true;
}

// Main-routine prologue (0x531f90 / 0x5322c0 / 0x532520): dead -> handled; attack behaviour NEAREST;
// when being hit, drop a passive IDLE posting (task 7, p1 == 0) on self and on an IDLE leader.
// Returns true when the routine must return 1 without task routing.
bool prologue(AiSystem& s, Unit& u, int& task) {
    if (s.world().dead(u)) return true;
    u.attack_behaviour = NEAREST;
    task = s.curr_task(u);
    if (!s.is_attacked(u.id)) return false;
    if (task == task::IDLE && s.curr_task_para(u, 1) == 0) {
        s.clear_task(u);
        if (const int l = s.leader_recno(u); l != 0)
            if (Unit* lu = s.world().unit(l); lu && s.curr_task(*lu) == task::IDLE) s.clear_task(*lu);
    }
    return true;
}

}  // namespace

// 0x531f90 AI_light_infantry (class 1)
bool light_infantry(AiSystem& s, Unit& u) {
    int t = 0;
    if (prologue(s, u, t)) return true;
    switch (t) {
        case task::RECRUIT_TROOP: case task::HELP_TROOP: case task::FLEE: case task::IDLE: return true;
        case task::DEFEND_PLACE: return inf_defend_place(s, u);
        case task::FOLLOW_UNIT: return s.leader_of(u) == 0 ? follow_gen(s, u) : inf_default(s, u);
        case task::RETREAT: return task_retreat(s, u);
        case task::RTS_MOVE_TO: return task_rts_move(s, u);
        case task::RTS_ATTACK_UNIT: return rts_attack_unit(s, u, roll_inf, false);
        case task::APPROACH_PLACE: return task_approach_place(s, u);
        case task::LARGE_WEAPON_PUSH: return large_weapon_push(s, u);
        case task::RTS_ATTACK_ITEM: return rts_attack_item(s, u, roll_inf);
        case task::RTS_ATTACK_MOVE: return rts_attack_move(s, u, roll_inf, false);
        case 101: case 102: case 103: case 104: case 105: return false;  // SPECIAL1..5 -> script fallback
        default: return inf_default(s, u);  // 0, 1, 9, 16, 18, other
    }
}

// 0x5322c0 AI_light_general (class 2)
bool light_general(AiSystem& s, Unit& u) {
    int t = 0;
    if (prologue(s, u, t)) return true;
    switch (t) {
        case task::NONE: case task::RECRUIT_TROOP: case task::HELP_TROOP: return gen_default(s, u);
        case task::ATTACK_PLACE: return gen_attack_place(s, u);
        case task::DEFEND_PLACE: return gen_defend_place(s, u);
        case task::FOLLOW_UNIT: case task::DEFEND_UNIT: return follow_gen(s, u);
        case task::RETREAT: return task_retreat(s, u);
        case task::RTS_MOVE_TO: return task_rts_move(s, u);
        case task::RTS_ATTACK_UNIT: return rts_attack_unit(s, u, roll_gen, false);
        case task::APPROACH_PLACE: return task_approach_place(s, u);
        case task::RTS_ATTACK_ITEM: return rts_attack_item(s, u, roll_gen);
        case task::RTS_ATTACK_MOVE: return rts_attack_move(s, u, roll_gen, false);
        default: return true;  // 5 FLEE, 7 IDLE, 9 SCOUT, 15, SPECIALn, other
    }
}

// 0x532520 AI_special_general (class 3)
bool special_general(AiSystem& s, Unit& u) {
    int t = 0;
    if (prologue(s, u, t)) return true;
    switch (t) {
        case task::NONE: case task::RECRUIT_TROOP: case task::HELP_TROOP: return special_default(s, u);
        case task::ATTACK_PLACE: return special_attack_place(s, u);
        case task::DEFEND_PLACE: return special_defend_place(s, u);
        case task::FOLLOW_UNIT: case task::DEFEND_UNIT: return follow_special(s, u);
        case task::RETREAT: return task_retreat(s, u);
        case task::RTS_MOVE_TO: return task_rts_move(s, u);
        case task::RTS_ATTACK_UNIT: return rts_attack_unit(s, u, roll_special, true);
        case task::APPROACH_PLACE: return task_approach_place(s, u);
        case task::RTS_ATTACK_ITEM: return rts_attack_item(s, u, roll_special);
        case task::RTS_ATTACK_MOVE: return rts_attack_move(s, u, roll_special, true);
        default: return true;  // 5 FLEE, 7 IDLE, 9 SCOUT, 15, SPECIALn, other
    }
}

// 0x546970 AI_after_block, classes 1-3
bool after_block_light(AiSystem& s, Unit& u) {
    switch (u.ai_class) {
        case 1: return roll_inf(s, u);
        case 2: return roll_gen(s, u);
        case 3: return roll_special(s, u);
        default: return false;
    }
}

}  // namespace joa::game::ai
