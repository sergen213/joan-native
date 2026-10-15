// Troops slice: spawn/delete, teams, orders/tasks and unit inventory builtins (SCRIPT_VM.md §8).
// Orders/tasks/teams route onto AiSystem; confidence per handler as in the §8 table (C unless noted).
#include <algorithm>

#include "formats/vfs_fs.hpp"
#include "game/mission.hpp"
#include "game/script_bind.hpp"

namespace joa::game {

namespace {

Value I(int v) { return Value::integer(v); }

Unit* unit(Mission& m, int recno) { return m.world().unit(recno); }

// Unit_get_leader 0x4216d0 + "+0x33 != 0": the leader unit owning a team, or nullptr.
Unit* team_leader(Mission& m, const Unit& u) {
    Unit* l = unit(m, m.ai().leader_of(u));
    return l && m.ai().is_leader(*l) ? l : nullptr;
}

// Script_get_unit_team_leader_by_team_id 0x6581f0: nations from last, each nation's sorted
// team-leader list (+0x1531, ascending recno) from last -> highest (nation, recno) leader wins.
// ponytail: the original keeps a leader in its old nation's list after raw +2 writes (144); we use u.nation.
int team_leader_by_id(Mission& m, int team) {
    int best = 0, best_nation = 0;
    for (const Unit& u : m.world().units())
        if (!u.deleted && u.nation >= 1 && u.team == team && m.ai().is_leader(u) && u.nation >= best_nation)
            best = u.id, best_nation = u.nation;
    return best;
}

// Units with +0x31 == team, from top (DynArray_size .. 1), deleted ones skipped.
template <class F> void for_team(Mission& m, int team, F f) {
    auto& us = m.world().units();
    for (size_t i = us.size(); i-- > 0;)
        if (!us[i].deleted && us[i].team == team) f(us[i]);
}

// FUN_0050b390: rank > 3 units drop their drop item unless one already lies in the world.
// ponytail: only World ground items are checked (not MapObjects), the key marker (+0x10a) and
// the UniqueItem re-roll of Unit_dropItem 0x50afe0 are not modelled.
void drop_keys(Mission& m, Unit& u) {
    World& w = m.world();
    if (!u.drop_item || m.scenario().mode == 2) return;
    for (const WorldItem& it : w.items())
        if (!it.deleted && it.item == u.drop_item) return;
    const ItemDef& d = m.data().items[size_t(u.drop_item)];
    w.add_item(d.code, u.drop_level, d.quantity, u.pos);
}

// Script_delete_unit 0x655420.
bool delete_unit(Mission& m, int recno) {
    Unit* u = unit(m, recno);
    if (!u) return false;
    m.ai().clear_task(*u);
    if (u->mount > 0 && unit(m, u->mount)) m.world().delete_unit(u->mount);
    if (u->rank > 3) drop_keys(m, *u);
    m.world().delete_unit(recno);
    return true;
}

// FUN_0050a480(item, bone, hand, 0): the map object held in a hand (+0x1bd / +0x1bf). The
// previously held object is deleted. held_item: 0 inventory weapon, -1 empty, >0 map object.
void set_hand(Mission& m, Unit& u, int hand, int item, int bone) {
    int& h = u.m.held_item[hand ? 1 : 0];
    if (h > 0 && h != item) m.objects().del_item(m, h);
    h = item;
    u.m.held_bone[hand ? 1 : 0] = bone;
    m.emit(MissionEvent::Kind::Ui, u.id, item, "hand_item");
}

// Unit_set_prisoner 0x519800.
void set_prisoner(Mission& m, Unit& u, int flag, int bound) {
    World& w = m.world();
    if (u.no_react && u.action == IDLE_T) return;
    w.stop(u);
    const int act = (!m.data().moves(u.cls) || std::int16_t(bound) == 0) ? IDLE : IDLE_T;  // +0x2f1 move table
    if (w.can_use_special(u, act)) w.set_action(u, act);
    u.no_react = (flag & 0xff) != 0;
    u.m.bound = bound & 0xff;
    set_hand(m, u, 0, -1, 0);
    set_hand(m, u, 1, -1, 0);
}

}  // namespace

// Unit_free_prisoner 0x5199b0: weapons back in hand (only for occupied inventory slots).
void free_prisoner(Mission& m, Unit& u) {
    if (!u.no_react) return;
    if (u.inv.slot(0)) set_hand(m, u, 0, 0, 0);
    if (u.inv.slot(1)) set_hand(m, u, 1, 0, 0);
    u.no_react = false;
    u.m.bound = 0;
    m.world().stop(u);
}

namespace {

// Unit_add_item 0x50fd90 (troop = 4th arg). Inventory placement goes through World::give_item.
// ponytail: the grid-bag placement / overflow-to-floor of FUN_00495f40 and its troop flag are
// World::give_item's simplified model (always fits).
int unit_add_item(Mission& m, Unit& u, int item, int quantity, int level) {
    World& w = m.world();
    const ItemDef& d = m.data().items[size_t(item)];
    if (d.item_class == "POTION" && d.use_immediately) {  // class 7, +0x23d
        if (d.type == 0x15) {                              // red potion: instant heal
            const int mx = w.max_hp(u);
            u.hp = mx < u.hp + d.damage * quantity ? mx : u.hp + d.damage * quantity;
        }
        return 1;
    }
    if (level < d.min_level || level > d.max_level) return 0;
    w.give_item(u, d.code, level, quantity);
    return 1;
}

// FUNC 333 give_item_to_unit 0x65c5f0 / 334 give_item_to_unit_troop 0x65c770 (H).
Value give_item(Mission& m, const BuiltinCall& c, bool troop) {
    Unit* u = unit(m, c.i(3));
    if (!u) return I(0);
    const int item = m.data().item_index(c.s(0));
    if (!item) return I(0);
    const int quantity = std::int16_t(c.i(2)), level = std::int16_t(c.i(1));
    const int r = unit_add_item(m, *u, item, quantity, level);
    if (r && u->id == m.world().player_id()) {
        const ItemDef& d = m.data().items[size_t(item)];
        m.emit(MissionEvent::Kind::Ui, item, quantity, "sms_given " + d.code);  // FUN_006e5700(SMS 4 "You have been given %1s!")
        auto& hints = m.state().item_hints_b;  // Game+0x728, FUN_00472a90 / FUN_004729c0
        if (!troop && d.show_armory && std::find(hints.begin(), hints.end(), std::uint16_t(item)) == hints.end()) {
            hints.push_back(std::uint16_t(item));
            m.emit(MissionEvent::Kind::Ui, item, level, "armory_hint " + d.code);  // FUN_006bbc90 + FUN_006ba4b0
        }
    }
    return I(r);
}

// ---------------------------------------------------------------------------------------------
// spawn

// 42 add_unit_ref 0x65b190 (H): preload the class (sprites/move table); always 1.
Value add_unit_ref(Mission& m, const BuiltinCall& c) {
    if (const int cls = m.data().unit_index(c.s(0)); cls >= 0) m.data().moves(cls);
    return I(1);
}

// 85 add_unit 0x655150 -> UnitArray_add 0x51dfc0 (Mission::add_unit): unit at the origin, yaw 0,
// or the restored CONTINUE profile hero of that class.
Value add_unit(Mission& m, const BuiltinCall& c) {
    const int id = m.add_unit(c.s(0));
    Unit* u = unit(m, id);
    if (const int nation = m.data().nation_id(c.s(1)); nation && u) {
        u->nation = nation;
        u->ai_controlled = true;  // +0x102
    }
    return I(id);
}

// 86 load_unit 0x655200 (H).
Value load_unit(Mission& m, const BuiltinCall& c) { return I(m.load_hero(c.s(0), c.s(1))); }

// 87 del_unit 0x655420.
Value del_unit(Mission& m, const BuiltinCall& c) { return I(delete_unit(m, std::int16_t(c.i(0)))); }

// 159 del_unit_team 0x659080.
Value del_unit_team(Mission& m, const BuiltinCall& c) {
    const int team = std::int16_t(c.i(0));
    if (const int l = team_leader_by_id(m, team)) {
        const std::vector<int> members = unit(m, l)->members;
        for (size_t i = members.size(); i-- > 0;) delete_unit(m, members[i]);
        delete_unit(m, l);
    } else {
        for_team(m, team, [&](Unit& u) {
            if (u.rank > 3) drop_keys(m, u);
            delete_unit(m, u.id);
        });
    }
    return I(1);
}

// 160 del_unit_team_member 0x659210: every non-leader unit of the team.
Value del_unit_team_member(Mission& m, const BuiltinCall& c) {
    for_team(m, std::int16_t(c.i(0)), [&](Unit& u) {
        if (m.ai().is_leader(u)) return;
        if (u.rank > 3) drop_keys(m, u);
        delete_unit(m, u.id);
    });
    return I(1);
}

// ---------------------------------------------------------------------------------------------
// teams

// 141 set_unit_team 0x657fd0.
Value set_unit_team(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    return I(u && m.ai().set_unit_team(*u, std::int16_t(c.i(1))));
}

// 142 get_unit_team_size 0x658160.
Value get_unit_team_size(Mission& m, const BuiltinCall& c) {
    const Unit* u = unit(m, std::int16_t(c.i(0)));
    const Unit* l = u ? team_leader(m, *u) : nullptr;
    return I(l ? int(l->members.size()) : 0);
}

// 143 get_unit_team_leader_by_team_id 0x6581f0.
Value get_team_leader(Mission& m, const BuiltinCall& c) { return I(team_leader_by_id(m, std::int16_t(c.i(0)))); }

// 144 set_team_nation 0x6582f0: raw +2 on leader and members.
Value set_team_nation(Mission& m, const BuiltinCall& c) {
    Unit* l = unit(m, team_leader_by_id(m, std::int16_t(c.i(0))));
    if (!l) return I(0);
    const int nation = m.data().nation_id(c.s(1));
    if (nation < 1) return I(0);
    l->nation = nation;
    for (int r : l->members)
        if (Unit* u = unit(m, r)) u->nation = nation;
    return I(1);
}

// 145 set_team_nation2 0x658420.
Value set_team_nation2(Mission& m, const BuiltinCall& c) {
    const int nation = m.data().nation_id(c.s(1));
    if (nation < 1) return I(0);
    for_team(m, std::int16_t(c.i(0)), [&](Unit& u) { u.nation = nation; });
    return I(1);
}

// 146 recruit_team_member 0x6584e0 (H).
Value recruit_team_member(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    if (!u || !team_leader(m, *u)) return I(0);
    m.ai().recruit_team_member(*u, c.s(1), c.i(2));
    return I(1);
}

// 147 deactivate_team_member 0x658650 / 148 activate_team_member 0x658730 (H): Unit_(de)activate
// (+0x2ed) on every member, last first; always 0.
Value set_team_members_active(Mission& m, const BuiltinCall& c, int active) {
    const Unit* u = unit(m, std::int16_t(c.i(0)));
    if (const Unit* l = u ? team_leader(m, *u) : nullptr)
        for (size_t i = l->members.size(); i-- > 0;)
            if (Unit* x = unit(m, l->members[i])) x->m.active = active;
    return I(0);
}
Value deactivate_team_member(Mission& m, const BuiltinCall& c) { return set_team_members_active(m, c, 0); }
Value activate_team_member(Mission& m, const BuiltinCall& c) { return set_team_members_active(m, c, 1); }

// 149 set_formation_pos 0x658810: returns the unit recno, 0 without a team.
Value set_formation_pos(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    if (!u || !team_leader(m, *u)) return I(0);
    m.ai().set_formation_pos(*u, {c.f(1), c.f(2), c.f(3)}, c.f(4));
    return I(std::int16_t(c.i(0)));
}

// 150 set_team_member_pos 0x658920: always 0.
Value set_team_member_pos(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    if (u && team_leader(m, *u)) m.ai().set_team_member_pos(*u);
    return I(0);
}

// 151 kill_team 0x658ad0 (H): killer credited to the main character DAT_00a5b9e8.
Value kill_team(Mission& m, const BuiltinCall& c) {
    World& w = m.world();
    auto kill = [&](Unit& u) {
        if (w.dead(u) || !(m.data().units[size_t(u.cls)].trigger_mask & 1)) return;
        u.lives = 0;
        w.set_hp(u, 0);
        w.unit_die(u, w.player_id());
    };
    const int team = c.i(0);
    if (Unit* l = unit(m, team_leader_by_id(m, std::int16_t(team)))) {
        const std::vector<int> members = l->members;
        for (size_t i = members.size(); i-- > 0;)
            if (Unit* x = unit(m, members[i])) kill(*x);
        if ((l = unit(m, l->id))) kill(*l);
    } else {
        for_team(m, team, kill);
    }
    return I(1);
}

// 152 disable_team_drop_item 0x658d90.
Value disable_team_drop_item(Mission& m, const BuiltinCall& c) {
    for_team(m, std::int16_t(c.i(0)), [](Unit& u) { u.drop_item = 0; });
    return I(1);
}

// 153 set_team_prisoner 0x65dc20 (H).
Value set_team_prisoner(Mission& m, const BuiltinCall& c) {
    for_team(m, std::int16_t(c.i(0)), [&](Unit& u) {
        if (!u.no_react) set_prisoner(m, u, c.i(1), c.i(2));
    });
    return I(1);
}

// 154 free_team_prisoner 0x65dcf0 (H).
Value free_team_prisoner(Mission& m, const BuiltinCall& c) {
    for_team(m, std::int16_t(c.i(0)), [&](Unit& u) { free_prisoner(m, u); });
    return I(1);
}

// 155 set_team_control_flag 0x658e10.
Value set_team_control_flag(Mission& m, const BuiltinCall& c) {
    const int hidden = c.i(1) == 0;
    for_team(m, std::int16_t(c.i(0)), [&](Unit& u) { u.m.control = hidden; });
    return I(1);
}

// 387 share_team_member 0x65ef40 (H): 1 when both units lead a team.
Value share_team_member(Mission& m, const BuiltinCall& c) {
    Unit* dst = unit(m, c.i(0));
    Unit* src = unit(m, c.i(1));
    if (!dst || !src) return I(0);
    if (!m.ai().is_leader(*dst) || !m.ai().is_leader(*src)) return I(0);
    m.ai().share_team_member(*dst, *src, c.i(2));
    return I(1);
}

// ---------------------------------------------------------------------------------------------
// tasks / orders

// 140 set_idle 0x657610: Unit_stop(0,1).
Value set_idle(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    if (u) m.world().stop(*u);
    return I(u != nullptr);
}

// 162 get_curr_task 0x6520d0: inherits the leader's task (not RTS_MOVE_TO) and track +0x3c9.
Value get_curr_task(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    if (!u) return I(0);
    if (m.ai().st(*u).task.id == 0)
        if (const Unit* l = unit(m, m.ai().leader_of(*u)); l && m.ai().st(*l).task.id != task::RTS_MOVE_TO)
            u->m.track = l->m.track;
    return I(m.ai().curr_task(*u));
}

// 163 get_curr_task_para 0x652190 / 164 get_curr_task_state 0x652280 / 165 set_curr_task_state
// 0x6522f0 (invalid recno crashes the original; 0 here).
Value get_curr_task_para(Mission& m, const BuiltinCall& c) {
    const Unit* u = unit(m, std::int16_t(c.i(0)));
    return I(u ? m.ai().curr_task_para(*u, c.i(1)) : 0);
}
Value get_curr_task_state(Mission& m, const BuiltinCall& c) {
    const Unit* u = unit(m, std::int16_t(c.i(0)));
    return I(u ? m.ai().curr_task_state(*u) : 0);
}
Value set_curr_task_state(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    if (!u || (m.ai().st(*u).task.id == 0 && !m.ai().leader_of(*u))) return I(0);
    m.ai().set_curr_task_state(*u, c.i(1));
    return I(1);
}

// 166 stop_curr_task 0x652370 (FUNC only: ACTION("stop_curr_task") never reaches the host).
Value stop_curr_task(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    if (!u) return I(0);
    m.ai().clear_task(*u);
    return I(1);
}

// 167 stop_all_team_task 0x6523c0.
Value stop_all_team_task(Mission& m, const BuiltinCall& c) {
    m.ai().stop_all_team_task(std::int16_t(c.i(0)));
    return I(1);
}

// 323 give_order (ACTION 0x661060 -> Unit_give_order 0x661340): (unit, name, prio byte,
// team byte, p1..p4, duration ms).
Value give_order(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    if (!u) return I(0);
    m.ai().give_order(*u, order_id_from_name(c.s(1)), static_cast<signed char>(c.i(2)), (c.i(3) & 0xff) != 0,
                      c.i(4), c.i(5), c.i(6), c.i(7), c.i(8));
    return I(1);
}

// 323 WAIT predicate ScriptNode_give_order_poll 0x6612e0 -> 0x661ca0. Outside a running cut scene
// (status 0, or fade states 6/7/8 without the 0xa5d3bf flag) the order is cancelled and the WAIT
// ends at once; otherwise it ends when the unit no longer has the order.
bool poll_give_order(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    if (!u) return true;
    const int id = order_id_from_name(c.s(1));
    const int s = m.ui().cut_scene_status();
    if (s == 0 || ((s == 6 || s == 7 || s == 8) && !m.ui().cut_scene_skip_flag())) {
        OrderQueue& q = m.ai().st(*u).orders;
        if (q.cur.action == id) {
            m.ai().finish_current(*u);
            return true;
        }
        for (size_t i = q.q.size(); i-- > 0;)
            if (q.q[i].action == id) {
                q.q.erase(q.q.begin() + std::ptrdiff_t(i));  // OrderQueue_remove_at
                return true;
            }
    }
    return !m.ai().has_order(*u, id);
}

// 324 stop_all_order (ACTION) 0x6618f0.
Value stop_all_order(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    if (!u) return I(0);
    m.ai().stop_all_order(*u);
    return I(1);
}

// 325 get_curr_order 0x651260.
Value get_curr_order(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, std::int16_t(c.i(0)));
    return I(u ? m.ai().curr_order(*u) : 0);
}

// 326 give_task (ACTION) 0x6619d0 / 327 give_team_task 0x661b40: (unit, name, prio byte, p1, p2,
// p3, duration ms; 0 = never expires).
Value give_task(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    if (!u) return I(0);
    m.ai().give_task(*u, task_id_from_name(c.s(1)), static_cast<signed char>(c.i(2)), c.i(3), c.i(4), c.i(5), c.i(6));
    return I(1);
}
Value give_team_task(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    if (!u) return I(0);
    m.ai().give_team_task(*u, task_id_from_name(c.s(1)), static_cast<signed char>(c.i(2)), c.i(3), c.i(4), c.i(5),
                          c.i(6));
    return I(1);
}

// ---------------------------------------------------------------------------------------------
// inventory

// 156 set_unit_curr_item 0x658ea0 (unit, map-object recno (0 = empty hand), bone name, hand).
Value set_unit_curr_item(Mission& m, const BuiltinCall& c) {
    static constexpr const char* kBones[10] = {"bip01 head", "bip01 R Hand", "bip01 L Hand", "bip01 Pelvis", "cannon",
                                               "box07",      "cylinder01",   "box01",        "box05",        "box07"};
    Unit* u = unit(m, std::int16_t(c.i(0)));
    if (!u) return I(0);
    const int item = std::int16_t(c.i(1)), hand = c.i(3) & 0xff;
    if (item == 0) {
        set_hand(m, *u, hand, -1, 0);
        return I(1);
    }
    int bone = 0;  // FUN_00423220: table 0x9f748c, ids 1..10
    for (int i = 0; i < 10 && !bone; ++i)
        if (formats::iequals(kBones[i], c.s(2))) bone = i + 1;
    if (!bone) return I(0);
    set_hand(m, *u, hand, item, bone);
    return I(1);
}

Value give_item_to_unit(Mission& m, const BuiltinCall& c) { return give_item(m, c, false); }
Value give_item_to_unit_troop(Mission& m, const BuiltinCall& c) { return give_item(m, c, true); }

// Equipped slots, ranged, ammo, bag (FUN_004985f0 / FUN_004987d0 order).
template <class F> int inventory_sum(const Unit& u, F match) {
    int n = 0;
    for (const ItemInstance& it : {u.inv.melee[0], u.inv.melee[1], u.inv.ranged, u.inv.ammo})
        if (it.item && match(it.item)) n += it.quantity;
    for (const ItemInstance& it : u.inv.bag)
        if (it.item && match(it.item)) n += it.quantity;
    return n;
}

// 340 get_item_class_quantity_in_unit_inventory 0x65ca40 (H).
Value get_item_class_quantity(Mission& m, const BuiltinCall& c) {
    const Unit* u = unit(m, c.i(1));
    if (!u || c.s(0).empty()) return I(0);
    const std::string_view cls = c.s(0);
    return I(inventory_sum(*u, [&](int item) { return formats::iequals(m.data().items[size_t(item)].item_class, cls); }));
}

// 341 get_item_quantity_in_unit_inventory 0x65cac0.
Value get_item_quantity(Mission& m, const BuiltinCall& c) {
    const Unit* u = unit(m, c.i(1));
    const int id = c.i(0);
    return I(u ? inventory_sum(*u, [&](int item) { return item == id; }) : 0);
}

// 371 remove_item_from_unit_inventory 0x65cb20 -> FUN_00498970 (H): equipped / ranged / ammo slots
// holding the item are emptied whole; bag entries lose 1 each (last first) until `quantity` entries
// were hit (quantity < 1: matching entries removed). Always 1 for a valid unit.
// ponytail: the original's three bag containers are one vector here, all with the counted rule.
Value remove_item(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(1));
    if (!u) return I(0);
    const int id = c.i(0), quantity = c.i(2);
    for (int h = 0; h < 2; ++h)
        if (u->inv.melee[h].item == id && id) {
            set_hand(m, *u, h, -1, 0);
            u->inv.melee[h] = {};
        }
    if (id && u->inv.ranged.item == id) u->inv.ranged = {};
    if (id && u->inv.ammo.item == id) u->inv.ammo = {};
    auto& bag = u->inv.bag;
    int hit = 0;
    for (size_t i = bag.size(); i-- > 0;) {
        if (bag[i].item != id) continue;
        if (quantity < 1) {
            bag.erase(bag.begin() + std::ptrdiff_t(i));
            continue;
        }
        if (--bag[i].quantity == 0) bag.erase(bag.begin() + std::ptrdiff_t(i));
        if (++hit == quantity) break;
    }
    return I(1);
}

}  // namespace

void bind_troops(BuiltinTable& t) {
    t.call[42] = &add_unit_ref;
    t.call[85] = &add_unit;
    t.call[86] = &load_unit;
    t.call[87] = &del_unit;
    t.call[140] = &set_idle;
    t.call[141] = &set_unit_team;
    t.call[142] = &get_unit_team_size;
    t.call[143] = &get_team_leader;
    t.call[144] = &set_team_nation;
    t.call[145] = &set_team_nation2;
    t.call[146] = &recruit_team_member;
    t.call[147] = &deactivate_team_member;
    t.call[148] = &activate_team_member;
    t.call[149] = &set_formation_pos;
    t.call[150] = &set_team_member_pos;
    t.call[151] = &kill_team;
    t.call[152] = &disable_team_drop_item;
    t.call[153] = &set_team_prisoner;
    t.call[154] = &free_team_prisoner;
    t.call[155] = &set_team_control_flag;
    t.call[156] = &set_unit_curr_item;
    t.call[159] = &del_unit_team;
    t.call[160] = &del_unit_team_member;
    t.call[162] = &get_curr_task;
    t.call[163] = &get_curr_task_para;
    t.call[164] = &get_curr_task_state;
    t.call[165] = &set_curr_task_state;
    t.call[166] = &stop_curr_task;
    t.call[167] = &stop_all_team_task;
    t.call[323] = &give_order;
    t.poll[323] = &poll_give_order;
    t.call[324] = &stop_all_order;
    t.call[325] = &get_curr_order;
    t.call[326] = &give_task;
    t.call[327] = &give_team_task;
    t.call[333] = &give_item_to_unit;
    t.call[334] = &give_item_to_unit_troop;
    t.call[340] = &get_item_class_quantity;
    t.call[341] = &get_item_quantity;
    t.call[371] = &remove_item;
    t.call[387] = &share_team_member;
}

}  // namespace joa::game
