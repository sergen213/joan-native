#include "game/unit_state.hpp"

#include <cctype>
#include <cstdio>

namespace joa::game {
namespace {
// AnimName_to_id 0x415820 strcmp chain order (CONFIRMED; 0x59..0x6a siege-crew/cart names HIGH).
const char* const kNames[] = {
    "DEFAULT", "IDLE", "IDLE2", "IDLE3", "IDLE_BAT", "IDLEUP", "IDLEDN", "IDLE_T", "JATTBI", "TIPTOE", "WALK", "RUN",
    "T_LEFT", "T_RIGHT", "MOVE_L", "MOVE_R", "MOVE_B", "MOVE_FL", "MOVE_FR", "MOVE_BL", "MOVE_BR", "JUMP_B", "JUMP_R",
    "JUMP_L", "DOWN", "JATTBD", "UP", "CHAIN", "ATTACK", "JATTACKB", "ENGATK", "ENGATK2", "ENGATK3", "ENGATK4",
    "DOWNATK", "ATTACKED", "ATTKED2", "ATTKEDR", "BATTKED", "LATTKED", "RATTKED", "DNATKED", "DIE", "GUARD1",
    "GUARD2", "GUARDR", "BACK", "CLIMB", "SHTLOAD", "SHTLOADC", "SHTLOAD2", "SHTOUT", "SHTOUT2", "SHTOUTC",
    "SHTIDLE", "SHTIDLEC", "SHTOUTH", "SHTIDLEH", "THROW", "THROWN", "STANDUP", "DIZZY", "STOP", "STOP_B",
    "KNOCKDN", "KNOCKDN2", "KNOCKDN3", "H_IDLE", "H_WALK", "H_BACK", "H_RUN", "H_STOP_B", "H_STOP", "H_ATTACK",
    "H_HITED", "H_BHITED", "H_LHITED", "H_RHITED", "H_DROP", "H_MNT_L", "H_MNT_R", "H_DMNT_L", "H_DMNT_R", "RUSH",
    "PUSH", "PUSH2", "PUSH_L", "PUSH_R", "BATTER_C", "C_IDLE_A", "C_IDLE_B", "C_DN_AB"};
constexpr int kCount = int(sizeof(kNames) / sizeof(kNames[0]));
}  // namespace

const char* action_name(int a) {
    static thread_local char buf[16];
    if (a >= 0 && a < kCount) return kNames[a];
    std::snprintf(buf, sizeof buf, "#%d", a);
    return buf;
}

int action_from_name(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    for (int i = 0; i < kCount; ++i) {
        std::string_view n = kNames[i];
        if (n.size() != s.size()) continue;
        bool eq = true;
        for (size_t k = 0; k < n.size() && eq; ++k) eq = std::toupper(static_cast<unsigned char>(s[k])) == n[k];
        if (eq) return i;
    }
    return -1;
}

bool is_attacking(int a) {
    switch (a) {
    case JATTBI: case JATTBD: case ATTACK: case JATTACKB: case ENGATK: case ENGATK2: case ENGATK3: case ENGATK4:
    case DOWNATK: case H_RUN: case H_ATTACK: case RUSH: return true;
    default: return false;
    }
}
bool is_knocked(int a) { return a >= KNOCKDN && a <= KNOCKDN3; }
bool is_knocked_ext(int a) { return is_knocked(a) || a == H_DROP || a == DNATKED; }
bool is_flinch(int a) { return a >= ATTACKED && a <= RATTKED; }
bool is_locomotion(int a) { return a == TIPTOE || a == WALK || a == RUN || (a >= MOVE_L && a <= MOVE_BR); }
bool can_act_state(int a) {
    return a == -1 || (a >= 0 && a <= 6) || (a >= 9 && a <= 0x14) || (a >= 0x2b && a <= 0x2d) || a == 0x36 || a == 0x37 ||
           a == 0x39 || (a >= 0x43 && a <= 0x46) || (a >= 0x54 && a <= 0x57) || a == 0x59 || a == 0x5a;
}

}  // namespace joa::game
