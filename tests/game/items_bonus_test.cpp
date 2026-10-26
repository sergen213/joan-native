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
        d.enhancer = d.item_class == "ENHANCER";
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

    // Applicability 0x4a42f0 (enhancers only, tested on the unit passed to Inv_bonus).
    auto req = [&](int item, int type, int value, int inc, bool over) { gd.items[size_t(item)].require.push_back({type, value, inc, over}); };
    const int holy = add("ENHANCER", 0x1d, 0, 9999, 0, {{DAMAGE_SUSTAINED, 20, 0}});
    req(holy, MAX_HIT_POINT, 50, 0, false);   // "Holy Protection Ring": HP <= 50 % of max HP
    const int might = add("ENHANCER", 0x1d, 0, 0, 0, {{WEAPON_DAMAGE, 10, 0}});
    req(might, STRENGTH, 10, 5, true);        // STR >= 10 + (level - 1) * 5
    const int pennant = add("ENHANCER", 0x1d, 0, 0, 0, {{STRENGTH, 5, 0}});
    req(pennant, MORALE_BASE, 60, 0, true);   // team morale 0x5103c0 >= 60
    const int banner = add("ENHANCER", 0x1d, 0, 0, 0, {{MORALE_BASE, 40, 0}});
    req(banner, LEADERSHIP, 50, 0, true);
    const int armour = add("ARMOR", 0x24, 0, 1, 0, {});
    const int light = add("ARMOR", 0x24, 0, 0, 0, {});
    gd.items[size_t(armour)].defense = 2, gd.items[size_t(light)].defense = 1;
    {
        World w(gd, nullptr);
        Unit u;
        u.base_max_hp = 100, u.hp = 60;
        u.inv.bag = {{holy, 1, 1}};
        CHECK(w.bonus(u, DAMAGE_SUSTAINED) == 0, "holy at 60 %% hp %d", w.bonus(u, DAMAGE_SUSTAINED));
        CHECK(u.inv.bonus(gd, DAMAGE_SUSTAINED) == 20, "bare inventory ignores requirements");
        u.hp = 50;
        CHECK(w.bonus(u, DAMAGE_SUSTAINED) == 20, "holy at 50 %% hp %d", w.bonus(u, DAMAGE_SUSTAINED));
        u.inv.bag = {{might, 1, 3}};   // threshold 10 + 2 * 5 = 20, inclusive
        u.str = 19;
        CHECK(w.bonus(u, WEAPON_DAMAGE) == 0, "might STR 19 %d", w.bonus(u, WEAPON_DAMAGE));
        u.str = 20;
        CHECK(w.bonus(u, WEAPON_DAMAGE) == 10, "might STR 20 %d", w.bonus(u, WEAPON_DAMAGE));
        // Equipped slots never take the test (0x497e10 calls 0x4a42f0 only in the enhancer loop).
        req(sword, STRENGTH, 999, 0, true);
        u.inv.bag.clear();
        u.inv.weapon_mode = 1;
        u.inv.melee[0] = {sword, 1, 1};
        CHECK(w.bonus(u, WEAPON_DAMAGE) == 20, "sword ignores requirements %d", w.bonus(u, WEAPON_DAMAGE));
    }
    {   // MORALE_BASE requirement = team morale 0x5103c0; Unit_set_morale tests requirements on the leader.
        World w(gd, nullptr);
        Unit l, m, solo;
        l.id = 1, l.team = 1, l.members = {2}, l.morale = 80, l.lead = 60;
        m.id = 2, m.team = 1, m.leader = 1, m.morale = 40;
        solo.id = 3, solo.morale = 40;
        w.units() = {l, m, solo};
        Unit& L = *w.unit(1);
        Unit& M = *w.unit(2);
        M.inv.bag = {{pennant, 1, 1}};
        w.unit(3)->inv.bag = {{pennant, 1, 1}};
        CHECK(w.team_morale(M) == 60 && w.team_morale(L) == 60, "team morale %d", w.team_morale(M));
        CHECK(w.bonus(M, STRENGTH) == 5, "pennant team 60 %d", w.bonus(M, STRENGTH));
        CHECK(w.bonus(*w.unit(3), STRENGTH) == 0, "pennant alone 40");
        L.morale = 79;   // (79 + 40) / 2 = 59
        CHECK(w.bonus(M, STRENGTH) == 0, "pennant team 59 %d", w.bonus(M, STRENGTH));
        M.inv.bag = {{banner, 1, 1}};
        M.morale = 30;
        w.set_morale(M, 10);   // leader LEAD 60 >= 50: MORALE_BASE 40 floors the drop
        CHECK(M.morale == 40, "banner floor %d", M.morale);
        L.lead = 40, M.morale = 30;
        w.set_morale(M, 10);   // leader LEAD 40: base 0 < 30 -> drop ignored (quirk)
        CHECK(M.morale == 30, "banner off %d", M.morale);
    }
    {   // Armour use 0x6d8ac0: the level only rises (Defense +0x56, mesh Damage +0x4e); the armour is consumed.
        World w(gd, nullptr);
        Unit u;
        u.armor_level = 1;
        u.inv.bag = {{armour, 1, 1}, {light, 1, 1}};
        CHECK(w.equip(u, 0) && u.armor_level == 2 && u.model_level == 1 && u.armor_item == armour, "armour up %d", u.armor_level);
        CHECK(w.equip(u, 0) && u.armor_level == 2 && u.armor_item == armour && u.inv.bag.empty(), "lighter armour consumed");
    }
    return finish();
}
