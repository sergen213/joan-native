// Campaign bot (test / diagnostic only). See autopilot_player.hpp.
#include "game/autopilot_player.hpp"

#include <cmath>
#include <cstdio>

#include "game/multimap_nav.hpp"

namespace joa::game {

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
    // No progress over 90 ticks: strafe and jump for 40, then replan.
    if (tick_ - check_tick_ >= 90) {
        if (d2(check_pos_, p.pos) < 40) unstick_until_ = tick_ + 40, unstick_dir_ ^= 1, ++stuck_count_, path_tick_ = -1000;
        else stuck_count_ = 0;
        check_pos_ = p.pos, check_tick_ = tick_;
    }
    if (tick_ < unstick_until_) {
        f.held.set(unstick_dir_ ? IN_STRAFE_LEFT : IN_STRAFE_RIGHT);
        if (tick_ + 30 == unstick_until_) f.press(IN_JUMP);
    }
}

void Autopilot::fight(InputFrame& f, const Unit& p, const Unit& foe) {
    World& w = m_.world();
    f.camera_yaw_deg = std::atan2(foe.pos.y - p.pos.y, foe.pos.x - p.pos.x) / kDeg;
    // Guard (Q) against the nearest enemy swinging at the player from the front arc; the guard holds
    // while any such swing lasts. A swing of our own cannot be cancelled, so it is only started when
    // nobody is about to hit.
    const Unit* threat = nullptr;
    for (const Unit& u : w.units())
        if (alive(w, u) && w.hostile_to_player(u) && d2(p.pos, u.pos) < 170 && is_attacking(u.action) &&
            std::fabs(w.rel_angle(u, p.pos)) < 60 && (!threat || d2(p.pos, u.pos) < d2(p.pos, threat->pos)))
            threat = &u;
    if (threat && !is_attacking(p.action)) {
        f.mouse_dx = w.rel_angle(p, threat->pos) / 0.18f;
        f.held.set(IN_DEFEND);
        return;
    }
    const float d = d2(p.pos, foe.pos);
    if (d > 85) {  // close in
        steer(f, p, foe.pos, false);
        return;
    }
    f.mouse_dx = w.rel_angle(p, foe.pos) / 0.18f;
    // A A A B: presses 8 ticks apart land inside each follow-up window [START_TIME, END_TIME).
    const int ph = tick_ % 40;
    if (ph == 0 || ph == 8 || ph == 16) f.press(IN_ATTACK_A);
    else if (ph == 24) f.press(IN_ATTACK_B);
}

InputFrame Autopilot::decide() {
    InputFrame f;
    World& w = m_.world();
    Unit* pp = w.unit(w.player_id());
    goal.clear();
    if (!pp || !alive(w, *pp) || m_.ui().game_cut_scene || w.cut_scene) return f;
    Unit& p = *pp;
    // Character screen: spend attribute points (STR, HP, DEF in turn).
    while (p.attr_points > 0) {
        static const int order[] = {1, 6, 3};
        w.allocate_point(p, order[(p.level + p.attr_points) % 3], false);
    }
    // F3 when hurt; below 30 % with no potion left, fall back to the nearest friendly hero (Jean...) and
    // only guard until regen / a level-up brings HP back to 70 %.
    const int hp_pct = p.hp * 100 / std::max(1, w.max_hp(p));
    if (hp_pct < 45 && tick_ - last_heal_ > 60) f.press(IN_HEAL_ITEM), last_heal_ = tick_, heal_hp_ = p.hp;
    if (hp_pct < 30 && tick_ - last_heal_ > 30 && p.hp <= heal_hp_) retreat_ = true;
    if (hp_pct >= 70) retreat_ = false;
    const Unit* ally = nullptr;
    for (const Unit& u : w.units())
        if (u.id != p.id && alive(w, u) && u.nation == p.nation && u.rank >= 4 && u.retreat != -1 &&
            (!ally || d2(p.pos, u.pos) < d2(p.pos, ally->pos)))
            ally = &u;
    // Interact indicator: talk, free prisoners, pick up items, recruit.
    if (prompt_ && tick_ - last_interact_ > 20 && prompt_kind_ != "mount" && prompt_kind_ != "bribe" &&
        prompt_kind_ != "recruit_screen" && prompt_kind_ != "workshop") {
        last_interact_ = tick_;
        f.press(IN_INTERACT);
        goal = "interact " + prompt_kind_;
        return f;
    }
    // Objective marker: the first active minimap way point (scripts point the player with them).
    const Vec3* wp = nullptr;
    Vec3 wp_pos;
    int wp_slot = 0;
    for (const auto& x : m_.ui().waypoints) {
        ++wp_slot;
        if (x.type == 0) continue;
        const Unit* fu = x.follow ? w.unit(x.follow) : nullptr;
        wp_pos = fu ? fu->pos : Vec3{x.pos[0], x.pos[1], x.pos[2]};
        wp = &wp_pos;
        break;
    }
    // Enemies: keep the current target while it lives and stays near; else the nearest reachable one
    // close by, or one that is attacking the player.
    const Unit* foe = nullptr;
    if (const Unit* t = w.unit(fight_target_); t && alive(w, *t) && t->m.active && w.hostile_to_player(*t) && d2(p.pos, t->pos) < 700)
        foe = t;
    if (!foe || foe->target != p.id) {  // archers close by first, then whoever targets the player, then the nearest
        float best = 1e30f;
        const float reach = wp ? 450.0f : 1e9f;
        for (const Unit& u : w.units()) {
            if (!alive(w, u) || !u.m.active || !w.hostile_to_player(u) || u.no_react || std::fabs(u.pos.z - p.pos.z) > 150) continue;
            const auto& c = w.data().units[std::size_t(u.cls)];
            if (c.unit_class == 8 || c.special == 2) continue;  // siege weapons, horses
            float d = d2(p.pos, u.pos);
            if (u.target == p.id) d *= 0.5f;
            if ((c.unit_class == 5 || c.unit_class == 6) && d < 350) d *= 0.3f;
            if (d < reach && d < best) best = d, foe = &u;
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
    fight_target_ = foe ? foe->id : 0;
    if (foe) {
        goal = "fight #" + std::to_string(foe->id) + " " + w.data().units[std::size_t(foe->cls)].code + " " + at(foe->pos);
        fight(f, p, *foe);
        return f;
    }
    // Travel together: an ally hero left more than 700 behind is waited for (walked back to).
    if (ally && d2(p.pos, ally->pos) > 700 && d2(p.pos, ally->pos) < 3000) {
        goal = "regroup with #" + std::to_string(ally->id);
        steer(f, p, ally->pos, false);
        return f;
    }
    // A reached marker that the script does not move on within 10 s means "continue from here": head
    // for the open WIN objective's territory (the minimap shows towns), else hunt the nearest enemy.
    if (wp && d2(done_wp_, *wp) > 1) {
        if (d2(p.pos, *wp) > 150) seen_tick_ = tick_;
        else if (tick_ - seen_tick_ > 300) done_wp_ = *wp;  // stood there 10 s: exhausted until it moves
        if (d2(done_wp_, *wp) > 1) {
            goal = "waypoint " + std::to_string(wp_slot) + " " + at(*wp);
            if (d2(p.pos, *wp) > 80) steer(f, p, *wp, false);
            return f;
        }
    }
    for (const EndCondition& c : m_.state().end) {
        if (!c.has_des || !c.enabled || c.type != EndCondition::Win || c.finish == 1 || c.territory_link.empty()) continue;
        const Territory* t = m_.places().territory(m_.places().territory_by_name(c.territory_link));
        if (!t) continue;
        const Vec3 g{float(t->center_x), float(t->center_y), p.pos.z};
        if (d2(p.pos, g) < 300) break;
        goal = "territory " + c.territory_link + " " + at(g);
        steer(f, p, g, false);
        return f;
    }
    const Unit* far = nullptr;
    for (const Unit& u : w.units())
        if (alive(w, u) && u.m.active && w.hostile_to_player(u) && !u.no_react && (!far || d2(p.pos, u.pos) < d2(p.pos, far->pos)))
            far = &u;
    if (far) {
        goal = "hunt #" + std::to_string(far->id) + " " + at(far->pos);
        steer(f, p, far->pos, false);
        return f;
    }
    // Nothing left: walk to the nearest item lying around, else stand.
    const WorldItem* best = nullptr;
    for (const WorldItem& it : w.items())
        if (!it.deleted && (!best || d2(p.pos, it.pos) < d2(p.pos, best->pos))) best = &it;
    if (best && d2(p.pos, best->pos) < 3000) {
        goal = "item " + at(best->pos);
        steer(f, p, best->pos, false);
        return f;
    }
    goal = "idle";
    return f;
}

void Autopilot::step() {
    ++tick_;
    const InputFrame in = decide();
    const int pid = m_.world().player_id();
    m_.tick(in);
    for (const GameEvent& e : m_.world().take_events()) {
        kills += e.type == Ev::Death && e.other == pid && pid;
        hits += e.type == Ev::Hit && e.other == pid && pid;
        hurt += e.type == Ev::Hit && e.unit == pid && pid;
        if (e.type == Ev::Hit && pid) dealt += e.other == pid ? e.value : 0, taken += e.unit == pid ? e.value : 0;
        player_deaths += e.type == Ev::Death && e.unit == pid && pid;
    }
    for (const MissionEvent& e : m_.take_events()) {
        using K = MissionEvent::Kind;
        if (e.kind == K::InteractPrompt) prompt_ = e.a != 0, prompt_kind_ = e.text;
        if (e.kind == K::ObjectiveFinish || e.kind == K::ObjectiveFail || e.kind == K::Minimap || e.kind == K::ObjectiveAdd)
            last_progress_tick = tick_;
        if (e.kind == K::ObjectiveFinish || e.kind == K::ObjectiveFail || e.kind == K::Victory || e.kind == K::Defeat)
            log.push_back(std::to_string(e.tick) + " " + event_kind_name(e.kind) + " " + std::to_string(e.a) + " " + e.text);
    }
}

// --- campaign driver ----------------------------------------------------------------------------

MissionReport play_mission(CampaignRunner& run, int max_ticks, int retries) {
    MissionReport r;
    r.no = run.mission()->scenario_no();
    r.stem = run.mission()->info().stem();
    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (attempt) run.retry();
        Mission& m = *run.mission();
        m.autosave = false;
        ++r.attempts;
        Autopilot ap(m);
        int t = 0;
        while (t < max_ticks && m.finish() == 0) ap.step(), ++t;
        r.ticks += t;
        r.kills += ap.kills;
        r.dealt += ap.dealt, r.taken += ap.taken;
        r.result = m.finish();
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
