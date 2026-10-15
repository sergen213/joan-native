// Script builtins of the places slice: places, facilities (barracks / shops / recruit /
// workshops), territories and nation relations (SCRIPT_VM.md §8; executor VA per handler).
// Unit iteration inside place areas walks the place's scene sectors in the original
// (World_query_levels + per-sector actor lists); here units are visited in ascending recno
// order (HYPOTHESIS: only the first match of 169 can differ).
#include <cmath>

#include "game/mission.hpp"
#include "game/script_bind.hpp"

namespace joa::game {

namespace {

Value I(int v) { return Value::integer(v); }

Unit* live(World& w, int id) {
    Unit* u = w.unit(id);
    return u && !u->deleted ? u : nullptr;
}

// Diplomacy_isHostile FUN_004b6860: same nation 0, either nation 0 -> 1, else relation flag.
bool hostile(const World& w, int a, int b) { return a == b ? false : (a == 0 || b == 0) ? true : w.hostile(a, b); }

// FUN_004b91a0 + FUN_004b67e0: nation code -> nation recno (0 unknown).
int nation_of(Mission& m, std::string_view code) { return m.data().nation_id(code); }

bool alive_in(const Unit& u, const Place& p) {  // hp < 1 && lives == 0 skipped, then AABB_contains
    return !u.deleted && !(u.hp < 1 && u.lives == 0) && u.pos.x >= p.box_min.x && u.pos.x <= p.box_max.x &&
           u.pos.y >= p.box_min.y && u.pos.y <= p.box_max.y && u.pos.z >= p.box_min.z && u.pos.z <= p.box_max.z;
}
// Non-combat types skipped by the "fightable" / by-nation counters: type class 0/8/9, special 2/3/7.
bool non_combat(const GameData& gd, const Unit& u) {
    const UnitClass& c = gd.units[size_t(u.cls)];
    return c.unit_class == 0 || c.unit_class == 8 || c.unit_class == 9 || c.special == 7 || c.special == 2 || c.special == 3;
}

// FUN_0049c470 family (171-177). hostile_mode: units hostile to nation `ref` (unit param's
// nation); else units of nation `ref`. Barrack tail: pending leader / member groups of the
// place's barracks count when the place nation is hostile to (equal to) `ref`.
int place_people(Mission& m, const Place& p, int ref, bool hostile_mode, bool fightable, bool generals, bool exist, bool barracks) {
    World& w = m.world();
    const GameData& gd = m.data();
    int n = 0;
    for (const Unit& u : w.units()) {
        if (!alive_in(u, p) || u.cls < 0) continue;
        if ((fightable || generals) && non_combat(gd, u)) continue;
        if (generals && u.rank < 4) continue;
        const bool match = hostile_mode ? (u.nation != 0 && hostile(w, ref, u.nation)) : u.nation == ref;
        if (!match) continue;
        if (exist) return 1;
        ++n;
    }
    if (!barracks) return n;
    // Place nation object (FUN_0047b600): a place without a valid nation crashes the original.
    if (p.nation < 1 || p.nation > int(gd.nations.size())) return n;
    if (hostile_mode ? !hostile(w, p.nation, ref) : p.nation != ref) return n;
    for (int fid : p.facilities) {
        const Facility* f = m.places().facility(fid);
        if (!f || f->kind != Facility::Barrack) continue;
        const SpawnBarrack& b = f->barrack;
        if (b.leader_type != 0) ++n;
        for (int g = 0; g < b.groups; ++g) {
            if (!exist) n += b.count[size_t(g)];
            else {
                if (n + b.count[size_t(g)] != 0) return 1;  // exist variants: leader alone only counts via the return
                n = 0;
            }
        }
    }
    return n;
}

SpawnBarrack* barrack(Mission& m, int fac) {  // vtbl+0x18 (only BARRACK returns an object)
    Facility* f = m.places().facility(fac);
    return f && f->kind == Facility::Barrack ? &f->barrack : nullptr;
}
Facility* recruit(Mission& m, int fac) {  // vtbl+0x24 (only RECRUIT)
    Facility* f = m.places().facility(fac);
    return f && f->kind == Facility::Recruit ? f : nullptr;
}

// 25 search_recruit_list_by_code 0x65b7c0 (C)
Value f25(Mission& m, const BuiltinCall& c) { return I(m.places().recruit_list_by_code(c.s(0))); }

// 65 is_inside_facility 0x653ca0 (C): no unit validity check; facility map == unit map always here.
bool inside_facility(Mission& m, int unit, int fac) {
    const Unit* u = m.world().unit(unit);
    const Facility* f = m.places().facility(fac);
    if (!u || !f || !m.places().place(f->place)) return false;
    return f->box_min.x <= u->pos.x && u->pos.x <= f->box_max.x && f->box_min.y <= u->pos.y && u->pos.y <= f->box_max.y &&
           f->box_min.z <= u->pos.z && u->pos.z <= f->box_max.z;
}
Value f65(Mission& m, const BuiltinCall& c) { return I(inside_facility(m, std::int16_t(c.i(0)), c.i(1))); }

// 66 is_unit_inside_facility 0x653d60 (C): highest recno first.
Value f66(Mission& m, const BuiltinCall& c) {
    const auto& us = m.world().units();
    for (int i = int(us.size()); i >= 1; --i)
        if (!us[size_t(i - 1)].deleted && inside_facility(m, std::int16_t(i), c.i(0))) return I(i);
    return I(0);
}

// 111 set_units_in_place_can_bribe_flag 0x656030 (H; the original tests the type only on the
// first actor of each sector list, here every unit of the type is tested)
Value f111(Mission& m, const BuiltinCall& c) {
    const Place* p = m.places().place(std::int16_t(c.i(1)));
    if (!p) return I(0);
    for (Unit& u : m.world().units())
        if (!u.deleted && u.cls + 1 == std::int16_t(c.i(0)) && m.places().inside(p->recno, u.pos) && u.m.bribe != 2)
            u.m.bribe = std::int16_t(c.i(2));
    return I(1);
}

// 168 search_place_by_name 0x653eb0 (C)
Value f168(Mission& m, const BuiltinCall& c) { return I(m.places().place_by_name(c.s(0))); }

// 169 search_for_unit_in_place 0x653ef0 -> FUN_0049c240 (H)
Value f169(Mission& m, const BuiltinCall& c) {
    const Place* p = m.places().place(std::int16_t(c.i(0)));
    if (!p) return I(0);
    World& w = m.world();
    const GameData& gd = m.data();
    const char mode = c.s(1).empty() ? 0 : c.s(1)[0];
    const int nn = int(gd.nations.size());
    const bool pn_valid = p->nation >= 1 && p->nation <= nn;
    for (const Unit& u : w.units()) {
        if (!alive_in(u, *p)) continue;
        if (mode == 'P') {
            if (u.id == w.player_id()) return I(u.id);
        } else if (u.cls >= 0 && (gd.units[size_t(u.cls)].trigger_mask & 0x2443) != 0) {
            if (mode == 'E') {
                if (hostile(w, p->nation, u.nation)) return I(u.id);
            } else if (mode == 'F') {
                const bool un_valid = u.nation >= 1 && u.nation <= nn;
                if (pn_valid && un_valid && (p->nation == u.nation || m.places().relation(p->nation, u.nation).status > 3))
                    return I(u.id);
            }
        }
    }
    return I(0);
}

// 170 is_unit_inside_place 0x653f30 (C)
Value f170(Mission& m, const BuiltinCall& c) {
    const Unit* u = live(m.world(), c.i(1));
    return I(u && m.places().place(c.i(0)) && m.places().inside(c.i(0), u->pos));
}

// 171-174: (place, unit) counters / existence tests vs the unit's nation (H).
Value unit_query(Mission& m, const BuiltinCall& c, bool fightable, bool exist) {
    const Place* p = m.places().place(std::int16_t(c.i(0)));
    const Unit* u = live(m.world(), std::int16_t(c.i(1)));
    if (!p || !u) return I(0);
    return I(place_people(m, *p, u->nation, true, fightable, false, exist, true));
}
Value f171(Mission& m, const BuiltinCall& c) { return unit_query(m, c, false, false); }  // 0x653fe0 -> 0x49c470
Value f172(Mission& m, const BuiltinCall& c) { return unit_query(m, c, true, false); }   // 0x654020 -> 0x49e500
Value f173(Mission& m, const BuiltinCall& c) { return unit_query(m, c, false, true); }   // 0x654060 -> 0x4a65d0
Value f174(Mission& m, const BuiltinCall& c) { return unit_query(m, c, true, true); }    // 0x6540a0 -> 0x4a8c20

// 175-177: (place, nation code). Per code (CONFIRMED): 175 -> FUN_004ab020 is the existence
// test, 176 -> FUN_004b79a0 the count (names are swapped vs behaviour), 177 counts generals.
Value nation_query(Mission& m, const BuiltinCall& c, bool generals, bool exist) {
    const Place* p = m.places().place(std::int16_t(c.i(0)));
    if (!p) return I(0);
    return I(place_people(m, *p, nation_of(m, c.s(1)), false, true, generals, exist, !generals));
}
Value f175(Mission& m, const BuiltinCall& c) { return nation_query(m, c, false, true); }   // 0x6540e0
Value f176(Mission& m, const BuiltinCall& c) { return nation_query(m, c, false, false); }  // 0x654120
Value f177(Mission& m, const BuiltinCall& c) { return nation_query(m, c, true, false); }   // 0x654160

// 178 get_spawn_unit_count_in_place 0x6541a0 (C)
Value f178(Mission& m, const BuiltinCall& c) {
    const Place* p = m.places().place(std::int16_t(c.i(0)));
    return I(p ? p->spawn_count : 0);
}

// 180 get_place_captured 0x654280 (C)
Value f180(Mission& m, const BuiltinCall& c) {
    const Place* p = m.places().place(c.i(0));
    return I(p ? p->captured : 0);
}

// Tail of 181/182: alive siege weapons (type class 8) inside the place area not of `nation`
// switch side; gates/walls in the area change owner.
void convert_area(Mission& m, const Place& p, int nation) {
    World& w = m.world();
    for (int i = int(w.units().size()); i >= 1; --i) {
        Unit& u = w.units()[size_t(i - 1)];
        if (u.deleted || u.nation == nation || w.dead(u) || u.cls < 0 || m.data().units[size_t(u.cls)].unit_class != 8) continue;
        if (m.places().inside(p.recno, u.pos)) u.nation = nation;
    }
    m.objects().set_owner_in_box(p.box_min, p.box_max, nation);
}

// 181 set_place_captured 0x6542b0 (H): the place nation itself is not changed.
Value f181(Mission& m, const BuiltinCall& c) {
    Place* p = m.places().place(c.i(0));
    if (!p) return I(1);
    const int flag = c.i(1);
    p->captured = flag;
    const int fr = nation_of(m, "FRENCH"), en = nation_of(m, "ENGLISH");
    int side = en;
    if (p->nation == fr) {
        if (flag == 0) side = fr;
    } else {
        if (p->nation != en) return I(1);
        if (flag != 0) side = fr;
    }
    convert_area(m, *p, side);
    return I(1);
}

// 182 set_place_nation_by_code 0x654560 (H)
Value f182(Mission& m, const BuiltinCall& c) {
    Place* p = m.places().place(c.i(0));
    const int n = nation_of(m, c.s(1));
    if (!p || n == 0) return I(1);
    p->nation = n;  // FUN_0042acd0 -> Place::set_nation 0x44f5c0 (facility callbacks are no-ops)
    convert_area(m, *p, n);
    return I(1);
}

// 183 get_place_unit_view_angle 0x6547a0 (C) / 184 set_place_unit_view_angle 0x6547e0 (C).
// Stored as degrees in AiSystem's place hook (the original keeps radians at place+0x1d2).
Value f183(Mission& m, const BuiltinCall& c) { return I(m.places().place(c.i(0)) ? m.ai().place_unit_view_angle(c.i(0)) : 0); }
Value f184(Mission& m, const BuiltinCall& c) {
    if (m.places().place(c.i(0))) m.ai().set_place_unit_view_angle(c.i(0), c.i(1));
    return I(1);
}

// 185 set_parking_dir 0x654830 (C)
Value f185(Mission& m, const BuiltinCall& c) {
    if (Place* p = m.places().place(c.i(0))) {
        const float a = float(c.i(1)) * 0.017453292f;
        for (float& d : p->parking_dir) d = a;
        m.ai().places[p->recno].slot_facing = p->parking_dir;
    }
    return I(1);
}

// 186 search_facility_by_name 0x6548b0 (C): from the top.
Value f186(Mission& m, const BuiltinCall& c) { return I(m.places().facility_by_name(c.s(0))); }

// 187 facility_barrack_add_leader 0x6548f0 -> SpawnBarrack_add_leader 0x467dd0 (H)
Value f187(Mission& m, const BuiltinCall& c) {
    const int fac = std::int16_t(c.i(0));
    if (SpawnBarrack* b = barrack(m, fac); b && b->leader_type == 0) {
        b->leader_type = std::int16_t(c.i(1));
        b->team = std::int16_t(c.i(2));
        b->leader = 0;
        b->leader_level = std::int16_t(c.i(3));
        b->name = std::string(c.s(4).substr(0, 64));
        b->title = std::string(c.s(5).substr(0, 64));
        if (Place* p = m.places().place(m.places().facility(fac)->place)) ++p->spawn_count;
    }
    return I(1);
}

// 188 facility_barrack_add_member 0x654960 -> SpawnBarrack_add_member_group 0x467ea0 (H)
Value f188(Mission& m, const BuiltinCall& c) {
    const int fac = std::int16_t(c.i(0));
    if (SpawnBarrack* b = barrack(m, fac); b && b->groups < 4) {
        const int count = std::int16_t(c.i(3));
        b->team = std::int16_t(c.i(2));
        b->type[size_t(b->groups)] = std::int16_t(c.i(1));
        b->count[size_t(b->groups)] = count;
        b->level[size_t(b->groups)] = std::int16_t(c.i(4));
        ++b->groups;
        if (Place* p = m.places().place(m.places().facility(fac)->place)) p->spawn_count += count;
    }
    return I(1);
}

// 189 get_unit_facility_distance 0x6549c0 (C): 3-D length to the facility point, 0xffff invalid.
Value f189(Mission& m, const BuiltinCall& c) {
    const Facility* f = m.places().facility(c.i(1));
    const Unit* u = live(m.world(), c.i(0));
    if (!f || !u) return I(0xffff);
    const float dx = f->pos.x - u->pos.x, dy = f->pos.y - u->pos.y, dz = f->pos.z - u->pos.z;
    return I(int(std::sqrt(dx * dx + dy * dy + dz * dz)));
}

// 190 set_facility_follow_unit_recno 0x654ae0 (H)
Value f190(Mission& m, const BuiltinCall& c) {
    if (SpawnBarrack* b = barrack(m, c.i(0))) b->follow = c.i(1);
    return I(1);
}

// 191 get_spawning_count 0x654b30 (C)
Value f191(Mission& m, const BuiltinCall& c) {
    int n = 0;
    if (const SpawnBarrack* b = barrack(m, std::int16_t(c.i(0))))
        for (int g = 0; g < b->groups; ++g) n += b->count[size_t(g)];
    return I(n);
}

// 192 reset_spawn_count_in_facility 0x6541d0 (C)
Value f192(Mission& m, const BuiltinCall& c) {
    const int fac = std::int16_t(c.i(0));
    if (const Facility* f = m.places().facility(fac))
        if (Place* p = m.places().place(f->place)) p->spawn_count = 0;
    if (SpawnBarrack* b = barrack(m, fac)) b->reset();
    return I(1);
}

// 193 set_can_recruit_flag 0x654bc0 (C) / 194 set_recruit_level 0x654c20 (C)
Value f193(Mission& m, const BuiltinCall& c) {
    if (Facility* f = recruit(m, std::int16_t(c.i(0)))) f->can_recruit = std::uint8_t(c.i(1));
    return I(1);
}
Value f194(Mission& m, const BuiltinCall& c) {
    if (Facility* f = recruit(m, std::int16_t(c.i(0)))) f->recruit_level = std::int8_t(c.i(1));
    return I(1);
}

// 195 set_barrack_captured 0x654c80 (H). The flag-model swap (RecruitFacility_update_flag_model)
// is visual only.
Value f195(Mission& m, const BuiltinCall& c) {
    Facility* f = recruit(m, std::int16_t(c.i(0)));
    if (!f || std::int16_t(c.i(1)) != 1) return I(1);
    if (Place* p = m.places().place(f->place)) {
        p->captured = 1;
        if (const Unit* pl = m.world().unit(m.world().player_id())) p->nation = pl->nation;
        p->pop = p->pop_max;
    }
    if (SpawnBarrack* b = barrack(m, f->spawn_barrack)) b->reset();
    return I(1);
}

// 196 facility_barrack_add_leader_order 0x654d70 (H): (fac, order, prio, unused, p1..p4, ms).
Value f196(Mission& m, const BuiltinCall& c) {
    if (SpawnBarrack* b = barrack(m, std::int16_t(c.i(0)))) {
        b->order.action = order_id_from_name(c.s(1));
        b->order.prio = std::int8_t(c.i(2));
        for (int i = 0; i < 4; ++i) b->order.p[i] = c.i(size_t(4 + i));
        b->order_ms = c.i(8);
    }
    return I(1);
}

// 197 shop_set_sell_item_list 0x654e30 (H) / 198 set_can_regen_sell_item 0x654ea0 (C).
// Return 1 for a valid facility recno, 0 otherwise.
Value f197(Mission& m, const BuiltinCall& c) {
    Facility* f = m.places().facility(std::int16_t(c.i(0)));
    if (f && f->kind == Facility::Shop) m.places().set_sell_list(f->recno, c.i(1));
    return I(f != nullptr);
}
Value f198(Mission& m, const BuiltinCall& c) {
    Facility* f = m.places().facility(std::int16_t(c.i(0)));
    if (f && f->kind == Facility::Shop) f->can_regen = c.i(1);
    return I(f != nullptr);
}

// 199 barrack_set_recruit_list 0x654f10 -> RecruitFacility_set_recruit_list 0x468790 (H)
Value f199(Mission& m, const BuiltinCall& c) {
    const int fac = std::int16_t(c.i(0));
    if (!m.places().facility_valid(fac)) return I(0);
    if (Facility* f = recruit(m, fac)) {
        if (const auto* r1 = m.places().recruit_rec(std::uint16_t(c.i(2)))) {
            f->list1 = c.i(2);
            if (r1->level > 0) f->recruit_level = std::int8_t(r1->level);
            if (r1->population > 0)
                if (Place* p = m.places().place(f->place)) p->pop_max = p->pop = r1->population;
            if (m.places().recruit_rec(std::uint16_t(c.i(3)))) f->list2 = c.i(3);
        }
        f->spawn_barrack = std::int16_t(c.i(1));
    }
    return I(1);
}

// 200 workshop_set_recruit_list 0x654fa0 -> Workshop_set_recruit_list 0x46b730 (H): product table + empty queue.
Value f200(Mission& m, const BuiltinCall& c) {
    Facility* f = m.places().facility(std::int16_t(c.i(0)));
    if (!f) return I(0);
    if (f->kind == Facility::Workshop) m.places().set_workshop_list(f->recno, std::uint16_t(c.i(1)));
    return I(1);
}

// 201 set_nation_friendship 0x653df0 (C)
Value f201(Mission& m, const BuiltinCall& c) {
    const int a = nation_of(m, c.s(0)), b = nation_of(m, c.s(1));
    if (a == 0 || b == 0) return I(0);
    m.places().relation(a, b).friendship = c.i(2);
    return I(1);
}

// 202 set_relation_status 0x655010 -> FUN_00499770 (H)
Value f202(Mission& m, const BuiltinCall& c) {
    const int a = nation_of(m, c.s(0)), b = nation_of(m, c.s(1));
    if (a == 0 || b == 0) return I(0);
    m.places().set_relation_status(m, a, b, std::uint8_t(c.i(2)));
    return I(1);
}

// 203 set_nation_military_strength 0x6550d0 (C): validation only, FUN_004b5580 is empty.
Value f203(Mission& m, const BuiltinCall& c) { return I(nation_of(m, c.s(0)) != 0); }

// 355 get_team_size 0x65d8a0 (C): nations from the top, each nation's unit list from its end
// (HYPOTHESIS: list order = recno order); first leader of team `id` -> its member count.
Value f355(Mission& m, const BuiltinCall& c) {
    World& w = m.world();
    const int id = c.i(0);
    for (int n = int(m.data().nations.size()); n >= 1; --n)
        for (int i = int(w.units().size()); i >= 1; --i) {
            const Unit& u = w.units()[size_t(i - 1)];
            if (!u.deleted && u.nation == n && u.team == id && m.ai().is_leader(u)) return I(int(std::int8_t(u.members.size())));
        }
    return I(0);
}

// 397 set_territory_nation 0x65f850 (C)
Value f397(Mission& m, const BuiltinCall& c) {
    int n = nation_of(m, c.s(1));
    if (n == 0 && upper(c.s(1)) == "NEUTRAL") n = -1;
    if (Territory* t = m.places().territory(m.places().territory_by_name(c.s(0)))) {
        t->owner = n;
        t->under_attack = 0;
    }
    return I(1);
}

}  // namespace

void bind_places(BuiltinTable& t) {
    t.call[25] = &f25;
    t.call[65] = &f65;
    t.call[66] = &f66;
    t.call[111] = &f111;
    t.call[168] = &f168;
    t.call[169] = &f169;
    t.call[170] = &f170;
    t.call[171] = &f171;
    t.call[172] = &f172;
    t.call[173] = &f173;
    t.call[174] = &f174;
    t.call[175] = &f175;
    t.call[176] = &f176;
    t.call[177] = &f177;
    t.call[178] = &f178;
    t.call[180] = &f180;
    t.call[181] = &f181;
    t.call[182] = &f182;
    t.call[183] = &f183;
    t.call[184] = &f184;
    t.call[185] = &f185;
    t.call[186] = &f186;
    t.call[187] = &f187;
    t.call[188] = &f188;
    t.call[189] = &f189;
    t.call[190] = &f190;
    t.call[191] = &f191;
    t.call[192] = &f192;
    t.call[193] = &f193;
    t.call[194] = &f194;
    t.call[195] = &f195;
    t.call[196] = &f196;
    t.call[197] = &f197;
    t.call[198] = &f198;
    t.call[199] = &f199;
    t.call[200] = &f200;
    t.call[201] = &f201;
    t.call[202] = &f202;
    t.call[203] = &f203;
    t.call[355] = &f355;
    t.call[397] = &f397;
}

}  // namespace joa::game
