// Order executor OrderQueue_process 0x4baf80 and the 22 action executors (GAMEPLAY_AI_TROOPS.md Part B §2).
#include <cmath>

#include "game/ai_routines.hpp"

namespace joa::game {

namespace {
constexpr float kDegToRad = 0.017453292f;

bool lock(int a) { return a == KNOCKDN || a == KNOCKDN2 || a == KNOCKDN3 || a == STANDUP || a == DNATKED || a == H_DROP; }
bool idle_class(int a) { return (a >= IDLE && a <= IDLEDN) || a == JATTBI || a == H_IDLE || a == 0x59 || a == 0x5a; }
bool engatk(int a) { return a >= ENGATK && a <= ENGATK4; }
bool shoot_state(int a) { return a == SHTLOAD || a == SHTLOAD2 || a == SHTOUT || a == SHTIDLE || a == SHTOUTH || a == SHTIDLEH; }
float xy_d2(Vec3 a, Vec3 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); }
float d2_3(Vec3 a, Vec3 b) { return xy_d2(a, b) + (a.z - b.z) * (a.z - b.z); }
// Byte-angle LUT (0x576ac0 cos / 0x5768d0 sin, 256 steps per turn).
float cos256(int a) { return std::cos(float(a & 255) * 3.14159265f / 128.0f); }
float sin256(int a) { return std::sin(float(a & 255) * 3.14159265f / 128.0f); }
}  // namespace

int AiSystem::reach(const Unit& u) const {  // REACH(u)
    const int r = w_.data().c.reach_dt;
    return (u.mount || class_of(u) == 8 || u.lod_tier > 1) ? 2 * r : r;
}

bool AiSystem::can_interrupt(Unit& u) {  // 0x4bb3e0
    const Order& c = st(u).orders.cur;
    const int a = u.action;
    const bool expired = c.expire < w_.now();
    switch (c.action) {
    case order::COMBO_ATTACK: return a != ATTACK && a != H_ATTACK;
    case order::SHOOT: return !(shoot_state(a) || a == SHTLOAD2);
    case order::JUMP: return u.posture != 1 || w_.anim_remaining(u) == 0;
    case order::BLOCK: return a != GUARD1 && a != GUARDR;
    case order::MOVE: return a != RUN && a != WALK && a != TIPTOE;
    case order::TURN: return expired;
    case order::IDLE: case order::GATE_OPEN: case order::RECRUIT_TROOP: return expired || w_.anim_remaining(u) == 0;
    case order::DOWN_ATTACK: return a != ATTACK && a != H_ATTACK;
    case order::ENERGY_ATTACK: return !engatk(a);
    case order::RUSH_ATTACK: return a != RUSH;
    default: return true;
    }
}

void AiSystem::clear_path(Unit& u) {  // 0x422f10
    AiUnit& s = st(u);
    s.path.clear();
    s.wp = 0;
    s.path_len = 0;
    s.sidestepped = 0;
}

void AiSystem::stop(Unit& u) {  // STOP cleanup pattern
    AiUnit& s = st(u);
    clear_path(u);
    s.wait_cnt = s.wait_len;
    w_.stop(u);
}

void AiSystem::cleanup(Unit& u, int action) {  // per-action cleanup functions (finish_current switch)
    AiUnit& s = st(u);
    switch (action) {
    case order::COMBO_ATTACK: case order::MOVE: case order::SHIFT: case order::TURN:
        u.combo.clear(); u.combo_idx = 0; w_.stop(u); break;
    case order::SHOOT: case order::BLOCK: case order::DOWN_ATTACK: case order::ENERGY_ATTACK: case order::RUSH_ATTACK:
        w_.stop(u); break;
    case order::JUMP: w_.set_action(u, DOWN); break;
    case order::MOVE_TO_TARGET: case order::MOVE_TO_RELATIVE_TARGET:
        if (s.orders.cur.p[3]) u.m.track = 0;
        stop(u); break;
    case order::MOVE_ON_TRACK: u.m.track = 0; stop(u); break;
    case order::APPROACH_ENEMY: case order::APPROACH_ITEM: case order::APPROACH_PLACE:
    case order::FOLLOW_FORMATION: case order::FOLLOW_UNIT:
        stop(u); break;
    case order::SPECIAL_MOVEMENT: u.combo.clear(); u.combo_idx = 0; w_.stop(u); break;
    default: break;  // IDLE, GATE_OPEN, RECRUIT_TROOP: none
    }
}

void AiSystem::finish_current(Unit& u) {  // 0x4bb660
    AiUnit& s = st(u);
    const int a = s.orders.cur.action;
    cleanup(u, a);
    s.orders.cur.action = 0;
}

void AiSystem::execute_order(Unit& u) {  // OrderQueue_process 0x4baf80
    AiUnit& s = st(u);
    auto& q = s.orders;
    const int now = w_.now();
    for (std::size_t k = q.q.size(); k > 0; --k)
        if (unsigned(q.q[k - 1].expire) < unsigned(now)) q.q.erase(q.q.begin() + std::ptrdiff_t(k - 1));
    if (lock(u.action)) return;
    if (q.cur.action == 0) {
        if (!q.pop_front()) return;
    } else if (!q.q.empty() && q.q.front().prio > q.cur.prio && can_interrupt(u)) {
        finish_current(u);
        q.pop_front();
    }
    Order& c = q.cur;
    const bool expired = c.expire < now;
    auto done = [&] { c.action = 0; };
    auto fin = [&] { finish_current(u); };
    const bool locked = lock(u.action);
    const Vec3 U = u.pos;  // "u or horse": horses are not simulated, the rider's position is used
    auto path_to = [&](Vec3 g, int budget) {
        const int r = move_to(u, g, budget, c.from_leader != 0, c.issuer);
        return r;
    };
    // LEADERFLAG for 7/9/12/13/14: ret 3 keeps the flag only if the issuer is alive and (has no queue or
    // its current order is FOLLOW_FORMATION).
    auto leaderflag = [&](int r) {
        if (r == 3) {
            const Unit* is = w_.unit(c.issuer);
            const AiUnit* iss = find(c.issuer);
            if (!is || w_.dead(*is) || !(iss == nullptr || iss->orders.cur.action == order::FOLLOW_FORMATION ||
                                         iss->orders.empty()))
                c.from_leader = 0;
        } else {
            c.from_leader = 0;
        }
    };
    auto budget_of = [&](float dd) { return (u.rank > 2 && dd >= 2.25e6f) ? 1000 : 500; };

    switch (c.action) {
    case order::COMBO_ATTACK: {  // 0x4bbf20
        if (locked || u.action == DOWNATK || engatk(u.action)) { done(); break; }
        auto key = [&](int i) -> char {
            if (i >= 12) return 0;
            return char((unsigned(c.p[i / 4]) >> ((3 - i % 4) * 8)) & 0xff);
        };
        if (u.mount) { done(); break; }  // mounted combat: horses are not simulated
        if (u.action == ATTACK) {
            const int n = u.combo_idx > 0 ? int(u.combo.size()) : 0;
            int i = 0;
            while (i < n && u.combo[std::size_t(i)] == key(i)) ++i;
            if (i == n && key(i)) {
                if (w_.anim_remaining(u) == 0) { u.combo.clear(); u.combo_idx = 0; w_.stop(u); done(); break; }
                // HYPOTHESIS: the chain window opening (anim rec +0x28) = the move's StartTime (u.win_open).
                if (now - u.anim_start < u.win_open) break;
                if (!w_.find_move(u, ATTACK, u.combo + key(i))) break;
                w_.set_action(u, ATTACK, key(i));
                break;
            }
            if (w_.anim_remaining(u) == 0) { u.combo.clear(); u.combo_idx = 0; w_.stop(u); done(); }
        } else if (is_flinch(u.action)) {
            if (expired) done();
        } else if (u.action == DIE || w_.dead(u)) {
            done();
        } else if (u.posture == 1 || expired) {
            if (expired) done();
        } else if (idle_class(u.action)) {
            u.combo.clear();
            u.combo_idx = 0;
            const char first = key(0);
            if (first && w_.find_move(u, ATTACK, std::string(1, first))) {
                w_.set_action(u, ATTACK, first);
                clear_path(u);
                s.wait_cnt = s.wait_len;
            }
        }
        break;
    }
    case order::SHOOT: {  // 0x4bd090
        if (locked) { done(); break; }
        if (c.p[2] == 1) { if (idle_class(u.action)) done(); break; }
        if (!idle_class(u.action)) {
            if (u.action == DIE || w_.dead(u)) { done(); break; }
            if (shoot_state(u.action)) break;
            if (expired) done();
            break;
        }
        if (expired) { done(); break; }
        int target = 0;
        bool aim = false;
        float a = 0;
        if (c.p[0] == 1) {
            if (!unit_dead(c.p[1])) { target = c.p[1]; aim = true; a = w_.rel_angle(u, w_.unit(target)->pos); }
        } else if (c.p[0] == 2) {
            if (auto it = item ? item(c.p[1]) : std::nullopt) { aim = true; a = w_.rel_angle(u, it->pos); }
        }
        if (aim && (a < -10 || a > 10)) break;  // wait until facing within +-10 deg
        u.combo.clear();
        u.combo_idx = 0;
        // HYPOTHESIS: item / untargeted shots fire along the current facing (World::ai_shoot needs a unit).
        if (target) w_.ai_shoot(u, target);
        else if (w_.find_move(u, SHTLOAD)) w_.set_action(u, SHTLOAD);
        clear_path(u);
        s.wait_cnt = s.wait_len;
        c.p[2] = 1;
        break;
    }
    case order::JUMP:  // 0x4bf510
        if (locked) { done(); break; }
        if (u.posture == 1) { if (expired) done(); }
        else w_.set_action(u, UP);
        break;
    case order::BLOCK: {  // 0x4bf240
        if (locked || u.mount) { done(); break; }
        const int p1 = c.p[0];
        if (p1 == 0 || !unit_dead(p1)) {
            if (!s.path.empty()) { clear_path(u); s.wait_cnt = s.wait_len; }
            if (u.action == GUARD1 || u.action == GUARDR) {
                if (expired) {
                    if (p1 == 0 || unit_dead(p1)) { w_.stop(u); done(); }
                    else if (!will_be_attacked(u, p1)) {
                        w_.stop(u); done();
                        ai::after_block_light(*this, u) || ai::after_block_heavy(*this, u) || ai::after_block_siege(*this, u);
                    }
                }
            } else {
                w_.set_action(u, GUARD1);
            }
        } else {
            w_.stop(u); done();
            ai::after_block_light(*this, u) || ai::after_block_heavy(*this, u) || ai::after_block_siege(*this, u);
        }
        break;
    }
    case order::MOVE:  // 0x4bf5f0
        if (locked) { done(); break; }
        if (u.action == RUN || u.action == WALK || u.action == TIPTOE) {
            if (w_.anim_remaining(u) == 0) w_.set_action(u, u.action);
            else if (expired) done();
        } else {
            u.posture = 0;
            w_.set_action(u, c.p[0] == 2 ? RUN : WALK);
        }
        break;
    case order::SHIFT: {  // 0x4bf780
        const int a = u.action;
        if (locked || a == DOWNATK || engatk(a)) { done(); break; }
        if (a == MOVE_L || a == MOVE_R || a == MOVE_B || a == JUMP_B || a == JUMP_R || a == JUMP_L) {
            if (w_.anim_remaining(u) == 0 || expired) { w_.stop(u); done(); }
        } else {
            u.posture = 0;
            static const int m[] = {0, MOVE_L, MOVE_R, MOVE_B, JUMP_B};
            if (c.p[0] >= 1 && c.p[0] <= 4) w_.set_action(u, m[c.p[0]]);
        }
        break;
    }
    case order::MOVE_TO_TARGET: case order::MOVE_TO_RELATIVE_TARGET: {  // 0x4bfa10
        const Vec3 g = c.action == order::MOVE_TO_TARGET
                           ? Vec3{float(c.p[0]), float(c.p[1]), float(c.p[2])}
                           : Vec3{c.p[0] + U.x, c.p[1] + U.y, c.p[2] + U.z};
        if (locked) { done(); u.m.track = 0; break; }
        if (expired) { fin(); break; }
        if (!idle_class(u.action)) break;
        const float dd = xy_d2(U, g);
        const float R = float(reach(u));
        if (dd <= R * R) { fin(); break; }
        if (c.p[3]) u.m.track = c.p[3];
        leaderflag(path_to(g, budget_of(dd)));
        break;
    }
    case order::MOVE_ON_TRACK: {  // 0x4bfd70
        auto t = tracks.find(c.p[0]);
        if (!c.p[0] || t == tracks.end() || expired || t->second.size() < 2) { fin(); break; }
        const Vec3 g = c.p[1] == 0 ? t->second.front() : t->second.back();
        const float dd = d2_3(U, g);
        const float R = float(reach(u));
        if (c.p[2] && dd <= R * R) c.p[1] = !c.p[1];
        u.m.track = c.p[0];
        leaderflag(path_to(g, budget_of(dd)));
        break;
    }
    case order::TURN: {  // 0x49cef0
        if (locked || busy_attacking(u)) { done(); break; }
        const auto& t = w_.data().units[std::size_t(u.cls)];
        if (t.unit_class == 8 && t.special != 6 && t.special != 10 && !all_attached(u, 'F')) { done(); break; }
        if (w_.anim_remaining(u) == 0 && !idle_class(u.action)) w_.stop(u);
        double diff = std::fmod(double(c.p[0]) - double(u.yaw_deg), 360.0);
        if (diff < 0) diff += 360.0;
        const int rd = int(std::floor(diff + 0.5));
        if (rd <= 1 || rd >= 359) done();
        else w_.set_yaw(u, float(c.p[0]));  // ponytail: +0x1e desired heading applied at once (turn rate not modelled)
        break;
    }
    case order::IDLE:  // 0x4c25a0
        if (locked) { done(); break; }
        if (!idle_class(u.action)) w_.stop(u);
        else if (w_.anim_remaining(u) == 0) w_.stop(u);
        else if (expired) done();
        break;
    case order::APPROACH_ENEMY: {  // 0x4bd420
        if (locked) { done(); break; }
        Unit* T = w_.unit(c.p[0]);
        if (!T || w_.dead(*T) || expired) { fin(); break; }
        if (busy_attacking(u)) break;
        const int p2 = c.p[1], p3 = c.p[2];
        const int extra = rand(p3);
        const float dd = d2_3(U, T->pos);
        if (float(p2) * p2 < dd && dd <= float(p2 + p3) * float(p2 + p3)) { fin(); break; }
        // dir(T->U) in degrees = heading toward U from T; + rand(270) - 135
        const float dir = std::atan2(U.y - T->pos.y, U.x - T->pos.x) / kDegToRad;
        const float ang = dir + float(rand(270)) - 135.0f;
        const float r = float(p2 + extra);
        const int b = int(std::floor(ang / 360.0f * 256.0f));
        Vec3 G{T->pos.x + r * cos256(b), T->pos.y + r * sin256(b), T->pos.z};
        if (c.p[3] == 0) { s.approach_goal = G; c.p[3] = 1; }
        else G = s.approach_goal;
        const float R = float(reach(u));
        if (xy_d2(U, G) <= R * R) { fin(); break; }
        const float tg = d2_3(T->pos, G);
        if (tg > float(p2 + p3) * float(p2 + p3) || tg <= float(p2) * p2) {
            clear_path(u);
            s.wait_cnt = s.wait_len;
            c.p[3] = 0;
        }
        leaderflag(path_to(G, budget_of(d2_3(U, G))));
        if (c.action && !s.path.empty() && xy_d2(s.path.back(), U) <= R * R) fin();
        break;
    }
    case order::APPROACH_ITEM: {  // 0x4bdbc0
        if (locked) { done(); break; }
        auto it = item ? item(c.p[0]) : std::nullopt;
        if (!it || expired) { fin(); break; }
        if (busy_attacking(u) || !idle_class(u.action)) break;
        const int extra = rand(c.p[2]);
        const float R = float(c.p[1] + extra);
        if (d2_3(U, it->pos) <= R * R) { fin(); break; }
        const int a = rand(360);
        const int b = int(std::floor(float(a) / 360.0f * 256.0f));
        const Vec3 G{it->pos.x + R * cos256(b), it->pos.y + R * sin256(b), it->pos.z};
        leaderflag(path_to(G, budget_of(d2_3(U, G))));
        break;
    }
    case order::APPROACH_PLACE: {  // 0x4bdf90
        if (locked) { done(); break; }
        auto pit = places.find(c.p[0]);
        if (pit == places.end() || expired) { fin(); break; }
        if (!((u.m.track == 0 || s.path.empty()) && !busy_attacking(u) && idle_class(u.action))) break;
        AiPlace& P = pit->second;
        Vec3 G = P.centre;
        auto zit = zones.find(c.p[1]);
        if (c.p[1] && zit != zones.end()) {
            const AiPlace& Z = zit->second;
            if (Z.contains(U)) { fin(); break; }
            G = Z.centre;
            G.x = Z.min.x + float(rand(int(Z.max.x - Z.min.x)));
            G.y = Z.min.y + float(rand(int(Z.max.y - Z.min.y)));
        } else {
            int slot = 0;
            if (P.contains(U)) {
                if (is_leader(u)) {
                    // Place_find_slot 0x45e620: own slot, else nearest free (3-D); 0 if none.
                    for (std::size_t i = 0; i < P.occupant.size(); ++i)
                        if (P.occupant[i] == u.id) slot = int(i) + 1;
                    if (!slot) {
                        float best = 0;
                        for (std::size_t i = 0; i < P.slots.size(); ++i) {
                            if (i < P.occupant.size() && P.occupant[i] != 0 && !unit_dead(P.occupant[i])) continue;
                            const float dd = d2_3(U, P.slots[i]);
                            if (!slot || dd < best) { best = dd; slot = int(i) + 1; }
                        }
                    }
                    if (slot) {
                        if (P.occupant.size() < P.slots.size()) P.occupant.resize(P.slots.size());
                        const bool was = P.occupant[std::size_t(slot - 1)] == u.id;
                        P.occupant[std::size_t(slot - 1)] = u.id;
                        s.place = c.p[0];
                        s.place_slot = slot;
                        if (!was) update_formation(u, true, true);
                        G.x = P.slots[std::size_t(slot - 1)].x;
                        G.y = P.slots[std::size_t(slot - 1)].y;
                    }
                } else if (Unit* L = w_.unit(leader_recno(u))) {
                    const AiUnit& ls = st(*L);
                    if (ls.place_slot && ls.place == c.p[0]) { slot = ls.place_slot; G = formation_pos(u); }
                }
            }
            if (!slot) {
                if (P.contains(U) && is_leader(u)) cleanup(u, order::APPROACH_PLACE), c.action = 0;  // no return
                G.x = P.min.x + float(rand(int(P.max.x - P.min.x)));
                G.y = P.min.y + float(rand(int(P.max.y - P.min.y)));
            } else if (xy_d2(U, G) <= float(reach(u)) * float(reach(u))) {
                fin();
                if (std::size_t(slot - 1) < P.slot_facing.size())
                    w_.set_yaw(u, P.slot_facing[std::size_t(slot - 1)] / kDegToRad);
                break;
            }
        }
        const float dd = d2_3(U, G);
        const int budget = dd <= 250000 ? 500 : dd <= 2.25e6f ? 800 : dd <= 6.25e6f ? 1000 : dd <= 1.225e7f ? 1500 : 2000;
        leaderflag(path_to(G, budget));
        break;
    }
    case order::FOLLOW_FORMATION: {  // 0x4be770
        if (locked) { done(); break; }
        if (expired) { fin(); break; }
        if (busy_attacking(u) || !idle_class(u.action)) break;
        Vec3 G;
        if (c.p[2] == 1) {
            G = {float(c.p[0]), float(c.p[1]), U.z};
        } else {
            G = formation_pos(u);
            c.p[0] = int(G.x); c.p[1] = int(G.y); c.p[2] = 1;
            if (AiTeam* T = team_of_leader(leader_of(u))) { T->prev_anchor = T->anchor; T->prev_angle = T->angle; }
        }
        const float dd = xy_d2(U, G);
        const float R = float(reach(u));
        if (dd <= R * R) {
            fin();
            if (AiTeam* T = team_of_leader(leader_recno(u)); T && s.spawned) w_.set_yaw(u, T->angle / kDegToRad);
            break;
        }
        const Unit* is = w_.unit(c.issuer);
        if (!is || w_.dead(*is) || !find(c.issuer) || !find(c.issuer)->orders.has(order::FOLLOW_FORMATION) ||
            xy_d2(U, is->pos) >= 160000.0f)
            c.from_leader = 0;
        if (const Unit* t = w_.unit(u.target); t && !w_.dead(*t) && d2_3(U, t->pos) < dd) { fin(); break; }
        const int r = path_to(G, dd >= 2.25e6f ? 1000 : 500);
        if (r != 3 && r != 0) c.from_leader = 0;
        break;
    }
    case order::FOLLOW_UNIT: {  // 0x4bed40
        if (locked) { done(); break; }
        if (expired) { fin(); break; }
        const Unit* F = w_.unit(c.p[0]);
        if (!F) { fin(); break; }
        if (busy_attacking(u) || !idle_class(u.action)) break;
        const float a = F->yaw_rad() + float(c.p[1]) * kDegToRad;
        const Vec3 G{F->pos.x + float(c.p[2]) * std::cos(a), F->pos.y + float(c.p[2]) * std::sin(a), F->pos.z};
        const float dd = xy_d2(U, G);
        const float R = float(reach(u));
        if (dd <= R * R) { fin(); if (leader_recno(u)) w_.set_yaw(u, F->yaw_deg); break; }
        const float dF = xy_d2(U, F->pos);
        if (dF < float(c.p[2]) * float(c.p[2])) { fin(); break; }
        if (const Unit* t = w_.unit(u.target); t && !w_.dead(*t) && d2_3(U, t->pos) < dd) { fin(); break; }
        const int r = path_to(G, dF >= 2.25e6f ? 1000 : 500);
        if (r != 3 && r != 0) c.from_leader = 0;
        break;
    }
    case order::GATE_OPEN: {  // 0x4a71f0
        const int st_ = gate_state(c.p[0]);
        if (st_ >= 0 && ((c.p[1] == 0 && st_ == 1) || (c.p[1] != 0 && st_ == 0)) && on_gate_request)
            on_gate_request(u.id, c.p[0], c.p[1]);
        done();
        break;
    }
    case order::RECRUIT_TROOP: {  // 0x4c09a0
        if (u.nation == 0 || !is_leader(u)) { done(); break; }
        const auto& units = w_.data().units;
        int t = -1;
        // HYPOTHESIS: type+0x14d (race) is not loaded; the class kingdom stands in for it.
        for (int k = int(units.size()) - 1; k >= 0; --k)
            if (units[std::size_t(k)].kingdom == w_.data().units[std::size_t(u.cls)].kingdom &&
                units[std::size_t(k)].ranking == 0 && units[std::size_t(k)].unit_class != 0) { t = k; break; }
        if (t < 0) { done(); break; }
        std::vector<int> fresh;
        while (int(u.members.size()) < kMaxTeam && spend_recruit && spend_recruit(u.nation)) {
            const int r = w_.spawn_unit(units[std::size_t(t)].code, u.nation, u.pos, u.yaw_deg);
            Unit* v = w_.unit(r);
            if (!v) break;
            join_team(*v, u.id);
            st(*v).spawned = 1;
            fresh.push_back(r);
        }
        Unit& L = *w_.unit(u.id);  // spawn may reallocate the unit array
        for (int r : fresh) {
            Unit* v = w_.unit(r);
            v->pos = {L.pos.x + st(*v).slot.x, L.pos.y + st(*v).slot.y, L.pos.z + st(*v).slot.z};
            w_.set_yaw(*v, L.yaw_deg);
        }
        st(L).orders.cur.action = 0;
        return;
    }
    case order::DOWN_ATTACK:  // 0x4bc8a0
        if (u.action == DOWNATK || expired || locked) { done(); break; }
        if (!u.mount && w_.can_act(u) && w_.find_move(u, DOWNATK)) w_.set_action(u, DOWNATK);
        else done();
        break;
    case order::ENERGY_ATTACK: {  // 0x4bca60
        if (expired && can_interrupt(u)) { fin(); break; }
        if (engatk(u.action) || locked) { done(); break; }
        if (u.mount || !w_.find_move(u, ENGATK) || !can_energy_attack(u)) { done(); break; }
        const auto& k = w_.data().c;
        const int mp = int(float(w_.max_ep(u)) * float(k.egatk_mp) / 100.0f);
        const int hp = int(float(w_.max_hp(u)) * float(k.egatk_hp) / 100.0f);
        w_.set_action(u, ENGATK);
        const Unit* p = w_.unit(w_.player_id());
        if (!(p && (p->id == u.id || (u.nation && !w_.hostile(p->nation, u.nation))))) {  // player's allies pay nothing
            w_.set_ep(u, u.ep - mp);
            w_.set_hp(u, u.hp - hp);
        }
        u.ts_ep_regen = now;
        break;
    }
    case order::RUSH_ATTACK: {  // 0x4bcdb0
        if (expired && u.action != RUSH) { fin(); break; }
        if (engatk(u.action) || locked) { done(); break; }
        const MoveRecord* r = w_.find_move(u, RUSH);
        if (u.mount || !w_.can_act(u) || !r) { done(); break; }
        if (r->energy_pt <= u.ep) { w_.set_ep(u, u.ep - r->energy_pt); w_.set_action(u, RUSH); }
        else done();
        break;
    }
    case order::SPECIAL_MOVEMENT: {  // 0x4c2680
        if (expired) { u.combo.clear(); u.combo_idx = 0; w_.stop(u); done(); break; }
        const int A = 0x6b + c.p[0];  // EXTRA<p1>
        if (u.action == A) {
            if (w_.anim_remaining(u) == 0) {
                if (c.p[2] < 1) { u.combo.clear(); u.combo_idx = 0; w_.stop(u); done(); }
                else { w_.set_action(u, A); --c.p[2]; }
            }
        } else if (idle_class(u.action) && c.p[2] > 0) {
            if (w_.set_action(u, A)) --c.p[2];
            else --c.p[2];  // no EXTRA clip for this class: the repeat is consumed anyway
        }
        break;
    }
    default:
        done();
        break;
    }
}

}  // namespace joa::game
