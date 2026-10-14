// Inventory slot getter 0x4974f0, Inv_bonus 0x497e10 (+ applicability 0x4a42f0, team morale 0x5103c0), recovery items 0x43b490/0x43b990, equipment.
#include "game/items_inventory.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <utility>

#include "game/world_sim.hpp"

namespace joa::game {

const ItemInstance* Inventory::slot(int s) const {
    if (weapon_mode == 0) return nullptr;
    if (weapon_mode == 2) return ranged.item ? &ranged : nullptr;
    return melee[s & 1].item ? &melee[s & 1] : nullptr;
}

int Inventory::bonus(const GameData& gd, int effect, const World* w, const Unit* req) const {
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
    for (auto [id, lvl] : best) {   // value first, then 0x4a42f0 (tested only for items that have the effect)
        const ItemDef& d = gd.items[size_t(id)];
        const auto v = d.effect_value(effect, lvl);
        if (!v || (w && req && !w->item_applies(d, lvl, *req))) continue;
        if (!combine(*v)) return 0;
    }
    return c;
}

// FUN_004a42f0: every EffectiveRequirement of an enhancer must hold on unit r. Threshold
// t = value + (level - MinLevel) * inc; MAX_HIT_POINT / MAX_ENERGY_POINT compare the current HP / EP (+0x135 /
// +0x145) with max * t / 100, STR..LEAD the effective stats (getters 0x5237e0.. include item bonuses: the original
// recurses through Inv_bonus the same way), MORALE_BASE the team morale 0x5103c0. BELOW: fails when t < stat;
// OVER: fails when stat < t (both inclusive).
bool World::item_applies(const ItemDef& d, int level, const Unit& r) const {
    for (const auto& q : d.require) {
        const int t = q.value + (level - d.min_level) * q.inc;
        int have = 0, need = t;
        switch (q.type) {
        case MAX_HIT_POINT: have = r.hp, need = max_hp(r) * t / 100; break;
        case MAX_ENERGY_POINT: have = r.ep, need = max_ep(r) * t / 100; break;
        case STRENGTH: have = stat_str(r); break;
        case DEXTERITY: have = stat_dex(r); break;
        case DEFENSE: have = stat_def(r); break;
        case LEADERSHIP: have = stat_lead(r); break;
        case MORALE_BASE: have = team_morale(r); break;
        default: continue;   // the original compares stale locals; no shipped item uses another type
        }
        if (q.over ? have < need : need < have) return false;
    }
    return true;
}

// FUN_005103c0: no team (+0x31 == 0) -> own morale; member -> its leader's value; leader -> integer mean of its
// own and every non-deleted member's morale.
int World::team_morale(const Unit& u) const {
    if (u.team == 0) return u.morale;
    if (u.members.empty()) {
        const Unit* l = u.leader && u.leader != u.id ? unit(u.leader) : nullptr;
        return l ? team_morale(*l) : u.morale;   // the original dereferences a null leader here
    }
    int sum = u.morale, n = 1;
    for (int id : u.members)
        if (const Unit* m = unit(id)) sum += m->morale, ++n;
    return sum / n;
}

// FUN_0050b4f0 (red 0x15: HP), FUN_0050ba00 (blue 0x17: EP), FUN_0050bcd0 (0x19: HP and EP) on a live unit.
// Without `force` a unit already at full HP (EP for 0x17) refuses the item (returns false, nothing used).
// HP: a team member passes the surplus to the lowest-HP live members of its leader's team (scan from the
// end, strict <); a leader gives amount/2 to every live member (0x50bcd0 then sets the member's EP to its
// new HP: original bug, kept). EP (0x50ba00): a leader gives amount/2 EP to every live member.
bool World::recover(Unit& u, int item, bool force) {
    if (dead(u) || item <= 0 || item >= int(gd_.items.size())) return false;
    const ItemDef& d = gd_.items[size_t(item)];
    if (d.type != 0x15 && d.type != 0x17 && d.type != 0x19) return false;
    const int amount = d.damage;   // item +0x4e
    auto members = [&](auto&& fn) {
        for (std::size_t i = u.members.size(); i-- > 0;)
            if (Unit* m = unit(u.members[i]); m && !m->deleted && m->id != u.id && !dead(*m)) fn(*m);
    };
    if (d.type == 0x17) {
        if (!force && u.ep >= max_ep(u)) return false;
        set_ep(u, std::min(max_ep(u), u.ep + amount));
        members([&](Unit& m) { set_ep(m, std::min(max_ep(m), m.ep + amount / 2)); });
    } else {
        if (!force && u.hp >= max_hp(u)) return false;
        const bool energy = d.type == 0x19;
        const int hp = std::min(max_hp(u), u.hp + amount);
        int rest = amount - (hp - u.hp);
        set_hp(u, hp);
        u.hp_trail = hp;
        if (energy) set_ep(u, std::min(max_ep(u), u.ep + amount));
        if (!u.members.empty()) {
            members([&](Unit& m) {
                const int h = std::min(max_hp(m), m.hp + amount / 2);
                set_hp(m, h);
                m.hp_trail = h;
                if (energy) set_ep(m, h);
            });
        } else if (Unit* l = unit(u.leader); l && l->id != u.id) {
            while (rest > 0) {
                Unit* worst = nullptr;
                for (std::size_t i = l->members.size(); i-- > 0;)
                    if (Unit* m = unit(l->members[i]); m && !m->deleted && !dead(*m) && m->hp < max_hp(*m) &&
                                                       (!worst || float(m->hp) / float(max_hp(*m)) < float(worst->hp) / float(max_hp(*worst))))
                        worst = m;
                if (!worst) break;
                const int v = std::min(max_hp(*worst), worst->hp + rest);
                rest -= v - worst->hp;
                set_hp(*worst, v);
                worst->hp_trail = v;
            }
        }
    }
    return true;
}

// F3 Ctrl_use_hp_item 0x43b490 / F4 0x43b990: the bag potion whose amount is closest to the missing HP
// (EP), ties to the lower bag index; HP: red 0x15 first, 0x19 only when the bag holds no 0x15; EP: blue
// 0x17 only. Used through recover() without force: refused (and kept) at full HP / EP.
bool World::use_recovery_item(Unit& u, bool energy) {
    const int need = energy ? max_ep(u) - u.ep : max_hp(u) - u.hp;
    auto pick = [&](int type) {
        int best = -1, best_d = 0;
        for (size_t i = 0; i < u.inv.bag.size(); ++i) {
            const ItemDef& d = gd_.items[size_t(u.inv.bag[i].item)];
            if (d.type != type || u.inv.bag[i].quantity < 1) continue;
            const int diff = std::abs(d.damage - need);
            if (best < 0 || diff < best_d) best = int(i), best_d = diff;
        }
        return best;
    };
    int best = pick(energy ? 0x17 : 0x15);
    if (best < 0 && !energy) best = pick(0x19);
    const int item = best < 0 ? 0 : u.inv.bag[size_t(best)].item;
    if (!recover(u, item, false)) return false;
    emit(Ev::Effect, u.id, item, gd_.items[size_t(item)].damage, "spiritual.txt");
    if (--u.inv.bag[size_t(best)].quantity < 1) u.inv.bag.erase(u.inv.bag.begin() + best);
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
            // 0x6d8ac0 (CONFIRMED): the armour is consumed; it raises the armour level only (Defense +0x56)
            // and then selects the hero mesh Unit_set_model_level(Damage +0x4e) (0x511060).
            if (u.armor_level < d.defense) u.armor_level = d.defense, u.model_level = d.damage, u.armor_item = it.item;
            u.inv.bag.erase(u.inv.bag.begin() + bag_index);
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

// Inventory right-click (0x6d8ac0): red potion 0x15 -> 0x50b4f0, blue 0x17 -> 0x50ba00, all 0x19 -> 0x50bcd0, each
// called with "force" = 1 (consumed even at full HP), see recover(). ARMOR class -> equip().
bool World::use_item(Unit& u, int bag_index) {
    if (bag_index < 0 || bag_index >= int(u.inv.bag.size()) || dead(u)) return false;
    ItemInstance& it = u.inv.bag[size_t(bag_index)];
    if (gd_.items[size_t(it.item)].item_class == "ARMOR") return equip(u, bag_index);
    if (!recover(u, it.item, true)) return false;
    emit(Ev::Effect, u.id, it.item, gd_.items[size_t(it.item)].damage, "spiritual.txt");
    if (--it.quantity < 1) u.inv.bag.erase(u.inv.bag.begin() + bag_index);
    return true;
}

bool World::unequip(Unit& u, int slot) {
    ItemInstance* s = slot == 0 ? &u.inv.melee[0] : slot == 1 ? &u.inv.melee[1] : slot == 2 ? &u.inv.ranged : slot == 3 ? &u.inv.ammo : nullptr;
    if (!s || !s->item) return false;
    u.inv.bag.push_back(std::exchange(*s, {}));
    if (u.inv.melee[0].item || u.inv.melee[1].item) u.inv.weapon_mode = 1;
    else u.inv.weapon_mode = u.inv.ranged.item ? 2 : 0;
    return true;
}

}  // namespace joa::game
