// Player interact handlers and indicator. Decomp: work/ghidra/export/joan_decomp.c at the FUNC
// addresses below. The searches walk the searcher's level actor list (+0x339 -> +0x8c, +0x33d
// links); here every unit is visited in ascending recno order (same rule for ties: `<=` keeps the
// last candidate at equal distance).
#include "game/interact_player.hpp"

#include <cmath>

#include "game/ai_system.hpp"
#include "game/mission.hpp"
#include "game/mount_ride.hpp"
#include "game/script_bind.hpp"

namespace joa::game {
namespace {

constexpr float kPi = 3.14159265f;

float dist3(Vec3 a, Vec3 b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// Common shape of 0x471c80 / 0x46c1f0 / 0x46fa80: nearest unit passing `ok` within r, bearing ±90.
template <class F> int nearest(Mission& m, const Unit& u, float r, F ok) {
    World& w = m.world();
    int best = 0;
    for (const Unit& o : w.units()) {
        if (o.deleted || o.id == u.id || o.id == w.player_id() || w.dead(o) || !o.m.active || !ok(o)) continue;
        const float d = dist3(o.pos, u.pos);
        const float a = w.rel_angle(u, o.pos);
        if (d <= r && a >= -90.0f && a <= 90.0f) r = d, best = o.id;
    }
    return best;
}

void result(Mission& m, const Unit& u, int target, const char* what) {
    m.emit(MissionEvent::Kind::Interact, u.id, target, what);
}

// 0x43bef0
bool free_prisoner_handler(Mission& m, Unit& u) {
    Unit* p = m.world().unit(find_prisoner(m, u, 100.0f));
    if (!p) return false;
    free_prisoner(m, *p);          // Unit_free_prisoner 0x5199b0
    m.ai().clear_task(*p);         // Unit_clear_task
    p->m.vision = 2000;            // +0x3c1
    ++m.state().citizen_saved;     // DAT_00a591c8
    result(m, u, p->id, "free_prisoner");
    return true;
}

// 0x43bd40: TRIGGER_INTERACT to the unit; its receiver (Unit_onMessage 0x75d540 case 0x200) writes
// the sender's +0x2e3 (World::on_attack_message), which the scripts poll.
bool talk_handler(Mission& m, Unit& u) {
    const int t = find_talker(m, u, 100.0f);
    if (!t) return false;
    Trigger tr;
    tr.type = 0x200, tr.target_kind = 1, tr.target_id = t, tr.src_kind = 1, tr.sender = u.id;
    tr.pos = u.pos, tr.radius = 100.0f, tr.start = tr.end = m.world().now();
    m.world().post_trigger(tr);
    result(m, u, t, "talk");
    return true;
}

// 0x43be40
bool bribe_handler(Mission& m, Unit& u) {
    Unit* t = m.world().unit(find_bribable(m, u, 200.0f));
    if (!t) return false;
    unit_bribe(m, *t, u);
    return true;   // the handler succeeds whenever a candidate exists (Unit_bribe's result is dropped)
}

// 0x43c210
bool recruit_handler(Mission& m, Unit& u) {
    const int r = m.ai().recruit_nearby(u);
    if (!r) return false;
    result(m, u, r, "recruit");
    return true;
}

// 0x43bc50: TRIGGER_INTERACT to the map item (radius 200), handled by MapObjects (gates, boxes,
// levers).
bool item_handler(Mission& m, Unit& u) {
    const int mr = find_interact_item(m, u, 200.0f);
    if (mr < 1) return false;
    Trigger tr;
    tr.type = 0x200, tr.target_kind = 2, tr.target_id = mr, tr.src_kind = 1, tr.sender = u.id;
    tr.pos = u.pos, tr.radius = 200.0f, tr.start = tr.end = m.world().now();
    m.world().post_trigger(tr);
    result(m, u, mr, "item");
    return true;
}

// 0x43bfe0: open the recruit screen (DAT_00a48959 unit, DAT_00a48955 facility, 0x6c0310(1)).
bool recruit_screen_handler(Mission& m, Unit& u) {
    const int f = find_recruit_facility(m, u);
    if (f < 1) return false;
    result(m, u, f, "recruit_screen");
    return true;
}

// 0x43c080: open the workshop screen (0x6ed3e0(1)). IRON / WOOD workshops (+0x79 != 0) open only for
// the player when ALENCON is the player and within 1024 of the workshop.
bool workshop_handler(Mission& m, Unit& u) {
    const int f = find_workshop(m, u);
    const Facility* fc = m.places().facility(f);
    if (!fc) return false;
    bool ok = fc->workshop_type == 0;
    if (!ok) {
        const int cls = m.data().unit_index("ALENCON");
        for (const Unit& a : m.world().units())
            if (!a.deleted && a.cls == cls && cls >= 0) {
                ok = a.id == u.id && dist3(a.pos, fc->pos) < 1024.0f;
                break;
            }
    }
    if (!ok) return false;
    result(m, u, f, "workshop_screen");
    return true;
}

bool facility_contains(const Facility& f, Vec3 p) {
    return f.box_min.x <= p.x && p.x <= f.box_max.x && f.box_min.y <= p.y && p.y <= f.box_max.y &&
           f.box_min.z <= p.z && p.z <= f.box_max.z;
}

// 0x514b80 / 0x5156d0: from the top, a facility of `kind` whose owner place has the unit's nation and
// whose box holds the unit.
int find_facility(Mission& m, const Unit& u, int kind) {
    auto& fs = m.places().facilities();
    for (int i = int(fs.size()); i >= 1; --i) {
        const Facility& f = fs[size_t(i - 1)];
        const Place* p = m.places().place(f.place);
        if (f.kind == kind && p && p->nation == u.nation && facility_contains(f, u.pos)) return i;
    }
    return 0;
}

}  // namespace

int find_prisoner(Mission& m, const Unit& u, float r) {   // 0x471c80
    World& w = m.world();
    return nearest(m, u, r, [&](const Unit& o) { return !w.hostile_to_player(o) && o.no_react && o.m.can_interact == 0; });
}

int find_talker(Mission& m, const Unit& u, float r) {     // 0x46c1f0
    const GameData& gd = m.data();
    return nearest(m, u, r, [&](const Unit& o) {
        return o.m.can_interact != 0 && (gd.units[size_t(o.cls)].trigger_mask & 0x200u) != 0;
    });
}

int find_bribable(Mission& m, const Unit& u, float r) {   // 0x46fa80: also |dz| <= 100
    World& w = m.world();
    return nearest(m, u, r, [&](const Unit& o) {
        return w.hostile_to_player(o) && o.m.bribe == 1 && std::fabs(o.pos.z - u.pos.z) <= 100.0f;
    });
}

int find_interact_item(Mission& m, const Unit& u, float r) {   // 0x4d6320 (modify array, from the top)
    MapObjects& ob = m.objects();
    const float yaw = u.yaw_rad();
    const float ux = std::cos(yaw), uy = std::sin(yaw);
    int best = 0;
    for (int mr = ob.modify_count(); mr >= 1; --mr) {
        const MapItem* it = ob.item(ob.modify_item(mr));
        const ObjRes* res = it ? ob.res(it->res) : nullptr;
        if (!res) continue;
        const bool gate = it->cls == 1, ladder = it->cls == 11;
        if (it->interact != 0x200 && !ladder) continue;
        const float fx = std::cos(it->dir), fy = std::sin(it->dir);
        if (it->nation != 0 && it->nation != u.nation) {
            if (gate) {   // a foreign gate opens from its back side only (0x41af80 bearing outside ±90)
                float a = std::atan2(u.pos.y - it->pos.y, u.pos.x - it->pos.x) - it->dir;
                a = std::remainder(a, 2.0f * kPi) / kPi * 180.0f;
                if (!((a <= -90.0f || a >= 90.0f) && it->hp > 0 && res->struction != 5)) continue;
            } else if (!ladder) {
                continue;
            }
        }
        if (gate) {
            if (!(it->state < 2 && (res->need_key == 0 || it->state < 1))) continue;
        } else if (ladder && u.m.climb) {
            continue;
        }
        if (ladder) {   // foot: in front, facing it; top: behind its plane, facing out (both within 90)
            const Vec3 top{it->pos.x, it->pos.y, it->pos.z + float(res->height)};
            const float side_f = fx * (u.pos.x - it->pos.x) + fy * (u.pos.y - it->pos.y);
            const float side_t = fx * (u.pos.x - top.x) + fy * (u.pos.y - top.y);
            const float facing = fx * ux + fy * uy;
            if (dist3(it->pos, u.pos) <= 90.0f && side_f >= 0.0f && facing <= 0.0f) return mr;
            if (dist3(top, u.pos) <= 90.0f && side_t <= 0.0f && facing > 0.0f) return mr;
            continue;
        }
        const float d = dist3(it->pos, u.pos);
        if (d <= r && std::abs(int(it->pos.z - u.pos.z)) <= int(r) / 3) r = d, best = mr;
    }
    return best;
}

int find_recruit_facility(Mission& m, const Unit& u) { return find_facility(m, u, Facility::Recruit); }
int find_workshop(Mission& m, const Unit& u) { return find_facility(m, u, Facility::Workshop); }

int bribe_cost(Mission& m, const Unit& t) {   // 0x4e8500 head (asm: fild/fdiv 10/fadd 1/fcomp 0.5)
    const Consts& c = m.data().c;
    const float f = std::max(0.5f, float(t.level - 21) / 10.0f + 1.0f);
    int cost = int(double(t.hp * (t.rank < 3 ? c.bri_comm : c.bri_capt)) * double(f));
    const std::string& code = m.data().units[size_t(t.cls)].code;
    if (code == "MER_EX_ARCHER" || code == "MER_EX_ARCHER_CAPTAIN") cost = int(double(cost) * double(1.6f));
    return cost;
}

bool unit_bribe(Mission& m, Unit& t, Unit& b) {
    if (t.m.bribe != 1 || b.deleted) return false;
    World& w = m.world();
    AiSystem& ai = m.ai();
    MissionState& s = m.state();
    const int cost = bribe_cost(m, t);
    if (s.gold < cost) {
        if (!s.bribe_no_gold_shown) {   // 0x4dd580 / 0x4ad710 message, once (DAT_00a591b5)
            ++s.bribe_no_gold_shown;
            result(m, b, t.id, "bribe_no_gold");
        }
        return false;
    }
    if (!w.hostile_to_player(t)) return false;
    s.gold -= cost;
    if (Unit* v = w.unit(t.m.vehicle)) {   // release the LARGE_WEAPON slot
        const auto k = size_t(t.m.slot);
        if (k < v->m.crew.size()) v->m.crew[k] = 0;
        if (k < v->m.reserved.size()) v->m.reserved[k] = 0;
        t.m.vehicle = t.m.slot = 0;
    }
    if (ai.is_leader(t) && !t.members.empty()) {
        for (int id : std::vector<int>(t.members))
            if (Unit* mu = w.unit(id)) ai.leave_team(*mu);
    } else {
        ai.leave_team(t);
    }
    t.nation = b.nation;   // Unit_set_nation 0x509250 (nation unit counts not modelled)
    ai.clear_task(t);
    ai.stop_all_order(t);
    t.target = 0, t.range_target = 0;   // +0x2d7 / +0x2db (+0x2d9 not modelled)
    for (Unit& o : w.units()) {
        if (o.deleted) continue;
        if (o.target == t.id) o.target = 0;
        if (o.range_target == t.id) o.range_target = 0;
    }
    if (t.rank < 3 && int(b.members.size()) < ai.max_troop_size(b)) ai.join_team(t, b.id);
    t.m.bribe = 2;
    ++s.enemy_bribed;
    s.enemy_bribed_cost += cost;
    m.emit(MissionEvent::Kind::Sound, 0, 0, "RTS_OTHER_NARRATOR_001");
    result(m, b, t.id, "bribe");
    return true;
}

void install_interact(Mission& m) {
    m.world().interact = [&m](Unit& u) {
        if (u.deleted || m.world().dead(u)) return false;   // UnitArray_is_deleted
        if (free_prisoner_handler(m, u) || talk_handler(m, u) || bribe_handler(m, u) || recruit_handler(m, u) ||
            item_handler(m, u))
            return true;
        // Ctrl_horse stops at the first screen that opens; on foot both are called.
        if (u.mount) return recruit_screen_handler(m, u) || workshop_handler(m, u);
        const bool a = recruit_screen_handler(m, u);
        return workshop_handler(m, u) || a;
    };
    m.world().interact_item = [&m](Unit& u) { return !u.deleted && !m.world().dead(u) && item_handler(m, u); };
}

InteractTarget interact_target(Mission& m) {   // FUN_006d3590 (the RTS branch DAT_00a5b9f8 == 4 shows no prompt)
    World& w = m.world();
    Unit* u = w.unit(w.player_id());
    if (!u || u->deleted || w.game_mode == 4) return {};
    using K = InteractTarget;
    // All five unit searches run every tick; the first non-zero (in this order) is reported.
    const int talk = find_talker(m, *u, 100.0f), bribe = find_bribable(m, *u, 200.0f);
    const int pris = find_prisoner(m, *u, 100.0f), rec = m.ai().find_recruitable(*u);
    int mount = 0;
    if (!u->mount) {
        mount = find_mount(w, *u, 2);
        if (const Unit* h = w.unit(mount); !h || h->m.rider != 0) mount = 0;   // 0x4f2170
    }
    if (talk) return {K::Talk, talk};
    if (bribe) return {K::Bribe, bribe};
    if (pris) return {K::Prisoner, pris};
    if (mount) return {K::Mount, mount};
    if (rec) return {K::Recruit, rec};
    if (const int it = find_interact_item(m, *u, 200.0f)) return {K::Item, it};
    if (const int f = find_recruit_facility(m, *u)) return {K::RecruitScreen, f};
    if (const int f = find_workshop(m, *u)) return {K::Workshop, f};
    return {};
}

const char* interact_kind_name(int k) {
    static constexpr const char* kNames[] = {"", "talk", "bribe", "prisoner", "mount", "recruit", "item", "recruit_screen", "workshop"};
    return k >= 0 && k <= 8 ? kNames[k] : "";
}

}  // namespace joa::game
