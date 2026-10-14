// Native unit AI classes 4-7 (heavy_inf, heavy_gen, archer, archer_gen): main routines 0x532780 / 0x532a00 /
// 0x532c60 / 0x532ef0 and their task routines. Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md Part B §3B;
// constants and call order checked against work/ghidra/export/joan_decomp.c and the joan.exe disassembly.
#include <cmath>
#include <string_view>

#include "game/ai_routines.hpp"

namespace joa::game::ai {
namespace {

constexpr int kTurnMs = 2500, kBlockMs = 1000, kShiftMs = 1500, kComboMs = 1500, kGenMs = 1000, kShootMs = 1500;
constexpr int kApproachMs = 30000, kFollowMs = 5000, kFormationMs = 10000;
constexpr int kMaxPitch = 65;          // archer target elevation limit (0x41)
constexpr int kFov = 135;              // heavy_inf leader-centred aggro cone (0x87)
constexpr int kArrive = 201;           // task 19 arrival (0xc9: dist < 201)

using Roll = bool (*)(AiSystem&, Unit&);

void turn(AiSystem& s, Unit& u, int prio, int tgt) {
    s.give_order(u, order::TURN, prio, false, s.rotation_to(u, tgt), 0, 0, 0, kTurnMs);
}
void turn_item(AiSystem& s, Unit& u, int prio, int item) {
    s.give_order(u, order::TURN, prio, false, s.item_rotation(u, item), 0, 0, 0, kTurnMs);
}
void approach(AiSystem& s, Unit& u, bool team, int tgt, int p2, int p3) {
    s.give_order(u, order::APPROACH_ENEMY, 1, team, tgt, p2, p3, 0, kApproachMs);
}
void approach_place(AiSystem& s, Unit& u, int prio, bool team, int place) {
    s.give_order(u, order::APPROACH_PLACE, prio, team, place, 0, 0, 0, kApproachMs);
}
void set_mode(AiSystem& s, Unit& u, Mode m) { s.set_mode(u, m); }

// search_for_unit(other, "T"): the current target of another unit (0 if it is gone).
int target_of(AiSystem& s, int recno) {
    Unit* o = s.world().unit(recno);
    return o && !o->deleted ? s.search_for_unit(*o, "T") : 0;
}

bool type_is(AiSystem& s, const Unit& u, std::string_view code) {   // strcmp(type+4, code) == 0
    const auto& units = s.world().data().units;
    return u.cls >= 0 && static_cast<std::size_t>(u.cls) < units.size() && units[u.cls].code == code;
}

bool is_item_box(AiSystem& s, int item) {   // FUNC 22 0x65b6a0
    auto it = s.item(item);
    return it && it->box;
}

// ENGLISH_EX_ARCHER(_CAPTAIN) barrel/gate targets: closed enemy gate first, then "EK".
int archer_item(AiSystem& s, Unit& u, std::string_view code, int radius) {
    if (!type_is(s, u, code)) return 0;
    int it = s.search_for_item(u, "EGC", radius);
    return it ? it : s.search_for_item(u, "EK", radius);
}

// Horizontal follow distance (+0x0e/+0x12, float products, sqrt in double, ftol); 0 when F is gone.
int follow_dist(AiSystem& s, const Unit& u, int f) {
    const Unit* o = s.world().unit(f);
    if (!o || o->deleted) return 0;
    float dy = u.pos.y - o->pos.y, dx = u.pos.x - o->pos.x;
    return static_cast<int>(std::sqrt(static_cast<double>(dy * dy + dx * dx)));
}

// 0x53cb10 heavy_inf melee roll (infantry 0x535c20 with rand(5) and the "B" blow).
bool heavyinf_roll(AiSystem& s, Unit& u) {
    if (s.busy_attacking(u)) return false;
    int r = s.rand(100);
    if (r < s.attack_chance(u)) {
        u.threat = s.base_attack_chance(u);
        int k = s.rand(5);
        const char* c = k == 0 ? "A" : k < 3 ? "AA" : "B";
        s.give_order(u, order::COMBO_ATTACK, 2, false, AiSystem::combo_para(c, 1), 0, 0, 0, kComboMs);
        if (s.rand(100) < 90) s.give_order(u, order::SHIFT, 1, false, 3, 0, 0, 0, kShiftMs);
    } else if (r >= 90) {
        s.give_order(u, order::SHIFT, 2, false, s.rand(3) + 1, 0, 0, 0, kShiftMs);
    }
    return true;
}

// 0x53f670 heavy_gen melee roll (no busy-anim check, 1000 ms orders).
bool heavygen_roll(AiSystem& s, Unit& u) {
    int r = s.rand(100);
    bool down = s.can_attack_down(u);
    bool fac = down && s.facing_knock_down_enemy(u);
    if (r < 60 && down && fac) {
        s.give_order(u, order::DOWN_ATTACK, 2, false, 0, 0, 0, 0, kGenMs);
    } else if (r < s.attack_chance(u)) {
        u.threat = s.base_attack_chance(u);
        int k = s.rand(14);
        const char* c = nullptr;
        int p3 = 1;
        if (k < 3) c = "A";
        else if (k < 5) c = "AA", p3 = 2;
        else if (k < 6) c = "AAA", p3 = 3;
        else if (k < 13) c = "B";
        if (c) {
            s.give_order(u, order::COMBO_ATTACK, 2, false, AiSystem::combo_para(c, 1), 0, p3, 0, kGenMs);
            if (s.rand(100) < 90) s.give_order(u, order::SHIFT, 1, false, 3, 0, 0, 0, kGenMs);
        } else if (s.can_energy_attack(u)) {
            s.give_order(u, order::ENERGY_ATTACK, 2, false, 0, 0, 0, 0, kGenMs);
        }
    } else if (r > 89) {
        s.give_order(u, order::SHIFT, 2, false, s.rand(3) + 1, 0, 0, 0, kGenMs);
    }
    return true;
}

// 0x541270 / 0x5432a0 (byte-identical): kind 1 = unit, 2 = item.
void shoot_roll(AiSystem& s, Unit& u, int kind, int tgt) {
    if (s.rand(10) < s.attack_chance(u)) {
        u.threat = s.base_attack_chance(u);
        s.give_order(u, order::SHOOT, 2, false, kind, tgt, 0, 0, kShootMs);
    }
}

// §2.1 engage prologue (mode selection). rand(50) is drawn whenever the first probe misses.
int engage(AiSystem& s, Unit& u, std::string_view type, int r1, int r2, int place = 0) {
    int e = s.search_for_unit(u, type, r1, false, place);
    if (e) {
        set_mode(s, u, ATTACK);
    } else {
        set_mode(s, u, ACTIVE);
        e = s.search_for_unit(u, type, r2, false, place);
        int r = s.rand(50);
        if (e && r == 0) set_mode(s, u, ATTACK);
    }
    return e;
}

// §2.1 attack-mode tail: block reaction, else TURN + melee roll.
bool strike(AiSystem& s, Unit& u, int e, int block_prio, int turn_prio, Roll roll) {
    if (s.will_be_attacked(u, e) && s.rand(100) <= s.block_chance(u)) {
        turn(s, u, block_prio, e);
        s.give_order(u, order::BLOCK, block_prio, false, e, 0, 0, 0, kBlockMs);
        return true;
    }
    turn(s, u, turn_prio, e);
    return roll(s, u);
}

// §2.2 near skirmish: a close non-archer enemy N -> 10% charge A, else TURN + side-step 1/2/3; A -> charge.
bool skirmish(AiSystem& s, Unit& u, int a, int n) {
    if (n > 0) {
        int c = s.class_of(n);
        if (c != 5 && c != 6) {
            int r = s.rand(10);
            if (r < 1) {
                approach(s, u, false, a, 60, 60);
            } else {
                turn(s, u, 1, a);
                s.give_order(u, order::SHIFT, 1, false, r < 5 ? 1 : r < 9 ? 2 : 3, 0, 0, 0, kShiftMs);
            }
            return true;
        }
    }
    if (a > 0) {
        approach(s, u, false, a, 60, 60);
        return true;
    }
    return false;
}

// Archer attack-mode fire: unit target if the line of fire is clear (else drop to ACTIVE), then item.
bool fire(AiSystem& s, Unit& u, int e, int item) {
    if (e) {
        if (!s.line_of_fire_blocked(u, e)) {
            turn(s, u, 3, e);
            shoot_roll(s, u, 1, e);
            return true;
        }
        set_mode(s, u, ACTIVE);
    }
    if (item) {
        turn_item(s, u, 3, item);
        shoot_roll(s, u, 2, item);
        return true;
    }
    return false;
}

void backoff(AiSystem& s, Unit& u, int e1) { approach(s, u, false, e1, s.unit_range(u, 1), 400); }
void close_in(AiSystem& s, Unit& u, bool team, int tgt) { approach(s, u, team, tgt, s.unit_range(u, 2) - 400, 400); }

// Shared leader prologue of 0x53b610 / 0x540070: regroup when the leader is beyond 1000.
// Returns -1 when handled (return 1), else the "L" search result.
int leader_block(AiSystem& s, Unit& u, int t) {
    if (s.leader_recno(u) <= 0) return 0;
    int l = s.search_for_unit(u, "L", 1000);
    if (!l) {
        s.give_order(u, order::FOLLOW_FORMATION, 2, true, 0, 0, 0, 0, kFormationMs);
        s.set_target(u, 0);
        return -1;
    }
    if (!t) s.give_order(u, order::FOLLOW_FORMATION, -2, true, 0, 0, 0, 0, kFormationMs);
    return l;
}

// ---- heavy infantry (class 4) -----------------------------------------------------------------

// 0x53b610 heavy_inf default (tasks 0/1/8/9/18/default, 10 in a team).
bool heavyinf_default(AiSystem& s, Unit& u) {
    int t = s.search_for_unit(u, "T");
    int l = leader_block(s, u, t);
    if (l < 0) return true;
    int e = engage(s, u, "E", 100, 150);
    if (s.attack_mode(u)) return e ? strike(s, u, e, 4, 3, heavyinf_roll) : true;
    if (!s.idle_mode(u)) return true;
    int lt = l > 0 ? target_of(s, l) : 0;
    u.attack_behaviour = NEAREST_HIGH_PRIORITY;
    int r1 = s.unit_range(u, 1);
    int a = s.search_for_unit(u, "E", s.curr_task_para(u, 1) == 0 ? r1 : r1 / 2, true);
    int n = s.search_for_unit(u, "E", s.unit_range(u, 1) / 2);
    if (skirmish(s, u, a, n)) return true;
    if ((l == 0 || lt > 0) && s.curr_task_para(u, 1) == 0) {
        u.attack_behaviour = NEAREST_HIGH_PRIORITY;
        a = s.search_for_unit(u, "E", s.unit_range(u, 2), true, 0, t == 0 ? kFov : 0);
        if (a > 0) {
            approach(s, u, false, a, 60, 60);
            return true;
        }
    }
    if (s.curr_task_para(u, 1) == 0 && t > 0 && s.unit_dist(u, t) <= s.unit_range(u, 3)) {
        approach(s, u, true, t, 60, 60);
        return true;
    }
    set_mode(s, u, IDLE_MODE);
    return true;
}

// 0x53beb0 heavy_inf DEFEND_PLACE.
bool heavyinf_defend_place(AiSystem& s, Unit& u) {
    int place = s.curr_task_para(u, 1);
    int t = s.search_for_unit(u, "T");
    int l = 0;
    if (s.leader_recno(u) > 0) {
        l = s.search_for_unit(u, "L", 1000);
        if (!t) s.give_order(u, order::FOLLOW_FORMATION, -2, true, 0, 0, 0, 0, kFormationMs);
    }
    int e = engage(s, u, "EP", 100, 150, place);
    if (s.attack_mode(u)) {
        s.set_place_unit_view_angle(place, 0);
        return e ? strike(s, u, e, 4, 3, heavyinf_roll) : true;
    }
    if (!s.idle_mode(u)) return true;
    int lt = l > 0 ? target_of(s, l) : 0;
    u.attack_behaviour = NEAREST_HIGH_PRIORITY;
    int a = s.search_for_unit(u, "EP", s.unit_range(u, 1), true, place);
    int n = s.search_for_unit(u, "EP", s.unit_range(u, 1) / 2, false, place);
    if (skirmish(s, u, a, n)) return true;
    if (l == 0 || lt > 0) {
        u.attack_behaviour = NEAREST_HIGH_PRIORITY;
        a = s.search_for_unit(u, "EP", s.unit_range(u, 2), true, place, t == 0 ? kFov : 0);
        if (a > 0) {
            approach(s, u, false, a, 60, 60);
            return true;
        }
    }
    if (t && s.inside_place(place, t) && s.unit_dist(u, t) <= s.unit_range(u, 3)) {
        approach(s, u, true, t, 60, 60);
        return true;
    }
    approach_place(s, u, -3, false, place);
    set_mode(s, u, IDLE_MODE);
    return true;
}

// 0x53c700 heavy_inf / 0x53f260 heavy_gen RTS_ATTACK_UNIT (block prio 4 / 5).
bool melee_rts_attack_unit(AiSystem& s, Unit& u, int block_prio, Roll roll) {
    int t = s.curr_task_para(u, 1);
    if (s.is_attacked(u.id) || s.unit_dead(t)) {
        s.clear_task(u);
        return true;
    }
    if (s.unit_dist(u, t) >= 150) {
        approach(s, u, false, t, 100, 50);
        return true;
    }
    set_mode(s, u, ATTACK);   // attack mode is now certain; the idle re-approach branch is unreachable
    return strike(s, u, t, block_prio, 4, roll);
}

// 0x53c960 heavy_inf / 0x53f4c0 heavy_gen RTS_ATTACK_ITEM.
bool melee_rts_attack_item(AiSystem& s, Unit& u, Roll roll) {
    int it = s.curr_task_para(u, 1);
    if (s.is_attacked(u.id)) {
        s.clear_task(u);
        return true;
    }
    if (s.item_dist(u, it) >= 150) {
        s.give_order(u, order::APPROACH_ITEM, 1, false, it, 100, 50, 0, kApproachMs);
        return true;
    }
    set_mode(s, u, ATTACK);
    if (!is_item_box(s, it)) {
        s.clear_task(u);
        return true;
    }
    turn_item(s, u, 4, it);
    return roll(s, u);
}

// 0x53cda0 heavy_inf / 0x53fa60 heavy_gen task 19 attack-move. Both call the infantry roll 0x53cb10.
// Engage radii are 150/200 here (pushes 0x96 / 200 at 0x53ce06, decomp 0x53fa60), not the doc's 100/150.
bool melee_attack_move(AiSystem& s, Unit& u) {
    s.search_for_unit(u, "T");
    int e = engage(s, u, "E", 150, 200);
    if (s.attack_mode(u) && e) return strike(s, u, e, 4, 3, heavyinf_roll);
    if (s.idle_mode(u)) {
        u.attack_behaviour = NEAREST_HIGH_PRIORITY;
        int a = s.search_for_unit(u, "E", s.unit_range(u, 1), true);
        int n = s.search_for_unit(u, "E", s.unit_range(u, 1) / 2);
        if (skirmish(s, u, a, n)) return true;
        int x = s.curr_task_para(u, 1), y = s.curr_task_para(u, 2), z = s.curr_task_para(u, 3);
        s.give_order(u, order::MOVE_TO_TARGET, 0, false, x, y, z, 0, kApproachMs);
        Vec3 dest{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        if (s.distance_to(u, dest) < kArrive) s.clear_task(u);
        set_mode(s, u, IDLE_MODE);
    }
    return true;
}

// ---- heavy general (class 5) ------------------------------------------------------------------

// 0x53d3b0 heavy_gen default (tasks 0/3/4).
bool heavygen_default(AiSystem& s, Unit& u) {
    int t = s.search_for_unit(u, "T");
    int e = engage(s, u, "E", 100, 150);
    if (s.attack_mode(u)) return e ? strike(s, u, e, 4, 3, heavygen_roll) : true;
    if (!s.idle_mode(u)) return true;
    u.attack_behaviour = NEAREST_HIGH_PRIORITY;
    int r1 = s.unit_range(u, 1);
    int a = s.search_for_unit(u, "E", s.curr_task_para(u, 1) == 0 ? r1 : r1 / 2, true);
    int n = s.search_for_unit(u, "E", s.unit_range(u, 1) / 2);
    if (skirmish(s, u, a, n)) return true;
    if (s.curr_task_para(u, 1) == 0) {
        u.attack_behaviour = NEAREST_HIGH_PRIORITY;
        a = s.search_for_unit(u, "E", s.unit_range(u, 2), true);
        if (a > 0) {
            approach(s, u, true, a, 60, 60);
            return true;
        }
        if (t && s.unit_dist(u, t) <= s.unit_range(u, 3)) approach(s, u, true, t, 60, 60);
    }
    return true;
}

// 0x53db10 heavy_gen ATTACK_PLACE. No NEAREST_HIGH_PRIORITY before the near probes (one push only).
bool heavygen_attack_place(AiSystem& s, Unit& u) {
    int t = s.search_for_unit(u, "T");
    int e = engage(s, u, "E", 100, 150);
    if (s.attack_mode(u)) return e ? strike(s, u, e, 4, 3, heavygen_roll) : true;
    if (!s.idle_mode(u)) return true;
    int a = s.search_for_unit(u, "E", s.unit_range(u, 1), true);
    int n = s.search_for_unit(u, "E", s.unit_range(u, 1) / 2);
    if (skirmish(s, u, a, n)) return true;
    u.attack_behaviour = NEAREST_HIGH_PRIORITY;
    a = s.search_for_unit(u, "E", s.unit_range(u, 2), true);
    if (a > 0) {
        approach(s, u, false, a, 60, 60);
        return true;
    }
    if (t && s.unit_dist(u, t) <= s.unit_range(u, 3)) {
        approach(s, u, true, t, 60, 60);
        return true;
    }
    set_mode(s, u, IDLE_MODE);
    approach_place(s, u, 0, true, s.curr_task_para(u, 1));
    return true;
}

// 0x53e240 heavy_gen DEFEND_PLACE.
bool heavygen_defend_place(AiSystem& s, Unit& u) {
    int place = s.curr_task_para(u, 1);
    int t = s.search_for_unit(u, "T");
    int e = engage(s, u, "EP", 100, 150, place);
    if (s.attack_mode(u)) {
        s.set_place_unit_view_angle(place, 0);
        return e ? strike(s, u, e, 5, 4, heavygen_roll) : true;
    }
    if (!s.idle_mode(u)) return true;
    u.attack_behaviour = NEAREST_HIGH_PRIORITY;
    int a = s.search_for_unit(u, "EP", s.unit_range(u, 1), true, place);
    int n = s.search_for_unit(u, "EP", s.unit_range(u, 1) / 2, false, place);
    if (skirmish(s, u, a, n)) return true;
    u.attack_behaviour = NEAREST_HIGH_PRIORITY;
    a = s.search_for_unit(u, "EP", s.unit_range(u, 2), true, place);
    if (a > 0) {
        approach(s, u, true, a, 60, 60);
        return true;
    }
    if (t && s.inside_place(place, t) && s.unit_dist(u, t) <= s.unit_range(u, 3)) {
        approach(s, u, true, t, 60, 60);
        return true;
    }
    set_mode(s, u, IDLE_MODE);
    approach_place(s, u, 0, true, place);
    return true;
}

// 0x53e9d0 melee FOLLOW_UNIT / DEFEND_UNIT (class 4 team-less, class 5). Roll is heavy_gen 0x53f670 for both.
bool melee_follow_unit(AiSystem& s, Unit& u) {
    int f = s.curr_task_para(u, 1);
    int ang = s.curr_task_para(u, 2);
    if (ang == 0) ang = 180;
    int d = follow_dist(s, u, f);
    int fd = s.follow_unit_distance(f);
    if (d > fd + 150) {
        s.give_order(u, order::FOLLOW_UNIT, 0, true, f, ang, fd, 0, kFollowMs);
        s.set_target(u, 0);
        return true;
    }
    int t = s.search_for_unit(u, "T");
    int e = engage(s, u, "E", 100, 150);
    if (s.attack_mode(u)) return e ? strike(s, u, e, 4, 3, heavygen_roll) : true;
    if (!s.idle_mode(u)) return true;
    int ft = f ? target_of(s, f) : 0;
    u.attack_behaviour = NEAREST_HIGH_PRIORITY;
    int a = s.search_for_unit(u, "E", s.unit_range(u, 1), true);
    int n = s.search_for_unit(u, "E", s.unit_range(u, 1) / 2);
    if (skirmish(s, u, a, n)) return true;
    if (ft) {
        u.attack_behaviour = NEAREST_HIGH_PRIORITY;
        a = s.search_for_unit(u, "E", s.unit_range(u, 2));
        if (a > 0) {
            approach(s, u, true, a, 70, 70);
            return true;
        }
        if (t && s.unit_dist(u, t) <= s.unit_range(u, 3)) {
            approach(s, u, true, t, 70, 70);
            return true;
        }
    }
    set_mode(s, u, IDLE_MODE);
    return true;
}

// ---- archers (classes 6/7) ----------------------------------------------------------------------

constexpr std::string_view kExArcher = "ENGLISH_EX_ARCHER", kExCaptain = "ENGLISH_EX_ARCHER_CAPTAIN";

// Body shared by 0x540070 / 0x541600 (after the leader block) and 0x542820 (after the follow leash).
// para1_gate: the idle aggro search only runs when task para1 == 0 (not in FOLLOW_UNIT).
bool archer_core(AiSystem& s, Unit& u, int t, std::string_view item_code, bool para1_gate) {
    int e2 = s.search_for_unit(u, "E", s.unit_range(u, 2), false, 0, 0, kMaxPitch);
    int e1 = s.search_for_unit(u, "E", s.unit_range(u, 1), false, 0, 0, kMaxPitch);
    int item = archer_item(s, u, item_code, s.unit_range(u, 2));
    if (e1) {
        backoff(s, u, e1);
        return true;
    }
    set_mode(s, u, e2 || item ? ATTACK : ACTIVE);
    if (s.attack_mode(u) && fire(s, u, e2, item)) return true;
    if (s.idle_mode(u)) {
        if (!para1_gate || s.curr_task_para(u, 1) == 0) {
            int a = s.search_for_unit(u, "E", s.unit_range(u, 2), true, 0, 0, kMaxPitch);
            if (a > 0) {
                close_in(s, u, false, a);
                return true;
            }
            if (t && s.unit_dist(u, t) <= s.unit_range(u, 3)) {
                close_in(s, u, true, t);
                return true;
            }
        }
        set_mode(s, u, IDLE_MODE);
    }
    return true;
}

// 0x540070 archer default (tasks 0/1/9/18/default, 10 in a team).
bool archer_default(AiSystem& s, Unit& u) {
    int t = s.search_for_unit(u, "T");
    if (leader_block(s, u, t) < 0) return true;
    return archer_core(s, u, t, kExArcher, true);
}

// 0x541600 archer_gen default (tasks 0/9/18/default): no leader block, captain items, roll 0x5432a0.
bool archergen_default(AiSystem& s, Unit& u) { return archer_core(s, u, s.search_for_unit(u, "T"), kExCaptain, true); }

// 0x540640 archer / 0x541b10 archer_gen DEFEND_PLACE.
bool archer_defend_place(AiSystem& s, Unit& u, bool gen) {
    int place = s.curr_task_para(u, 1);
    int t = s.search_for_unit(u, "T");
    if (!gen && s.leader_recno(u) > 0) {
        s.search_for_unit(u, "L", 1000);   // result unused (no regroup here)
        if (!t) s.give_order(u, order::FOLLOW_FORMATION, -2, true, 0, 0, 0, 0, kFormationMs);
    }
    int e2 = s.search_for_unit(u, "EP", s.unit_range(u, 2), false, place, 0, kMaxPitch);
    int e1 = s.search_for_unit(u, "EP", s.unit_range(u, 1), false, place, 0, kMaxPitch);
    int item = archer_item(s, u, gen ? kExCaptain : kExArcher, s.unit_range(u, 2));
    if (e1) {
        backoff(s, u, e1);
        return true;
    }
    set_mode(s, u, e2 || item ? ATTACK : ACTIVE);
    if (s.attack_mode(u)) {
        s.set_place_unit_view_angle(place, 0);
        if (fire(s, u, e2, item)) return true;
    }
    if (s.idle_mode(u)) {
        int a = s.search_for_unit(u, "EP", s.unit_range(u, 2), true, place, 0, kMaxPitch);
        if (a > 0) {
            close_in(s, u, false, a);
            return true;
        }
        if (t && s.inside_place(place, t) && s.unit_dist(u, t) <= s.unit_range(u, 3)) {
            close_in(s, u, true, t);
            return true;
        }
        set_mode(s, u, IDLE_MODE);
        if (gen && a == 0) approach_place(s, u, 0, true, place);
    }
    return true;
}

// 0x5420a0 archer_gen ATTACK_PLACE: place passed to the "E" probes, idle search not leader-centred,
// no para1 gate, and APPROACH_PLACE instead of IDLE.
bool archergen_attack_place(AiSystem& s, Unit& u) {
    int t = s.search_for_unit(u, "T");
    int place = s.curr_task_para(u, 1);
    int e2 = s.search_for_unit(u, "E", s.unit_range(u, 2), false, place, 0, kMaxPitch);
    int e1 = s.search_for_unit(u, "E", s.unit_range(u, 1), false, place, 0, kMaxPitch);
    int item = archer_item(s, u, kExCaptain, s.unit_range(u, 2));
    if (e1) {
        backoff(s, u, e1);
        return true;
    }
    set_mode(s, u, e2 || item ? ATTACK : ACTIVE);
    if (s.attack_mode(u) && fire(s, u, e2, item)) return true;
    if (s.idle_mode(u)) {
        int a = s.search_for_unit(u, "E", s.unit_range(u, 2), false, 0, 0, kMaxPitch);
        if (a > 0) {
            close_in(s, u, false, a);
            return true;
        }
        if (t && s.unit_dist(u, t) <= s.unit_range(u, 3)) {
            close_in(s, u, true, t);
            return true;
        }
        if (a == 0) approach_place(s, u, 0, true, place);
    }
    return true;
}

// 0x540c10 archer / 0x5425d0 archer_gen TOWER_ATTACK: hold the post, shoot unit and item in one tick.
bool archer_tower_attack(AiSystem& s, Unit& u, bool gen) {
    int radius = s.curr_task_para(u, 1);
    u.attack_behaviour = NEAREST_ARCHER;
    int e = s.search_for_unit(u, "E", radius, false, 0, 0, kMaxPitch);
    int item = archer_item(s, u, gen ? kExCaptain : kExArcher, gen ? radius : s.unit_range(u, 2));
    set_mode(s, u, e || item ? ATTACK : ACTIVE);
    if (s.attack_mode(u)) {
        if (e && !s.line_of_fire_blocked(u, e)) {
            turn(s, u, 3, e);
            shoot_roll(s, u, 1, e);
        }
        if (item) {
            turn_item(s, u, 3, item);
            shoot_roll(s, u, 2, item);
        }
    }
    return true;
}

// 0x542820 ranged FOLLOW_UNIT (class 6 team-less, class 7): solo leash at prio -1, captain items.
bool archer_follow_unit(AiSystem& s, Unit& u) {
    int f = s.curr_task_para(u, 1);
    int ang = s.curr_task_para(u, 2);
    if (ang == 0) ang = 180;
    int d = follow_dist(s, u, f);
    int fd = s.follow_unit_distance(f);
    if (d > fd + 150) {
        s.give_order(u, order::FOLLOW_UNIT, -1, false, f, ang, fd, 0, kFollowMs);
        s.set_target(u, 0);
        return true;
    }
    return archer_core(s, u, s.search_for_unit(u, "T"), kExCaptain, false);
}

// 0x540e80 archer / 0x542e60 archer_gen RTS_ATTACK_UNIT.
bool archer_rts_attack_unit(AiSystem& s, Unit& u) {
    int t = s.curr_task_para(u, 1);
    if (s.is_attacked(u.id) || s.unit_dead(t)) {
        s.clear_task(u);
        return true;
    }
    if (s.unit_dist(u, t) < s.unit_range(u, 2)) {
        set_mode(s, u, ATTACK);
        if (!s.line_of_fire_blocked(u, t)) {
            turn(s, u, 4, t);
            shoot_roll(s, u, 1, t);
            return true;
        }
        set_mode(s, u, ACTIVE);
    }
    close_in(s, u, false, t);
    return true;
}

// 0x5410b0 archer / 0x543090 archer_gen RTS_ATTACK_ITEM (gen: TURN + unconditional SHOOT prio 3).
bool archer_rts_attack_item(AiSystem& s, Unit& u, bool gen) {
    int it = s.curr_task_para(u, 1);
    if (s.is_attacked(u.id)) {
        s.clear_task(u);
        return true;
    }
    if (s.item_dist(u, it) < s.unit_range(u, 2)) {
        set_mode(s, u, ATTACK);
        if (!is_item_box(s, it)) {
            s.clear_task(u);
            return true;
        }
        if (gen) {
            turn_item(s, u, 4, it);
            s.give_order(u, order::SHOOT, 3, false, 2, it, 0, 0, kShootMs);
        } else {
            shoot_roll(s, u, 2, it);
        }
        return true;
    }
    s.give_order(u, order::APPROACH_ITEM, 1, false, it, s.unit_range(u, 2) - 400, 400, 0, kApproachMs);
    return true;
}

// 0x541300 archer / 0x543330 archer_gen task 19 attack-move.
bool archer_attack_move(AiSystem& s, Unit& u) {
    s.search_for_unit(u, "T");
    int e2 = s.search_for_unit(u, "E", s.unit_range(u, 2), false, 0, 0, kMaxPitch);
    int e1 = s.search_for_unit(u, "E", s.unit_range(u, 1), false, 0, 0, kMaxPitch);
    if (e1) {
        backoff(s, u, e1);
        return true;
    }
    set_mode(s, u, e2 ? ATTACK : ACTIVE);
    if (s.attack_mode(u) && fire(s, u, e2, 0)) return true;
    if (s.idle_mode(u)) {
        int x = s.curr_task_para(u, 1), y = s.curr_task_para(u, 2), z = s.curr_task_para(u, 3);
        s.give_order(u, order::MOVE_TO_TARGET, 0, false, x, y, z, 0, kApproachMs);
        Vec3 dest{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        if (s.distance_to(u, dest) < kArrive) s.clear_task(u);
        set_mode(s, u, IDLE_MODE);
    }
    return true;
}

// ---- main routine prologue ------------------------------------------------------------------------

// Shared head of 0x532780..0x532ef0. Returns true when the tick is consumed (dead, or being hit).
bool prologue(AiSystem& s, Unit& u, Behaviour b, int& task) {
    if (s.unit_dead(u.id)) return true;
    u.attack_behaviour = b;
    task = s.curr_task(u);
    if (!s.is_attacked(u.id)) return false;
    int p1 = s.curr_task_para(u, 1);
    if (task == task::IDLE && p1 == 0) {
        s.clear_task(u);
        if (int l = s.leader_recno(u)) {
            Unit* lu = s.world().unit(l);
            if (lu && !lu->deleted && s.curr_task(*lu) == task::IDLE) s.clear_task(*lu);
        }
    }
    return true;
}

}  // namespace

// 0x532780
bool heavy_infantry(AiSystem& s, Unit& u) {
    int task = 0;
    if (prologue(s, u, NEAREST, task)) return true;
    switch (task) {
    case task::RECRUIT_TROOP: case task::HELP_TROOP: case task::FLEE: case task::IDLE: return true;
    case task::DEFEND_PLACE: return heavyinf_defend_place(s, u);
    case task::FOLLOW_UNIT: return s.leader_of(u) ? heavyinf_default(s, u) : melee_follow_unit(s, u);
    case task::RETREAT: return task_retreat(s, u);
    case task::RTS_MOVE_TO: return task_rts_move(s, u);
    case task::RTS_ATTACK_UNIT: return melee_rts_attack_unit(s, u, 4, heavyinf_roll);
    case task::APPROACH_PLACE: return task_approach_place(s, u);
    case task::RTS_ATTACK_ITEM: return melee_rts_attack_item(s, u, heavyinf_roll);
    case task::RTS_ATTACK_MOVE: return melee_attack_move(s, u);
    default: return heavyinf_default(s, u);
    }
}

// 0x532a00
bool heavy_general(AiSystem& s, Unit& u) {
    int task = 0;
    if (prologue(s, u, NEAREST, task)) return true;
    switch (task) {
    case task::NONE: case task::RECRUIT_TROOP: case task::HELP_TROOP: return heavygen_default(s, u);
    case task::ATTACK_PLACE: return heavygen_attack_place(s, u);
    case task::DEFEND_PLACE: return heavygen_defend_place(s, u);
    case task::FOLLOW_UNIT: case task::DEFEND_UNIT: return melee_follow_unit(s, u);
    case task::RETREAT: return task_retreat(s, u);
    case task::RTS_MOVE_TO: return task_rts_move(s, u);
    case task::RTS_ATTACK_UNIT: return melee_rts_attack_unit(s, u, 5, heavygen_roll);
    case task::APPROACH_PLACE: return task_approach_place(s, u);
    case task::RTS_ATTACK_ITEM: return melee_rts_attack_item(s, u, heavygen_roll);
    case task::RTS_ATTACK_MOVE: return melee_attack_move(s, u);
    default: return true;
    }
}

// 0x532c60
bool archer(AiSystem& s, Unit& u) {
    int task = 0;
    if (prologue(s, u, WEAKEST, task)) return true;
    switch (task) {
    case task::RECRUIT_TROOP: case task::HELP_TROOP: case task::FLEE: case task::IDLE: return true;
    case task::DEFEND_PLACE: return archer_defend_place(s, u, false);
    case task::TOWER_ATTACK: return archer_tower_attack(s, u, false);
    case task::FOLLOW_UNIT: return s.leader_of(u) ? archer_default(s, u) : archer_follow_unit(s, u);
    case task::RETREAT: return task_retreat(s, u);
    case task::RTS_MOVE_TO: return task_rts_move(s, u);
    case task::RTS_ATTACK_UNIT: return archer_rts_attack_unit(s, u);
    case task::APPROACH_PLACE: return task_approach_place(s, u);
    case task::RTS_ATTACK_ITEM: return archer_rts_attack_item(s, u, false);
    case task::RTS_ATTACK_MOVE: return archer_attack_move(s, u);
    default: return archer_default(s, u);
    }
}

// 0x532ef0
bool archer_general(AiSystem& s, Unit& u) {
    int task = 0;
    if (prologue(s, u, WEAKEST, task)) return true;
    switch (task) {
    case task::ATTACK_PLACE: return archergen_attack_place(s, u);
    case task::RECRUIT_TROOP: case task::HELP_TROOP: case task::FLEE: case task::IDLE: return true;
    case task::DEFEND_PLACE: return archer_defend_place(s, u, true);
    case task::TOWER_ATTACK: return archer_tower_attack(s, u, true);
    case task::FOLLOW_UNIT: return archer_follow_unit(s, u);
    case task::RETREAT: return task_retreat(s, u);
    case task::RTS_MOVE_TO: return task_rts_move(s, u);
    case task::RTS_ATTACK_UNIT: return archer_rts_attack_unit(s, u);
    case task::APPROACH_PLACE: return task_approach_place(s, u);
    case task::RTS_ATTACK_ITEM: return archer_rts_attack_item(s, u, true);
    case task::RTS_ATTACK_MOVE: return archer_attack_move(s, u);
    default: return archergen_default(s, u);
    }
}

// 0x546970 AI_after_block, classes 4-7.
bool after_block_heavy(AiSystem& s, Unit& u) {
    switch (u.ai_class) {
    case 4: return heavyinf_roll(s, u);
    case 5: return heavygen_roll(s, u);
    case 6: case 7: shoot_roll(s, u, 1, 0); return true;
    default: return false;
    }
}

}  // namespace joa::game::ai
