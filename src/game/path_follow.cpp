// Path request and following: Unit_move_to 0x505dd0, Unit_update_movement 0x52daa0, sidestep 0x4f7bf0
// (GAMEPLAY_AI_TROOPS.md Part C §3).
#include <cmath>

#include "game/ai_system.hpp"
#include "game/mount_ride.hpp"
#include "game/multimap_nav.hpp"

namespace joa::game {

namespace {
constexpr int kWkDist = 50;        // WK_DIST (GAMEPLAY.RES): walk below, run above
constexpr int kMaxWaitCount = 30;  // MAX_W_C
constexpr int kInitWaitCount = 5;  // INIT_W_C
constexpr int kStuckMs = 500;
float xy_d2(Vec3 a, Vec3 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); }
}  // namespace

// budget: the A* expansion cap (PathSpace_astar 0x66ce30 counts it down per popped poly): OrderExec_* pass 500..2000
// by distance, sidestep / click 100.
int AiSystem::move_to(Unit& u, Vec3 goal, int budget, bool from_leader, int issuer) {
    AiUnit& s = st(u);
    (void)issuer;
    if (u.m.physics_disabled) {  // +0x3b8: direct steering
        s.path = {goal};
        s.wp = 0;
        s.path_goal = goal;
        s.path_len = std::sqrt(xy_d2(u.pos, goal));
        return 1;
    }
    if (from_leader) {  // follow-leader shortcut: copy the leader's remaining path with an offset
        if (const Unit* L = w_.unit(leader_recno(u)); L && !w_.dead(*L) && xy_d2(u.pos, L->pos) <= 102400.0f) {
            const AiUnit& ls = st(*L);
            if (ls.wp >= int(ls.path.size())) return 3;
            const Vec3 off{u.pos.x - L->pos.x, u.pos.y - L->pos.y, u.pos.z - L->pos.z};
            s.path.assign(ls.path.begin() + ls.wp, ls.path.end());
            for (auto& p : s.path) p = {p.x + off.x, p.y + off.y, p.z + off.z};
            s.wp = 0;
            s.path_goal = goal;
            s.path_len = ls.path_len;
            return 1;
        }
    }
    s.path_goal = goal;
    s.wp = 0;
    if (!nav_) {
        s.path = {goal};
    } else {
        PathQuery q;
        q.from = {u.pos.x, u.pos.y, u.pos.z};
        q.to = {goal.x, goal.y, goal.z};
        const auto& t = w_.data().units[std::size_t(u.cls)];
        // BODY_R*2; cavalry/siege/horse-like units use 10 x their shape radius.
        q.radius = (t.unit_class == 8 || t.special == 2 || u.mount) ? 10.0f * t.cyl_radius : 2.0f * float(w_.data().c.body_r);
        q.body_r = float(w_.data().c.body_r);
        q.max_expand = budget;
        auto r = nav_->find_path(q);
        s.path.clear();
        for (auto& p : r.points) s.path.push_back({p.x, p.y, p.z});
    }
    float len = 0;
    Vec3 prev = u.pos;
    for (auto& p : s.path) { len += std::sqrt(xy_d2(prev, p)); prev = p; }
    s.path_len = len;
    return s.path.empty() ? 2 : 1;
}

void AiSystem::update_movement(Unit& u) {  // 0x52daa0
    AiUnit& s = st(u);
    // Unit_process phase 1 runs movement only for units with a body actor (+0x2f5): inactive units
    // (+0x2ed == 0, e.g. in a streamed-out level) keep their path but stand still.
    if (!u.m.active) return;
    // 0x52daa0 asm 0x52e75d..0x52e79f: the hero the player steers (+0x102 == 0) follows no path outside cut scenes and
    // point & click (controller +0x10 == 2). An order her troop copies to her as its leader (give_order APPROACH_ENEMY)
    // otherwise set RUN every tick after the keyboard controller had stopped her: she ran in place.
    if (!w_.cut_scene && u.id == w_.player_id() && w_.controller().mode != 2 && !u.ai_controlled) return;
    if (s.path.empty()) { s.stuck_since = -1; s.last_pos = u.pos; return; }
    const int a = u.action;
    // Riders keep steering during H_RUN / H_ATTACK: 0x52daa0 runs the path step for every live unit and
    // only skips the gear call while H_RUN; the horse carries the rider (mount_sync).
    if (is_knocked_ext(a) || is_flinch(a) || (is_attacking(a) && !u.mount) || a == DIE || a == STANDUP || u.airborne) {
        s.last_pos = u.pos;
        return;
    }
    const Unit* horse = u.mount ? w_.unit(u.mount) : nullptr;
    // Mounted: the horse's gait is the locomotion; arrival / stuck end in Unit_rider_stop (0x4533e0).
    const bool moving = horse ? (horse->action == WALK || horse->action == RUN || horse->action == MOVE_B) : is_locomotion(a);
    auto halt = [&] {
        if (horse) rider_stop(w_, u);
        else w_.stop(u);
    };
    const int now = w_.now();
    const int cls = class_of(u);
    const int base = w_.data().c.reach_dt;
    float R = float(base);
    if (cls == 8 || u.mount) R *= u.lod_tier <= 1 ? 5.0f : 10.0f;
    else if (u.lod_tier > 1) R *= 5.0f;
    // Last waypoint (0x52daa0): 2 x REACH_DT for riders / siege / LOD > 1 (it replaces the 5x/10x radius),
    // REACH_DT for LARGE_WEAPON_PUSH, else the running radius.
    auto radius_for = [&](int i) {
        float r = R;
        if (i == int(s.path.size()) - 1) {
            if (cls == 8 || u.mount || u.lod_tier > 1) r = 2.0f * float(base);
            if (s.task.id == task::LARGE_WEAPON_PUSH) r = float(base);
        }
        return r;
    };
    while (s.wp < int(s.path.size()) && xy_d2(u.pos, s.path[std::size_t(s.wp)]) <= radius_for(s.wp) * radius_for(s.wp)) ++s.wp;
    if (s.wp >= int(s.path.size())) {  // arrived
        clear_path(u);
        s.wait_len = s.wait_cnt = kInitWaitCount;
        s.stuck_since = -1;
        if (moving) halt();
        return;
    }
    // Blocked handling: stuck > 500 ms -> wait countdown -> sidestep once, then replan.
    const bool moved = xy_d2(u.pos, s.last_pos) > 1.0f;
    s.last_pos = u.pos;
    if (s.wait_cnt < s.wait_len) {  // waiting
        if (--s.wait_cnt > 0) return;
        Vec3 goal = s.path_goal;
        if (!s.sidestepped) {  // 0x4f7bf0
            s.sidestepped = 1;
            const int side = s.side;
            s.side = (s.side + 1) & 1;
            const float ang = u.yaw_deg + (side ? 1.0f : -1.0f) * float(90 + rand(25));
            const float dist = float(rand(100) + 20);
            const Vec3 p{u.pos.x + dist * std::cos(ang * 0.017453292f), u.pos.y + dist * std::sin(ang * 0.017453292f), u.pos.z};
            if (!nav_ || nav_->find_poly_xy(p.x, p.y, p.z) >= 0) {
                move_to(u, p, 100, false, 0);
                std::vector<Vec3> head = s.path;
                Unit tmp = u;
                tmp.pos = p;
                move_to(tmp, goal, 100, false, 0);  // path from the sidestep point to the old destination
                std::vector<Vec3> tail = s.path;
                s.path = head;
                s.path.insert(s.path.end(), tail.begin(), tail.end());
                s.path_goal = goal;
                s.sidestepped = 1;
            }
        } else {
            move_to(u, goal, 100, false, 0);
            s.sidestepped = 1;
        }
        s.wait_len = s.wait_len + 1 >= kMaxWaitCount ? kInitWaitCount : s.wait_len + 1;
        s.wait_cnt = s.wait_len;
        s.stuck_since = -1;
        return;
    }
    if (!moved && moving) {
        if (s.stuck_since < 0) s.stuck_since = now;
        else if (now - s.stuck_since > kStuckMs) {
            --s.wait_cnt;  // enter waiting
            halt();
            return;
        }
    } else {
        s.stuck_since = -1;
    }
    // Steer: face the waypoint (ponytail: instant turn, the original turns in place when |dh| > 3 deg),
    // walk when the whole path is shorter than WK_DIST, else run.
    const Vec3& wp = s.path[std::size_t(s.wp)];
    w_.set_yaw(u, std::atan2(wp.y - u.pos.y, wp.x - u.pos.x) / 0.017453292f);
    if (horse) {  // 0x52daa0: mounted -> FUN_00466d50 (run gear) unless the rider is in H_RUN
        if (a != H_RUN) horse_gear(w_, u, 2);
        return;
    }
    const int gait = float(kWkDist) < s.path_len ? RUN : WALK;
    if (u.action != gait && (w_.can_act(u) || is_locomotion(u.action))) w_.set_action(u, gait);
}

}  // namespace joa::game
