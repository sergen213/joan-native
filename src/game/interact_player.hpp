// Player interact key (R): the seven handlers the controllers try in order (PlayerCtrl_kbd_mouse
// 0x430480, Ctrl_horse 0x43c5c0): free prisoner 0x43bef0, talk 0x43bd40, bribe 0x43be40, recruit
// 0x43c210, item 0x43bc50, then recruit screen 0x43bfe0 and workshop screen 0x43c080; and the
// interact indicator FUN_006d3590 (Game_process_tick). Spec: docs/GAMEPLAY_IMPLEMENTATION.md
// "Interact".
#pragma once

namespace joa::game {

class Mission;
struct Unit;

// Installs World::interact (true when a handler consumed the key).
void install_interact(Mission& m);

// FUN_006d3590: the first target the indicator shows for the player, or kind 0 when hidden.
struct InteractTarget {
    enum Kind : int { None, Talk, Bribe, Prisoner, Mount, Recruit, Item, RecruitScreen, Workshop };
    int kind = None;
    int id = 0;   // unit recno, modify-item recno or facility recno
    bool operator==(const InteractTarget&) const = default;
};
InteractTarget interact_target(Mission& m);
const char* interact_kind_name(int kind);

// Search functions (also used by the indicator). 0 = none.
int find_prisoner(Mission& m, const Unit& u, float r);      // 0x471c80
int find_talker(Mission& m, const Unit& u, float r);        // 0x46c1f0
int find_bribable(Mission& m, const Unit& u, float r);      // 0x46fa80
int find_interact_item(Mission& m, const Unit& u, float r); // 0x4d6320 (modify recno)
int find_recruit_facility(Mission& m, const Unit& u);       // 0x514b80
int find_workshop(Mission& m, const Unit& u);               // 0x5156d0
// Unit_bribe 0x4e8500: target joins the briber's side. False when not bribable / not enough gold.
bool unit_bribe(Mission& m, Unit& target, Unit& briber);
int bribe_cost(Mission& m, const Unit& target);

}  // namespace joa::game
