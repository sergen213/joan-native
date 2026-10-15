// Inventory slot getter 0x4974f0, Inv_bonus 0x497e10 (+ applicability 0x4a42f0, team morale 0x5103c0), recovery items 0x43b490/0x43b990, equipment.
#include "game/items_inventory.hpp"

#include <algorithm>
#include <cmath>
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

// FUN_0050b4f0 (red 0x15: HP), FUN_0050ba00 (blue 0x17: EP), FUN_0050bcd0 (0x19: HP and EP) on a live unit (or a dead
// one with `even_dead`, their second argument: Unit_is_in_danger(1) after a lethal hit).
// Without `force` a unit already at full HP (EP for 0x17) refuses the item (returns false, nothing used).
// HP: a team member passes the surplus to the lowest-HP live members of its leader's team (scan from the
// end, strict <); a leader gives amount/2 to every live member (0x50bcd0 then sets the member's EP to its
// new HP: original bug, kept). EP (0x50ba00): a leader gives amount/2 EP to every live member.
bool World::recover(Unit& u, int item, bool force, bool even_dead) {
    if ((dead(u) && !even_dead) || item <= 0 || item >= int(gd_.items.size())) return false;
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

namespace {
constexpr int kBagW[3] = {4, 6, 6}, kBagH[3] = {1, 8, 4};   // 0x457290 args of containers +0x12 / +0x16 / +0x1a
bool is_enhancer(const GameData& gd, int item) { return gd.items[size_t(item)].item_class == "ENHANCER"; }
// FUN_00495f40: bows (0xb) take arrows 0xd..0xf, crossbows (0xc) bolts 0x10..0x12; an empty side accepts anything.
bool ammo_fits(int ranged_type, int ammo_type) {
    if (!ranged_type || !ammo_type) return true;
    return ranged_type == 0xb ? ammo_type <= 0xf : ammo_type >= 0x10;
}
void weapon_mode(Inventory& inv) {
    inv.weapon_mode = inv.melee[0].item || inv.melee[1].item ? 1 : inv.ranged.item ? 2 : 0;
}

// Unit_add_item 0x495f40 into `b` (laid out) of a unit of class `cls`; returns the pieces left over (all of them =
// refused, nothing changed that matters: callers drop `b` then). `troop` (param 8, script give_item_to_unit_troop):
// stacking items go to the troop grid only.
int store(const GameData& gd, Inventory& b, int cls, ItemInstance it, bool troop) {
    const ItemDef& d = gd.items[size_t(it.item)];
    const bool usable = d.usable_unit < 0 || d.usable_unit == cls;
    const int ranged_type = b.ranged.item ? gd.items[size_t(b.ranged.item)].type : 0;
    const int ammo_type = b.ammo.item ? gd.items[size_t(b.ammo.item)].type : 0;
    it.bag = -1;
    const auto cell = [&](ItemInstance n, int first) {   // a new bag cell: personal (0x4976e0(.., 0)), then troop (1)
        for (int g = first; g <= 2; ++g)
            if (b.place(gd, n, g)) return b.bag.push_back(n), true;
        return false;
    };
    if (d.type == 8 || d.type == 9 || d.type == 10) {   // Inventory_set_melee_slot when both melee slots are empty
        if (usable && !b.melee[0].item && !b.melee[1].item) return (d.type == 8 ? b.melee[1] : b.melee[0]) = it, 0;
        return cell(it, 1) ? 0 : it.quantity;
    }
    if (d.type == 0xb || d.type == 0xc) {               // Inventory_set_ranged into an empty, ammo-compatible slot
        if (usable && !b.ranged.item && ammo_fits(d.type, ammo_type)) return b.ranged = it, 0;
        return cell(it, 1) ? 0 : it.quantity;
    }
    if (is_enhancer(gd, it.item)) {                     // 0x4975d0 enhancer grid (usable), else the bags
        if (ItemInstance n = it; usable && b.place(gd, n, 0)) return b.bag.push_back(n), 0;
        return cell(it, 1) ? 0 : it.quantity;
    }
    // Stacks: ammo slot (empty + compatible: Inventory_set_ammo up to MaxQuantity; same item: 0x456d60 top-up), then
    // per bag same item+level stacks from the last one and new first-fit cells (0x4577a0). Refused (0) when the ammo
    // slot takes nothing new, no bag has a free cell and the stacks cannot take all of it.
    const bool ammo = d.type >= 0xd && d.type <= 0x12;
    const int max_q = std::max(1, d.max_quantity), first = troop && !ammo ? 2 : 1;
    const auto same = [&](const ItemInstance& s) { return s.item == it.item && s.level == it.level; };
    const bool ammo_new = ammo && !b.ammo.item && ammo_fits(ranged_type, d.type);
    int space = ammo && same(b.ammo) ? max_q - b.ammo.quantity : 0;
    bool any_cell = false;
    for (const ItemInstance& s : b.bag) space += s.bag >= first && same(s) ? std::max(0, max_q - s.quantity) : 0;
    for (int g = first; g <= 2 && !any_cell; ++g) any_cell = b.place(gd, it, g);
    it.bag = -1;
    if (!ammo_new && !any_cell && space < it.quantity) return it.quantity;
    int left = it.quantity;
    const auto top_up = [&](ItemInstance& s) {
        const int n = std::min(left, std::max(0, max_q - s.quantity));
        s.quantity += n, left -= n;
    };
    if (ammo_new) b.ammo = it, b.ammo.quantity = 0;
    if (ammo && same(b.ammo)) top_up(b.ammo);
    for (int g = first; g <= 2 && left > 0; ++g) {
        for (size_t i = b.bag.size(); i-- > 0 && left > 0;)
            if (b.bag[i].bag == g && same(b.bag[i])) top_up(b.bag[i]);
        for (ItemInstance n = it; left > 0; left -= n.quantity) {
            n.quantity = std::min(left, max_q);
            if (!b.place(gd, n, g)) break;
            b.bag.push_back(n);
        }
    }
    return left;
}
}  // namespace

int World::give_item(Unit& u, std::string_view code, int level, int quantity, bool troop) {
    const int id = gd_.item_index(code);
    if (!id || quantity < 1) return 0;
    Inventory b = u.inv;
    b.layout(gd_);
    const int left = store(gd_, b, u.cls, {id, quantity, level ? level : gd_.items[size_t(id)].min_level}, troop);
    if (left == quantity) return 0;
    weapon_mode(b);
    u.inv = std::move(b);
    return quantity - left;
}

bool World::buy_item(Unit& u, int item, int level, int quantity) {
    if (item <= 0 || item >= int(gd_.items.size()) || quantity < 1) return false;
    const ItemDef& d = gd_.items[size_t(item)];
    const int max_q = std::max(1, d.max_quantity);
    Inventory b = u.inv;
    b.layout(gd_);
    ItemInstance n{item, quantity, level ? level : d.min_level, 1, 0, 0};
    bool cell = false;
    for (int x = 0; x < kBagW[1] && !cell; ++x)   // FUN_006c9d20: column by column
        for (int y = 0; y < kBagH[1] && !cell; ++y) n.col = x, n.row = y, cell = b.fits(gd_, n);
    const auto same = [&](const ItemInstance& s) { return s.bag == 1 && s.item == item && s.level == n.level; };
    int space = 0;
    for (const ItemInstance& s : b.bag) space += same(s) ? std::max(0, max_q - s.quantity) : 0;
    if (!cell && space < quantity) return false;   // FUN_006c9f10
    int left = quantity;
    for (size_t i = b.bag.size(); i-- > 0 && left > 0;)   // FUN_00457ba0: stacks from the last one, rest in the cell
        if (same(b.bag[i])) {
            const int k = std::min(left, std::max(0, max_q - b.bag[i].quantity));
            b.bag[i].quantity += k, left -= k;
        }
    if (left > 0) n.quantity = left, b.bag.push_back(n);
    u.inv = std::move(b);
    return true;
}

int World::drop_item(Unit& at, ItemInstance it) {
    if (it.item <= 0 || it.item >= int(gd_.items.size()) || it.quantity < 1) return 0;
    // 0x512830: 10 tries at a random angle (Misc_random(360) on the unit's frame), class radius + item radius (15, as
    // the pickup) + 15 away, onto a floor polygon (+0.2); none found = the item is destroyed (returns 0).
    const float r = gd_.units[size_t(at.cls)].cyl_radius + 30.0f;
    for (int i = 0; i < 10; ++i) {
        const float a = float(rng().next(360)) * 0.017453292f + at.yaw_rad();
        Vec3 p{at.pos.x - r * std::cos(a), at.pos.y - r * std::sin(a), at.pos.z};
        float h = 0;
        if (physics_ ? !physics_->floor(*this, p.x, p.y, p.z, h) : !terrain_) continue;
        p.z = (physics_ ? h : terrain_->height(p.x, p.y)) + 0.2f;
        items_.push_back({int(items_.size()) + 1, it.item, it.level, it.quantity, p, false});
        emit(Ev::ItemDrop, at.id, items_.back().id, it.item, gd_.items[size_t(it.item)].code);
        return items_.back().id;
    }
    return 0;
}

bool World::discard_item(Unit& from, int src, int bag_index, Unit& at) {
    ItemInstance* slot = src == 0 ? &from.inv.melee[0] : src == 1 ? &from.inv.melee[1] : src == 2 ? &from.inv.ranged
                         : src == 3 ? &from.inv.ammo : nullptr;
    if (!slot && (src != 10 || bag_index < 0 || bag_index >= int(from.inv.bag.size()))) return false;
    const ItemInstance it = slot ? *slot : from.inv.bag[size_t(bag_index)];
    if (it.item <= 0 || it.item >= int(gd_.items.size()) || !gd_.items[size_t(it.item)].can_drop || !drop_item(at, it))
        return false;
    if (slot) *slot = {};
    else from.inv.bag.erase(from.inv.bag.begin() + bag_index);
    weapon_mode(from.inv);
    return true;
}

bool Inventory::fits(const GameData& gd, const ItemInstance& it) const {
    if (it.bag < 0 || it.bag > 2 || (it.bag == 0 && !is_enhancer(gd, it.item))) return false;
    const ItemDef& d = gd.items[size_t(it.item)];
    if (it.col < 0 || it.row < 0 || it.col + d.width > kBagW[it.bag] || it.row + d.height > kBagH[it.bag]) return false;
    for (const ItemInstance& o : bag) {
        if (&o == &it || o.bag != it.bag) continue;
        const ItemDef& e = gd.items[size_t(o.item)];
        if (it.col < o.col + e.width && o.col < it.col + d.width && it.row < o.row + e.height && o.row < it.row + d.height)
            return false;
    }
    return true;
}

bool Inventory::place(const GameData& gd, ItemInstance& it, int g) const {
    for (it.bag = g, it.row = 0; it.row < kBagH[g]; ++it.row)
        for (it.col = 0; it.col < kBagW[g]; ++it.col)
            if (fits(gd, it)) return true;
    it.bag = -1;
    return false;
}

void Inventory::layout(const GameData& gd) {
    std::vector<int> want(bag.size());
    for (size_t i = 0; i < bag.size(); ++i) want[i] = std::exchange(bag[i].bag, -1);
    for (size_t i = 0; i < bag.size(); ++i)   // kept cells, checked against the earlier kept ones only
        if (bag[i].bag = want[i]; bag[i].bag >= 0 && !fits(gd, bag[i])) bag[i].bag = -1;
    for (ItemInstance& it : bag)
        if (it.bag < 0) (void)((is_enhancer(gd, it.item) && place(gd, it, 0)) || place(gd, it, 1) || place(gd, it, 2));
}

bool World::inventory_reachable(const Unit& u, float view_sx, float view_sy) const {
    const Unit* pl = unit(player_id());   // FUN_00717a90 on the minimap (+0x13 = the player)
    return !u.deleted && u.m.control == 0 && !u.no_react &&
           (!pl || (std::fabs(u.pos.x - pl->pos.x) * view_sx < 100.0f && std::fabs(u.pos.y - pl->pos.y) * view_sy < 100.0f));
}

bool World::transfer_item(Unit& from, int src, int bag_index, Unit& to, float view_sx, float view_sy) {
    if (!inventory_reachable(from, view_sx, view_sy) || !inventory_reachable(to, view_sx, view_sy) || to.rank < 4) return false;
    // The window detaches the item from the giver first (pointer item +0x12), so a hero can hand it to himself.
    Inventory a = from.inv, b_own;
    ItemInstance* slot = src == 0 ? &a.melee[0] : src == 1 ? &a.melee[1] : src == 2 ? &a.ranged : src == 3 ? &a.ammo : nullptr;
    ItemInstance it;
    if (slot) it = std::exchange(*slot, {});
    else if (src == 10 && bag_index >= 0 && bag_index < int(a.bag.size())) it = a.bag[size_t(bag_index)], a.bag.erase(a.bag.begin() + bag_index);
    if (it.item <= 0 || it.item >= int(gd_.items.size()) || it.quantity < 1) return false;
    const bool self = &from == &to;
    Inventory& b = self ? a : (b_own = to.inv);
    b.layout(gd_);
    const int left = store(gd_, b, to.cls, it, false);
    if (left == it.quantity || (left > 0 && self)) return false;
    if (left > 0) {   // the rest stays where it was
        it.quantity = left;
        if (slot) *slot = it;
        else a.bag.insert(a.bag.begin() + bag_index, it);
    }
    weapon_mode(a), weapon_mode(b);
    from.inv = std::move(a);
    if (!self) to.inv = std::move(b);
    return true;
}

}  // namespace joa::game
