// Inventory slot getter 0x4974f0, Inv_bonus 0x497e10, recovery items 0x43b490/0x43b990, equipment.
#include "game/items_inventory.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>

#include "game/world_sim.hpp"

namespace joa::game {

const ItemInstance* Inventory::slot(int s) const {
    if (weapon_mode == 0) return nullptr;
    if (weapon_mode == 2) return ranged.item ? &ranged : nullptr;
    return melee[s & 1].item ? &melee[s & 1] : nullptr;
}

int Inventory::bonus(const GameData& gd, int effect) const {
    int c = (effect == MORALE_DROP || effect == EP_CONSUME) ? 100 : 0;
    auto combine = [&](int v) -> bool {   // false = stop (MORALE_DROP immunity)
        switch (effect) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 0x14: c += v; break;
        case 9: case 0x16: c += (100 - c) * v / 100; break;
        case 0x12: c = std::max(c, v); break;
        case 0x13: if (v != 0) return false; break;
        case 0x15: c = c * v / 100; break;
        default: c += (c + 100) * v / 100; break;   // 7, 8, 0xa, 0xb..0x11, 0x17
        }
        return true;
    };
    auto add = [&](const ItemInstance& it) {
        if (!it.item) return true;
        auto v = gd.items[size_t(it.item)].effect_value(effect, it.level);
        return v ? combine(*v) : true;
    };
    if (weapon_mode == 1) {
        if (melee[0].item && !(melee[0].item == ranged.item && melee[0].level == ranged.level && ranged.item)) {
            if (!add(melee[0])) return 0;
        }
        if (!add(melee[1]) || !add(ranged)) return 0;
    }
    std::map<int, int> best;   // enhancers: only the highest level of duplicate ids
    for (const auto& it : bag)
        if (it.item && gd.items[size_t(it.item)].item_class == "ENHANCER") best[it.item] = std::max(best[it.item], it.level);
    for (auto [id, lvl] : best)
        if (!add(ItemInstance{id, 1, lvl})) return 0;
    return c;
}

bool World::use_recovery_item(Unit& u, bool energy) {
    const int need = energy ? max_ep(u) - u.ep : max_hp(u) - u.hp;
    int best = -1, best_d = 0;
    for (size_t i = 0; i < u.inv.bag.size(); ++i) {
        const ItemDef& d = gd_.items[size_t(u.inv.bag[i].item)];
        const bool ok = energy ? d.type == 0x17 : (d.type == 0x15 || d.type == 0x19);
        if (!ok || u.inv.bag[i].quantity < 1) continue;
        const int diff = std::abs(d.damage - need);
        if (best < 0 || diff < best_d) best = int(i), best_d = diff;
    }
    if (best < 0) return false;
    ItemInstance& it = u.inv.bag[size_t(best)];
    const int amount = gd_.items[size_t(it.item)].damage;   // item +0x4e
    if (energy) set_ep(u, u.ep + amount);
    else set_hp(u, u.hp + amount);   // HYPOTHESIS: 0x50bcd0/0x50b4f0 add the amount
    emit(Ev::Effect, u.id, it.item, amount, "spiritual.txt");
    if (--it.quantity < 1) u.inv.bag.erase(u.inv.bag.begin() + best);
    return true;
}

bool World::give_item(Unit& u, std::string_view code, int level, int quantity) {
    const int id = gd_.item_index(code);
    if (!id) return false;
    const ItemDef& d = gd_.items[size_t(id)];
    if (!level) level = d.min_level;
    for (auto& it : u.inv.bag)
        if (it.item == id && it.level == level && it.quantity + quantity <= std::max(1, d.max_quantity)) {
            it.quantity += quantity;
            return true;
        }
    if (u.inv.ammo.item == id) { u.inv.ammo.quantity += quantity; return true; }
    u.inv.bag.push_back({id, quantity, level});
    return true;
}

bool World::equip(Unit& u, int bag_index) {
    if (bag_index < 0 || bag_index >= int(u.inv.bag.size())) return false;
    ItemInstance it = u.inv.bag[size_t(bag_index)];
    const ItemDef& d = gd_.items[size_t(it.item)];
    if (d.usable_unit >= 0 && d.usable_unit != u.cls) return false;
    ItemInstance* slot = nullptr;
    switch (d.type) {
    case 9: case 10: slot = &u.inv.melee[0]; break;   // right hand / two hand
    case 8: slot = &u.inv.melee[1]; break;            // left hand
    case 0xb: case 0xc: slot = &u.inv.ranged; break;
    case 0xd: case 0xe: case 0xf: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: slot = &u.inv.ammo; break;
    default:
        if (d.item_class == "ARMOR") {
            std::swap(u.armor_item, it.item);
            if (it.item) u.inv.bag[size_t(bag_index)] = {it.item, 1, gd_.items[size_t(it.item)].min_level};
            else u.inv.bag.erase(u.inv.bag.begin() + bag_index);
            return true;
        }
        return false;
    }
    const ItemInstance old = *slot;
    *slot = it;
    if (old.item) u.inv.bag[size_t(bag_index)] = old;
    else u.inv.bag.erase(u.inv.bag.begin() + bag_index);
    if (u.inv.melee[0].item || u.inv.melee[1].item) u.inv.weapon_mode = 1;
    else if (u.inv.ranged.item) u.inv.weapon_mode = 2;
    return true;
}

}  // namespace joa::game
