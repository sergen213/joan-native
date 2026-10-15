// RecruitWorkshop: recruit facilities and siege workshops (f_recr.cpp / f_work.cpp). The barracks and workshop
// screens (engine hud_recruit / hud_workshop) only fill these queues; the facilities spawn the units.
#include <algorithm>

#include "formats/vfs_fs.hpp"
#include "game/mission.hpp"

namespace joa::game {

// RecruitFacility_queue_order 0x468cd0 (DynArray_add of {type, team, count}).
bool Places::recruit(int facility_recno, int type, int team, int count) {
    Facility* f = facility(facility_recno);
    if (!f || f->kind != Facility::Recruit || count <= 0) return false;
    f->queue.push_back({type, team, count});
    return true;
}

// RecruitFacility_process 0x468600: available citizens (+0x93), then one queued unit per tick into the spawn
// barrack (+0x91) once its member groups are empty: SpawnBarrack_add_member_group(type, team, 1, level +0x98) and
// the barrack order OrderRec_set(IDLE, 5, facility x, y, z, 0) for 5000 ms.
void Places::tick_recruit(Mission&, Facility& f) {
    if (const Place* o = place(f.place)) f.available = int(double(o->pop) - 0.95 * double(o->pop_max));
    Facility* bf = facility(f.spawn_barrack);
    if (!bf || bf->kind != Facility::Barrack || f.queue.empty()) return;   // the original dereferences a null barrack
    SpawnBarrack& b = bf->barrack;
    if (b.groups != 0) return;
    Facility::Queued& q = f.queue.front();
    b.team = q.team;   // 0x467ea0 (as facility_barrack_add_member)
    b.type[0] = q.type, b.count[0] = 1, b.level[0] = f.recruit_level;
    b.groups = 1;
    if (Place* p = place(bf->place)) ++p->spawn_count;
    b.order = Order{};
    b.order.action = 7, b.order.prio = 5;   // order::IDLE
    b.order.p[0] = int(f.pos.x), b.order.p[1] = int(f.pos.y), b.order.p[2] = int(f.pos.z);
    b.order_ms = 5000;
    if (--q.count == 0) f.queue.erase(f.queue.begin());   // FUN_00468d20: delete entry 1
}

// Workshop_set_recruit_list 0x46b730: products from the RECRUIT.RES record (Workshop_set_product 0x46a6f0, at most
// four, cost = class Gold +0x161, crew = record int, build ms = record float x 1000), then the build queue is
// emptied (+0x112 = 0; the per-product queued counts are kept, as the original).
void Places::set_workshop_list(int facility_recno, int list) {
    Facility* f = facility(facility_recno);
    const RecruitRec* r = recruit_rec(list);
    if (!f || !r || !m_) return;
    const GameData& gd = m_->data();
    f->recruit_list = list;
    for (const RecruitRec::Entry& e : r->entries) {
        if (e.type < 1 || e.type > int(gd.units.size())) continue;
        auto it = std::find_if(f->products.begin(), f->products.end(), [&](const Facility::Product& p) { return p.type == e.type; });
        if (it == f->products.end()) {
            if (f->products.size() == 4) continue;
            it = f->products.insert(f->products.end(), Facility::Product{});
        }
        it->type = e.type, it->crew = e.crew, it->ms = std::int16_t(e.ms);   // short field +0xbc source
        it->cost = std::int16_t(gd.units[size_t(e.type - 1)].gold);
    }
    f->builds.clear();
    f->current = -1;   // ponytail: the original leaves +0x2f04 pointing into the emptied queue
}

// FUN_0046ab40: queue one weapon (needs the player unit DAT_00a47c36, a product of that type, the product's crew
// count of registered builders (+0xf0, never registered here: crew 0 in every shipped list) and < 20 queued).
bool Places::build_siege(Mission& m, int facility_recno, int type) {
    Facility* f = facility(facility_recno);
    World& w = m.world();
    const Unit* p = w.unit(w.player_id());
    if (!f || f->kind != Facility::Workshop || f->builds.size() == 0x50 || !p || p->deleted) return false;
    auto it = std::find_if(f->products.begin(), f->products.end(), [&](const Facility::Product& x) { return x.type == type; });
    if (it == f->products.end() || it->crew > 0 || it->queued >= 20) return false;
    std::uint32_t t = std::uint32_t(w.now());
    if (f->last_queue_ms >= t) t = f->last_queue_ms + 1;
    f->builds.push_back({type, 9999, 0, t, 0, it->ms});
    ++it->queued;
    f->last_queue_ms = t;
    return true;
}

// Workshop process 0x46a040: start the oldest queued weapon (RTS_SIEGE_008), spawn it after its build time at the
// first free spot around the facility rect (0x46b150), built +1, SMS "Siege weapon built." (WORKSHOP 17).
void Places::tick_workshop(Mission& m, Facility& f) {
    World& w = m.world();
    const std::uint32_t now = std::uint32_t(w.now());
    if (f.current < 0) {
        if (f.builds.empty()) return;
        int best = -1;
        std::uint32_t oldest = f.builds[0].queued_ms;
        for (int i = 0; i < int(f.builds.size()); ++i)
            if (f.builds[size_t(i)].queued_ms <= oldest) oldest = f.builds[size_t(i)].queued_ms, best = i;
        f.current = best;
        f.builds[size_t(best)].building = 1;
        f.builds[size_t(best)].start_ms = now;
        m.emit(MissionEvent::Kind::Sound, 0, 0, "RTS_SIEGE_008");   // GameAudio skips it while RTS_SIEGE_009 plays (0x473140)
        return;   // 0x46c4d0: build progress bar over the workshop (GameUiBridge workshop bars)
    }
    if (f.current >= int(f.builds.size())) {
        f.current = -1;
        return;
    }
    const Facility::Build b = f.builds[size_t(f.current)];
    if (now < b.start_ms + std::uint32_t(b.ms)) return;
    auto prod = std::find_if(f.products.begin(), f.products.end(), [&](const Facility::Product& x) { return x.type == b.type; });
    const GameData& gd = m.data();
    const Place* owner = place(f.place);
    if (prod == f.products.end() || b.type < 1 || b.type > int(gd.units.size()) || !owner) return;
    const UnitClass& uc = gd.units[size_t(b.type - 1)];
    // 0x46b150: walk the rect's sides 200 units outside it (200-unit steps), yaw facing outwards, until the spot
    // is free (0x6932d0: path space + unit shapes; here walkable terrain + cylinder overlap).
    const float r = uc.cyl_radius, off = std::max(r, 200.0f);
    auto free_at = [&](float x, float y) {
        if (w.terrain() && !w.terrain()->walkable(x, y)) return false;
        for (const Unit& o : w.units()) {
            if (o.deleted) continue;
            const float rr = r + gd.units[size_t(o.cls)].cyl_radius, dx = o.pos.x - x, dy = o.pos.y - y;
            if (dx * dx + dy * dy < rr * rr) return false;
        }
        return true;
    };
    float x = f.box_min.x - 200.0f, y = f.box_min.y - 200.0f - r, yaw = 4.712389f;
    int side = 0;
    while (side < 4 && !free_at(x, y)) {
        switch (side) {
            case 0:
                x += 200.0f, y = f.box_min.y - off, yaw = 4.712389f;
                if (x > f.box_max.x + 200.0f) x = f.box_max.x + 200.0f, side = 1;
                break;
            case 1:
                y += 200.0f, x = f.box_max.x + off, yaw = 0.0f;
                if (y > f.box_max.y + 200.0f) y = f.box_max.y + 200.0f, side = 2;
                break;
            case 2:
                x -= 200.0f, y = f.box_max.y + off, yaw = 1.5707964f;
                if (x < f.box_min.x - 200.0f) x = f.box_min.x - 200.0f, side = 3;
                break;
            default:
                y -= 200.0f, x = f.box_min.x - off, yaw = 3.1415927f;
                if (y < f.box_min.y - 200.0f) y = f.box_min.y - 200.0f, side = 4;
                break;
        }
    }
    if (side >= 4) return;   // no spot: the unit is deleted again and the weapon stays current (retried next tick)
    const float z = w.terrain() ? w.terrain()->height(x, y) : f.pos.z;
    const int id = w.spawn_unit(uc.code, owner->nation, {x, y, z}, yaw / 0.017453292f);
    Unit* u = w.unit(id);
    if (!u) return;
    u->ai_controlled = true;   // +0x102
    u->team = b.team;
    AiSystem& ai = m.ai();
    for (const Unit& o : w.units())   // Unit_find_leader_by_team_id 0x51fe60
        if (!o.deleted && o.id != u->id && o.team == u->team)
            if (const int l = ai.leader_of(o)) {
                ai.join_team(*u, l);
                break;
            }
    // 0x46a040 would queue the workshop's default order (+0x81 OrderRec, OrderQueue_add for +0x9e ms) here, but no
    // code writes +0x81 (ctor FUN_00469e90 zeroes it; no script builtin / loader / UI sets it): never any order.
    if (m.scenario().mode == 2) u->m.training = 2;
    f.builds[size_t(f.current)] = f.builds.back();   // swap-remove (0x93-byte copy of the last entry)
    f.builds.pop_back();
    --prod->queued, ++prod->built;
    f.current = -1;
    m.emit(MissionEvent::Kind::Ui, b.type, 0, "sms_siege_built");   // FUN_006e5700(WORKSHOP 17)
    m.emit(MissionEvent::Kind::Sound, 0, 0, "RTS_SIEGE_009");
}

// ---------------------------------------------------------------- screen copy (FacilityOrder)

namespace {
// BarrUI_count_troop 0x6bd4b0: UnitClass unit_class 1..6 -> slot (pole 3 -> +0x4e, mace 4 -> +0x4a).
int slot_of_class(int unit_class) {
    static constexpr int k[7] = {-1, 0, 1, 3, 2, 4, 5};
    return unit_class >= 0 && unit_class <= 6 ? k[unit_class] : -1;
}
int slot_of_type(const GameData& gd, int type) {
    if (type < 1 || type > int(gd.units.size())) return -1;
    for (int s = 0; s < 6; ++s)
        if (formats::iequals(gd.units[size_t(type - 1)].code, FacilityOrder::kRecruitCodes[s])) return s;   // 0x5270b0
    return -1;   // ponytail: the original keeps the previous row's slot for other classes (none in shipped lists)
}
}  // namespace

// BarrUI_open 0x6bda00: gold, able citizens = clamp(pop - 0.95 pop_max, 0, 20) of the owner place, the recruiting
// unit's troop by class (BarrUI_count_troop: members, the spawn barrack's pending groups and this facility's queue for
// its troop id).
FacilityOrder FacilityOrder::barracks(Mission& m, int fac, int who) {
    FacilityOrder o;
    Places& pl = m.places();
    const Facility* f = pl.facility(fac);
    if (!f || f->kind != Facility::Recruit) return o;
    o.facility = fac, o.recruiter = who, o.money = m.state().gold;
    if (const Place* own = pl.place(f->place)) o.citizens = std::clamp(int(double(own->pop) - 0.95 * double(own->pop_max)), 0, 20);
    World& w = m.world();
    Unit* p = w.unit(who);
    if (!p || p->deleted) return o;
    o.max_troop = m.ai().max_troop_size(*p);
    const GameData& gd = m.data();
    for (int id : p->members)
        if (const Unit* u = w.unit(id); u && !u->deleted)
            if (const int s = slot_of_class(gd.units[size_t(u->cls)].unit_class); s >= 0) ++o.have[size_t(s)];
    if (const Facility* b = pl.facility(f->spawn_barrack); b && b->kind == Facility::Barrack)
        for (int g = 0; g < b->barrack.groups && b->barrack.team == p->team; ++g)
            if (const int t = b->barrack.type[size_t(g)]; t >= 1 && t <= int(gd.units.size()))
                if (const int s = slot_of_class(gd.units[size_t(t - 1)].unit_class); s >= 0) o.have[size_t(s)] += b->barrack.count[size_t(g)];
    for (const auto& q : f->queue) {
        if (q.team != p->team) break;
        if (q.type >= 1 && q.type <= int(gd.units.size()))
            if (const int s = slot_of_class(gd.units[size_t(q.type - 1)].unit_class); s >= 0) o.have[size_t(s)] += q.count;
    }
    for (int n : o.have) o.total += n;
    return o;
}

// FUN_006ea020: built counts (+0xdc) per product, money = gold (NORMAL) or the facility's resource (IRON / WOOD).
FacilityOrder FacilityOrder::siege_workshop(Mission& m, int fac) {
    FacilityOrder o;
    const Facility* f = m.places().facility(fac);
    if (!f || f->kind != Facility::Workshop) return o;
    o.facility = fac, o.workshop = true;
    for (size_t i = 0; i < f->products.size() && i < 4; ++i) o.have[i] = f->products[i].built, o.total += o.have[i];
    o.money = f->workshop_type == 0 ? m.state().gold : f->resource;
    return o;
}

int FacilityOrder::row_type(Mission& m, int row) const {
    const Facility* f = m.places().facility(facility);
    if (!f || row < 0) return 0;
    if (workshop) return row < 4 && row < int(f->products.size()) ? f->products[size_t(row)].type : 0;
    const Places::RecruitRec* rec = m.places().recruit_rec(f->list1);
    return rec && row < 6 && row < int(rec->entries.size()) ? rec->entries[size_t(row)].type : 0;
}

int FacilityOrder::row_price(Mission& m, int row) const {
    const int t = row_type(m, row);
    if (t < 1 || t > int(m.data().units.size())) return 0;
    const UnitClass& uc = m.data().units[size_t(t - 1)];
    const Facility* f = m.places().facility(facility);
    return workshop && f && f->workshop_type != 0 ? uc.resource : uc.gold;
}

int FacilityOrder::slot(Mission& m, int row) const {
    if (workshop) return row_type(m, row) ? row : -1;
    return slot_of_type(m.data(), row_type(m, row));
}

bool FacilityOrder::more(Mission& m, int row) {
    const int s = slot(m, row), c = row_price(m, row);
    const Facility* f = m.places().facility(facility);
    if (s < 0 || !f) return false;
    if (workshop) {   // FUN_006ec480: while built + new < capacity (+0x7d) and the money covers it
        if (f->capacity <= total || money < cost + c) return false;
    } else {          // BarrUI_handle_input: troop max, citizens, gold
        const Unit* p = m.world().unit(recruiter);
        if ((p && !p->deleted && max_troop <= total) || citizens < 1 || money < cost + c) return false;
        --citizens, ++recruited;
    }
    cost += c, ++total, ++add[size_t(s)];
    return true;
}

bool FacilityOrder::less(Mission& m, int row) {
    const int s = slot(m, row);
    if (s < 0 || add[size_t(s)] < 1) return false;
    if (!workshop) ++citizens, --recruited;
    cost -= row_price(m, row), --total, --add[size_t(s)];
    return true;
}

// BarrUI_confirm 0x6bdc00: one queue order per class with new recruits (the recruiting unit's troop id, 9999 without
// one), gold -= cost, owner population -= recruited. FUN_006ea1a0: one queue entry per new weapon (0x46ab40, team
// 9999), then the money copy is written back (gold, or the IRON / WOOD facility's resource).
void FacilityOrder::commit(Mission& m) {
    Places& pl = m.places();
    Facility* f = pl.facility(facility);
    if (!f) return;
    if (workshop) {
        const std::vector<Facility::Product> products = f->products;
        for (size_t i = 0; i < products.size() && i < 4; ++i)
            for (int k = 0; k < add[i]; ++k) pl.build_siege(m, facility, products[i].type);
        money -= cost;
        if (f->workshop_type == 0) m.state().gold = money;
        else f->resource = money;
    } else {
        const Unit* p = m.world().unit(recruiter);
        const bool player = p && !p->deleted;
        for (int s = 0; s < 6; ++s)
            if (add[size_t(s)] != 0) pl.recruit(facility, m.data().unit_index(kRecruitCodes[s]) + 1, player ? p->team : 9999, add[size_t(s)]);
        money -= cost;
        m.state().gold = money;
        if (Place* o = pl.place(f->place)) o->pop -= recruited;
    }
    recruited = cost = 0, add = {};
}

}  // namespace joa::game
