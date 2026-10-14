// Native unit AI, classes 8-15 (cannon .. pig) and the shared RETREAT / RTS_MOVE_TO / APPROACH_PLACE task
// routines. Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md Part B §3C, §A5.6; bodies checked against
// work/ghidra/export/joan_decomp.c and joan.exe disassembly. rand() call order matches the original.
#include "game/ai_routines.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>

namespace joa::game::ai {
namespace {

bool valid_unit(AiSystem& sys, int recno) { return sys.world().unit(recno) != nullptr; }  // !UnitArray_is_deleted

// x87 __ftol: truncate; NaN / out of range -> integer indefinite (0x80000000).
int ftol(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return std::numeric_limits<int>::min();
    return static_cast<int>(v);
}

bool is_box_gate_wall(AiSystem& sys, int item) {  // is_item_box 0x65b6a0 / is_item_gate 0x65b700 / is_item_wall 0x65b760
    auto it = sys.item(item);
    return it && (it->box || it->gate || it->wall);
}

bool is_gate(AiSystem& sys, int item) {
    auto it = sys.item(item);
    return it && it->gate;
}

// Item kind (+0x4e) and hit points (kind 1: vf4c()+0xc2, kind 4: vf58()+0xc6, others 999).
// HYPOTHESIS: kind 1 (structure with gate state) <=> get_gate_state != -1; kind 4 <=> non-gate wall/box;
// AiItem::hp carries the structure hit points for both.
int item_kind(AiSystem& sys, const AiItem& it, int item) {
    if (sys.gate_state(item) != -1) return 1;
    return it.wall || it.box ? 4 : 0;
}

// HYPOTHESIS: the 2500 range needs the weapon actor's model rec (+0x309 -> body +4 == 11, model array
// 0xa570a8), which is not decoded or modelled; every cannon uses the base 1600.
int cannon_range(const Unit&) { return 1600; }

// 0x544c40 AI_cannon_shoot_roll: 40% SHOOT.
bool cannon_shoot_roll(AiSystem& sys, Unit& u, int kind, int tgt) {
    if (sys.rand(10) < 4) sys.give_order(u, order::SHOOT, 2, false, kind, tgt, 0, 0, 1500);
    return true;
}

// 0x544e00 AI_autobow_shoot.
bool autobow_shoot(AiSystem& sys, Unit& u, int kind, int tgt) {
    sys.give_order(u, order::SHOOT, 2, false, kind, tgt, 0, 0, 1500);
    return true;
}

// 0x545d00 AI_trebuchet_shoot.
bool trebuchet_shoot(AiSystem& sys, Unit& u, int kind, int tgt) {
    sys.give_order(u, order::SHOOT, 2, false, kind, tgt, 0, 0, 5000);
    return true;
}

// 0x545380 AI_ram_attack_roll: 50% COMBO_ATTACK "A" when the front crew is attached.
bool ram_attack_roll(AiSystem& sys, Unit& u) {
    if (sys.all_attached(u, 'F') != 0 && sys.rand(10) < 5)
        sys.give_order(u, order::COMBO_ATTACK, 2, false, AiSystem::combo_para("A", 1), 0, 0, 0, 1500);
    return true;
}

// Fire block shared by 0x543800 / 0x543c10. true = the routine returned after ordering a shot.
bool cannon_fire(AiSystem& sys, Unit& u, int range) {
    int tgt = sys.search_for_unit(u, "S", range, false, 0, 0, 65);
    if (tgt == 0) tgt = sys.search_for_unit(u, "E", range, false, 0, 0, 65);
    const int item = sys.search_for_item(u, "EC", 3000, false);
    sys.set_mode(u, tgt != 0 || item != 0 ? ATTACK : ACTIVE);
    if (AiSystem::attack_mode(u)) {
        if (item != 0) {
            sys.give_order(u, order::TURN, 3, false, sys.item_rotation(u, item), 0, 0, 0, 2500);
            sys.give_order(u, order::SHOOT, 2, false, 2, item, 0, 0, 1500);
            return true;
        }
        if (tgt != 0) {
            if (!sys.line_of_fire_blocked(u, tgt)) {
                sys.give_order(u, order::TURN, 3, false, sys.rotation_to(u, tgt), 0, 0, 0, 2500);
                cannon_shoot_roll(sys, u, 1, tgt);
                return true;
            }
            sys.set_mode(u, ACTIVE);
        }
    }
    return false;
}

// 0x543800 AI_cannon_default (tasks 0, 1, 11).
bool cannon_default(AiSystem& sys, Unit& u) {
    sys.search_for_unit(u, "T");  // result unused
    if (sys.leader_recno(u) > 0 && sys.search_for_unit(u, "L", 1000) == 0) {
        sys.give_order(u, order::FOLLOW_FORMATION, 2, true, 0, 0, 0, 0, 10000);
        sys.set_target(u, 0);
        return true;
    }
    const int range = cannon_range(u);
    if (sys.all_attached(u, 'F') == 0) return true;
    if (cannon_fire(sys, u, range)) return true;
    if (AiSystem::idle_mode(u)) {
        int it = 0;
        if (sys.curr_task_para(u, 1) == 0 && (it = sys.search_for_item(u, "EC", 4000, false)) > 0)
            sys.give_order(u, order::APPROACH_ITEM, 1, false, it, 800, 400, 0, 30000);
        else
            sys.set_mode(u, IDLE_MODE);
    }
    return true;
}

// 0x543c10 AI_cannon_task19_move_fire.
bool cannon_task19(AiSystem& sys, Unit& u) {
    sys.search_for_unit(u, "T");  // result unused
    const int range = cannon_range(u);
    if (sys.all_attached(u, 'F') == 0) return true;
    if (cannon_fire(sys, u, range)) return true;
    if (AiSystem::idle_mode(u)) {
        const int x = sys.curr_task_para(u, 1), y = sys.curr_task_para(u, 2), z = sys.curr_task_para(u, 3);
        sys.give_order(u, order::MOVE_TO_TARGET, 0, false, x, y, z, 0, 30000);
        if (sys.distance_to(u, Vec3{float(x), float(y), float(z)}) < 201) sys.clear_task(u);
        sys.set_mode(u, IDLE_MODE);
    }
    return true;
}

// 0x5442d0 AI_cannon_rts_attack_unit (task 13).
bool cannon_rts_attack_unit(AiSystem& sys, Unit& u) {
    const int tgt = sys.curr_task_para(u, 1);
    if (sys.is_attacked(u.id) || !valid_unit(sys, tgt) || sys.unit_dead(tgt)) {
        sys.clear_task(u);
        return true;
    }
    if (sys.unit_dist(u, tgt) >= sys.unit_range(u, 2)) {
        sys.give_order(u, order::APPROACH_ENEMY, 1, false, tgt, sys.unit_range(u, 2) - 2000, 2000, 0, 30000);
        return true;
    }
    sys.set_mode(u, ATTACK);
    if (AiSystem::attack_mode(u)) {
        if (!sys.line_of_fire_blocked(u, tgt)) {
            sys.give_order(u, order::TURN, 4, false, sys.rotation_to(u, tgt), 0, 0, 0, 2500);
            return cannon_shoot_roll(sys, u, 1, tgt);
        }
        sys.set_mode(u, ACTIVE);
    }
    if (AiSystem::idle_mode(u))
        sys.give_order(u, order::APPROACH_ENEMY, 1, false, tgt, sys.unit_range(u, 2) - 2000, 2000, 0, 30000);
    return true;
}

// Angle quirk of 0x544690 (0x5448a6..0x544913): the relative bearing is converted to an Angle (turns,
// wrapped to [0,1)), the absolute yaw is subtracted again, result in degrees rounded half away from zero.
int cannon_item_aim(const Unit& u, int rot) {
    auto wrap = [](double t) { return t - std::floor(t); };
    const double a = wrap(rot / 360.0), b = wrap(u.yaw_rad() / (2.0 * std::numbers::pi));
    return static_cast<int>(std::lround(wrap(a - b) * 360.0));
}

// 0x544690 AI_cannon_rts_attack_item (task 17).
bool cannon_rts_attack_item(AiSystem& sys, Unit& u) {
    const int item = sys.curr_task_para(u, 1);
    auto it = sys.item(item);
    if (sys.is_attacked(u.id) || !it) {
        sys.clear_task(u);
        return true;
    }
    const int kind = item_kind(sys, *it, item);
    const int hp = kind == 1 || kind == 4 ? it->hp : 999;
    if (hp < 1) {
        sys.clear_task(u);
        return true;
    }
    const float half = float(it->size) / 2.0f;  // ItemRes(item+0xa)+0x24 footprint / 2
    if (sys.item_dist(u, item) < sys.unit_range(u, 2)) {
        sys.set_mode(u, ATTACK);
        if (AiSystem::attack_mode(u)) {
            if (!is_box_gate_wall(sys, item)) {
                sys.clear_task(u);
                return true;
            }
            const int rot = sys.item_rotation(u, item);
            const int d = cannon_item_aim(u, rot);
            if (d < 2 || d > 358)
                sys.give_order(u, order::SHOOT, 3, false, 2, item, 0, 0, 1500);
            else
                sys.give_order(u, order::TURN, 3, false, rot, 0, 0, 0, 2500);
        }
        if (AiSystem::idle_mode(u))
            sys.give_order(u, order::APPROACH_ITEM, 1, false, item, sys.unit_range(u, 2) + int(half), 400, 0, 30000);
    } else {
        sys.give_order(u, order::APPROACH_ITEM, 1, false, item, sys.unit_range(u, 2) + int(half), 400, 0, 30000);
    }
    return true;
}

// 0x544cb0 AI_autobow_default (task 0): fixed mount, +-10 deg cone, never turns.
bool autobow_default(AiSystem& sys, Unit& u) {
    u.attack_behaviour = NEAREST_ARCHER;
    const int tgt = sys.search_for_unit(u, "E", 3000, false, 0, 10, 65);
    sys.set_mode(u, tgt != 0 ? ATTACK : ACTIVE);
    if (AiSystem::attack_mode(u) && tgt != 0) {
        const int a = sys.rotation_to(u, tgt);  // get_angle_diff 0x652f80
        if (a < 10 && a > -10) return autobow_shoot(sys, u, 1, tgt);
        u.target = 0;
    }
    return true;
}

// 0x544e50 AI_batterram_move_attack (task 101): follow the track, ram the closed gate para3 within 300.
bool batterram_move_attack(AiSystem& sys, Unit& u) {
    const int gate = sys.curr_task_para(u, 3);
    sys.set_mode(u, IDLE_MODE);
    if (!is_gate(sys, gate) || sys.gate_state(gate) != 0) {
        sys.set_mode(u, ACTIVE);
        sys.give_order(u, order::MOVE_ON_TRACK, 1, false, sys.curr_task_para(u, 1), sys.curr_task_para(u, 2), 0, 0,
                       30000);
        return true;
    }
    if (sys.item_dist(u, gate) > 299) {
        if (sys.all_attached(u, 'M') != 0) {
            sys.set_mode(u, ACTIVE);
            sys.give_order(u, order::MOVE_ON_TRACK, 1, false, sys.curr_task_para(u, 1), sys.curr_task_para(u, 2), 0,
                           0, 30000);
        }
        return true;
    }
    if (sys.all_attached(u, 'M') == 0) {
        // HYPOTHESIS: get_item_bearing 0x65f0f0 (0x5059b0) == item_rotation (same signed relative bearing).
        const int b = sys.item_rotation(u, gate);
        if (b < -30 || b > 30) return true;
    } else {
        sys.give_order(u, order::TURN, 4, false, sys.item_rotation(u, gate), 0, 0, 0, 2500);
    }
    sys.set_mode(u, ATTACK);
    return ram_attack_roll(sys, u);
}

// 0x5450b0 AI_batterram_rts_attack_item (task 17, front crew attached).
bool batterram_rts_attack_item(AiSystem& sys, Unit& u) {
    const int item = sys.curr_task_para(u, 1);
    auto it = sys.item(item);
    if (!it) {
        sys.clear_task(u);
        return true;
    }
    sys.set_mode(u, IDLE_MODE);
    int hp = 999, reach = 0;
    const int kind = item_kind(sys, *it, item);
    if (kind == 1) {
        if (sys.gate_state(item) != 0) return true;  // open gate: task kept
        hp = it->hp;
        reach = 300;
    } else if (kind == 4) {
        hp = it->hp;
        reach = 400;
    }
    if (hp < 1) {
        sys.clear_task(u);
        return true;
    }
    if (sys.item_dist(u, item) < reach) {
        if (sys.all_attached(u, 'M') == 0) {
            const int b = sys.item_rotation(u, item);  // get_item_bearing 0x65f0f0, see above
            if (b < -30 || b > 30) return true;
        } else {
            sys.give_order(u, order::TURN, 4, false, sys.item_rotation(u, item), 0, 0, 0, 2500);
        }
        sys.set_mode(u, ATTACK);
        ram_attack_roll(sys, u);
    } else if (sys.all_attached(u, 'M') != 0) {
        sys.set_mode(u, ACTIVE);
        sys.give_order(u, order::APPROACH_ITEM, 1, false, item, 100, 100, 0, 30000);
    }
    return true;
}

// 0x545430 AI_cart_follow_unit / 0x545900 AI_citizen_follow_unit (task 10); only the duration differs.
bool follow_unit(AiSystem& sys, Unit& u, int duration_ms) {
    const int lead = sys.curr_task_para(u, 1);
    int keep = sys.curr_task_para(u, 2);
    if (keep == 0) keep = 180;
    const int d = sys.unit_dist(u, lead);
    const int cur = sys.search_for_unit(u, "T");
    if (d < 401) {
        if (cur == 0) sys.give_order(u, order::FOLLOW_UNIT, -1, false, lead, keep, 200, 0, duration_ms);
    } else {
        sys.give_order(u, order::FOLLOW_UNIT, 5, false, lead, keep, 200, 0, duration_ms);
        sys.set_target(u, 0);
    }
    return true;
}

// rand(200)-100, non-zero values pushed out by `push` (citizen 80, pig 50).
int rel_offset(AiSystem& sys, int push) {
    const int v = sys.rand(200) - 100;
    return v > 0 ? v + push : v < 0 ? v - push : 0;
}

// 0x545590 AI_citizen_none (task 0): flee from enemies within 600, else wander.
bool citizen_none(AiSystem& sys, Unit& u) {
    const int e = sys.search_for_unit(u, "E", 600);
    const Unit* en = e > 0 ? sys.world().unit(e) : nullptr;
    if (en) {
        // atan, not atan2 (direction sign lost) - original quirk.
        const float a = float(std::atan((double(u.pos.y) - double(en->pos.y)) / (double(u.pos.x) - double(en->pos.x))));
        int dz = sys.rand(400) - 400;
        const int r = sys.rand(400);
        const int dx = ftol(std::sin(double(a)) * double(r));
        const int dy = ftol(std::cos(double(a)) * double(r));
        if (dz > 0) dz += 100;
        if (dz < 0) dz -= 100;
        if (sys.rand(5) <= 3) {
            sys.stop_all_order(u);
            sys.give_order(u, order::MOVE_TO_RELATIVE_TARGET, -1, false, dx, dy, dz, 0, 2000);
        } else {
            sys.stop_all_order(u);
            sys.give_order(u, order::SPECIAL_MOVEMENT, 20, false, 1, 0, 1, 0, 2000);
        }
    } else if (sys.rand(100) >= 90) {
        const int ox = rel_offset(sys, 80);
        const int oy = rel_offset(sys, 80);
        const int oz = rel_offset(sys, 80);
        sys.give_order(u, order::MOVE_TO_RELATIVE_TARGET, -1, false, ox, oy, oz, 0, 5000);
    }
    return true;
}

// 0x545a60 AI_citizen_walk_in_area (task 101): para1/para2 centre, para3 half-size.
bool citizen_walk_in_area(AiSystem& sys, Unit& u) {
    if (sys.rand(100) >= 90) {
        const int x = sys.curr_task_para(u, 1), y = sys.curr_task_para(u, 2), r = sys.curr_task_para(u, 3);
        const int rx = sys.rand(r * 2);
        const int ry = sys.rand(r * 2);
        sys.give_order(u, order::MOVE_TO_TARGET, -1, false, x + (rx - r), y + (ry - r), 0, 0, 30000);
    }
    return true;
}

// 0x543fd0 AI_trebuchet_default (tasks 0, 11): no formation follow, no TURN.
bool trebuchet_default(AiSystem& sys, Unit& u) {
    sys.search_for_unit(u, "T");                                     // result unused
    if (sys.leader_recno(u) > 0) sys.search_for_unit(u, "L", 1000);  // result unused
    if (sys.all_attached(u, 'F') == 0) return true;
    int tgt = sys.search_for_unit(u, "S", 1600, false, 0, 0, 65);
    if (tgt == 0) tgt = sys.search_for_unit(u, "E", 1600, false, 0, 90, 65);
    const int item = sys.search_for_item(u, "EC", 3200, false);
    sys.set_mode(u, tgt != 0 || item != 0 ? ATTACK : ACTIVE);
    if (AiSystem::attack_mode(u)) {
        if (item != 0) {
            sys.give_order(u, order::SHOOT, 2, false, 2, item, 0, 0, 5000);
            return true;
        }
        if (tgt != 0) {
            if (!sys.line_of_fire_blocked(u, tgt)) {
                trebuchet_shoot(sys, u, 1, tgt);
                return true;
            }
            sys.set_mode(u, ACTIVE);
        }
    }
    if (AiSystem::idle_mode(u)) {
        int it = 0;
        if (sys.curr_task_para(u, 1) == 0 && (it = sys.search_for_item(u, "EC", 4000, false)) > 0)
            sys.give_order(u, order::APPROACH_ITEM, 1, false, it, 800, 400, 0, 30000);
        else
            sys.set_mode(u, IDLE_MODE);
    }
    return true;
}

// 0x545b80 AI_trebuchet_attack_place (task 1).
bool trebuchet_attack_place(AiSystem& sys, Unit& u) {
    const int place = sys.curr_task_para(u, 1);
    const int tgt = sys.search_for_unit(u, "EP", 16384, false, place, 0, 65);
    sys.set_mode(u, tgt != 0 ? ATTACK : ACTIVE);
    if (!AiSystem::attack_mode(u)) {
        if (AiSystem::idle_mode(u)) sys.set_mode(u, IDLE_MODE);
        return true;
    }
    if (tgt == 0) return true;
    sys.give_order(u, order::TURN, 3, false, sys.rotation_to(u, tgt), 0, 0, 0, 2500);
    return trebuchet_shoot(sys, u, 1, tgt);
}

// 0x544500 AI_trebuchet_rts_attack_unit (task 13): never approaches.
bool trebuchet_rts_attack_unit(AiSystem& sys, Unit& u) {
    const int tgt = sys.curr_task_para(u, 1);
    if (sys.is_attacked(u.id) || !valid_unit(sys, tgt) || sys.unit_dead(tgt)) {
        sys.clear_task(u);
        return true;
    }
    if (sys.unit_dist(u, tgt) >= sys.unit_range(u, 4) * 3) return true;
    sys.set_mode(u, ATTACK);
    if (AiSystem::attack_mode(u)) {
        if (!sys.line_of_fire_blocked(u, tgt)) {
            sys.give_order(u, order::TURN, 4, false, sys.rotation_to(u, tgt), 0, 0, 0, 2500);
            return trebuchet_shoot(sys, u, 1, tgt);
        }
        sys.set_mode(u, ACTIVE);
    }
    return true;
}

// 0x544aa0 AI_trebuchet_rts_attack_item (task 17): no hp check, out of range -> nothing.
bool trebuchet_rts_attack_item(AiSystem& sys, Unit& u) {
    const int item = sys.curr_task_para(u, 1);
    if (sys.is_attacked(u.id)) {
        sys.clear_task(u);
        return true;
    }
    if (sys.item_dist(u, item) < sys.unit_range(u, 4) * 3) {
        sys.set_mode(u, ATTACK);
        if (AiSystem::attack_mode(u)) {
            if (!is_box_gate_wall(sys, item)) {
                sys.clear_task(u);
            } else {
                sys.give_order(u, order::TURN, 4, false, sys.item_rotation(u, item), 0, 0, 0, 2500);
                sys.give_order(u, order::SHOOT, 3, false, 2, item, 0, 0, 1500);
            }
        }
    }
    return true;
}

// Pig scatter: MOVE_TO_RELATIVE_TARGET by +-[51..150] per axis (all three rand() before the anim change).
void pig_scatter(AiSystem& sys, Unit& u, bool camp) {
    const int ox = rel_offset(sys, 50);
    const int oy = rel_offset(sys, 50);
    const int oz = rel_offset(sys, 50);
    if (camp) u.special = 4;  // change_animation_special "CAMP" (AnimVariant_from_name 0x416aa0 -> 4)
    sys.give_order(u, order::MOVE_TO_RELATIVE_TARGET, -1, false, ox, oy, oz, 0, 5000);
}

}  // namespace

// 0x533170 CANNON (8)
bool cannon(AiSystem& sys, Unit& u) {
    u.attack_behaviour = WEAKEST;
    if (sys.world().dead(u) || sys.is_attacked(u.id)) return true;
    const int m = sys.all_attached(u, 'M');
    const int f = sys.all_attached(u, 'F');
    if (m == 0 && f == 0) {
        sys.stop_all_order(u);
        return true;
    }
    const int t = sys.curr_task(u);
    if (t == task::IDLE) return true;
    if (m != 0) {
        if (t == task::RTS_MOVE_TO) return task_rts_move(sys, u);
        if (t == task::APPROACH_PLACE) return task_approach_place(sys, u);
        if (t == task::RTS_ATTACK_MOVE) return cannon_task19(sys, u);
    }
    if (f != 0) {
        if (t == task::NONE || t == task::RETREAT || t == task::ATTACK_PLACE) return cannon_default(sys, u);
        if (t == task::RTS_ATTACK_UNIT) return cannon_rts_attack_unit(sys, u);
        if (t == task::RTS_ATTACK_ITEM) return cannon_rts_attack_item(sys, u);
    }
    return true;
}

// 0x533370 AUTOBOW (9)
bool autobow(AiSystem& sys, Unit& u) {
    u.attack_behaviour = WEAKEST;
    if (sys.world().dead(u) || sys.is_attacked(u.id)) return true;
    if (sys.curr_task(u) == task::NONE) return autobow_default(sys, u);
    return true;
}

// 0x533440 BATTERRAM (10)
bool batterram(AiSystem& sys, Unit& u) {
    u.attack_behaviour = WEAKEST;
    if (u.action == DIE || sys.is_attacked(u.id)) return true;  // is_died 0x653c60
    const int m = sys.all_attached(u, 'M');
    const int f = sys.all_attached(u, 'F');
    if (m == 0 && f == 0) {
        sys.stop_all_order(u);
        return true;
    }
    const int t = sys.curr_task(u);
    if (t == task::RTS_MOVE_TO || t == task::RTS_ATTACK_MOVE) return m != 0 ? task_rts_move(sys, u) : true;
    if (t == task::RTS_ATTACK_ITEM && f != 0) return batterram_rts_attack_item(sys, u);
    if (t == task::SPECIAL + 1) return batterram_move_attack(sys, u);
    return true;
}

// 0x5335e0 CAMP_HORSE (11)
bool camp_horse(AiSystem& sys, Unit& u) {
    u.attack_behaviour = NEAREST;
    if (u.action != DIE && !sys.is_attacked(u.id) && sys.search_for_unit(u, "E", 100) != 0 && sys.rand(100) > 70)
        sys.give_order(u, order::SPECIAL_MOVEMENT, 7, false, 2, 0, 1, 0, 10000);
    return true;
}

// 0x5336e0 CART (12)
bool cart(AiSystem& sys, Unit& u) {
    u.attack_behaviour = NEAREST;
    u.special = 0;  // change_animation_special "DEFAULT"
    // `if (!unit+0x2e && get_cut_scene_flag()) return 1;` is unreachable here: during a cut scene the unit
    // loop only runs phase 3 (think) for units with +0x2e (cutscene_process) set (UnitArray_process
    // cut-scene branch 0x4301c0, GAMEPLAY_AI_TROOPS.md). HYPOTHESIS: World exposes no cut-scene flag; omitted.
    if (sys.world().dead(u)) return true;
    switch (sys.curr_task(u)) {
    case task::FOLLOW_UNIT: return follow_unit(sys, u, 5000);
    case task::APPROACH_PLACE: return task_approach_place(sys, u);
    case task::RTS_MOVE_TO: return task_rts_move(sys, u);
    default: return true;  // 0, 7 and others: nothing
    }
}

// 0x533810 CITIZEN (13)
bool citizen(AiSystem& sys, Unit& u) {
    u.attack_behaviour = NEAREST;
    u.special = 0;  // change_animation_special "DEFAULT"
    if (sys.world().dead(u)) return true;
    const int t = sys.curr_task(u);
    if (t == task::NONE) return citizen_none(sys, u);
    if (t == task::IDLE) return true;
    if (t == task::FOLLOW_UNIT) follow_unit(sys, u, 20000);  // falls through, returns 1
    if (t == task::APPROACH_PLACE) return task_approach_place(sys, u);
    if (t == task::SPECIAL + 1) return citizen_walk_in_area(sys, u);
    return true;
}

// 0x533920 TREBUCHET (14)
bool trebuchet(AiSystem& sys, Unit& u) {
    u.attack_behaviour = WEAKEST;
    if (sys.world().dead(u) || sys.is_attacked(u.id)) return true;
    const int m = sys.all_attached(u, 'M');
    const int f = sys.all_attached(u, 'F');
    if (m == 0 && f == 0) {
        sys.stop_all_order(u);
        return true;
    }
    const int t = sys.curr_task(u);
    if (t == task::IDLE) return true;
    if (m != 0) {
        if (t == task::RTS_MOVE_TO || t == task::RTS_ATTACK_MOVE) return task_rts_move(sys, u);
        if (t == task::APPROACH_PLACE) return task_approach_place(sys, u);
    }
    if (f != 0) {
        if (t == task::NONE || t == task::RETREAT) return trebuchet_default(sys, u);
        if (t == task::ATTACK_PLACE) return trebuchet_attack_place(sys, u);
        if (t == task::RTS_ATTACK_UNIT) return trebuchet_rts_attack_unit(sys, u);
        if (t == task::RTS_ATTACK_ITEM) return trebuchet_rts_attack_item(sys, u);
    }
    return true;
}

// 0x533b10 PIG (15)
bool pig(AiSystem& sys, Unit& u) {
    u.attack_behaviour = NEAREST;
    u.special = 0;  // change_animation_special "DEFAULT"
    if (sys.world().dead(u)) return true;
    if (sys.is_attacked(u.id)) {
        pig_scatter(sys, u, true);
    } else if (sys.search_for_unit(u, "E", 100) != 0 && sys.rand(100) > 80) {
        pig_scatter(sys, u, true);
    } else if (sys.search_for_unit(u, "F", 100) != 0 && sys.rand(100) > 95) {
        pig_scatter(sys, u, true);
    } else if (sys.rand(100) >= 80) {
        pig_scatter(sys, u, false);
    }
    return true;
}

// 0x545d50 AI_task_retreat (RETREAT 11): run to place para1; inside it or para3 timer <= 0 -> escaped.
bool task_retreat(AiSystem& sys, Unit& u) {
    const int place = sys.curr_task_para(u, 1);
    const int timer = sys.curr_task_para(u, 3);
    sys.give_order(u, order::APPROACH_PLACE, 99, true, place, 0, 0, 0, 30000);
    if (sys.inside_place(place, u.id) || timer < 1) {
        sys.set_unit_dead(u);                                   // 0x656410
        sys.world().stop(u);                                    // set_idle 0x657610 -> Unit_stop(0,1)
        sys.clear_task(u);                                      // stop_curr_task
        sys.give_task(u, task::RETREAT, 99, 0, 0, 0, 0);
    }
    return true;
}

// 0x545ea0 AI_task_rts_move_to (RTS_MOVE_TO 12): walk to (para1..3); done within the arrival radius.
bool task_rts_move(AiSystem& sys, Unit& u) {
    if (sys.is_attacked(u.id)) {
        sys.clear_task(u);
        return true;
    }
    const int x = sys.curr_task_para(u, 1), y = sys.curr_task_para(u, 2), z = sys.curr_task_para(u, 3);
    sys.give_order(u, order::MOVE_TO_TARGET, 99, false, x, y, z, 0, 20000);
    int reach = sys.world().data().c.reach_dt;  // REACH_DT 0xa3cc28
    if (u.mount != 0 || sys.class_of(u) == 8 || u.lod_tier > 1) reach *= 5;
    const double r = float(reach);
    const double dx = double(x) - u.pos.x, dy = double(y) - u.pos.y;
    if (std::abs(ftol(double(z) - u.pos.z)) < 31 && dx * dx + dy * dy <= r * r) sys.clear_task(u);
    return true;
}

// 0x546050 AI_task_approach_place (APPROACH_PLACE 14): para1 place, para2 = team flag (disasm 0x546065..).
bool task_approach_place(AiSystem& sys, Unit& u) {
    const int place = sys.curr_task_para(u, 1);
    const int team = sys.curr_task_para(u, 2);
    sys.give_order(u, order::APPROACH_PLACE, 1, team != 0, place, 0, 0, 0, 3000);
    return true;
}

// 0x546970 AI_after_block, siege cases (kind 1, target 0 as pushed by the dispatcher).
bool after_block_siege(AiSystem& sys, Unit& u) {
    switch (u.ai_class) {
    case 8: return cannon_shoot_roll(sys, u, 1, 0);   // 0x544c40
    case 9: return autobow_shoot(sys, u, 1, 0);       // 0x544e00
    case 10: return ram_attack_roll(sys, u);          // 0x545380
    case 14: return trebuchet_shoot(sys, u, 1, 0);    // 0x545d00
    default: return false;
    }
}

}  // namespace joa::game::ai
