#include "game/orders_queue.hpp"

#include <cctype>
#include <cstdlib>
#include <string>

namespace joa::game {

namespace {
const char* const kTaskNames[] = {
    "",           "ATTACK_PLACE", "ATTACK_UNIT",     "RECRUIT_TROOP",  "HELP_TROOP",      "FLEE",
    "DEFEND_PLACE", "IDLE",       "TOWER_ATTACK",    "SCOUT",          "FOLLOW_UNIT",     "RETREAT",
    "RTS_MOVE_TO", "RTS_ATTACK_UNIT", "APPROACH_PLACE", "LARGE_WEAPON_PUSH", "",           "RTS_ATTACK_ITEM",
    "DEFEND_UNIT", ""};
const char* const kOrderNames[] = {
    "",           "COMBO_ATTACK",  "SHOOT",           "JUMP",           "BLOCK",           "MOVE",
    "SHIFT",      "MOVE_TO_TARGET", "MOVE_TO_RELATIVE_TARGET", "MOVE_ON_TRACK", "TURN",   "IDLE",
    "APPROACH_ENEMY", "APPROACH_ITEM", "APPROACH_PLACE", "FOLLOW_FORMATION", "FOLLOW_UNIT", "GATE_OPEN",
    "RECRUIT_TROOP", "DOWN_ATTACK",  "ENERGY_ATTACK",   "RUSH_ATTACK",    "SPECIAL_MOVEMENT"};

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i])) return false;
    return true;
}
}  // namespace

int task_id_from_name(std::string_view n) {
    for (int i = 1; i < int(std::size(kTaskNames)); ++i)
        if (*kTaskNames[i] && ieq(n, kTaskNames[i])) return i;
    if (n.size() > 7 && ieq(n.substr(0, 7), "SPECIAL")) return task::SPECIAL + std::atoi(std::string(n.substr(7)).c_str());
    return 0;
}

const char* task_name(int id) {
    if (id > 0 && id < int(std::size(kTaskNames))) return kTaskNames[id];
    static thread_local std::string s;
    if (id > task::SPECIAL) return (s = "SPECIAL" + std::to_string(id - task::SPECIAL)).c_str();
    return "";
}

int order_id_from_name(std::string_view n) {
    for (int i = 1; i < int(std::size(kOrderNames)); ++i)
        if (ieq(n, kOrderNames[i])) return i;
    return 0;
}

const char* order_name(int id) { return id > 0 && id < order::COUNT ? kOrderNames[id] : ""; }

int OrderQueue::add(const Order& o, bool* retargeted) {
    if (retargeted) *retargeted = false;
    if (cur.action == o.action) {
        if (o.action == order::BLOCK) {
            cur.expire = o.expire;
        } else if (o.action == order::APPROACH_ENEMY && (cur.p[0] != o.p[0] || cur.p[1] != o.p[1] || cur.p[2] != o.p[2])) {
            if (retargeted) *retargeted = true;  // caller: cleanup_12 then restore
            cur.p[0] = o.p[0]; cur.p[1] = o.p[1]; cur.p[2] = o.p[2]; cur.p[3] = o.p[3];
            cur.action = order::APPROACH_ENEMY;  // prio / expire / flags not updated
        }
        return 0;
    }
    std::size_t idx = q.size();
    for (std::size_t k = q.size(); k > 0; --k) {
        if (q[k - 1].action == o.action) return 0;  // duplicate queued -> rejected
        if (o.prio > q[k - 1].prio) idx = k - 1;
    }
    q.insert(q.begin() + std::ptrdiff_t(idx), o);
    return int(idx) + 1;
}

bool OrderQueue::has(int action) const {
    if (cur.action == action) return true;
    for (const auto& o : q)
        if (o.action == action) return true;
    return false;
}

bool OrderQueue::pop_front() {
    if (q.empty()) return false;
    cur = q.front();
    q.erase(q.begin());
    return true;
}

}  // namespace joa::game
