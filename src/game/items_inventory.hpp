// Unit inventory (unit+0x1c1, 0x1e bytes; built for EVERY unit by Unit_init 0x4f8550) and the
// item-effect aggregation Inv_bonus 0x497e10. GAMEPLAY_CHARACTER_COMBAT.md Part A §1b, Part C §4.3,
// Part D §4.1 (CONFIRMED). Plain data: item = 1-based index into GameData::items (0 = none).
#pragma once

#include <cstdint>
#include <vector>

namespace joa::game {

class GameData;

// Effect ids (itemres "DamageType"/"AdditionalEffectType" names, parser 0x4a45f0, CONFIRMED).
enum Effect : int {
    MAX_HIT_POINT = 1, MAX_ENERGY_POINT, STRENGTH, DEXTERITY, DEFENSE, LEADERSHIP, WEAPON_DAMAGE,
    WEAPON_DAMAGE_MULTIPLIER, REDUCE_MELEE_DAMAGE_SUSTAINED, ARROW_DEFLECT_OFF, A_ATTACK_DAMAGE,
    B_ATTACK_DAMAGE, JUMP_ATTACK_B_DAMAGE, RUSH_ATTACK_DAMAGE, DOWN_ATTACK_DAMAGE, RANGE_DISTANCE,
    RATE_OF_FIRE, MORALE_BASE, MORALE_DROP, FATAL_KILL, EP_CONSUME, DAMAGE_SUSTAINED, TROOP_SIZE,
};

// Item instance (0x12 bytes: +0 id, +2 quantity, +4 level).
struct ItemInstance {
    int item = 0;
    int quantity = 0;
    int level = 0;
    // RecruitWorkshop: bag-grid cell of a bag item (original item +0x15 col / +0x16 row in one of the three bag
    // grids: 0 enhancers 4x1, 1 personal 6x8, 2 troop 6x4); bag -1 = not placed yet (the character window places it
    // first-fit). UI layout only: kept out of visit() (World::hash / WRLD), saved in Mission's "INVG" section.
    int bag = -1, col = 0, row = 0;
    template <class V> void visit(V& v) { v(item), v(quantity), v(level); }
};

struct Inventory {
    int weapon_mode = 0;           // +0x00: 0 none, 1 melee, 2 ranged only
    ItemInstance melee[2];         // +0x02 / +0x06 (primary / secondary)
    ItemInstance ranged;           // +0x0a (item type 0xb bow / 0xc crossbow)
    ItemInstance ammo;             // +0x0e (quantity = arrow count)
    std::vector<ItemInstance> bag; // +0x12.. bag objects: potions, enhancers, spare items

    // 0x4974f0: mode 0 -> null; mode 2 -> ranged; mode 1 -> melee[slot].
    const ItemInstance* slot(int s) const;
    // Inv_bonus 0x497e10 (integer math, C '/'): equipped slots 0,1 and ranged (mode 1 only),
    // then ENHANCER-class bag items (duplicate ids: highest level only).
    int bonus(const GameData& gd, int effect) const;
    template <class V> void visit(V& v) { v(weapon_mode), v(melee[0]), v(melee[1]), v(ranged), v(ammo), v(bag); }
};

}  // namespace joa::game
