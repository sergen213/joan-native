// Target selection, perception ranges, aggression and the small FUNC helpers the AI routines use.
// Spec: GAMEPLAY_AI_TROOPS.md §A5.2-A5.4, Part B §3B/§3C helper tables; decomp 0x54b480.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "game/ai_routines.hpp"
#include "game/nav_mesh.hpp"

namespace joa::game {

namespace {
constexpr float kRadToDeg = 57.29578f;

bool alive_or_player(const World& w, const Unit& u, int player) { return !w.dead(u) || u.id == player; }

// 0x527790: target type priority by searcher class (row) x candidate class (column 1..6, 8).
int type_priority(int searcher, int cand) {
    static const int t[9][9] = {
        {}, {0, 4, 2, 1, 3, 5, 6, 0, 0}, {0, 6, 3, 4, 5, 2, 1, 0, 0}, {0, 5, 4, 3, 6, 2, 1, 0, 0},
        {0, 4, 1, 2, 3, 6, 5, 0, 0}, {0, 2, 5, 6, 1, 4, 3, 0, 0}, {0, 2, 5, 6, 1, 4, 3, 0, 0},
        {}, {0, 1, 2, 3, 4, 5, 6, 0, 7}};
    if (searcher < 1 || searcher > 8 || cand < 0 || cand > 8) return 0;
    return t[searcher][cand];
}
}  // namespace

int AiSystem::rand(int n) { return int(w_.rng().next(std::uint32_t(n))); }

int AiSystem::class_of(const Unit& u) const { return u.cls >= 0 ? w_.data().units[std::size_t(u.cls)].unit_class : -1; }
int AiSystem::class_of(int recno) const {
    const Unit* u = w_.unit(recno);
    return u && !u->deleted ? class_of(*u) : -1;
}
int AiSystem::priority_of(const Unit& u) const { return type_priority(class_of(u), class_of(u)); }

bool AiSystem::unit_dead(int recno) const {
    const Unit* u = w_.unit(recno);
    return !u || u->deleted || w_.dead(*u);
}

bool AiSystem::busy_attacking(const Unit& u) const { return is_attacking(u.action); }

bool AiSystem::is_attacked(int recno) const {  // 0x6512d0
    const Unit* u = w_.unit(recno);
    if (!u || u->deleted) return false;
    const int a = u->action;
    return (a >= ATTACKED && a <= DIE) || (a >= KNOCKDN && a <= KNOCKDN3);
}

bool AiSystem::will_be_attacked(const Unit& victim, int attacker) const {  // 0x651430
    const Unit* a = w_.unit(attacker);
    if (!a || a->deleted) return false;
    const int s = a->action;
    if (!(s == ATTACK || (s >= ENGATK && s <= ENGATK4) || s == RUSH)) return false;
    return std::fabs(w_.rel_angle(*a, victim.pos)) < 90.0f;
}

int AiSystem::unit_range(const Unit& u, int kind) const {  // 0x65b280
    static const int r[9][4] = {{0, 0, 0, 0},         {800, 1600, 2500, 0}, {600, 1200, 2500, 0},
                                {600, 800, 2500, 0},   {1000, 2000, 2500, 0}, {250, 1200, 1600, 0},
                                {250, 1400, 1800, 0},  {0, 0, 0, 0},          {400, 1600, 1600, 3200}};
    const int c = class_of(u);
    if (c < 0 || c > 8 || kind < 1 || kind > 4) return 0;
    return r[c][kind - 1];
}

int AiSystem::base_attack_chance(const Unit& u) const {  // 0x511e70
    const auto& t = w_.data().units[std::size_t(u.cls)];
    if (t.ranking == 4 || t.ranking == 5) return 90;
    return (t.unit_class == 5 || t.unit_class == 6) ? 30 : 50;
}

int AiSystem::attack_chance(const Unit& u) const {  // 0x511f80
    const int d = w_.difficulty;
    if (d < 0 || d > 3) return u.threat;
    return int(float(u.threat) * w_.data().c.diff_a[d]);
}

int AiSystem::block_chance(const Unit& u) const {  // 0x512070
    (void)u;
    const int d = std::clamp(w_.difficulty, 0, 3);
    return int(float(w_.data().c.blockper) * w_.data().c.diff_a[d]);
}

int AiSystem::unit_dist(const Unit& a, int b) const {  // 0x653150
    const Unit* t = w_.unit(b);
    if (!t || t->deleted) return 0;
    return distance_to(a, t->pos);
}

int AiSystem::distance_to(const Unit& u, Vec3 p) const {
    const float dx = p.x - u.pos.x, dy = p.y - u.pos.y, dz = p.z - u.pos.z;
    return int(std::sqrt(dx * dx + dy * dy + dz * dz));
}

int AiSystem::item_dist(const Unit& a, int item_id) const {
    auto it = item ? item(item_id) : std::nullopt;
    return it ? distance_to(a, it->pos) : 0;
}

int AiSystem::rotation_to(const Unit& u, int target) const {  // 0x652ee0
    const Unit* t = w_.unit(target);
    // 0x652ee0 = ftol(yaw_deg - relAngle): the absolute heading toward the target, which is what the
    // TURN executor compares with the unit heading (reading the doc's "relative bearing" literally would
    // make every TURN face a meaningless direction).
    return t && !t->deleted ? int(u.yaw_deg - w_.rel_angle(u, t->pos)) : 0;
}

int AiSystem::item_rotation(const Unit& u, int item_id) const {
    auto it = item ? item(item_id) : std::nullopt;
    return it ? int(u.yaw_deg - w_.rel_angle(u, it->pos)) : 0;  // absolute heading, as rotation_to
}

bool AiSystem::line_of_fire_blocked(const Unit& u, int target) const {  // 0x5130a0
    const Unit* t = w_.unit(target);
    if (!t || t->deleted) return true;
    const ITerrainQuery* ter = w_.terrain();
    if (!ter) return false;
    const Vec3 a{u.pos.x, u.pos.y, u.pos.z + 50}, b = t->pos;
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    // ponytail: steps of 1 unit as the original; ~len samples per call, only used by siege/archer rolls.
    for (float s = 1; s < len; s += 1) {
        const float f = s / len;
        if (ter->height(a.x + f * dx, a.y + f * dy) > a.z + f * dz) return true;
    }
    return false;
}

int AiSystem::nearest_incoming_missile(const Unit& u, bool enemy_only, int radius) const {  // 0x467c30
    for (const auto& p : w_.projectiles()) {
        if (p.destroyed) continue;
        const Unit* s = w_.unit(p.shooter);
        if (!s || s->deleted) continue;
        if (u.target != 0 && u.target != p.shooter) continue;
        if (enemy_only && !w_.hostile(u.nation, s->nation)) continue;
        const float k = World::kUnitsPerMetre;
        const float dx = p.pos_m.x * k - u.pos.x, dy = p.pos_m.y * k - u.pos.y, dz = p.pos_m.z * k - u.pos.z;
        if (dx * dx + dy * dy + dz * dz <= float(radius) * float(radius)) return p.id;
    }
    return 0;
}

int AiSystem::all_attached(const Unit& u, char side) const {  // 0x656bb0
    const auto& pts = w_.data().units[std::size_t(u.cls)].attach_points;
    if (!u.m.can_attach || pts.empty()) return 0;
    const int mask = side == 'M' ? 1 : 2;
    Unit& self = const_cast<Unit&>(u);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (!(pts[i].usage & mask)) continue;
        const int c = i < u.m.crew.size() ? u.m.crew[i] : 0;
        if (c < 1) return 0;
        const Unit* cu = w_.unit(c);
        if (!cu || cu->m.vehicle != u.id) {  // stale crew entry: clear the slot
            self.m.crew[i] = 0;
            if (i < self.m.reserved.size()) self.m.reserved[i] = 0;
            return 0;
        }
    }
    return 1;
}

int AiSystem::combo_para(std::string_view s, int i) {  // 0x6511f0 / 0x4bbac0
    if (i < 1 || i > 3) return 0;
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) {
        const std::size_t idx = std::size_t((i - 1) * 4 + k);
        v = (v << 8) | (idx < s.size() ? (unsigned char)s[idx] : 0u);
    }
    return int(v);
}

bool AiSystem::inside_place(int place, int recno) const {
    auto it = places.find(place);
    const Unit* u = w_.unit(recno);
    return it != places.end() && u && !u->deleted && it->second.contains(u->pos);
}

int AiSystem::gate_state(int item_id) const {
    auto it = item ? item(item_id) : std::nullopt;
    return it && it->gate ? it->gate_open : -1;
}

bool AiSystem::can_attack_down(const Unit& u) const { return w_.can_use_special(u, DOWNATK); }  // 0x652aa0

bool AiSystem::can_rush_attack(const Unit& u) const {  // 0x652b00
    const Unit* p = w_.unit(w_.player_id());
    if (p && !p->deleted && !w_.dead(*p) && w_.hostile(u.nation, p->nation) && u.nation != 0) return true;
    return w_.can_use_special(u, RUSH);
}

bool AiSystem::can_energy_attack(const Unit& u) const {  // 0x652c10
    const Unit* p = w_.unit(w_.player_id());
    if (p && !p->deleted && u.nation != 0 && w_.hostile(u.nation, p->nation)) return true;
    const auto& c = w_.data().c;
    // HYPOTHESIS: Inventory_aggregate_effect(0x15) taken as 100 % (no energy-cost items on AI units).
    const int mp = int(float(w_.max_ep(u)) * float(c.egatk_mp) / 100.0f);
    const int hp = int(float(w_.max_hp(u)) * float(c.egatk_hp) / 100.0f);
    if (u.ep < mp || u.hp <= 3 * hp) return false;
    return w_.can_use_special(u, ENGATK);
}

bool AiSystem::facing_knock_down_enemy(const Unit& u) const {  // 0x652820
    for (const auto& o : w_.units()) {
        if (o.deleted || o.id == u.id || !(o.action >= KNOCKDN && o.action <= KNOCKDN3)) continue;
        if (o.nation == 0 || !w_.hostile(u.nation, o.nation)) continue;
        const float dx = o.pos.x - u.pos.x, dy = o.pos.y - u.pos.y, dz = o.pos.z - u.pos.z;
        if (dx * dx + dy * dy + dz * dz > 3600.0f) continue;
        if (std::fabs(w_.rel_angle(u, o.pos)) <= 90.0f) return true;
    }
    return false;
}

int AiSystem::search_for_unit(Unit& u, std::string_view type, int radius, bool centre_on_leader, int place,
                              int max_yaw, int max_pitch) {
    if (type.empty()) return 0;
    if (radius == 0) radius = 1600;
    if (type[0] == 'T') return w_.unit(u.target) && !w_.unit(u.target)->deleted ? u.target : 0;
    const int player = w_.player_id();
    const int lid = leader_of(u);
    Vec3 c = u.pos;
    if (centre_on_leader && lid != 0 && lid != u.id) c = w_.unit(lid)->pos;
    else centre_on_leader = false;
    const bool by_place = type.size() > 1 && type[1] == 'P';
    const float eye = float(int(float(w_.data().units[std::size_t(u.cls)].height) * 0.8f));
    auto los_ok = [&](const Unit& t) {
        return !nav_ || !nav_->segment_blocked({u.pos.x, u.pos.y, u.pos.z}, {t.pos.x, t.pos.y, t.pos.z}, eye);
    };
    auto pitch_deg = [&](const Unit& t) {  // 0x505b50
        const float dx = t.pos.x - u.pos.x, dy = t.pos.y - u.pos.y;
        return std::fabs(std::atan2(t.pos.z - u.pos.z, std::sqrt(dx * dx + dy * dy)) * kRadToDeg);
    };
    auto d2 = [](Vec3 a, Vec3 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z); };
    auto in_cart_slot = [&](const Unit& t) {
        const Unit* wpn = w_.unit(t.m.vehicle);  // riding in a cart / siege-tower slot (flag 2)
        if (!wpn) return false;
        const auto& wt = w_.data().units[std::size_t(wpn->cls)];
        return wt.special == 0xb && t.m.slot >= 0 && std::size_t(t.m.slot) < wt.attach_points.size() &&
               wt.attach_points[std::size_t(t.m.slot)].usage == 2;
    };

    // Sticky current target ('E').
    if (const Unit* t = w_.unit(u.target); t && !t->deleted && type[0] == 'E') {
        bool keep = false;
        if (!w_.dead(*t) && t->retreat != -1 && d2(c, t->pos) < float(radius) * float(radius) && rand(100) < 96) {
            keep = !in_cart_slot(*t);
            if (keep && by_place) {
                auto pl = places.find(place);
                keep = pl != places.end() && pl->second.contains(t->pos);
            }
            if (keep && max_pitch != 0 && int(pitch_deg(*t)) > max_pitch) keep = false;
            if (keep && los_ok(*t)) return u.target;
        }
        u.target = 0;
    }

    if (type[0] == 'L') {
        if (lid != 0) {
            const Unit& l = *w_.unit(lid);
            if (!w_.dead(l) && l.retreat != -1) return d2(c, l.pos) < float(radius) * float(radius) ? lid : 0;
        }
        return -1;  // original quirk: no leader -> -1
    }

    const float r2 = float(radius) * float(radius);
    float best = r2;
    int res = 0, best_metric = -1, best_rank = -1, best_prio = 0;
    const int cap = radius < 200 ? 2 : radius < 1000 ? 5 : radius < 2000 ? 10 : radius < 3000 ? 20 : 100;
    int accepted = 0;
    const int my_class = class_of(u);
    // ponytail: linear scan in recno order instead of the scene-sector walk (same candidates, different
    // visiting order); fine for <= a few hundred units.
    for (auto& o : w_.units()) {
        if (accepted > cap) break;
        if (o.deleted || o.id == u.id) continue;
        if ((o.hp < 1 && o.lives == 0) || o.retreat == -1) continue;
        float dd = d2(c, o.pos);
        if (dd > r2) continue;
        const auto& ot = w_.data().units[std::size_t(o.cls)];
        bool ok = false;
        switch (type[0]) {
        case 'P': ok = o.id == player; break;
        case 'E':
            if (o.nation == 0 || !alive_or_player(w_, o, player) || !w_.hostile(u.nation, o.nation)) break;
            if (!los_ok(o) || !(ot.trigger_mask & 1) || ot.unit_class == 8 || in_cart_slot(o)) break;
            if (by_place) {
                auto pl = places.find(place);
                if (pl == places.end() || !pl->second.contains(o.pos)) break;
            }
            ok = true;
            break;
        case 'S':
            ok = o.nation != 0 && alive_or_player(w_, o, player) && w_.hostile(u.nation, o.nation) &&
                 ot.unit_class == 8 && (ot.trigger_mask & 0x40) && los_ok(o);
            break;
        case 'F':  // 0x5095e0: same nation, or allied (HYPOTHESIS: "not hostile" stands for relation > 3)
            ok = u.nation != 0 && o.nation != 0 && (o.nation == u.nation || !w_.hostile(u.nation, o.nation));
            break;
        case 'O': ok = o.m.training != 0; break;
        default: break;
        }
        if (!ok) continue;
        int rad = radius;
        if (ot.unit_class == 0 && rad > 399) rad = int(double(rad) / 1.2);
        const float rr = float(rad) * float(rad);
        if (dd > rr) continue;
        if (max_yaw != 0 && int(std::fabs(w_.rel_angle(u, o.pos))) > max_yaw) continue;
        if (max_pitch != 0 && int(pitch_deg(o)) > max_pitch) continue;
        ++accepted;
        switch (u.attack_behaviour) {
        case ai::NEAREST:
            if (centre_on_leader) dd = d2(u.pos, o.pos);
            if (rr <= best) { best = dd; res = o.id; }
            if (dd < best && rand(100) < 51) { best = dd; res = o.id; }
            break;
        case ai::WEAKEST: {
            const int m = o.lives * w_.max_hp(o) + o.hp;
            if (best_metric == -1) { res = o.id; best_metric = m; }
            if (m < best_metric && rand(100) < 51) { best_metric = m; res = o.id; }
            break;
        }
        case ai::IMPORTANT:
            if (best_rank == -1) { best_rank = o.rank; res = o.id; }
            if (best_rank < o.rank && rand(100) < 51) { best_rank = o.rank; res = o.id; }
            break;
        case ai::NEAREST_HIGH_PRIORITY:
            if (rr <= best || res == 0) {
                best = dd; res = o.id; best_prio = type_priority(my_class, ot.unit_class);
            } else {
                const int cp = type_priority(my_class, ot.unit_class);
                if (best_prio < cp) { best = dd; res = o.id; best_prio = cp; }
                else if (cp == best_prio && dd < best && rand(100) < 81) { best = dd; res = o.id; }
            }
            break;
        case ai::NEAREST_ARCHER:
            if (rr <= best || res == 0) {
                best = dd; res = o.id;
            } else {
                const int bc = class_of(res);
                const bool cand_arch = ot.unit_class == 5 || ot.unit_class == 6, best_arch = bc == 5 || bc == 6;
                if (cand_arch) {
                    if (!best_arch) { best = dd; res = o.id; }
                    else if (dd < best && rand(100) < 81) { best = dd; res = o.id; }
                } else if (best_arch) {
                    if (dd < best / 2.0f) { best = dd; res = o.id; }
                } else if (dd < best && rand(100) < 81) {
                    best = dd; res = o.id;
                }
            }
            break;
        default: break;
        }
    }
    if (type[0] == 'E' && rand(100) < 51) {
        const Unit* t = w_.unit(u.target);
        if (t && !t->deleted && !w_.dead(*t) && t->retreat != -1) {
            if (res == 0) return 0;
            u.target = res;
            return res;
        }
        u.target = res;
    }
    return res;
}

int AiSystem::search_for_item(Unit& u, std::string_view type, int radius, bool centre_on_leader) {  // 0x503680
    // HYPOTHESIS (decomp only partly read): nearest enemy item matching the type letters within radius;
    // "EG?" gate closed(C)/open(O) with hp > 0, "EC" closed gate/wall/box with hp > 0, "E" any enemy item.
    if (!item || !item_ids || type.empty() || type[0] != 'E') return 0;
    const int lid = leader_of(u);
    Vec3 c = u.pos;
    if (centre_on_leader && lid != 0 && lid != u.id) c = w_.unit(lid)->pos;
    float best = float(radius) * float(radius);
    int res = 0;
    for (int id : item_ids()) {
        auto it = item(id);
        if (!it || it->nation == 0 || !w_.hostile(u.nation, it->nation)) continue;
        if (type.size() > 1 && type[1] == 'G') {
            if (!it->gate) continue;
            if (type.size() > 2 && type[2] == 'C' && !(it->gate_open == 0 && it->hp > 0)) continue;
            if (type.size() > 2 && type[2] == 'O' && it->gate_open != 1) continue;
        } else if (type.size() > 1 && type[1] == 'C') {
            if (!(it->gate || it->wall || it->box) || it->hp <= 0 || (it->gate && it->gate_open != 0)) continue;
        }
        const float dx = it->pos.x - c.x, dy = it->pos.y - c.y, dz = it->pos.z - c.z;
        const float dd = dx * dx + dy * dy + dz * dz;
        if (dd < best) { best = dd; res = id; }
    }
    return res;
}

}  // namespace joa::game
