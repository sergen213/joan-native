// Script builtins of the objects slice (SCRIPT_VM.md §8): items, gates, walls, boxes on the
// mission item array (MapObjects). Executor VAs per handler; confidence noted when not CONFIRMED.
// Common deviation: the original dereferences invalid recnos / wrong-class objects (crash or
// garbage) in the cases marked "no check"; here those read 0 / do nothing.
#include <cmath>

#include "game/mission.hpp"
#include "game/script_bind.hpp"

namespace joa::game {
namespace {

Unit* unit(Mission& m, int r) {
    Unit* u = m.world().unit(r);
    return u && !u->deleted ? u : nullptr;
}
MapItem* obj(Mission& m, int r) { return m.objects().item(r); }
MapItem* obj_cls(Mission& m, int r, int cls) {
    MapItem* it = obj(m, r);
    return it && it->cls == cls ? it : nullptr;
}
Value I(int v) { return Value::integer(v); }

// 5 search_for_item 0x6519d0: alive unit -> Unit_search_for_item 0x503680 (AiSystem).
Value search_for_item(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    if (!u || m.world().dead(*u)) return I(0);
    return I(m.ai().search_for_item(*u, c.s(1), c.i(2), c.i(3) != 0));
}
// 6 get_item_rotation 0x653000 (HIGH): (int)(unit facing deg - relative angle unit->item).
Value get_item_rotation(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    const MapItem* it = obj(m, c.i(1));
    if (!u || !it) return I(0);
    return I(int(u->yaw_deg - m.world().rel_angle(*u, it->pos)));
}
// 7 get_item_dist 0x653060
Value get_item_dist(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    const MapItem* it = obj(m, c.i(1));
    if (!u || !it) return I(0);
    const float dx = it->pos.x - u->pos.x, dy = it->pos.y - u->pos.y, dz = it->pos.z - u->pos.z;
    return I(int(std::sqrt(dx * dx + dy * dy + dz * dz)));
}
// 9 item_code_to_id 0x651a30
Value item_code_to_id(Mission& m, const BuiltinCall& c) { return I(m.objects().res_by_code(c.s(0))); }
// 10 add_item_ref 0x651a60 (HIGH): resource refcount / preload list only.
Value add_item_ref(Mission&, const BuiltinCall&) { return I(1); }
// 11 add_item 0x651ad0: create, Pickable -> floor marker, owner nation. Unknown code: 0
// (the original creates from ItemRes 0).
Value add_item(Mission& m, const BuiltinCall& c) {
    MapObjects& o = m.objects();
    const int id = o.res_by_code(c.s(0));
    const int r = id ? o.create(m, id) : 0;
    if (!r) return I(0);
    MapItem& it = *o.item(r);
    const ObjRes& res = *o.res(id);
    if (res.pickable) it.on_floor = true, it.level = res.min_level, it.quantity = res.quantity;
    it.nation = m.data().nation_id(c.s(1));
    return I(r);
}
// 12 del_item 0x651e70
Value del_item(Mission& m, const BuiltinCall& c) {
    m.objects().del_item(m, c.i(0));
    return I(1);
}
// 13 del_item_by_name 0x651ea0
Value del_item_by_name(Mission& m, const BuiltinCall& c) {
    while (const int r = m.objects().find_by_name(c.s(0))) m.objects().del_item(m, r);
    return I(1);
}
// 14 set_item_pos 0x651f00
Value set_item_pos(Mission& m, const BuiltinCall& c) {
    MapItem* it = obj(m, c.i(0));
    if (!it) return I(0);
    it->pos = {c.f(1), c.f(2), c.f(3)};
    return I(c.i(0));
}
// 15 set_item_dir 0x651fa0
Value set_item_dir(Mission& m, const BuiltinCall& c) {
    MapItem* it = obj(m, c.i(0));
    if (!it) return I(0);
    it->dir = float(c.i(1)) * 0.017453292f;
    return I(1);
}
// 16 set_item_nation 0x652010
Value set_item_nation(Mission& m, const BuiltinCall& c) {
    MapItem* it = obj(m, c.i(0));
    if (!it) return I(0);
    it->nation = m.data().nation_id(c.s(1));
    return I(it->nation);
}
// 17 add_item_box 0x651c90 (code, name, x, y, z, degrees, content)
Value add_item_box(Mission& m, const BuiltinCall& c) {
    MapObjects& o = m.objects();
    const int id = o.res_by_code(c.s(0));
    const int r = id ? o.create(m, id) : 0;
    if (!r) return I(0);
    MapItem& it = *o.item(r);
    it.pos = {c.f(2), c.f(3), c.f(4)};
    it.dir = float(c.i(5)) * 0.017453292f;
    it.name = std::string(c.s(1).substr(0, 64));
    o.box_content(m, r, c.s(6));
    return I(r);
}
// 18 search_item_by_name 0x65b570 / 19 search_item_by_code 0x65b5b0
Value search_item_by_name(Mission& m, const BuiltinCall& c) { return I(m.objects().find_by_name(c.s(0))); }
Value search_item_by_code(Mission& m, const BuiltinCall& c) { return I(m.objects().find_by_code(c.s(0))); }
// 20 search_item_sell_list_by_code 0x65b5f0 -> FUN_004ce900
Value search_item_sell_list_by_code(Mission& m, const BuiltinCall& c) { return I(m.objects().sell_list(c.s(0))); }
// 21 is_item_deleted_by_name 0x65b630
Value is_item_deleted_by_name(Mission& m, const BuiltinCall& c) {
    const MapItem* it = obj(m, c.i(0));
    return I(!it || !formats::iequals(it->name, c.s(1)) ? 1 : 0);
}
// 22 is_item_box 0x65b6a0 / 23 is_item_gate 0x65b700 / 24 is_item_wall 0x65b760
Value is_item_box(Mission& m, const BuiltinCall& c) { return I(obj_cls(m, c.i(0), 10) ? 1 : 0); }
Value is_item_gate(Mission& m, const BuiltinCall& c) { return I(obj_cls(m, c.i(0), 1) ? 1 : 0); }
Value is_item_wall(Mission& m, const BuiltinCall& c) { return I(obj_cls(m, c.i(0), 4) ? 1 : 0); }
// 26 get_gate_hp 0x650b90 / 27 get_gate_max_hp 0x650c10
Value get_gate_hp(Mission& m, const BuiltinCall& c) {
    const MapItem* g = obj_cls(m, c.i(0), 1);
    return I(g ? g->hp : 0);
}
Value get_gate_max_hp(Mission& m, const BuiltinCall& c) {
    const MapItem* g = obj_cls(m, c.i(0), 1);
    return I(g ? g->max_hp : 0);
}
// 28 set_gate_hp 0x650c90 / 29 set_gate_max_hp 0x650cf0 (no check; always 0)
Value set_gate_hp(Mission& m, const BuiltinCall& c) {
    if (MapItem* g = obj_cls(m, c.i(0), 1)) g->hp = c.i(1);
    return I(0);
}
Value set_gate_max_hp(Mission& m, const BuiltinCall& c) {
    if (MapItem* g = obj_cls(m, c.i(0), 1)) g->max_hp = c.i(1);
    return I(0);
}
// 30 gate_open 0x652440: state of modify item #1 (does not open anything).
Value gate_open(Mission& m, const BuiltinCall&) {
    const MapItem* g = obj_cls(m, m.objects().modify_item(1), 1);
    return I(g ? int(static_cast<signed char>(g->state)) : -1);
}
// 31 get_gate_state 0x6524a0
Value get_gate_state(Mission& m, const BuiltinCall& c) {
    const MapItem* g = obj_cls(m, c.i(0), 1);
    return I(g ? int(static_cast<signed char>(g->state)) : -1);
}
// 32 set_gate_need_key 0x652520 / 33 get_gate_need_key 0x652590
Value set_gate_need_key(Mission& m, const BuiltinCall& c) {
    MapItem* it = obj(m, c.i(0));
    if (!it) return I(-1);
    if (it->cls == 1) it->need_key = c.i(1) & 0xff;
    return I(int(static_cast<signed char>(c.i(1))));
}
Value get_gate_need_key(Mission& m, const BuiltinCall& c) {
    MapItem* it = obj(m, c.i(0));
    if (!it) return I(-1);
    return I(it->cls == 1 ? int(static_cast<signed char>(it->need_key)) : 0);
}
// 34 set_gate_invincible 0x6525f0
Value set_gate_invincible(Mission& m, const BuiltinCall& c) {
    MapItem* it = obj(m, c.i(0));
    if (!it) return I(-1);
    if (it->cls == 1) it->invincible = c.i(1) & 0xff;
    return I(int(static_cast<signed char>(c.i(1))));
}
// 35 get_gate_attacked 0x652660
Value get_gate_attacked(Mission& m, const BuiltinCall& c) {
    const MapItem* g = obj_cls(m, c.i(0), 1);
    return I(g ? int(static_cast<signed char>(g->attacked)) : -1);
}
// 36 get_wall_hp 0x650d50 / 37 set_wall_hp 0x650db0 / 38 get_wall_max_hp 0x650e10
Value get_wall_hp(Mission& m, const BuiltinCall& c) {
    const MapItem* w = obj_cls(m, c.i(0), 4);
    return I(w ? w->hp : 0);
}
Value set_wall_hp(Mission& m, const BuiltinCall& c) {
    if (MapItem* w = obj_cls(m, c.i(0), 4)) w->hp = c.i(1);
    return I(0);
}
Value get_wall_max_hp(Mission& m, const BuiltinCall& c) {
    const MapItem* w = obj_cls(m, c.i(0), 4);
    return I(w ? w->max_hp : 0);
}
// 39 break_wall 0x650e70 (Y): Trigger(0x40, objects, target = ITEM recno (quirk: TriggerSys reads it
// as a modify recno), src unit, item pos, r 1000, now). Always 0.
Value break_wall(Mission& m, const BuiltinCall& c) {
    const MapItem* it = obj(m, c.i(0));
    if (!unit(m, c.i(1)) || !it) return I(0);
    Trigger t;
    t.type = 0x40, t.target_kind = 2, t.target_id = c.i(0), t.src_kind = 1, t.sender = c.i(1);
    t.pos = it->pos, t.radius = 1000.0f, t.start = t.end = m.world().now();
    m.world().post_trigger(t);
    return I(0);
}
// 40 set_wall_invincible 0x650f40
Value set_wall_invincible(Mission& m, const BuiltinCall& c) {
    MapItem* it = obj(m, c.i(0));
    if (!it) return I(-1);
    if (it->cls == 4) it->invincible = c.i(1) & 0xff;
    return I(int(static_cast<signed char>(c.i(1))));
}
// 179 change_all_item_in_place 0x654240 -> FUN_004c4440 (HIGH)
Value change_all_item_in_place(Mission& m, const BuiltinCall& c) {
    if (const Place* p = m.places().place(c.i(0)))
        m.objects().change_in_box(m, p->box_min, p->box_max, c.i(1), c.i(2), p->nation);
    return I(1);
}
// 240 change_mission_gold_to_real_gold 0x659b80: boxes named s7_goldbag (ascending recnos).
Value change_mission_gold_to_real_gold(Mission& m, const BuiltinCall&) {
    MapObjects& o = m.objects();
    const int gold = o.res_by_code("S7_LGOLDBAG");
    for (int r = 1; r <= o.size(); ++r)
        if (MapItem* it = o.item(r); it && it->cls == 10 && formats::iequals(it->name, "s7_goldbag"))
            it->content = gold, it->name = "S7_Barrels";
    return I(1);
}
// 332 add_gate_trigger 0x65c530 (HIGH): Trigger(0x20, objects, item modify recno, src = player,
// r 1.0, fires at now + delay). The original's position is uninitialised stack; item pos here.
Value add_gate_trigger(Mission& m, const BuiltinCall& c) {
    const MapItem* it = obj(m, c.i(0));
    if (!it) return I(0);
    Trigger t;
    t.type = 0x20, t.target_kind = 2, t.target_id = it->modify, t.src_kind = 1, t.sender = m.world().player_id();
    t.pos = it->pos, t.radius = 1.0f, t.start = t.end = m.world().now() + c.i(1);
    m.world().post_trigger(t);
    return I(1);
}
// 372 set_item_can_interact_flag 0x65e930
Value set_item_can_interact_flag(Mission& m, const BuiltinCall& c) {
    MapItem* it = obj(m, c.i(0));
    if (!it) return I(0);
    it->interact = c.i(1) ? 0x200 : 0;
    return I(1);
}
// 374 is_item_on_floor 0x65ea00 (returns the marker pointer: 1 here)
Value is_item_on_floor(Mission& m, const BuiltinCall& c) {
    const MapItem* it = obj(m, c.i(0));
    return I(it && it->on_floor ? 1 : 0);
}
// 375-377 get_item_pos_x/y/z 0x65ea50 / 0x65ead0 / 0x65eb50 (marker pos == item pos here)
Value get_item_pos_x(Mission& m, const BuiltinCall& c) { const MapItem* it = obj(m, c.i(0)); return Value::real(it ? it->pos.x : 0.0f); }
Value get_item_pos_y(Mission& m, const BuiltinCall& c) { const MapItem* it = obj(m, c.i(0)); return Value::real(it ? it->pos.y : 0.0f); }
Value get_item_pos_z(Mission& m, const BuiltinCall& c) { const MapItem* it = obj(m, c.i(0)); return Value::real(it ? it->pos.z : 0.0f); }
// 388 get_item_angle_diff 0x65f0f0 (HIGH)
Value get_item_angle_diff(Mission& m, const BuiltinCall& c) {
    Unit* u = unit(m, c.i(0));
    const MapItem* it = obj(m, c.i(1));
    if (!u || !it) return I(0);
    return I(int(m.world().rel_angle(*u, it->pos)));
}

}  // namespace

void bind_objects(BuiltinTable& t) {
    t.call[5] = &search_for_item;
    t.call[6] = &get_item_rotation;
    t.call[7] = &get_item_dist;
    t.call[9] = &item_code_to_id;
    t.call[10] = &add_item_ref;
    t.call[11] = &add_item;
    t.call[12] = &del_item;
    t.call[13] = &del_item_by_name;
    t.call[14] = &set_item_pos;
    t.call[15] = &set_item_dir;
    t.call[16] = &set_item_nation;
    t.call[17] = &add_item_box;
    t.call[18] = &search_item_by_name;
    t.call[19] = &search_item_by_code;
    t.call[20] = &search_item_sell_list_by_code;
    t.call[21] = &is_item_deleted_by_name;
    t.call[22] = &is_item_box;
    t.call[23] = &is_item_gate;
    t.call[24] = &is_item_wall;
    t.call[26] = &get_gate_hp;
    t.call[27] = &get_gate_max_hp;
    t.call[28] = &set_gate_hp;
    t.call[29] = &set_gate_max_hp;
    t.call[30] = &gate_open;
    t.call[31] = &get_gate_state;
    t.call[32] = &set_gate_need_key;
    t.call[33] = &get_gate_need_key;
    t.call[34] = &set_gate_invincible;
    t.call[35] = &get_gate_attacked;
    t.call[36] = &get_wall_hp;
    t.call[37] = &set_wall_hp;
    t.call[38] = &get_wall_max_hp;
    t.call[39] = &break_wall;
    t.call[40] = &set_wall_invincible;
    t.call[179] = &change_all_item_in_place;
    t.call[240] = &change_mission_gold_to_real_gold;
    t.call[332] = &add_gate_trigger;
    t.call[372] = &set_item_can_interact_flag;
    t.call[374] = &is_item_on_floor;
    t.call[375] = &get_item_pos_x;
    t.call[376] = &get_item_pos_y;
    t.call[377] = &get_item_pos_z;
    t.call[388] = &get_item_angle_diff;
    t.unimplemented[314] = "drop_item_on_floor: needs the unit hand-item model (FUN_0050a480, held_item); unused by shipped scripts";
    t.unimplemented[362] = "set_item_path_item_owner: PATH-class items (vtbl+100) absent from shipped maps; unused by shipped scripts";
}

}  // namespace joa::game
