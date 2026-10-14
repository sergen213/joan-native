// Inv_bonus 0x497e10 stacking rules (GAMEPLAY_CHARACTER_COMBAT.md Part C §4.3), synthetic items.
#include "sim_fixture.hpp"

using namespace joa::game;

int main() {
    GameData gd;
    gd.items.resize(1);
    auto add = [&](const char* cls, int type, int primary, int dmg, int inc, std::vector<ItemDef::Extra> extra) {
        ItemDef d;
        d.code = "T" + std::to_string(gd.items.size());
        d.item_class = cls;
        d.type = type;
        d.primary_effect = primary;
        d.damage = dmg, d.inc_damage = inc, d.min_level = 1;
        d.extra = std::move(extra);
        gd.items.push_back(d);
        return int(gd.items.size()) - 1;
    };
    const int sword = add("WEAPON", 9, 0, 10, 2, {{STRENGTH, 2, 1}, {WEAPON_DAMAGE, 20, 0}});
    const int ring = add("ENHANCER", 0x22, 0, 0, 0, {{STRENGTH, 3, 0}, {WEAPON_DAMAGE, 10, 0}, {DAMAGE_SUSTAINED, 50, 0}});
    const int amulet = add("ENHANCER", 0x21, 0, 0, 0, {{EP_CONSUME, 50, 0}, {MORALE_BASE, 40, 0}, {DAMAGE_SUSTAINED, 50, 0}});
    const int potion = add("POTION", 0x15, 0, 0, 0, {{STRENGTH, 100, 0}});
    const int charm = add("ENHANCER", 0x23, 0, 0, 0, {{MORALE_DROP, 1, 0}});

    Inventory inv;
    inv.weapon_mode = 1;
    inv.melee[0] = {sword, 1, 3};   // level 3: value = base + (3-1)*inc
    // Additive: STR = 2 + 2*1 = 4.
    CHECK(inv.bonus(gd, STRENGTH) == 4, "sword STR %d", inv.bonus(gd, STRENGTH));
    // Multiplicative-ish 7: c += (c+100)*v/100 -> 0 + 100*20/100 = 20.
    CHECK(inv.bonus(gd, WEAPON_DAMAGE) == 20, "sword WD %d", inv.bonus(gd, WEAPON_DAMAGE));
    inv.bag = {{ring, 1, 1}, {ring, 1, 4}, {potion, 1, 1}};
    // Duplicate enhancer ids count once (highest level); potions never count.
    CHECK(inv.bonus(gd, STRENGTH) == 7, "ring STR %d", inv.bonus(gd, STRENGTH));
    // 20 then ring 10: c += (20+100)*10/100 = 12 -> 32.
    CHECK(inv.bonus(gd, WEAPON_DAMAGE) == 32, "ring WD %d", inv.bonus(gd, WEAPON_DAMAGE));
    inv.bag.push_back({amulet, 1, 1});
    // 0x16: c += (100-c)*v/100 -> 50, then 50 + 50*50/100 = 75.
    CHECK(inv.bonus(gd, DAMAGE_SUSTAINED) == 75, "sustain %d", inv.bonus(gd, DAMAGE_SUSTAINED));
    // 0x15 starts at 100 and scales: 100*50/100 = 50.
    CHECK(inv.bonus(gd, EP_CONSUME) == 50, "ep consume %d", inv.bonus(gd, EP_CONSUME));
    CHECK(inv.bonus(gd, MORALE_BASE) == 40, "morale base %d", inv.bonus(gd, MORALE_BASE));
    // 0x13 starts at 100; any item with a non-zero value makes the whole aggregate 0 (immunity).
    CHECK(inv.bonus(gd, MORALE_DROP) == 100, "morale drop default %d", inv.bonus(gd, MORALE_DROP));
    inv.bag.push_back({charm, 1, 1});
    CHECK(inv.bonus(gd, MORALE_DROP) == 0, "morale drop immune %d", inv.bonus(gd, MORALE_DROP));
    // Weapon mode 0 ignores equipped slots; mode 2 slot() returns the ranged item for both slots.
    inv.weapon_mode = 0;
    CHECK(inv.slot(0) == nullptr, "mode 0 slot");
    inv.weapon_mode = 2;
    inv.ranged = {sword, 1, 1};
    CHECK(inv.slot(0) == &inv.ranged && inv.slot(1) == &inv.ranged, "mode 2 slots");
    return finish();
}
