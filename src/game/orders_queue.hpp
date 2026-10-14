// Tasks (long-lived goals) and orders (short actions) — joan.exe Part B §1-§2 of
// docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md. Plain data; executors live in AiSystem.
#pragma once

#include <string_view>
#include <vector>

namespace joa::game {

// Task ids (FUN_004e4610). 16 has no name and no setter; 19 = RTS attack-move (only set by 0x44cfb0).
namespace task {
enum : int {
    NONE = 0, ATTACK_PLACE = 1, ATTACK_UNIT, RECRUIT_TROOP, HELP_TROOP, FLEE, DEFEND_PLACE, IDLE, TOWER_ATTACK,
    SCOUT, FOLLOW_UNIT, RETREAT, RTS_MOVE_TO, RTS_ATTACK_UNIT, APPROACH_PLACE, LARGE_WEAPON_PUSH, UNNAMED_16,
    RTS_ATTACK_ITEM, DEFEND_UNIT, RTS_ATTACK_MOVE, SPECIAL = 100  // SPECIALn = 100+n
};
}
int task_id_from_name(std::string_view name);   // case-insensitive, "SPECIAL3" -> 103; 0 if unknown
const char* task_name(int id);                  // "" for 0 / unknown

// Order (action) ids (Order_id_from_name 0x4bbbc0, jump table 0x4bb2a6).
namespace order {
enum : int {
    NONE = 0, COMBO_ATTACK = 1, SHOOT, JUMP, BLOCK, MOVE, SHIFT, MOVE_TO_TARGET, MOVE_TO_RELATIVE_TARGET,
    MOVE_ON_TRACK, TURN, IDLE, APPROACH_ENEMY, APPROACH_ITEM, APPROACH_PLACE, FOLLOW_FORMATION, FOLLOW_UNIT,
    GATE_OPEN, RECRUIT_TROOP, DOWN_ATTACK, ENERGY_ATTACK, RUSH_ATTACK, SPECIAL_MOVEMENT, COUNT
};
}
int order_id_from_name(std::string_view name);  // case-insensitive; 0 if unknown
const char* order_name(int id);

// Task struct (Unit+0x2a5 current, +0x2be parked), 0x19 bytes packed in the original.
struct Task {
    int id = 0;
    int prio = 0;            // signed char
    int p1 = 0, p2 = 0, p3 = 0;
    int expire = 0;          // absolute game ms, 0 = never
    int state = 0;           // per-task FSM state
    int para(int i) const { return i == 1 ? p1 : i == 2 ? p2 : i == 3 ? p3 : 0; }
    template <class V> void visit(V& v) { v(id), v(prio), v(p1), v(p2), v(p3), v(expire), v(state); }
};

// OrderRecord (0x21 bytes).
struct Order {
    int action = 0;
    int prio = 0;            // signed char
    int p[4] = {};           // p1..p4 (executors use them as scratch)
    int from_leader = 0;     // "use issuer's path"
    int issuer = 0;          // recno of the leader that propagated it
    int expire = 0;          // absolute ms, always set (now + duration)
    template <class V> void visit(V& v) { v(action), v(prio), v(p[0]), v(p[1]), v(p[2]), v(p[3]), v(from_leader), v(issuer), v(expire); }
};

// OrderQueue (0x3e bytes): current record + queued records sorted by descending prio (FIFO among equals).
struct OrderQueue {
    Order cur;
    std::vector<Order> q;
    // OrderQueue::add 0x4bac60. Returns 1-based insert index, 0 if merged/rejected.
    // The BLOCK refresh / APPROACH_ENEMY retarget side effects on `cur` are applied here; the caller
    // runs the APPROACH_ENEMY cleanup when *retargeted is set.
    int add(const Order& o, bool* retargeted = nullptr);
    bool has(int action) const;               // 0x4bb8d0: current or queued
    bool pop_front();                         // 0x4baf10
    bool empty() const { return cur.action == 0 && q.empty(); }
    template <class V> void visit(V& v) { v(cur), v(q); }
};

}  // namespace joa::game
