// Campaign bot (test / diagnostic only). See autopilot_player.hpp.
#include "game/autopilot_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "game/multimap_nav.hpp"

namespace joa::game {

Autopilot::Autopilot(Mission& m) : plan_(make_plan(m.info().stem())), m_(m) {}

namespace {
constexpr float kDeg = 0.017453292f;
float d2(Vec3 a, Vec3 b) { return std::hypot(b.x - a.x, b.y - a.y); }
std::string at(Vec3 p) {
    char b[48];
    std::snprintf(b, sizeof b, "(%.0f,%.0f)", p.x, p.y);
    return b;
}
bool alive(const World& w, const Unit& u) { return !u.deleted && !w.dead(u) && u.hp > 0; }
}  // namespace

// --- decisions ----------------------------------------------------------------------------------

void Autopilot::steer(InputFrame& f, const Unit& p, Vec3 g, bool replan) {
    // A gate opened / closed (by anyone) frees / shuts its passage: the old path is stale.
    unsigned gates = 0;
    for (int r = 1; r <= m_.objects().size(); ++r)
        if (const MapItem* it = m_.objects().item(r); it && it->cls == 1) gates = gates * 31 + unsigned(it->state);
    if (gates != gates_) gates_ = gates, replan = true;
    // Path on the mission navmesh (generous expansion budget: a player knows the roads).
    if (replan || d2(path_goal_, g) > 250 || tick_ - path_tick_ > 300 || wp_ >= path_.size()) {
        path_.clear(), wp_ = 0, path_goal_ = g, path_tick_ = tick_;
        if (NavWorld* nav = m_.nav()) {
            PathQuery q;
            q.from = {p.pos.x, p.pos.y, p.pos.z};
            q.to = {g.x, g.y, g.z};
            q.max_expand = 4000;
            for (const NavPt& n : nav->find_path(q).points) path_.push_back({n.x, n.y, n.z});
        }
        if (path_.empty()) path_.push_back(g);
    }
    while (wp_ + 1 < path_.size() && d2(p.pos, path_[wp_]) < 70) ++wp_;
    const Vec3 t = path_[wp_];
    f.camera_yaw_deg = std::atan2(t.y - p.pos.y, t.x - p.pos.x) / kDeg;
    f.held.set(IN_FORWARD);
    // No progress over 90 ticks: strafe and jump for 40, then replan. Stuck again (a house corner the
    // path mesh cuts, a unit in a doorway): run off sideways, alternating sides and further each time.
    if (tick_ - check_tick_ >= 90) {
        if (d2(check_pos_, p.pos) < 40) {
            ++stuck_count_, unstick_dir_ ^= 1, path_tick_ = -1000;
            unstick_until_ = tick_ + (stuck_count_ < 2 ? 40 : 30 + 20 * std::min(stuck_count_, 8));
        } else {
            stuck_count_ = 0;
        }
        check_pos_ = p.pos, check_tick_ = tick_;
    }
    if (tick_ < unstick_until_) {
        if (stuck_count_ >= 2) f.camera_yaw_deg += unstick_dir_ ? 90.0f : -90.0f;
        else f.held.set(unstick_dir_ ? IN_STRAFE_LEFT : IN_STRAFE_RIGHT);
        if (tick_ + 30 == unstick_until_) f.press(IN_JUMP);
    }
}

// u is told to follow the player (FOLLOW_UNIT on her: mission 1's Jean, mission 4's wagon and villagers).
bool Autopilot::following(const Unit& u) const {
    return m_.ai().curr_task(u) == task::FOLLOW_UNIT && m_.ai().curr_task_para(u, 1) == m_.world().player_id();
}

// Where the player's people are: the centroid of the units not hostile to the player (the town's people).
Vec3 Autopilot::home() const {
    const World& w = m_.world();
    float sx = 0, sy = 0;
    int n = 0;
    for (const Unit& u : w.units())
        if (alive(w, u) && u.id != w.player_id() && !w.hostile_to_player(u)) sx += u.pos.x, sy += u.pos.y, ++n;
    return n ? Vec3{sx / float(n), sy / float(n), 0} : Vec3{};
}

// Hero key (Mission::switch_hero 1..4: Joan, Jean, La Hire, Alencon), 0 for anyone else.
int Autopilot::hero_key(const Unit& u) const {
    const std::string& c = m_.world().data().units[std::size_t(u.cls)].code;
    return c == "JOAN_OF_ARC" ? 1 : c == "JEAN_DE_METZ" ? 2 : c == "LAHIRE" ? 3 : c == "ALENCON" ? 4 : 0;
}

// A friend in the player's team dropping a MISSION item (mission 1's letter bearer, team 1 once escorted).
const Unit* Autopilot::bearer() const {
    const World& w = m_.world();
    const Unit* p = w.unit(w.player_id());
    for (const Unit& u : w.units())
        if (p && p->team && u.id != p->id && alive(w, u) && u.team == p->team && !w.hostile_to_player(u) && u.drop_item > 0 &&
            u.drop_item < int(w.data().items.size()) && w.data().items[std::size_t(u.drop_item)].item_class == "MISSION")
            return &u;
    return nullptr;
}

// How far q lies on the friendly side of a gate: its facing points out (the gate interact refuses a foreign unit
// in front of it, FUN_0041af80; mission 2's Castle Gate faces the English camp).
static float gate_depth(const MapItem& g, Vec3 q) { return -(std::cos(g.dir) * (q.x - g.pos.x) + std::sin(g.dir) * (q.y - g.pos.y)); }

// The friendly side of a gate: 100 behind it.
Vec3 Autopilot::gate_inside(const MapItem& g) const {
    return {g.pos.x - std::cos(g.dir) * 100, g.pos.y - std::sin(g.dir) * 100, g.pos.z};
}

// Walkable regions: (nav level, path-mesh component) of the poly under q (one off the mesh, a tent at a camp's centre,
// takes the nearest poly); -1 without a nav mesh. A closed gate splits a component (NavMesh::set_blocked).
int Autopilot::region(Vec3 q) const {
    const NavWorld* nav = m_.nav();
    if (!nav) return -1;
    int poly = nav->find_poly_xy(q.x, q.y, q.z);
    if (const int lv = nav->level_at(q.x, q.y); poly < 0 && lv >= 0) {
        const NavLevel& l = nav->levels()[std::size_t(lv)];
        if (const int n = l.mesh.nearest_poly(q.x - l.ox, q.y - l.oy, q.z); n >= 0) poly = l.base + n;
    }
    return poly < 0 ? -1 : nav->level_of_poly(poly) * 100000 + nav->poly(poly).component;
}

// Regions of different levels meet at the level seams: a poly reaching its level's box edge, and the open poly of the next
// level just across it at about the same height. Ladders (map item class 11) lead from the region at their foot (60 in
// front) to the one at their top (60 behind their plane, at the item height: a wall walkway). Rebuilt when a gate changes
// state (components change with it).
void Autopilot::build_seams() {
    const NavWorld* nav = m_.nav();
    if (!nav || (seam_gates_ == gates_ && seams_built_)) return;
    seam_gates_ = gates_, seams_built_ = true, seams_.clear(), linked_.clear(), ladders_.clear(), no_ladder_.clear();
    const MapObjects& o = m_.objects();
    for (int r = 1; r <= o.size(); ++r)
        if (const MapItem* l = o.item(r); l && l->cls == 11)
            if (const ObjRes* res = o.res(l->res)) {
                const float fx = std::cos(l->dir), fy = std::sin(l->dir);
                const Vec3 ft{l->pos.x + fx * 60, l->pos.y + fy * 60, l->pos.z}, top{l->pos.x - fx * 60, l->pos.y - fy * 60, l->pos.z + float(res->height)};
                ladders_.push_back({r, ft, top, region(ft), region(top)});
            }
    const auto& lv = nav->levels();
    for (int li = 0; lv.size() > 1 && li < int(lv.size()); ++li) {
        const NavLevel& l = lv[std::size_t(li)];
        for (int qi = 0; qi < int(l.mesh.polys().size()); ++qi) {
            const NavPoly& q = l.mesh.polys()[std::size_t(qi)];
            if (q.state == 1) continue;
            const float x0 = float(q.bbox[0]) + l.ox, y0 = float(q.bbox[1]) + l.oy, x1 = float(q.bbox[3]) + l.ox, y1 = float(q.bbox[4]) + l.oy;
            const Vec3 c{float(q.cx) + l.ox, float(q.cy) + l.oy, float(q.cz)};
            Vec3 across = c, in = c;  // 64 past the box edge in the next level / 64 short of it in this one
            if (x1 > l.x1 - 64) across.x = l.x1 + 64, in.x = l.x1 - 64;
            else if (x0 < l.ox + 64) across.x = l.ox - 64, in.x = l.ox + 64;
            else if (y1 > l.y1 - 64) across.y = l.y1 + 64, in.y = l.y1 - 64;
            else if (y0 < l.oy + 64) across.y = l.oy - 64, in.y = l.oy + 64;
            else continue;
            in.z = nav->poly_height(l.base + qi, in.x, in.y);
            const int o = nav->find_poly_xy(across.x, across.y, in.z);
            if (o < 0 || nav->level_of_poly(o) == li || nav->poly(o).state == 1) continue;
            across.z = nav->poly_height(o, across.x, across.y);
            if (std::fabs(across.z - in.z) > 100) continue;
            seams_.push_back({li * 100000 + q.component, nav->level_of_poly(o) * 100000 + nav->poly(o).component, across, in});
        }
    }
}

// Whether the player can walk from region a to region b (unknown regions count as linked).
bool Autopilot::linked(int a, int b) {
    if (a == b || a < 0 || b < 0) return true;
    build_seams();
    if (const auto it = linked_.find({a, b}); it != linked_.end()) return it->second;
    std::set<int> seen{a};
    std::vector<int> todo{a};
    while (!todo.empty() && !seen.count(b)) {
        const int cur = todo.back();
        todo.pop_back();
        for (const Seam& s : seams_)
            if (s.a == cur && seen.insert(s.b).second) todo.push_back(s.b);
    }
    return linked_[{a, b}] = seen.count(b) > 0;
}

// region() of a unit, cached for 30 ticks (target selection asks for many units every tick).
int Autopilot::unit_region(const Unit& u) {
    auto& [t, r] = unit_region_[u.id];
    if (t <= 0 || tick_ - t > 30) t = tick_, r = region(u.pos);
    return r;
}

// A foe the player could not get at is left alone for 60 s, twice as long each further time (up to 8 min), while it stays
// within 300 of where it stood: the verdict is about that spot. Mission 6 Normal gave up a squad below Meung's wall walk
// (Joan on it, the path to them ending short), which then walked 800 to the Bastard's camp and killed him (LOSE) beside
// a Joan that took none of them on.
void Autopilot::give_up(int id) {
    Unreach& u = unreach_[id];
    u.until = tick_ + (1800 << std::min(u.times++, 3));
    if (const Unit* f = m_.world().unit(id)) u.at = f->pos;
}
bool Autopilot::unreached(const Unit& u) const {
    const auto it = unreach_.find(u.id);
    return it != unreach_.end() && tick_ < it->second.until && d2(u.pos, it->second.at) < 300;
}

// The centre of the first enabled, unfinished WIN objective's territory (the minimap shows the towns / camps) not given up on
// (travel there made no headway: mission 4's Tourelles lies behind its shut gate); `index` its end-condition index. The
// centre's height is one on a region the player can walk to (mission 4: Saint Augustin's centre is both the tower's walled-in
// ground floor and its upper floor). False if none.
bool Autopilot::open_territory(Vec3& g, int* index) {
    const Unit* p = m_.world().unit(m_.world().player_id());
    for (std::size_t i = 0; i < m_.state().end.size(); ++i) {
        const EndCondition& c = m_.state().end[i];
        if (!c.has_des || !c.enabled || c.type != EndCondition::Win || c.finish == 1 || c.territory_link.empty()) continue;
        if (const auto s = territory_skip_.find(int(i)); s != territory_skip_.end() && tick_ < s->second) continue;
        if (const Territory* t = m_.places().territory(m_.places().territory_by_name(c.territory_link))) {
            g = {float(t->center_x), float(t->center_y), p ? p->pos.z : g.z};
            const int here = p ? region(p->pos) : -1;
            for (float z : {g.z, 0.0f, 200.0f, 400.0f, 600.0f, -200.0f, -400.0f, -600.0f})
                if (linked(here, region({g.x, g.y, z}))) {
                    g.z = z;
                    break;
                }
            if (index) *index = int(i);
            return true;
        }
    }
    return false;
}

// Multi-level worlds (one nav mesh per level, scenarios 2-8): the engine's planner stays on the start level's mesh when
// the goal lies on it, so a region of that level cut off by a closed gate (mission 3: the plateau before the shut front
// gate of camp du beau vallon, whose way round is the road below through the next level) only gets a path to the gate.
// The bot routes like a player who knows the map: over regions linked at the level seams, Dijkstra by straight-line
// distance, and returns the first seam crossing on the way (null when no detour applies).
const Autopilot::Seam* Autopilot::detour(const Unit& p, Vec3 g) {
    const int from = region(p.pos), to = region(g);
    if (from < 0 || to < 0 || from == to) return nullptr;
    build_seams();
    // Dijkstra over regions; a region's position is where the route first entered it.
    std::map<int, float> cost{{from, 0.0f}};
    std::map<int, std::pair<Vec3, int>> at;  // region -> (entry point, first seam of the route to it)
    at[from] = {p.pos, -1};
    std::set<int> done;
    for (;;) {
        int cur = -1;
        for (const auto& [n, c] : cost)
            if (!done.count(n) && (cur < 0 || c < cost[cur])) cur = n;
        if (cur < 0) {  // every way left runs through a crossing that failed: try them all again
            if (bad_vias_.empty()) return nullptr;
            bad_vias_.clear();
            return detour(p, g);
        }
        if (cur == to) return at[cur].second < 0 ? nullptr : &seams_[std::size_t(at[cur].second)];
        done.insert(cur);
        for (std::size_t i = 0; i < seams_.size(); ++i)
            if (const Seam& s = seams_[i]; s.a == cur && !done.count(s.b) &&
                                           std::none_of(bad_vias_.begin(), bad_vias_.end(), [&](Vec3 v) { return d2(v, s.at) < 300; })) {
                const float c = cost[cur] + d2(at[cur].first, s.at);
                if (!cost.count(s.b) || c < cost[s.b]) cost[s.b] = c, at[s.b] = {s.at, cur == from ? int(i) : at[cur].second};
            }
    }
}

// Walk to g. The path mesh shuts a closed gate's passage, so a path ending well short of g, or one leaving it by a
// step no one walks (mission 2: up the wall stairs and off the wall top onto the coarse world-mesh leg outside, a
// 520 drop), with a closed interactable gate near that point is blocked by that gate: walk up to it and open it
// (R at its indicator). A gate the bot opened itself is closed behind it once the player is back through it
// (clear of the door leaves) and heading for somewhere inside (mission 2 stage 4 only calls the second wave
// with the gate shut).
void Autopilot::travel(InputFrame& f, const Unit& p, Vec3 g) {
    const MapObjects& o = m_.objects();
    travel_tick_ = tick_;
    if (const MapItem* it = o.item(opened_gate_);
        it && it->state == 1 && d2(p.pos, it->pos) < 1000 && gate_depth(*it, p.pos) > 60 && gate_depth(*it, g) > 0) {
        goal += " (close gate behind)";
        shut_tick_ = tick_;
        if (const Vec3 in = gate_inside(*it); d2(p.pos, in) > 60) steer(f, p, in, false);
        else f.mouse_dx = m_.world().rel_angle(p, it->pos) / 0.18f;
        return;
    }
    // A gate held shut against a host outside (8+ within 2500: mission 2 stage 5, where opening it sends the second
    // wave at the town hall) is not opened from inside; also not once the walk to it is under way (the bot stood at mission
    // 2's Castle Gate for the whole stage while the English cannons broke it).
    const auto held_shut = [&](const MapItem& it) {
        if (gate_depth(it, p.pos) <= 0) return false;
        int out = 0;
        for (const Unit& u : m_.world().units())
            out += alive(m_.world(), u) && m_.world().hostile_to_player(u) && d2(u.pos, it.pos) < 2500 && gate_depth(it, u.pos) < 0;
        return out >= 8;
    };
    if (const MapItem* it = o.item(open_gate_); it && it->state == 0 && d2(open_goal_, g) < 500 && !held_shut(*it)) {
        goal += " via gate";
        if (d2(p.pos, it->pos) > 100) steer(f, p, it->pos, false);
        return;
    }
    // A region no walk reaches (a wall walkway, or the ground from one): by a ladder.
    if (const int to = region(g); !linked(region(p.pos), to) && use_ladder(f, p, to)) return;
    // A cross-level detour under way (detour()): walk to its seam on this level (the engine plans a goal on another level
    // over the coarse world mesh: mission 3 walked away from a crossing 150 off), then straight across. A seam not reached
    // in 90 s or not crossed in 10 s is not tried again.
    if (via_tick_ >= 0 && d2(via_goal_, g) < 1500 && d2(p.pos, via_) > 60) {
        if (!via_cross_ && d2(p.pos, via_in_) <= 60) via_cross_ = true, via_tick_ = tick_;
        if (tick_ - via_tick_ < (via_cross_ ? 300 : 2700)) {
            goal += " detour " + at(via_);
            if (!via_cross_) {
                steer(f, p, via_in_, false);
            } else {
                f.camera_yaw_deg = std::atan2(via_.y - p.pos.y, via_.x - p.pos.x) / kDeg;
                f.held.set(IN_FORWARD);
            }
            return;
        }
        bad_vias_.push_back(via_), detour_tick_ = -1000;
    }
    via_tick_ = -1;
    open_gate_ = 0;
    steer(f, p, g, false);
    Vec3 stop = path_.back();
    bool blocked = d2(stop, g) >= 300 || std::fabs(stop.z - g.z) >= 150;
    for (std::size_t i = 0; !blocked && i + 1 < path_.size(); ++i)
        if (std::fabs(path_[i + 1].z - path_[i].z) > 200) stop = path_[i], blocked = true;
    // A goal in a region no walk links to the player's, with a path that looks whole (the coarse world-mesh leg of a
    // cross-level plan runs straight through a shut gate: mission 6 stood 90000 ticks at Orleans' shut east gate): a gate
    // near the player. A path end under the goal only by height counts as whole (a minimap marker stands 200 up: mission 5
    // stood at Orleans' shut South Gate for good). Opt-in (gate_unlinked): on for every mission it lost mission 2 in 9 of 9 attempts.
    if (gate_unlinked && d2(stop, g) < 300 && !linked(region(p.pos), region(g))) stop = p.pos, blocked = true;
    if (!blocked) return;
    // Only a gate the interact opens for the player (FUN_0041af80: a foreign gate from its back side only, and only with HP
    // left: mission 3's English camp gates have none) and one needing no key (mission 3's prison gate).
    float best = 1000;
    for (int r = 1; r <= o.size(); ++r)
        if (const MapItem* it = o.item(r); it && it->cls == 1 && it->state == 0 && it->interact == 0x200 && !it->need_key &&
                                           (it->nation == 0 || it->nation == p.nation || (it->hp > 0 && gate_depth(*it, p.pos) > 0)) &&
                                           d2(it->pos, stop) < best)
            best = d2(it->pos, stop), open_gate_ = r, open_goal_ = g;
    if (const MapItem* it = o.item(open_gate_); it && held_shut(*it)) open_gate_ = 0;
    if (!open_gate_ && tick_ - detour_tick_ > 300) {
        detour_tick_ = tick_;
        if (const Seam* s = detour(p, g)) via_ = s->at, via_in_ = s->in, via_goal_ = g, via_tick_ = tick_, via_cross_ = false;
    }
}

// ms until the hit of u's current melee swing lands (animation start + the move script's StartTime,
// when the attack trigger fires); -1 when u is not swinging or that hit is past.
int Autopilot::swing_eta(const Unit& u) const {
    const World& w = m_.world();
    if (u.action != ATTACK) return -1;
    const AnimScript* s = w.script_of(w.move(u));
    if (!s) return -1;
    const int t = u.anim_start + s->start_time - w.now();
    return t >= 0 ? t : -1;
}

// Ctrl_energy_attack's conditions (PlayerController::energy_attack): learned (ENGATK skill 0xd, Joan's ENGATK record is
// not combo-available) and its price, EP_CONSUME % of EGATK_MP % of max EP and EGATK_HP % of max HP, kept 10 HP clear.
bool Autopilot::energy_ok(const Unit& p) const {
    const World& w = m_.world();
    if (!w.can_use_special(p, ENGATK) && w.skill_level(p, 0xd, {}) == 0) return false;
    const Consts& c = w.data().c;
    const int cost_ep = int(float(w.bonus(p, EP_CONSUME)) * (float(w.max_ep(p) * c.egatk_mp) / 100.0f) / 100.0f);
    const int cost_hp = int(float(w.max_hp(p) * c.egatk_hp) / 100.0f);
    return p.ep >= cost_ep && p.hp > cost_hp + 10;
}

// A shot at t lands: t is on its feet (an arrow aimed at a standing body flies over one knocked down and explodes
// far behind), and the release lock (PlayerController::range_fire: the nearest hostile within d/100 + AIM_R of the
// aim ray from the eye) picks t itself, not another body on the way.
bool Autopilot::clear_shot(const Unit& p, const Unit& t) const {
    const World& w = m_.world();
    if (is_knocked_ext(t.action) || t.action == STANDUP || t.action == DIE) return false;
    const GameData& gd = w.data();
    const Vec3 cam{p.pos.x, p.pos.y, p.pos.z + float(gd.units[std::size_t(p.cls)].height)};
    const auto mid = [&](const Unit& e) {
        return Vec3{e.pos.x - cam.x, e.pos.y - cam.y, e.pos.z + float(gd.units[std::size_t(e.cls)].height) * 0.5f - cam.z};
    };
    const Vec3 a = mid(t);
    // Not through one of the player's own gates: an arrow (sphere r 10; explosive: Gate_take_ranged) stops on a closed
    // one's CollisionType box (MapObjects::sweep_items), and mission 2's volleys at the foes outside broke the Castle
    // Bridge (300 HP): the town flooded. Tested in legs under the sweep's 1000 window.
    const float la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const int legs = int(la / 900) + 1;
    for (int i = 0; i < legs; ++i) {
        float tt;
        const float f = float(i) / float(legs);
        const MapItem* g = m_.objects().item(m_.objects().sweep_items({cam.x + a.x * f, cam.y + a.y * f, cam.z + a.z * f}, 10,
                                                                      {a.x / float(legs), a.y / float(legs), a.z / float(legs)}, tt));
        if (g && g->cls == 1 && g->nation == p.nation) return false;
    }
    for (const Unit& e : w.units()) {
        if (e.id == t.id || e.deleted || w.dead(e) || !w.hostile(e.nation, p.nation)) continue;
        const Vec3 v = mid(e);
        const float vv = v.x * v.x + v.y * v.y + v.z * v.z, vf = (v.x * a.x + v.y * a.y + v.z * a.z) / la;
        const float d = std::sqrt(vv);
        if (d < la && std::sqrt(std::max(0.0f, vv - vf * vf)) <= d / 100.0f + float(gd.c.aim_r)) return false;
    }
    return true;
}

// The enemy at the centre of the densest group (enemies within the explosive blast's 150 of it) between 60 and
// `range` away; with `steady`, only one an arrow can be put on now (clear_shot). n = the group's size.
const Unit* Autopilot::densest(const Unit& p, float range, bool steady, int& n) const {
    const World& w = m_.world();
    const Unit* best = nullptr;
    n = 0;
    for (const Unit& u : w.units()) {
        if (!alive(w, u) || !w.hostile_to_player(u) || u.no_react || (!steady && unreached(u))) continue;
        const float d = d2(p.pos, u.pos);
        if (d < 60 || d > range || std::fabs(u.pos.z - p.pos.z) > 700) continue;
        int k = 0;
        for (const Unit& v : w.units()) k += alive(w, v) && w.hostile_to_player(v) && d2(u.pos, v.pos) < 150;
        if (k > n && (!steady || clear_shot(p, u))) n = k, best = &u;
    }
    return best;
}

void Autopilot::fight(InputFrame& f, Unit& p, const Unit& foe) {
    World& w = m_.world();
    // Explosive volley: the densest group of 2+ within 800, also the crowd around the player: the blast hits every
    // enemy within 150 of the impact for ~3x a sword hit, knocks them all down and spares friends and the shooter.
    {
        int n = 0;
        if (const Unit* best = arrows(p, true) > explosive_reserve ? densest(p, 800, true, n) : nullptr; best && n >= 2 && load_ammo(p, true))
            return shoot(f, p, *best);
    }
    // Bow at range (arrow damage is full up to FALL_S 800): the foe 150..800 away (also down from a wall) and nobody
    // about to strike the player (within 100, or within 200 and after her).
    const bool other_level = std::fabs(foe.pos.z - p.pos.z) > 150;
    if (const float d = d2(p.pos, foe.pos); d >= 150 && d < 800 &&
                                           std::fabs(foe.pos.z - p.pos.z) < 700 && clear_shot(p, foe) && load_ammo(p, false)) {
        bool threat = false;
        for (const Unit& u : w.units())
            if (alive(w, u) && w.hostile_to_player(u) && !u.no_react)
                threat |= d2(p.pos, u.pos) < (u.target == p.id ? 200.0f : 100.0f) && std::fabs(u.pos.z - p.pos.z) < 150;
        if (!threat || other_level) return shoot(f, p, foe);
    }
    // A foe on another level (a wall top) out of reach of the sword: back off until the bow has an angle on it. Without
    // arrows to shoot, close in instead (a wall walk the path reaches: Meung's crossbowmen; chase_ gives up on the rest).
    if (other_level && (arrows(p, false) > 0 || arrows(p, true) > explosive_reserve)) {
        steer(f, p, {2 * p.pos.x - foe.pos.x, 2 * p.pos.y - foe.pos.y, p.pos.z}, false);
        return;
    }
    f.camera_yaw_deg = std::atan2(foe.pos.y - p.pos.y, foe.pos.x - p.pos.x) / kDeg;
    // Guard (Q) against swings about to land on the player (an English swing lands ~350 ms after it
    // starts). Guarding blocks only +-BLOCKANG around the facing and the controller does not turn while
    // guarding, so the guard is aimed at the middle of the incoming swings first. Our own swing cannot
    // be cancelled: a combo is only continued while no second enemy is ready to strike.
    float lo = 1e9f, hi = -1e9f;
    int eta = -1;
    bool others = false;
    for (const Unit& u : w.units()) {
        if (!alive(w, u) || !w.hostile_to_player(u) || u.no_react) continue;
        const float d = d2(p.pos, u.pos);
        if (d > 140 || std::fabs(w.rel_angle(u, p.pos)) > 70) continue;
        if (u.id != foe.id && !is_flinch(u.action) && !is_knocked_ext(u.action) && u.action != STANDUP) others = true;
        const int e = swing_eta(u);
        if (e < 0 || e > 400) continue;
        const float a = w.rel_angle(p, u.pos);
        lo = std::min(lo, a), hi = std::max(hi, a);
        if (eta < 0 || e < eta) eta = e;
    }
    const bool swinging = is_attacking(p.action);
    // Surrounded (3+ within 110): the energy attack (C) instead of guarding in a ring where swings come from
    // every side; it hits everyone around for ~7 A hits (Joan level 4: 29 vs 4) and knocks them down.
    {
        int ring = 0;
        for (const Unit& u : w.units())
            ring += alive(w, u) && w.hostile_to_player(u) && !u.no_react && d2(p.pos, u.pos) < 110 && !is_knocked_ext(u.action);
        if (ring >= 3 && !swinging && w.can_act(p) && energy_ok(p)) {
            f.press(IN_ENERGY);
            return;
        }
    }
    // Below 45 % HP the guard comes back; above it, attacking beats guarding (every hit flinches its targets and
    // the combo cleaves; a guard in a ring of attackers only blocks the front: mission 2 kills 3 -> 18 per run).
    const bool aggro = p.hp * 100 >= 45 * w.max_hp(p);
    if (eta >= 0 && !swinging && !aggro) {
        const float a = (lo + hi) * 0.5f;
        if (std::fabs(a) > 40 && eta > 120) f.mouse_dx = a / 0.18f;
        else f.held.set(IN_DEFEND);
        return;
    }
    const float d = d2(p.pos, foe.pos);
    // A knocked-down foe in reach: Q is the down attack (0x4363a0 runs before the guard).
    if ((is_knocked(foe.action) || foe.action == DNATKED) && !foe.down_attacked && d < 58 && !swinging) {
        const float a = w.rel_angle(p, foe.pos);
        if (std::fabs(a) > 35) f.mouse_dx = a / 0.18f;
        else f.held.set(IN_DEFEND);
        return;
    }
    // Close in. A foe the path mesh cannot bring the player to (behind mission 3's locked prison gate: the path to where he
    // stood ends short of it), or one chased for 30 s in all without getting within 150 (mission 3: the G Defend Place hill
    // guards above the dock road), is left for 60 s. The path's end is judged against its own goal, not where the foe is
    // now: a foe who moved 150..250 since (the replan distance) gave up every man of a melee at once (S6L1: the squad
    // that killed the duke on the road north stood "unreached" beside him).
    if (d <= 150 && !other_level) chase_.erase(foe.id);
    if (d > 85 || other_level) {
        steer(f, p, foe.pos, false);
        if ((d2(path_.back(), path_goal_) > 150 && d2(p.pos, path_.back()) < 100) || ++chase_[foe.id] > 900)
            give_up(foe.id), chase_.erase(foe.id);
        return;
    }
    f.mouse_dx = w.rel_angle(p, foe.pos) / 0.18f;
    if (!swinging) {
        if (others && !w.can_act(p)) return;
        if (others && !aggro) {  // two or more in reach: step back so they bunch up in front
            f.held.set(IN_BACKWARD);
            if (tick_ % 2 == 0) f.press(IN_ATTACK_A);
            return;
        }
        if (tick_ % 2 == 0) f.press(IN_ATTACK_A);
        return;
    }
    // A A A B: queue the next step late in the current step's follow-up window [START, END_TIME),
    // pressed on even ticks so the press follows a release (Player_attackInput phase 2 -> 3).
    if (p.action == ATTACK && p.combo_idx > 0 && !p.follow_queued && (!others || aggro) && p.combo.size() < 4 && tick_ % 2 == 0) {
        const int since = w.now() - p.anim_start;
        if (since >= p.win_close - 140 && since < p.win_close) f.press(p.combo == "AAA" ? IN_ATTACK_B : IN_ATTACK_A);
    }
}

// Ammo kind of an item: 1 explosive (projectile kind 2, item types 0xf / 0x12), 0 other arrows/bolts, -1 not ammo.
static int ammo_kind(const World& w, int item) {
    const int t = w.data().items[std::size_t(item)].type;
    return t < 0xd || t > 0x14 ? -1 : (t == 0xf || t == 0x12) ? 1 : 0;
}

// Arrows of a kind the player carries (ammo slot + bag); 0 without a bow.
int Autopilot::arrows(const Unit& p, bool explosive) const {
    const World& w = m_.world();
    if (!p.inv.ranged.item) return 0;
    int n = p.inv.ammo.item && ammo_kind(w, p.inv.ammo.item) == int(explosive) ? p.inv.ammo.quantity : 0;
    for (const ItemInstance& it : p.inv.bag) n += ammo_kind(w, it.item) == int(explosive) ? it.quantity : 0;
    return n;
}

// Ammo slot: the arrows the bow fires. Swapping them is the character screen's drag into the ammo slot
// (World::equip). Returns false without such arrows.
bool Autopilot::load_ammo(Unit& p, bool explosive) {
    World& w = m_.world();
    if (!p.inv.ranged.item) return false;
    if (p.inv.ammo.item && p.inv.ammo.quantity > 0 && ammo_kind(w, p.inv.ammo.item) == int(explosive)) return true;
    for (std::size_t i = 0; i < p.inv.bag.size(); ++i)
        if (ammo_kind(w, p.inv.bag[i].item) == int(explosive) && p.inv.bag[i].quantity > 0) return w.equip(p, int(i));
    // Plain wanted, only explosive left (in the slot or the bag): keep shooting, not into the plan's explosive_reserve.
    if (explosive || arrows(p, true) <= explosive_reserve) return false;
    return (p.inv.ammo.item && p.inv.ammo.quantity > 0) || load_ammo(p, true);
}

// Bow (range mode, Z): the camera aims at the target's middle (the release locks the nearest enemy within
// |v|/100 + AIM_R of the aim ray, 0x42c260), A starts drawing (SHTLOAD), A in SHTIDLE releases. Explosive arrows
// go at groups: their ground blast hits every enemy within 150 (no friendly fire).
void Autopilot::shoot(InputFrame& f, Unit& p, const Unit& foe) {
    World& w = m_.world();
    int group = 0;
    for (const Unit& u : w.units()) group += alive(w, u) && w.hostile_to_player(u) && d2(u.pos, foe.pos) < 150;
    if (!load_ammo(p, group >= 2 && arrows(p, true) > explosive_reserve)) load_ammo(p, false);
    shoot_at(f, p, {foe.pos.x, foe.pos.y, foe.pos.z + float(w.data().units[std::size_t(foe.cls)].height) * 0.5f},
             p.action == SHTIDLE && clear_shot(p, foe));
}

// Range mode with the camera at `at`: draw whenever possible, release (A in SHTIDLE) when `release`.
void Autopilot::shoot_at(InputFrame& f, Unit& p, Vec3 at, bool release) {
    World& w = m_.world();
    shot_ = true;
    const float dx = at.x - p.pos.x, dy = at.y - p.pos.y;
    const float dz = at.z - (p.pos.z + float(w.data().units[std::size_t(p.cls)].height));
    f.camera_yaw_deg = std::atan2(dy, dx) / kDeg;
    f.camera_pitch_deg = std::atan2(dz, std::hypot(dx, dy)) / kDeg;
    if (!w.controller().range_mode) {
        if (w.can_act(p) && tick_ % 2 == 0) f.press(IN_RANGE_MODE);
        return;
    }
    if (tick_ % 2 == 0 && ((p.action == SHTIDLE && release) || (p.action != SHTIDLE && p.action != SHTLOAD && p.action != SHTOUT && w.can_act(p))))
        f.press(IN_ATTACK_A);
}

// Buy screen open (start_buy_interface): the click handler's purchase (shop_buy: FUN_006c5a00 on the facility's stock,
// the item's Quantity at the stock level, gold -= shop_price), once per opening. A heavier armour than the one worn is
// bought and worn first (inventory right-click, World::use_item); with a bow, explosive arrows up to 20, then plain.
void Autopilot::shop(Unit& p) {
    if (shop_ms_ == m_.ui().interface_ms) return;
    shop_ms_ = m_.ui().interface_ms;
    World& w = m_.world();
    const GameData& gd = w.data();
    int& gold = m_.state().gold;
    const std::vector<ShopStock> stock = shop_stock(m_, m_.ui().buy_shop);
    auto buy = [&](const ShopStock& s) {
        const ItemDef& d = gd.items[std::size_t(s.item)];
        if (gold - shop_price(gd, s) < shop_gold_keep || shop_buy(m_, m_.ui().buy_shop, s, p) != 0) return false;
        log.push_back(std::to_string(m_.ticks()) + " buy " + d.code + " gold " + std::to_string(gold));
        return true;
    };
    // Ammo of the launcher in hand (projectile kinds, proj_type_for): a bow fires arrows (0xe plain, 0xf explosive), a
    // crossbow bolts (0xd / 0x10 / 0x11 plain, 0x12 explosive). Counted by kind over the ammo slot and the bag.
    const int bow = p.inv.ranged.item ? gd.items[std::size_t(p.inv.ranged.item)].type : 0;
    auto fits = [&](int item, bool explosive) {
        const int t = gd.items[std::size_t(item)].type;
        if (bow == 0xb) return t == (explosive ? 0xf : 0xe);
        return bow == 0xc && (explosive ? t == 0x12 : t == 0xd || t == 0x10 || t == 0x11);
    };
    auto count = [&](bool explosive) {
        int n = p.inv.ammo.item && fits(p.inv.ammo.item, explosive) ? p.inv.ammo.quantity : 0;
        for (const ItemInstance& it : p.inv.bag) n += fits(it.item, explosive) ? it.quantity : 0;
        return n;
    };
    auto arrows = [&](bool explosive, int max) {
        for (const ShopStock& s : stock)
            if (fits(s.item, explosive))
                while (count(explosive) < max && buy(s)) {}
    };
    arrows(true, shop_explosive_max);
    arrows(false, shop_plain_first);
    for (const ShopStock& s : stock) {
        const ItemDef& d = gd.items[std::size_t(s.item)];
        if (d.item_class == "ARMOR" && (d.usable_unit < 0 || d.usable_unit == p.cls) && d.defense > p.armor_level && buy(s))
            for (std::size_t i = p.inv.bag.size(); i-- > 0;)
                if (p.inv.bag[i].item == s.item) {
                    w.use_item(p, int(i));
                    break;
                }
    }
    arrows(false, shop_plain_max);
}

// Character screen (F1, the player's hero): every attribute point into attr_kind (default HIT_POINT, the walkthroughs'
// rule; a plan may pick STRENGTH: melee attack = STR x 3 + weapon + 100), every skill point into ADD on the lowest of
// the A / AA / AAA / AAAB combos the bot swings (damage x (lvl+10)/11), and a stronger one-hand sword from the bag into
// the hand (mission 1's archer captain drops JOAN_SWORD_LV2).
void Autopilot::character_screen(Unit& p) {
    World& w = m_.world();
    const GameData& gd = w.data();
    while (p.attr_points > 0 && w.allocate_point(p, attr_kind, false)) {}
    while (p.skill_points > 0) {
        int best = -1;
        for (std::size_t i = 0; i < p.upgrades.size(); ++i) {
            const UpgradeEntry& e = p.upgrades[i];
            const UpItem& up = gd.upitems[std::size_t(e.upitem)];
            if (up.type == 9 && e.applied > 0 && e.combo_level < 10 &&
                (up.para1 == "A" || up.para1 == "AA" || up.para1 == "AAA" || up.para1 == "AAAB") &&
                (best < 0 || e.combo_level < p.upgrades[std::size_t(best)].combo_level))
                best = int(i);
        }
        if (best < 0 || !w.allocate_point(p, 8, false, best)) break;
    }
    const ItemInstance& hand = p.inv.melee[0];
    const int cur = hand.item ? gd.items[std::size_t(hand.item)].base_damage(hand.level) : 0;
    for (std::size_t i = 0; i < p.inv.bag.size(); ++i) {
        const ItemDef& d = gd.items[std::size_t(p.inv.bag[i].item)];
        if (d.type == 9 && (d.usable_unit < 0 || d.usable_unit == p.cls) && d.base_damage(p.inv.bag[i].level) > cur) {
            w.equip(p, int(i));
            break;
        }
    }
}

InputFrame Autopilot::decide() {
    InputFrame f;
    World& w = m_.world();
    Unit* pp = w.unit(w.player_id());
    goal.clear();
    if (m_.ui().buy_open && last_talk_ && tick_ - last_talk_tick_ < 300) shopkeepers_.insert(last_talk_);
    if (pp && alive(w, *pp) && m_.ui().buy_open) {
        shop(*pp);
        if (plan_) plan_->shop(*this, *pp);
    }
    if (!pp || !alive(w, *pp) || m_.ui().game_cut_scene || w.cut_scene) return f;
    Unit& p = *pp;
    character_screen(p);
    // The other heroes' points: the in-game hint says "switch to that character to assign their available points". With
    // nobody hostile within 400, switch to a hero holding points (the key path), spend them on his character screen
    // above, switch back (two ticks). Mission 2: Jean holds 10 attribute points (+40 HP), switchable once the attack starts.
    if (Unit* home = w.unit(home_hero_); home_hero_ && home && p.id != home_hero_) {
        m_.switch_hero(hero_key(*home));
        home_hero_ = 0;
        return f;
    }
    home_hero_ = 0;
    const auto calm = [&](const Unit& h) {
        for (const Unit& u : w.units())
            if (alive(w, u) && w.hostile_to_player(u) && d2(u.pos, h.pos) < 400) return false;
        return true;
    };
    if (!m_.ui().buy_open && hero_key(p) && calm(p) && tick_ >= switch_retry_)
        for (const Unit& u : w.units())
            if (u.id != p.id && alive(w, u) && hero_key(u) && (u.attr_points > 0 || u.skill_points > 0) && calm(u)) {
                if (m_.switch_hero(hero_key(u))) home_hero_ = p.id;
                else switch_retry_ = tick_ + 900;   // not controllable yet (mission 2 before the attack)
                return f;
            }
    // F3 below half HP while food lasts; below 30 % with no food left, fall back to the nearest friendly
    // hero (Jean...) and only guard until regen / a level-up brings HP back to 70 %.
    const int hp_pct = p.hp * 100 / std::max(1, w.max_hp(p));
    int food = 0, ep_food = 0;
    for (const ItemInstance& it : p.inv.bag) {
        const int t = w.data().items[std::size_t(it.item)].type;
        food += (t == 0x15 || t == 0x19) ? it.quantity : 0;
        ep_food += t == 0x17 ? it.quantity : 0;
    }
    if (hp_pct < 50 && food && tick_ - last_heal_ > 30) f.press(IN_HEAL_ITEM), last_heal_ = tick_;
    // F4 (apples, EP) when the energy attack is short of EP and an enemy is near.
    else if (ep_food && (w.can_use_special(p, ENGATK) || w.skill_level(p, 0xd, {})) && !energy_ok(p) && tick_ - last_heal_ > 30)
        for (const Unit& u : w.units())
            if (alive(w, u) && w.hostile_to_player(u) && d2(p.pos, u.pos) < 300) {
                f.press(IN_EP_ITEM), last_heal_ = tick_;
                break;
            }
    if (hp_pct < 30 && !food) retreat_ = true;
    if (hp_pct >= 70 || food) retreat_ = false;
    // On a ladder (climb() mounted it, or R at an item indicator in passing): up the rungs to the walkway.
    if (p.m.climb) {
        goal = "climb";
        f.held.set(climb_up_ ? IN_FORWARD : IN_BACKWARD);
        return f;
    }
    if (plan_ && plan_->first(*this, f, p)) return f;
    // Wards: allied heroes that die for good (retreat 0: a hero with the retreat flag, Jean, withdraws instead; mission
    // 2 is lost with the Bastard) and friends a minimap marker follows (mission 1's letter bearer before the escort), the bearer in the player's team. Enemies within 400
    // of them or after them count at 0.3 of their distance, from up to 4000 away.
    std::vector<const Unit*> wards;
    for (const Unit& u : w.units())
        if (u.id != p.id && alive(w, u) && u.nation == p.nation && u.rank >= 4 && u.retreat == 0) wards.push_back(&u);
    for (const auto& x : m_.ui().waypoints)
        if (const Unit* u = x.type && x.follow ? w.unit(x.follow) : nullptr; u && u->id != p.id && alive(w, *u) && !w.hostile_to_player(*u))
            wards.push_back(u);
    if (const Unit* b = bearer()) wards.push_back(b);
    // Holds: the territory of a finished WIN objective the player's nation now owns (a captured camp: mission 3 takes it back,
    // and objective 1 with it, once no French stands inside and the English reinforcement waves sent after the capture walk
    // in; not mission 4's Tourelles, named by the rescue of Jean de Metz). Enemies within 1500 of it count like enemies on a
    // ward, and 4+ there call the bot back.
    std::vector<Vec3> holds;
    for (const EndCondition& c : m_.state().end)
        if (c.has_des && c.enabled && c.type == EndCondition::Win && c.finish == 1 && !c.territory_link.empty())
            if (const Territory* t = m_.places().territory(m_.places().territory_by_name(c.territory_link)); t && t->owner == p.nation)
                holds.push_back({float(t->center_x), float(t->center_y), p.pos.z});
    const auto near_hold = [&](Vec3 q) { return std::any_of(holds.begin(), holds.end(), [&](Vec3 h) { return d2(q, h) < 1500; }); };
    // A switchable ally hero standing by a ward that dies for good when the bot may command him (mission 2: Jean beside the
    // Bastard as the attack starts) is put on hold there (Shift + his key, Unit_toggle_hold with his men; the walkthrough
    // holds Jean): he guards the ward instead of following the player across the city.
    for (const Unit* h : wards)
        for (Unit& u : w.units())
            if (h->retreat == 0 && u.id != p.id && hero_key(u) && alive(w, u) && u.m.control == 0 && !held_.count(u.id) &&
                d2(u.pos, h->pos) < 1000) {
                if (!m_.ai().is_holding(u)) m_.ai().toggle_hold(u, true);  // a plan may have held him already
                held_.insert(u.id);
            }
    const Unit* ally = nullptr;
    for (const Unit& u : w.units())
        if (u.id != p.id && alive(w, u) && u.nation == p.nation && u.rank >= 4 && u.retreat != -1 &&
            (!ally || d2(p.pos, u.pos) < d2(p.pos, ally->pos)))
            ally = &u;
    // Interact indicator: talk, free prisoners, pick up items, recruit. A unit already talked to is not
    // pestered again for 100 s (scripts replay their dialog on every R). The indicator is computed before
    // the physics step moves the player on, so she stops first and presses R once it stayed lit (a player
    // stops at the icon too); R pressed while still walking lands out of reach.
    // Never a word to a unit following the player: a second one stops mission 4's wagon. The NPC the errand logic below walks
    // up to (talk_key_, chosen 300 ticks after the last word at the earliest) or a plan leads to (plan_talk) is talked to:
    // mission 4's priest answered the first word before the wagon was within 600, and the bot then stood at him for 80000
    // ticks without pressing R again.
    const Unit* talker = prompt_kind_ == "talk" ? w.unit(prompt_id_) : nullptr;
    const bool fresh_talk = (prompt_kind_ != "talk" || !talked_.count(prompt_id_) || tick_ - talked_[prompt_id_] > 3000 ||
                             (talk_news_[prompt_id_] < news_ && tick_ - talked_[prompt_id_] > 300) ||
                             (((prompt_id_ == talk_key_ && tick_ - talk_seen_ <= 1) || prompt_id_ == plan_talk) && tick_ - talked_[prompt_id_] > 300)) &&
                            !(talker && following(*talker));
    // Siege crew walking to its weapon is recruitable (Unit_find_recruitable_unit 0x48dfb0 has no task test), but R
    // there takes the man the player just assigned (S7L1's trebuchet lost its third F man to Joan this way).
    const Unit* recruit = prompt_kind_ == "recruit" ? w.unit(prompt_id_) : nullptr;
    const bool crew_prompt = recruit && m_.ai().curr_task(*recruit) == task::LARGE_WEAPON_PUSH;
    // A gate is toggled only on purpose, never in passing: closed (gate_ below) or opened to get through (travel).
    const int prompt_item = prompt_kind_ == "item" ? m_.objects().modify_item(prompt_id_) : 0;
    const MapItem* pit = m_.objects().item(prompt_item);
    const bool gate_prompt = pit && pit->cls == 1;
    const bool ladder_prompt = pit && pit->cls == 11;  // only on purpose (the hunt's climb): R at its top took the player back down
    const bool open_it = gate_prompt && prompt_item == open_gate_ && pit->state == 0 && tick_ - travel_tick_ <= 1;
    const bool close_it = gate_prompt && prompt_item == opened_gate_ && pit->state == 1 && tick_ - shut_tick_ <= 1;
    if (prompt_ && !ladder_prompt && !crew_prompt && fresh_talk && tick_ - last_interact_ > 20 && prompt_kind_ != "mount" && prompt_kind_ != "bribe" &&
        (!gate_prompt || prompt_item == gate_ || open_it || close_it) && prompt_kind_ != "recruit_screen" && prompt_kind_ != "workshop") {
        goal = "interact " + prompt_kind_;
        if (tick_ - prompt_tick_ < 3) return f;
        last_interact_ = tick_;
        if (prompt_kind_ == "talk") talked_[prompt_id_] = tick_, talk_news_[prompt_id_] = news_, last_talk_ = prompt_id_, last_talk_tick_ = tick_;
        if (open_it) opened_gate_ = prompt_item;
        f.press(IN_INTERACT);
        return f;
    }
    // Objective marker: the nearest active minimap way point (scripts point the player with them) that was not
    // stood at for 10 s already without the script moving it (mission 2 shows the shop and the blacksmith at once).
    // A marker the script hides and shows again is a new errand (mission 2 stage 4: back to the town hall).
    const Vec3* wp = nullptr;
    Vec3 wp_pos;
    int wp_slot = 0, slot = 0;
    wp_shown_.resize(m_.ui().waypoints.size());
    for (const auto& x : m_.ui().waypoints) {
        ++slot;
        const bool was = wp_shown_[std::size_t(slot - 1)];
        wp_shown_[std::size_t(slot - 1)] = x.type != 0;
        if (x.type == 0) continue;
        const Unit* fu = x.follow ? w.unit(x.follow) : nullptr;
        const Vec3 at_pos = fu ? fu->pos : Vec3{x.pos[0], x.pos[1], x.pos[2]};
        if (!was) std::erase_if(done_wps_, [&](Vec3 d) { return d2(d, at_pos) < 1; });
        if (std::any_of(done_wps_.begin(), done_wps_.end(), [&](Vec3 d) { return d2(d, at_pos) < 1; })) continue;
        if (plan_ && !plan_->use_waypoint(*this, slot, at_pos)) continue;
        if (wp && d2(p.pos, at_pos) >= d2(p.pos, wp_pos)) continue;
        wp_pos = at_pos, wp = &wp_pos, wp_slot = slot;
    }
    // A way point on an open gate the player may close: the script asks for it (mission 2: "Someone must
    // approach and close the gate"). Walk up to it from the friendly side and press R at its indicator (the
    // passage closes unless a rider / siege weapon stands within 300). A gate the bot opened itself to get at
    // enemies outside stays open while any of them lives within 4000 outside.
    int gate = 0;
    if (wp)
        for (int r = 1; r <= m_.objects().size(); ++r)
            if (const MapItem* it = m_.objects().item(r); it && it->cls == 1 && it->state == 1 && it->interact == 0x200 && d2(it->pos, *wp) < 150)
                gate = r;
    if (const MapItem* g = m_.objects().item(gate); g && gate == opened_gate_) {
        const Vec3 in = gate_inside(*g);
        for (const Unit& u : w.units())
            if (alive(w, u) && u.m.active && w.hostile_to_player(u) && !u.no_react && std::fabs(u.pos.z - g->pos.z) < 150 &&
                d2(u.pos, g->pos) < 4000 && (u.pos.x - g->pos.x) * (in.x - g->pos.x) + (u.pos.y - g->pos.y) * (in.y - g->pos.y) < 0)
                gate = 0;
    }
    if (!gate) gate_seen_ = 0;
    else if (!gate_seen_) gate_seen_ = tick_;
    gate_ = gate && tick_ - gate_seen_ >= gate_delay ? gate : 0;
    if (const MapItem* g = m_.objects().item(gate_)) {
        goal = "close gate " + at(g->pos);
        const Vec3 in = gate_inside(*g);
        if (d2(p.pos, in) > 60) steer(f, p, in, false);
        else f.mouse_dx = w.rel_angle(p, g->pos) / 0.18f;
        return f;
    }
    if (gate) wp = nullptr;  // not yet: the marker does not pull the player
    if (plan_ && plan_->before_targets(*this, f, p)) return f;
    // Enemies: keep the current target while it lives and stays near; else the nearest reachable one
    // close by, or one that is attacking the player. While hurt (< 70 %) an enemy that has not noticed
    // the player yet is left alone: rest first. (Mission 2: leashing Joan to the Bastard lost on citizens in
    // 6/6 seeds; roaming with the citizen-killer priority below kept 4-13 of 18 alive.)
    const bool fresh = hp_pct >= rest_pct;
    const Unit* foe = nullptr;
    const Unit* lurker = nullptr;  // nearest unaware enemy within 600
    // Reachable: not given up on (a foe the path mesh did not bring the player to, or one chased too long). A component test
    // (same nav level, other path-mesh component) dropped every mission 2 attacker (kills 0, the Bastard lost); the hunt
    // below ranks by regions instead.
    const bool bow = p.inv.ranged.item && p.inv.ammo.item && p.inv.ammo.quantity > 0;
    const int here = region(p.pos);
    const auto reachable = [&](const Unit& u) { return !unreached(u); };
    // A kept target more than 250 off (a wall archer the bow duels) is dropped while someone within 120 is after the player:
    // mission 6 stood shooting a tower archer while eight melee units beat Joan to death.
    const bool pressed = std::any_of(w.units().begin(), w.units().end(), [&](const Unit& u) {
        return alive(w, u) && w.hostile_to_player(u) && u.target == p.id && d2(p.pos, u.pos) < 120;
    });
    if (const Unit* t = w.unit(fight_target_);
        t && alive(w, *t) && t->m.active && w.hostile_to_player(*t) && d2(p.pos, t->pos) < 700 && reachable(*t) &&
        !(pressed && d2(p.pos, t->pos) > 250))
        foe = t;
    if (!foe || foe->target != p.id) {  // archers close by first, then whoever targets the player, then the nearest
        float best = 1e30f;
        // On the way to the open objective's territory (more than 2000 off) only foes within 800 are taken on: mission 3
        // Normal timed out chasing every patrol within 3000 between the two camps.
        Vec3 obj{};
        const float reach = wp ? 450.0f : open_territory(obj) && d2(p.pos, obj) > 2000 ? 800.0f : 3000.0f;  // beyond: "hunt" walks there
        // Enemies within 2000 of the NPC an errand (no marker) leads to, or inside a place he stands in, count too (mission 4:
        // the priest and councillor answer "Not now!" while any English stands in the Fierbois place, people_by_nation_exist_in_place;
        // a lone archer 3700 off at its edge kept them so for 100000 ticks).
        const Unit* tk = tick_ - talk_seen_ < 300 && talk_errand_ ? w.unit(talk_key_) : nullptr;
        std::vector<int> tk_places;
        if (tk)
            for (const Place& pl : m_.places().places())
                if (m_.places().inside(pl.recno, tk->pos)) tk_places.push_back(pl.recno);
        for (const Unit& u : w.units()) {
            if (!alive(w, u) || !u.m.active || !w.hostile_to_player(u) || u.no_react) continue;
            float d = d2(p.pos, u.pos);
            // Same level, or another (a wall top, the ground below one) within bow range.
            if (const float dz = std::fabs(u.pos.z - p.pos.z); dz > 150 && !(bow && dz < 700 && d < 800)) continue;
            const auto& c = w.data().units[std::size_t(u.cls)];
            const float weight = plan_ ? plan_->foe_weight(*this, p, u) : MissionPlan{}.foe_weight(*this, p, u);
            if (weight <= 0) continue;
            d *= weight;
            const bool aware = u.target == p.id || d < 250;
            if (!aware && !fresh) {
                if (d < 600 && (!lurker || d < d2(p.pos, lurker->pos))) lurker = &u;
                continue;
            }
            if (u.target == p.id) d *= 0.5f;
            // An enemy after a citizen first (mission 2 is lost at 18 dead citizens).
            if (const Unit* t = w.unit(u.target); t && alive(w, *t) && w.data().units[std::size_t(t->cls)].unit_class == 0 && !w.hostile_to_player(*t))
                d *= 0.4f;
            if ((c.unit_class == 5 || c.unit_class == 6) && d < 350) d *= 0.3f;
            const bool on_ward = near_hold(u.pos) || (tk && d2(u.pos, tk->pos) < 2000) ||
                                 std::any_of(tk_places.begin(), tk_places.end(), [&](int pl) { return m_.places().inside(pl, u.pos); }) ||
                                 std::any_of(wards.begin(), wards.end(), [&](const Unit* h) { return u.target == h->id || d2(u.pos, h->pos) < 400; });
            if (on_ward) d *= 0.3f;
            if (d < (on_ward ? 4000.0f : reach) && d < best && reachable(u)) best = d, foe = &u;
        }
    }
    if (retreat_) {
        fight_target_ = 0;
        const Unit* near = foe && d2(p.pos, foe->pos) < 170 ? foe : nullptr;
        if (near && is_attacking(near->action)) {
            f.mouse_dx = w.rel_angle(p, near->pos) / 0.18f;
            f.held.set(IN_DEFEND);
        } else if (ally && d2(p.pos, ally->pos) > 120) {
            steer(f, p, ally->pos, false);
        } else if (foe && d2(p.pos, foe->pos) < 400) {  // away from the enemy
            steer(f, p, {2 * p.pos.x - foe->pos.x, 2 * p.pos.y - foe->pos.y, p.pos.z}, false);
        }
        goal = "retreat hp " + std::to_string(hp_pct) + "%";
        return f;
    }
    // A ward swarmed (4+ enemies within 500) more than 700 away: break off and run to it; the fight there (volleys into
    // the crowd) beats trading blows where the player stands (mission 2 Easy: Bastard losses 4/12 -> 0/12).
    for (const Unit* h : wards) {
        int n = 0;
        for (const Unit& u : w.units()) n += alive(w, u) && w.hostile_to_player(u) && !u.no_react && d2(u.pos, h->pos) < 500;
        if (n >= 4 && d2(p.pos, h->pos) > 700) {
            goal = "rescue #" + std::to_string(h->id) + " from " + std::to_string(n);
            fight_target_ = 0;
            travel(f, p, h->pos);
            return f;
        }
    }
    for (const Vec3 h : holds) {
        int n = 0;
        for (const Unit& u : w.units()) n += alive(w, u) && w.hostile_to_player(u) && !u.no_react && d2(u.pos, h) < 1500;
        if (n >= 4 && d2(p.pos, h) > 1500) {
            goal = "hold " + at(h) + " from " + std::to_string(n);
            fight_target_ = 0;
            travel(f, p, h);
            return f;
        }
    }
    // Told to come back and close the gate (mission 2 stage 4 only calls the second wave with the player in the
    // city behind a shut gate: "Return to the castle immediately, and close the gate!"): back through the gate the
    // bot opened itself, which travel() closes behind her.
    if (const MapItem* g = m_.objects().item(opened_gate_); go_home_ && g && !(foe && foe->target == p.id && d2(p.pos, foe->pos) < 300)) {
        if (g->state == 0 && gate_depth(*g, p.pos) > 0) {
            go_home_ = false;
        } else {
            goal = "home";
            travel(f, p, home());
            return f;
        }
    }
    // Explosive arrows are worth most on a big group (a squad gathered at its rally point): with some left and
    // nobody on the player, walk up to the densest group of 5+ within 3000 (the volley starts at 800). A group the walk
    // gets no 300 nearer to in 60 s (below a cliff, behind a wall: S7L1, mission 4's Abtower walkway) is given up on whole.
    if (int n = 0; arrows(p, true) > explosive_reserve && !(foe && (d2(p.pos, foe->pos) < 300 || foe->target == p.id)))
        if (const Unit* g = densest(p, 3000, false, n); g && n >= 5 && d2(p.pos, g->pos) > 700) {
            auto& [best, headway, last] = volley_way_;
            if (tick_ - last > 60 || d2(p.pos, g->pos) < best - 300) best = d2(p.pos, g->pos), headway = tick_;
            last = tick_;
            if (tick_ - headway <= 1800) {
                goal = "volley at #" + std::to_string(g->id) + " group " + std::to_string(n) + " " + at(g->pos);
                travel(f, p, g->pos);
                return f;
            }
            for (const Unit& u : w.units())
                if (alive(w, u) && w.hostile_to_player(u) && d2(u.pos, g->pos) < 150) give_up(u.id);
            last = -1000;
        }
    fight_target_ = foe ? foe->id : 0;
    if (foe) {
        goal = "fight #" + std::to_string(foe->id) + " " + w.data().units[std::size_t(foe->cls)].code + " " + at(foe->pos);
        fight(f, p, *foe);
        return f;
    }
    if (lurker) {  // hurt: back away from an enemy that has not noticed the player
        goal = "avoid #" + std::to_string(lurker->id);
        steer(f, p, {2 * p.pos.x - lurker->pos.x, 2 * p.pos.y - lurker->pos.y, p.pos.z}, false);
        return f;
    }
    // Loot: crates and barrels hold food and arrows (the mission-1 tutorial says so), dropped items lie
    // where units died. Break / walk over the nearest within reach, skipping ones that took too long.
    if (loot(f, p)) return f;
    // Rest: Joan regenerates (1 HP / 10 s early on); nothing is gained by walking into the next fight hurt. With
    // food to spare (more than 10) she eats instead of waiting (mission 2 stage 4: 29 food, 2 HP per 20 s).
    // An ally hero told to follow the player (FOLLOW_UNIT, Jean in mission 1) travels with her; heroes
    // that guard a place are not waited for.
    const bool follows = ally && m_.ai().curr_task(*ally) == task::FOLLOW_UNIT && m_.ai().curr_task_para(*ally, 1) == p.id;
    if (!fresh && p.retreat != -1) {
        goal = "rest hp " + std::to_string(hp_pct) + "%";
        if (food > rest_food && tick_ - last_heal_ > 30) f.press(IN_HEAL_ITEM), last_heal_ = tick_;
        if (follows && d2(p.pos, ally->pos) > 250) steer(f, p, ally->pos, false);
        return f;
    }
    // Travel together: a following ally (Jean) left more than 700 behind, unless busy with a foe, and the letter bearer
    // (a friend dropping a MISSION item: the mission 1 guards only open to him) are waited for, standing: walking back
    // turned a follower around too (mission 1: Joan and Jean walked back and forth for minutes). 30 s per wait at most.
    const Unit* busy = ally ? w.unit(ally->target) : nullptr;
    // A follower the wait did not bring back within its 30 s (mission 3: Jean below a cliff) is not waited for again for 2 min.
    const Unit* lag = follows && tick_ >= no_wait_until_ && !(busy && alive(w, *busy) && w.hostile_to_player(*busy)) &&
                              d2(p.pos, ally->pos) > 700 && d2(p.pos, ally->pos) < 3000 ? ally : nullptr;
    if (const Unit* b = bearer(); b && d2(p.pos, b->pos) > 700 && d2(p.pos, b->pos) < 4000) lag = b;
    if (!lag) wait_tick_ = -1;
    else if (wait_tick_ < 0) wait_tick_ = tick_;
    if (lag == ally && lag && tick_ - wait_tick_ >= 900) no_wait_until_ = tick_ + 3600;
    if (lag && tick_ - wait_tick_ < 900) {
        goal = "wait for #" + std::to_string(lag->id);
        return f;
    }
    // An escort (a citizen or cart told to follow the player: mission 4's wagon, which keeps to its cart track, and the
    // villagers) more than 400 behind is waited for while it closes in (200 nearer within 30 s), so that it is at hand at the
    // talk it was fetched for (the priest only takes the goods with the wagon within 600).
    const Unit* escort = nullptr;
    for (const Unit& u : w.units())
        if (const int uc = w.data().units[std::size_t(u.cls)].unit_class; alive(w, u) && (uc == 0 || uc == 9) && following(u) &&
                                                                          d2(p.pos, u.pos) > 400 && (!escort || d2(p.pos, u.pos) > d2(p.pos, escort->pos)))
            escort = &u;
    if (escort && tick_ >= escort_skip_) {  // none for 2 min after a wait without headway
        auto& [best, headway, last] = escort_way_;
        if (tick_ - last > 60 || d2(p.pos, escort->pos) < best - 200) best = d2(p.pos, escort->pos), headway = tick_;
        last = tick_;
        if (tick_ - headway < 900) {
            goal = "wait for escort #" + std::to_string(escort->id) + " " + at(escort->pos);
            return f;
        }
        escort_skip_ = tick_ + 3600;
    }
    // A marker often stands at a door with the person to see inside (the shops in mission 2): near the
    // marker, walk up to an NPC showing the talk icon (can_interact) not talked to in the last 100 s (mission 2
    // stage 4 sends the player back to the Bastard). A reached marker
    // the script does not move on within 10 s means "continue from here": the next marker, the open WIN
    // objective's territory (the minimap shows towns), else the nearest enemy.
    // Without a marker the talk icons are the errands (mission 4: the priest sends the player to the wagon, the wagon back
    // to the priest, the councillor to five villagers): a friend showing one is talked to once, and again once something new
    // happened that his own talk did not bring (news_: an objective, a talk icon, a follower, a MISSION item), 10 s on at
    // the earliest; nearest first. Never one following the player (a second word stops mission 4's wagon), never a
    // shopkeeper twice (his talk opened the buy screen), never one no walk leads to.
    // Only friends the script gave the talk icon during the mission (mission 4: the wagon after the priest's word, Jean de Metz
    // once captured, the councillor, the villagers) or standing inside the territory an open WIN objective names or named earlier
    // (mission 4's priest stays in Fierbois once objective 3 points at Église de lumière): mission 2's gate guard, talked to in
    // passing, lost the town.
    std::vector<const Territory*> errand;
    for (std::size_t i = 0; i < m_.state().end.size(); ++i)
        if (const EndCondition& c = m_.state().end[i]; c.has_des && c.enabled && c.type == EndCondition::Win && c.finish != 1) {
            if (!c.territory_link.empty()) errand_territories_[int(i)].insert(c.territory_link);
            for (const std::string& n : errand_territories_[int(i)])
                if (const Territory* t = m_.places().territory(m_.places().territory_by_name(n))) errand.push_back(t);
        }
    const auto in_errand = [&](const Unit& u) {
        return std::any_of(errand.begin(), errand.end(), [&](const Territory* t) { return Places::in_territory(*t, u.pos.x, u.pos.y); });
    };
    // An escort under way (a follower whose talk icon a giver's talk brought) is taken back to that giver once every other
    // talk icon his talk brought has been talked to (mission 4: the five villagers before the councillor): nothing else.
    const Unit* giver = nullptr;
    for (const Unit& u : w.units())
        if (const auto g = giver_.find(u.id); g != giver_.end() && alive(w, u) && following(u))
            if (const Unit* gu = w.unit(g->second); gu && alive(w, *gu) && gu->m.can_interact) giver = gu;
    const Unit* npc = nullptr;
    for (const Unit& u : w.units()) {
        if (!giver || u.id == p.id || !alive(w, u) || !u.m.can_interact || following(u) || talked_.count(u.id)) continue;
        if (const auto g = giver_.find(u.id); g != giver_.end() && g->second == giver->id && (!npc || d2(p.pos, u.pos) < d2(p.pos, npc->pos)))
            npc = &u;
    }
    if (giver && !npc && tick_ - talked_[giver->id] > 300) npc = giver;
    for (const Unit& u : w.units()) {
        if (giver || u.id == p.id || !alive(w, u) || !u.m.can_interact || w.hostile_to_player(u)) continue;
        const auto t = talked_.find(u.id);
        const bool want = wp ? d2(p.pos, *wp) < 900 && d2(u.pos, *wp) < 600 && (t == talked_.end() || tick_ - t->second > 3000)
                             : (t == talked_.end() || (talk_news_[u.id] < news_ && tick_ - t->second > 300)) && !shopkeepers_.count(u.id) &&
                                   !following(u) && (new_talkers_.count(u.id) || in_errand(u)) && linked(here, unit_region(u)) &&
                                   (!plan_ || plan_->use_talker(*this, u));
        if (want && (!npc || d2(p.pos, u.pos) < d2(p.pos, npc->pos))) npc = &u;
    }
    if (npc) {
        goal = "talk to #" + std::to_string(npc->id) + " " + npc->name;
        if (npc->id != talk_key_ || tick_ - talk_seen_ > 30) talk_key_ = npc->id, talk_tick_ = talk_near_ = tick_;  // a new approach
        talk_seen_ = tick_, talk_errand_ = !wp;
        if (d2(p.pos, npc->pos) > 400) talk_near_ = tick_;
        // No talk icon within 30 s of reaching him, or 3 min on the way: give up on him.
        if (tick_ - talk_near_ > 900 || tick_ - talk_tick_ > 5400) talked_[npc->id] = tick_, talk_news_[npc->id] = news_;
        if (d2(p.pos, npc->pos) > 60 && wp) steer(f, p, npc->pos, false);
        else if (d2(p.pos, npc->pos) > 60) travel(f, p, npc->pos);
        else f.mouse_dx = w.rel_angle(p, npc->pos) / 0.18f;
        return f;
    }
    if (wp) {
        if (d2(p.pos, *wp) > 150) seen_tick_ = tick_;
        else if (tick_ - seen_tick_ > 300) done_wps_.push_back(*wp), seen_tick_ = tick_;  // stood there 10 s
        goal = "waypoint " + std::to_string(wp_slot) + " " + at(*wp);
        if (d2(p.pos, *wp) > 80) travel(f, p, *wp);
        return f;
    }
    if (plan_ && plan_->before_hunt(*this, f, p)) return f;
    // The open WIN objective's territory (the minimap shows the towns / camps), once per 100 s: reached, the hunt below
    // clears it (mission 3: camp du beau vallon is taken once no English is left inside it). No headway (500 closer) in
    // 60 s: left for 5 min, the next objective's territory first (mission 4: Tourelles is shut until its gate falls).
    Vec3 h = home();
    if (int i = 0; open_territory(h, &i)) {
        if (d2(p.pos, h) < 300) territory_seen_[i] = tick_;
        if (const auto s = territory_seen_.find(i); s == territory_seen_.end() || tick_ - s->second >= 3000) {
            auto& [best, headway, last] = territory_way_[i];
            if (tick_ - last > 60 || d2(p.pos, h) < best - 500) best = d2(p.pos, h), headway = tick_;  // a new trip, or headway
            last = tick_;
            if (tick_ - headway > 1800) {
                territory_skip_[i] = tick_ + 9000, last = -1000;
                return f;
            }
            goal = "territory " + m_.state().end[std::size_t(i)].territory_link + " " + at(h);
            travel(f, p, h);
            return f;
        }
    }
    // Hunt the enemy left nearest to the player and to the open objective's territory (else her people, home(): the town
    // under attack before the garrisons of the next ones), those on the player's level (not a wall top above / the ground
    // below) and those a walk (or a ladder) leads to first.
    // While an objective territory is open the target is kept for 30 s while it lives (scores flip as the player walks:
    // mission 3 went back and forth between the camp's wall archers and a far tower for minutes). Not otherwise: mission 2's
    // town defence lost the Bastard in 3/3 seeds with the hunt stuck on one far attacker.
    const Unit* far = nullptr;
    float far_d = 0;
    Vec3 foot{};
    bool up = true;
    if (const Unit* t = w.unit(hunt_target_);
        t && tick_ - hunt_tick_ < 900 && open_territory(foot) && alive(w, *t) && w.hostile_to_player(*t) && !unreached(*t))
        far = t;
    for (const Unit& u : w.units())
        if (!far && alive(w, u) && u.m.active && w.hostile_to_player(u) && !u.no_react && !unreached(u)) {
            const float weight = plan_ ? plan_->foe_weight(*this, p, u) : MissionPlan{}.foe_weight(*this, p, u);
            if (weight <= 0) continue;
            const int r = unit_region(u);
            const float d = weight * (d2(p.pos, u.pos) + d2(h, u.pos) + (std::fabs(u.pos.z - p.pos.z) > 150 ? 5000.0f : 0.0f) +
                                      (here == r || here < 0 || r < 0 ? 0.0f : ladder_to(here, r, foot, up) ? 2000.0f : linked(here, r) ? 10000.0f : 20000.0f));
            if (!far_d || d < far_d) far_d = d, hunt_target_ = u.id, hunt_tick_ = tick_;
        }
    if (!far) far = far_d ? w.unit(hunt_target_) : nullptr;
    if (far) {
        goal = "hunt #" + std::to_string(far->id) + " " + at(far->pos);
        // A wall top (mission 3: the archers on camp du beau vallon's walls) is reached by its ladder (travel()).
        travel(f, p, far->pos);
        if (!open_gate_ && !path_.empty() && d2(path_goal_, far->pos) < 250 && d2(path_.back(), far->pos) > 150 && d2(p.pos, path_.back()) < 100)
            give_up(far->id);
        return f;
    }
    // Nothing left: everything lying around, else stand.
    if (loot(f, p, 4000)) return f;
    goal = "idle";
    return f;
}

// A ladder (map item class 11, ends cached by build_seams()) between region `from` and region `to`: up from its foot or down
// from its top, else the first of two (down from one watchtower, up the next: camp du Champagne). `end` = the end to walk
// to, `up` = the direction; nearest end to the player, null if none.
const MapItem* Autopilot::ladder_to(int from, int to, Vec3& end, bool& up) {
    build_seams();
    if (no_ladder_.count({from, to})) return nullptr;
    const Unit* p = m_.world().unit(m_.world().player_id());
    const auto leads = [&](const Ladder& l, bool u, int a, int b) { return linked(a, u ? l.foot_region : l.top_region) && linked(u ? l.top_region : l.foot_region, b); };
    const MapItem* best = nullptr;
    for (int hops = 1; hops <= 2 && !best; ++hops)   // ponytail: two hops cover the maps seen; a BFS when one needs more
        for (const Ladder& l : ladders_)
            for (const bool u : {true, false}) {
                const Vec3 e = u ? l.foot : l.top;
                bool ok = leads(l, u, from, to);
                for (std::size_t k = 0; hops == 2 && !ok && k < ladders_.size(); ++k)
                    for (const bool v : {true, false}) ok |= &ladders_[k] != &l && leads(l, u, from, v ? ladders_[k].foot_region : ladders_[k].top_region) && leads(ladders_[k], v, u ? l.top_region : l.foot_region, to);
                if (p && ok && (!best || d2(p->pos, e) < d2(p->pos, end))) best = m_.objects().item(l.item), end = e, up = u;
            }
    if (!best) no_ladder_.insert({from, to});
    return best;
}

// Up or down a ladder towards region `to` when no walk leads there. At the end, facing the ladder (foot) or away from it
// over the edge (top), R mounts it (ladder_input: within 100 of that end; foot: in front, facing it; top: behind its
// plane, facing out); decide() then holds W / S on the rungs. False when no ladder joins the two regions.
bool Autopilot::use_ladder(InputFrame& f, const Unit& p, int to) {
    Vec3 end{};
    bool up = true;
    const MapItem* l = ladder_to(region(p.pos), to, end, up);
    if (!l) return false;
    goal += (up ? " ladder up " : " ladder down ") + at(l->pos);
    climb_up_ = up;
    const float fx = std::cos(l->dir), fy = std::sin(l->dir);
    const Vec3 look = up ? l->pos : Vec3{l->pos.x + fx * 200, l->pos.y + fy * 200, l->pos.z};
    if (d2(p.pos, end) > 40) steer(f, p, end, false);
    else if (const float a = m_.world().rel_angle(p, look); std::fabs(a) > 30) f.mouse_dx = a / 0.18f;
    else if (tick_ % 10 == 0) f.press(IN_INTERACT);
    return true;
}

bool Autopilot::loot(InputFrame& f, const Unit& p, float radius) {
    World& w = m_.world();
    MapObjects& o = m_.objects();
    const GameData& gd = w.data();
    Vec3 best_pos{};
    int best_key = 0;
    float best_d = radius;
    bool box = false;
    // Floor items only when the pickup would take them (Unit_auto_pickup 0x49da30): gold, S7's gold bars / food, red food
    // that fits the HP missing, else what the bag has room for (World::can_give: give_item 0x495f40 without the change),
    // MISSION items too (S9L1's two gold keys and a letter, 40000 off, drew a full-bagged Joan for 900 ticks each). With a
    // full bag the bot walked over items it could not take (S5L1: 45000 ticks beside dropped apples).
    const auto takes = [&](int item, int level, int quantity) {
        const ItemDef& d = gd.items[std::size_t(item)];
        if (d.item_class == "GOLD" || (d.item_class == "MISSION" && (d.code == "S7_GOLDBAR" || d.code.starts_with("S7_food"))) ||
            (d.type == 0x15 && d.damage < w.max_hp(p) - p.hp))
            return true;
        return w.can_give(p, d.code, level, quantity) > 0;
    };
    // `far`: a MISSION item (mission 1's Imperial Scroll, dropped by the messenger when he falls: "You must retrieve
    // the letter from the stricken French archer") is fetched from anywhere, before anything else lying around.
    // `item` 0: a box.
    auto consider = [&](int key, Vec3 pos, int item, int level, int quantity, bool far = false) {
        const bool is_box = item == 0;
        if ((!far && std::fabs(pos.z - p.pos.z) > (is_box ? 60.0f : 150.0f)) || skip_.count(key)) return;
        const float d = far ? 0.0f : d2(p.pos, pos);
        if (d >= best_d) return;
        for (const Unit& u : w.units())  // left alone while an enemy stands next to it
            if (alive(w, u) && u.m.active && w.hostile_to_player(u) && !u.no_react && d2(u.pos, pos) < 500) return;
        if (!is_box && !takes(item, level, quantity)) return;
        best_d = d, best_pos = pos, best_key = key, box = is_box;
    };
    for (int r = 1; r <= o.size(); ++r) {
        const MapItem* it = o.item(r);
        if (!it) continue;
        if (it->cls == 10 && it->stage > 0 && it->content) consider(-r, it->pos, 0, 0, 0);  // BOX with something inside
        else if (it->on_floor && it->res > 0 && it->res < int(gd.items.size()) && gd.items[std::size_t(it->res)].pickable)
            consider(-r, it->pos, it->res, it->level, it->quantity);
    }
    for (const WorldItem& it : w.items())
        if (!it.deleted && it.item > 0 && it.item < int(gd.items.size()) && gd.items[std::size_t(it.item)].pickable)
            consider(it.id, it.pos, it.item, it.level, it.quantity, gd.items[std::size_t(it.item)].item_class == "MISSION");
    if (!best_key) return false;
    // 30 s spent on a target in all (not reached / not taken): given up. Counted per target: two or three boxes out of
    // reach that take turns as the nearest (S7L1: barrels behind St-Pierre's inner fence) never reset each other's time.
    if (++loot_spent_[best_key] > 900) return skip_.insert(best_key), false;
    goal = std::string(box ? "break box " : "pick up ") + at(best_pos);
    if (box && d2(p.pos, best_pos) < 75) {
        const float a = w.rel_angle(p, best_pos);
        f.mouse_dx = a / 0.18f;
        if (std::fabs(a) < 20 && tick_ % 20 == 0) f.press(IN_ATTACK_A);
        return true;
    }
    steer(f, p, best_pos, false);
    return true;
}

// The inventory window's drop (World::discard_item) for bag room: loot() would walk straight back over it.
bool Autopilot::discard(Unit& p, int bag_index) {
    World& w = m_.world();
    const std::size_t n = w.items().size();
    if (!w.discard_item(p, 10, bag_index, p)) return false;
    if (w.items().size() > n) skip_.insert(w.items().back().id);
    return true;
}

// Bag room for purchases (one cell a loaf, arrows a 1x4 stack, a sword a 2x4 block), as S4L1's: apples (EP food) beyond 5
// and spare weapons (item types 8..0xc; the character screen already holds the best sword) go on the floor (discard), the
// food moves into the troop grid (F3 eats from every grid) and the personal grid is packed again, the bigger items first
// (the inventory window's drags; cells are UI layout only).
void Autopilot::make_room(Unit& p) {
    const GameData& gd = m_.world().data();
    const auto type = [&](const ItemInstance& it) { return gd.items[std::size_t(it.item)].type; };
    for (int i = int(p.inv.bag.size()), apples = 0; i-- > 0;)
        if (const int t = type(p.inv.bag[std::size_t(i)]); (t == 0x17 && ++apples > 5) || (t >= 8 && t <= 0xc)) discard(p, i);
    for (ItemInstance& it : p.inv.bag)
        if (ItemInstance n = it; it.bag == 1 && (type(it) == 0x15 || type(it) == 0x17 || type(it) == 0x19) && p.inv.place(gd, n, 2)) it = n;
    for (ItemInstance& it : p.inv.bag)
        if (it.bag == 1) it.bag = -1;
    for (ItemInstance& it : p.inv.bag)
        if (const ItemDef& d = gd.items[std::size_t(it.item)]; it.bag < 0 && d.width * d.height > 1 && d.item_class != "ENHANCER")
            if (ItemInstance n = it; p.inv.place(gd, n, 1)) it = n;
    p.inv.layout(gd);
}

void Autopilot::step() {
    ++tick_;
    shot_ = false;
    InputFrame in = decide();
    if (m_.world().controller().range_mode && !shot_) in = InputFrame{}, in.press(IN_RANGE_MODE);  // Z again: leave range mode
    const int pid = m_.world().player_id();
    static const int dbg_from = std::getenv("JOA_BOT_DBG") ? std::atoi(std::getenv("JOA_BOT_DBG")) : -1;
    if (dbg_from >= 0 && tick_ >= dbg_from && tick_ < dbg_from + 200) {
        World& w = m_.world();
        if (const Unit* p = w.unit(pid)) {
            std::printf("t%d me %s act %d yaw %.0f hp %d in:%s%s%s%s mdx %.0f |", tick_, at(p->pos).c_str(), p->action, p->yaw_deg, p->hp,
                        in.held[IN_DEFEND] ? " Q" : "", in.pressed[IN_ATTACK_A] ? " A" : "", in.pressed[IN_ATTACK_B] ? " B" : "",
                        in.held[IN_FORWARD] ? " W" : "", in.mouse_dx);
            for (const Unit& u : w.units())
                if (alive(w, u) && w.hostile_to_player(u) && d2(p->pos, u.pos) < 250)
                    std::printf(" #%d d%.0f act %d face %.0f rel %.0f;", u.id, d2(p->pos, u.pos), u.action, w.rel_angle(u, p->pos),
                                w.rel_angle(*p, u.pos));
            std::printf(" %s\n", goal.c_str());
        }
    }
    m_.tick(in);
    World& tw = m_.world();
    for (const GameEvent& e : tw.take_events()) {
        kills += e.type == Ev::Death && e.other == pid && pid;
        hits += e.type == Ev::Hit && e.other == pid && pid;
        hurt += e.type == Ev::Hit && e.unit == pid && pid;
        if (e.type == Ev::Hit && pid) dealt += e.other == pid ? e.value : 0, taken += e.unit == pid ? e.value : 0;
        player_deaths += e.type == Ev::Death && e.unit == pid && pid;
        if (trace && e.type == Ev::ProjectileSpawn && e.unit == pid && pid)
            std::printf("[bot %d] shot #%d %s at #%d %s\n", tick_, e.other, e.name.c_str(), fight_target_, goal.c_str());
        if (trace && e.type == Ev::ProjectileHit && e.value >= 2)
            std::printf("[bot %d] projectile #%d type %d hit %s #%d\n", tick_, e.other, e.value, e.name.empty() ? "unit" : e.name.c_str(), e.unit);
        if (trace && e.type == Ev::Hit && e.unit != pid)  // hits taken by the other heroes (both sides) / the Bastard
            if (const Unit* v = tw.unit(e.unit); v && v->rank >= 4)
                if (const Unit* a = tw.unit(e.other))
                    std::printf("[bot %d] %s hit -%d (hp %d) by #%d %s team %d %s\n", tick_, v->name.c_str(), e.value, v->hp, a->id,
                                tw.data().units[std::size_t(a->cls)].code.c_str(), a->team, e.name.c_str());
        if (trace && e.type == Ev::Action && e.value == ATTACK) atk_start_[e.unit] = tick_;
        if (trace && e.type == Ev::Death)
            if (const Unit* v = tw.unit(e.unit)) {
                const Unit* k = tw.unit(e.other);
                std::printf("[bot %d] death #%d %s at %s by #%d %s team %d\n", tick_, v->id, tw.data().units[std::size_t(v->cls)].code.c_str(),
                            at(v->pos).c_str(), e.other, k ? tw.data().units[std::size_t(k->cls)].code.c_str() : "-", k ? k->team : 0);
            }
        if (trace && e.type == Ev::Hit && e.unit == pid && pid)
            if (const Unit* a = tw.unit(e.other))
                std::printf("[bot %d] hit -%d by #%d %s action %d (my action %d) windup %d dist %.0f\n", tick_, e.value, a->id,
                            tw.data().units[std::size_t(a->cls)].code.c_str(), a->action, tw.unit(pid)->action,
                            tick_ - atk_start_[a->id], d2(a->pos, tw.unit(pid)->pos));
        if (trace && e.type == Ev::Hit && e.other == pid && pid)
            if (const Unit* d = tw.unit(e.unit); d && tw.unit(pid))
                std::printf("[bot %d] dealt -%d to #%d %s hp %d (my action %d combo %s morale %d; its armor %d morale %d)\n", tick_, e.value, d->id,
                            tw.data().units[std::size_t(d->cls)].code.c_str(), d->hp, tw.unit(pid)->action, tw.unit(pid)->combo.c_str(),
                            tw.unit(pid)->morale, d->armor_level, d->morale);
    }
    bool news = false;
    for (const MissionEvent& e : m_.take_events()) {
        using K = MissionEvent::Kind;
        if (e.kind == K::InteractPrompt) {
            prompt_ = e.a != 0, prompt_id_ = e.b, prompt_kind_ = e.text, prompt_tick_ = tick_;
            if (trace) std::printf("[bot %d] prompt %d #%d %s\n", tick_, e.a, e.b, e.text.c_str());
        }
        if ((e.kind == K::AttentionMessage || e.kind == K::Interact) && trace) std::printf("[bot %d] %s %d %d %s\n", tick_, event_kind_name(e.kind), e.a, e.b, e.text.c_str());
        if (e.kind == K::AttentionMessage && e.text.find("close the gate") != std::string::npos && opened_gate_) go_home_ = true;
        if (e.kind == K::ObjectiveFinish || e.kind == K::ObjectiveFail || e.kind == K::Minimap || e.kind == K::ObjectiveAdd)
            last_progress_tick = tick_, news = true;
        if (e.kind == K::ObjectiveFinish || e.kind == K::ObjectiveFail || e.kind == K::Victory || e.kind == K::Defeat)
            log.push_back(std::to_string(e.tick) + " " + event_kind_name(e.kind) + " " + std::to_string(e.a) + " " + e.text);
    }
    // News for the talk errands (decide()): talk icons, followers, MISSION items carried. Whatever comes within 10 s of a talk
    // or of the cut scene after it is that talk's own outcome.
    // A talk icon a talk brings (the wagon after the priest's word, the villagers after the councillor's) has that NPC as its
    // giver: what it starts (an escort) ends with him.
    if (m_.ui().game_cut_scene || tw.cut_scene) {
        if (last_cut_tick_ != tick_ - 1) cut_start_tick_ = tick_;
        last_cut_tick_ = tick_;
    }
    const bool own = tick_ - last_talk_tick_ < 300 ||
                     (cut_start_tick_ >= last_talk_tick_ && cut_start_tick_ - last_talk_tick_ < 300 && tick_ - last_cut_tick_ < 300);
    std::uint64_t sig = 0;
    for (const Unit& u : tw.units()) {
        if (!alive(tw, u)) continue;
        if (!u.m.can_interact) mute_.insert(u.id);
        if (tw.hostile_to_player(u)) continue;
        if (u.m.can_interact && mute_.count(u.id) && new_talkers_.insert(u.id).second && own && last_talk_) giver_[u.id] = last_talk_;
        if (u.m.can_interact || following(u)) sig = sig * 1000003 + std::uint64_t(u.id) * 2 + following(u);
    }
    if (const Unit* p = tw.unit(pid))
        for (const ItemInstance& it : p->inv.bag)
            if (tw.data().items[std::size_t(it.item)].item_class == "MISSION") sig = sig * 31 + std::uint64_t(it.item);
    if (std::exchange(news_sig_, sig) != sig) news = true;
    if (news && ++news_ && last_talk_ && own) talk_news_[last_talk_] = news_;
    if (plan_) plan_->after_tick(*this);
    if (trace)
        for (std::size_t i = seen_units_; i < tw.units().size(); ++i, ++seen_units_)
            if (const Unit& u = tw.units()[i]; !u.deleted)
                std::printf("[bot %d] spawn #%d %s at %s\n", tick_, u.id, tw.data().units[std::size_t(u.cls)].code.c_str(), at(u.pos).c_str());
    if (trace && tick_ % trace == 0) {
        World& w = m_.world();
        if (const Unit* p = w.unit(pid)) {
            int food = 0;
            for (const ItemInstance& it : p->inv.bag) {
                const int t = w.data().items[std::size_t(it.item)].type;
                food += (t == 0x15 || t == 0x19) ? it.quantity : 0;
            }
            const Unit* j = nullptr;
            for (const Unit& u : w.units())
                if (!u.deleted && u.id != p->id && u.nation == p->nation && u.rank >= 4) j = &u;
            std::printf("[bot %d] %s hp %d/%d ep %d/%d lv %d xp %d food %d stage %d kills %d ally %s hp %d task %d tgt %d | %s\n", tick_,
                        at(p->pos).c_str(), p->hp, w.max_hp(*p), p->ep, w.max_ep(*p), p->level, p->xp, food, m_.scenario().stage, kills,
                        j ? at(j->pos).c_str() : "-", j ? j->hp : 0, j ? m_.ai().curr_task(*j) : 0, j ? j->target : 0, goal.c_str());
        }
    }
}

// --- campaign driver ----------------------------------------------------------------------------

MissionReport play_mission(CampaignRunner& run, int max_ticks, int retries) {
    MissionReport r;
    r.no = run.mission()->scenario_no();
    r.stem = run.mission()->info().stem();
    for (int attempt = 0; attempt <= retries; ++attempt) {
        // RETRY: the original reseeds from time(NULL); the bot uses seed + attempt (deterministic, but a
        // retry does not replay the identical loss).
        if (attempt) {
            const std::uint32_t base = run.seed;
            run.seed = base + std::uint32_t(attempt);
            run.retry();
            run.seed = base;
        }
        Mission& m = *run.mission();
        m.autosave = false;
        ++r.attempts;
        Autopilot ap(m);
        if (const char* t = std::getenv("JOA_BOT_TRACE")) ap.trace = std::atoi(t);
        int t = 0;
        // The game-over box (call_gameover_msg: a hero that must live died, too many citizens killed...)
        // freezes every unit until the player picks RETRY, also when no LOSE record ends the mission.
        while (t < max_ticks && m.finish() == 0 && !m.state().game_over) ap.step(), ++t;
        r.ticks += t;
        r.kills += ap.kills;
        r.dealt += ap.dealt, r.taken += ap.taken;
        r.result = m.finish() ? m.finish() : m.state().game_over ? 2 : 0;
        r.hash = m.hash();
        r.objectives_done = r.objectives_total = 0;
        r.log = ap.log;
        std::string blocking;
        for (std::size_t i = 0; i < m.state().end.size(); ++i) {
            const EndCondition& c = m.state().end[i];
            if (!c.has_des || !c.enabled || (c.type != EndCondition::Win && c.type != EndCondition::Side)) continue;
            ++r.objectives_total;
            if (c.finish == 1) ++r.objectives_done;
            else if (blocking.empty() && c.type == EndCondition::Win) blocking = "#" + std::to_string(i + 1) + " " + c.des;
        }
        if (r.result == 1) {
            r.stuck.clear();
            break;
        }
        if (r.result == 2) ++r.deaths;
        const Unit* p = m.world().unit(m.world().player_id());
        r.stuck = (r.result == 2 ? "lost: " + m.state().game_over_text : std::string("timeout")) + " | objective " + blocking +
                  " | bot " + ap.goal + " | player " + (p ? at(p->pos) : std::string("none")) + " | last progress tick " +
                  std::to_string(ap.last_progress_tick);
        if (r.result != 2) break;  // timeout: no retry
    }
    return r;
}

}  // namespace joa::game
