// Teams (troops), recruiting, hold toggle and RTS commands (GAMEPLAY_AI_TROOPS.md Part B §4-§5).
#include <algorithm>
#include <cmath>
#include <set>

#include "game/ai_system.hpp"
#include "game/multimap_nav.hpp"

namespace joa::game {

namespace {
float d2_3(Vec3 a, Vec3 b) {
    return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
}
}  // namespace

// ---------------------------------------------------------------- membership

bool AiSystem::is_leader(const Unit& u) const { return teams_.count(u.id) != 0; }

int AiSystem::leader_of(const Unit& u) const {  // 0x4216d0
    if (is_leader(u)) return u.id;
    return u.leader && w_.unit(u.leader) ? u.leader : 0;
}

int AiSystem::leader_recno(const Unit& u) const {  // 0x6569f0
    const int l = leader_of(u);
    return l == u.id ? 0 : l;
}

void AiSystem::create_team(Unit& u) {  // 0x447600
    AiTeam& t = teams_[u.id];
    t.ftype = 1;
    t.anchor = t.prev_anchor = u.pos;
    t.angle = t.prev_angle = u.yaw_rad();
}

void AiSystem::join_team(Unit& m, int leader) {  // 0x438f80
    if (leader == 0) { leave_team(m); return; }
    disband_team(m);
    Unit* L = w_.unit(leader);
    if (!L || !is_leader(*L) || L->id == m.id) return;
    if (int(L->members.size()) >= kMaxTeam || !(L->nation == 0 || m.nation == 0 || m.nation == L->nation)) return;
    m.leader = leader;
    st(m).orphan = 0;
    m.team = L->team;
    if (std::find(L->members.begin(), L->members.end(), m.id) == L->members.end()) {  // Team::add_member 0x5287d0
        L->members.push_back(m.id);
        update_formation(*L, true, true);
    }
    if (m.level < L->level) w_.set_level(m, L->level);  // 0x523190
}

void AiSystem::leave_team(Unit& m) {  // 0x42fcb0
    if (!m.leader) return;
    if (Unit* L = w_.unit(m.leader)) {  // Team::remove_member 0x5288f0
        auto it = std::find(L->members.begin(), L->members.end(), m.id);
        if (it != L->members.end()) {
            L->members.erase(it);
            update_formation(*L, true, true);
        }
    }
    m.leader = 0;
    m.team = 0;
    m.m.control = 0;
}

void AiSystem::disband_team(Unit& L) {  // 0x42dd70
    if (!is_leader(L)) return;
    const std::vector<int> mem = L.members;
    for (auto it = mem.rbegin(); it != mem.rend(); ++it)
        if (Unit* m = w_.unit(*it)) {
            leave_team(*m);
            st(*m).orphan = 1;
        }
    L.members.clear();
    teams_.erase(L.id);
}

int AiSystem::max_troop_size(Unit& l) {  // 0x448080
    int lead = w_.stat_lead(l);
    lead = (w_.bonus(l, 0x17) + 100) * lead / 100;  // TROOP_SIZE item effect
    const int n = std::max(std::max(0, (lead - 20) / 5) + 9, int(l.members.size()));
    return std::min(n, kMaxTeam);
}

void AiSystem::auto_join(Unit& L) {  // 0x4655a0
    if (!is_leader(L) || w_.dead(L) || L.retreat != 0) return;
    const int cap = max_troop_size(L);  // constant over the loop: max(base, members) only grows to base by the joins
    for (auto& o : w_.units()) {
        if (int(L.members.size()) >= cap) return;
        // Cheap compares first (most units are teamed): the map lookups of is_leader dominated S9L1's tick (~200 leaders).
        if (o.team != 9999 || o.deleted || o.id == L.id || o.retreat != 0 || o.leader || o.nation != L.nation) continue;
        if (w_.dead(o) || is_leader(o) || st(o).orphan) continue;
        const auto& t = w_.data().units[std::size_t(o.cls)];
        if (t.special > 1 || t.unit_class == 0 || t.unit_class == 8 || t.unit_class == 9) continue;
        if (d2_3(o.pos, L.pos) > float(kAbsorbRadius) * kAbsorbRadius) continue;
        join_team(o, L.id);
        stop_all_order(o);
        w_.stop(o);
        clear_task(o);
    }
}

bool AiSystem::set_unit_team(Unit& u, int team_id) {  // 0x657fd0
    if (is_leader(u) || u.rank > 2) {
        if (!is_leader(u)) create_team(u);
        u.team = team_id;
        for (auto& o : w_.units())
            if (!o.deleted && o.id != u.id && o.team == team_id && o.leader != u.id) join_team(o, u.id);
        return true;
    }
    for (auto& o : w_.units())
        if (!o.deleted && o.team == team_id && is_leader(o)) { join_team(u, o.id); return true; }
    u.team = team_id;
    return false;
}

int AiSystem::recruit_team_member(Unit& u, std::string_view class_code, int n) {  // 0x6584e0
    const int lid = leader_of(u);
    int made = 0;
    for (int i = 0; i < n; ++i) {
        Unit* L = w_.unit(lid);
        if (!L || int(L->members.size()) == kMaxTeam) break;
        const int r = w_.spawn_unit(class_code, u.nation, L->pos, L->yaw_deg);
        Unit* v = w_.unit(r);
        if (!v) break;
        join_team(*v, lid);
        st(*v).spawned = 1;
        ++made;
    }
    return made;
}

int AiSystem::share_team_member(Unit& dst, Unit& src, int n) {  // 0x65ef40
    Unit* D = w_.unit(leader_of(dst));
    Unit* S = w_.unit(leader_of(src));
    if (!D || !S) return 0;
    int moved = 0;
    while (moved < n && !S->members.empty() && int(D->members.size()) < max_troop_size(*D)) {
        Unit* m = w_.unit(S->members.back());
        if (!m) { S->members.pop_back(); continue; }
        leave_team(*m);
        join_team(*m, D->id);
        ++moved;
    }
    return moved;
}

void AiSystem::on_unit_removed(int recno) {
    Unit& u = w_.units()[std::size_t(recno - 1)];
    if (u.leader) {
        if (Unit* L = w_.unit(u.leader)) {
            auto& v = L->members;
            v.erase(std::remove(v.begin(), v.end(), recno), v.end());
        }
        u.leader = 0;
    }
    if (teams_.count(recno)) {
        for (int m : std::vector<int>(u.members))
            if (Unit* mu = w_.unit(m)) { mu->leader = 0; mu->team = 0; mu->m.control = 0; st(*mu).orphan = 1; }
        u.members.clear();
        teams_.erase(recno);
    }
}

// ---------------------------------------------------------------- player commands

bool AiSystem::is_holding(const Unit& u) {
    const AiUnit& s = st(u);
    return s.task.id == 0 && s.task.p1 != 0;
}

void AiSystem::toggle_hold(Unit& u, bool recurse) {  // Unit_toggle_hold 0x5121c0
    if (u.m.control == 1) return;
    AiUnit& s = st(u);
    if (w_.game_mode == 4 || u.id != w_.player_id()) {
        if (s.task.id == 0 && s.task.p1 != 0) {  // resume
            s.task = s.task2;
            s.task2 = {0, 0, 1};
        } else {  // hold
            s.task2 = s.task;
            s.task = {0, 0, 1};
        }
    }
    stop_all_order(u);
    clear_path(u);
    if (recurse && is_leader(u))
        for (int m : std::vector<int>(u.members))
            if (Unit* mu = w_.unit(m)) toggle_hold(*mu, false);
}

int AiSystem::find_recruitable(Unit& p) {  // 0x48dfb0(100)
    if (p.team == 0 || !is_leader(p) || w_.dead(p) || int(p.members.size()) >= max_troop_size(p)) return 0;
    int best = 0;
    float bd = 0;
    for (auto& o : w_.units()) {
        if (o.deleted || o.id == p.id || o.nation != p.nation || o.team == p.team || o.rank >= 3) continue;
        if (o.id == w_.player_id() || o.m.control || o.m.vehicle || o.no_react || w_.dead(o)) continue;
        const auto& t = w_.data().units[std::size_t(o.cls)];
        if (t.unit_class == 0 || t.unit_class == 8 || t.unit_class == 9 || t.special > 1) continue;
        const float dd = d2_3(o.pos, p.pos);
        if (dd > 100.0f * 100.0f || std::fabs(w_.rel_angle(p, o.pos)) > 90.0f) continue;
        if (!best || dd < bd) { best = o.id; bd = dd; }
    }
    return best;
}

int AiSystem::recruit_nearby(Unit& p) {  // 0x43c210
    const int best = find_recruitable(p);
    if (!best) return 0;
    Unit& o = *w_.unit(best);
    leave_team(o);
    join_team(o, p.id);
    clear_task(o);
    stop_all_order(o);
    return best;
}

bool AiSystem::rts_selectable(const Unit& u) {  // 0x453620
    if (u.deleted || w_.dead(u)) return false;
    const auto& t = w_.data().units[std::size_t(u.cls)];
    if (t.unit_class == 0 || t.special == 2 || t.special == 7 || u.m.control) return false;
    if (st(u).task.id == task::LARGE_WEAPON_PUSH || u.m.vehicle) return false;
    if (!t.attach_points.empty() && !(all_attached(u, 'M') && all_attached(u, 'F'))) return false;
    return true;
}

std::vector<int> AiSystem::rts_select_with_team(int recno) {
    std::vector<int> sel;
    Unit* u = w_.unit(recno);
    if (!u || !rts_selectable(*u)) return sel;
    sel.push_back(recno);
    if (is_leader(*u))
        for (int m : u->members)
            if (Unit* mu = w_.unit(m); mu && rts_selectable(*mu)) sel.push_back(m);
    return sel;
}

namespace {
// Common RTS prologue: stop_task, end current order, drop queued orders; returns the priority to use.
int rts_reset(AiSystem& ai, Unit& u) {
    const int p = ai.st(u).task.prio;
    ai.clear_task(u);
    ai.stop_all_order(u);
    return p + 1;
}
}  // namespace

bool AiSystem::rts_move(std::span<const int> sel, Vec3 dest, bool attack_move) {  // Rts_cmd_move 0x44cfb0
    if (nav_ && nav_->find_poly_xy(dest.x, dest.y, dest.z) < 0) return false;  // 0x693580 destination test
    std::vector<Unit*> us;
    for (int id : sel)
        if (Unit* u = w_.unit(id); u && !w_.dead(*u)) us.push_back(u);
    if (us.empty()) return false;
    std::map<Unit*, int> prio;
    for (Unit* u : us) prio[u] = rts_reset(*this, *u);
    std::set<int> team_ids;
    std::map<int, int> cnt, k;
    Vec3 C;
    for (Unit* u : us) {
        team_ids.insert(u->team);
        ++cnt[u->team];
        C.x += u->pos.x; C.y += u->pos.y; C.z += u->pos.z;
    }
    C.x /= float(us.size()); C.y /= float(us.size());
    const float dir = std::atan2(dest.y - C.y, dest.x - C.x);
    const float c = std::cos(dir), s = std::sin(dir);
    const int S = 400;
    const int nt = int(team_ids.size());
    const int cols = int(std::ceil(std::sqrt(double(nt))));
    const float half = float((cols - 1) * S) / 2.0f;
    int i = 0;
    for (int t : team_ids) {
        const float row = float(-(i / cols) * S), lat = half - float((i % cols) * S);
        const Vec3 slot{dest.x + c * row - s * lat, dest.y + s * row + c * lat, dest.z};
        ++i;
        for (Unit* u : us) {
            if (u->team != t || st(*u).task.id == task::RTS_MOVE_TO) continue;
            Vec3 p;
            if (!leader_of(*u) || cnt[t] < 2) {  // individual grid
                const float sp = class_of(*u) == 8 ? 200.0f : 80.0f;
                const int cc = int(std::ceil(std::sqrt(double(cnt[t]))));
                const float h = float(cc - 1) * sp / 2.0f;
                const int j = k[t]++;
                const float r = -float(j / cc) * sp, l = h - float(j % cc) * sp;
                p = {float(int(slot.x)) + c * r - s * l, float(int(slot.y)) + s * r + c * l, slot.z};
            } else {
                if (AiTeam* T = team_of_leader(u->id)) {
                    T->anchor = slot;
                    T->angle = dir;
                    update_formation(*u, true, true);
                }
                const AiUnit& us_ = st(*u);
                p = {float(int(slot.x)) + c * us_.local_fwd - s * us_.local_side,
                     float(int(slot.y)) + s * us_.local_fwd + c * us_.local_side, float(int(slot.z)) + us_.slot.z};
            }
            set_task(*u, {attack_move ? task::RTS_ATTACK_MOVE : task::RTS_MOVE_TO, prio[u], int(p.x), int(p.y), int(p.z),
                          w_.now() + kRtsTaskMs, 0});
        }
    }
    return true;
}

void AiSystem::rts_attack_unit(std::span<const int> sel, int target) {  // 0x44eb80
    for (int id : sel)
        if (Unit* u = w_.unit(id); u && !w_.dead(*u)) set_task(*u, {task::RTS_ATTACK_UNIT, rts_reset(*this, *u), target, 0, 0, 0, 0});
}

void AiSystem::rts_attack_item(std::span<const int> sel, int item_id) {  // 0x44f1f0
    if (!item || !item(item_id)) return;
    for (int id : sel)
        if (Unit* u = w_.unit(id); u && !w_.dead(*u)) set_task(*u, {task::RTS_ATTACK_ITEM, rts_reset(*this, *u), item_id, 0, 0, 0, 0});
}

bool AiSystem::rts_assign(std::span<const int> sel, int target, bool confirmed, bool* needs_confirm) {  // 0x44f750
    if (needs_confirm) *needs_confirm = false;
    Unit* T = w_.unit(target);
    if (!T) return false;
    const auto& tt = w_.data().units[std::size_t(T->cls)];
    if (T->m.can_attach && !tt.attach_points.empty()) {  // A: siege crew -> LARGE_WEAPON_PUSH
        bool any = false;
        for (int id : sel) {
            Unit* u = w_.unit(id);
            if (!u || w_.dead(*u) || is_leader(*u)) continue;
            const std::string& code = w_.data().units[std::size_t(u->cls)].code;
            if (code.find("LIGHT_INFANTRY") == std::string::npos) continue;
            const bool mfull = all_attached(*T, 'M') == 1;
            if (mfull && all_attached(*T, 'F') == 1) break;
            leave_team(*u);
            set_task(*u, {task::LARGE_WEAPON_PUSH, rts_reset(*this, *u), target, mfull ? 1 : 0, 0, 0, 0});
            any = true;
        }
        return any;
    }
    std::vector<Unit*> cand;
    bool officer = false;
    for (int id : sel) {
        Unit* u = w_.unit(id);
        if (!u || w_.dead(*u)) continue;
        if (is_leader(*u) || u->rank >= 3) officer = true;
        else if (u->team != T->team && u->id != T->id) cand.push_back(u);
    }
    auto by_dist = [&](Unit* a, Unit* b) { return d2_3(a->pos, T->pos) < d2_3(b->pos, T->pos); };
    if (!officer && is_leader(*T)) {
        const int cap = max_troop_size(*T);
        const int size = int(T->members.size());
        const int n_new = int(cand.size());
        if (cap < n_new + size) {
            if (!confirmed && size > 0) {
                if (needs_confirm) *needs_confirm = true;
                return false;
            }
            std::vector<Unit*> mem;
            for (int m : T->members)
                if (Unit* mu = w_.unit(m)) mem.push_back(mu);
            std::sort(mem.begin(), mem.end(), by_dist);
            const int keep = std::max(0, cap - n_new);
            for (std::size_t j = std::size_t(keep); j < mem.size(); ++j) leave_team(*mem[j]);
            std::sort(cand.begin(), cand.end(), by_dist);
            const int room = cap - int(T->members.size());
            for (int j = 0; j < std::min(n_new, room); ++j) {
                leave_team(*cand[std::size_t(j)]);
                join_team(*cand[std::size_t(j)], T->id);
            }
        } else {
            for (Unit* u : cand) {
                if (int(T->members.size()) >= cap) break;
                const int k = class_of(*u);
                if (k == 8 || k == 9) continue;
                leave_team(*u);
                join_team(*u, T->id);
            }
        }
        return true;
    }
    if (officer && T->rank > 3) {
        for (int id : sel)
            if (Unit* u = w_.unit(id); u && !w_.dead(*u) && u->rank > 2)
                set_task(*u, {task::FOLLOW_UNIT, rts_reset(*this, *u), target, 0, 0, 0, 0});
        return true;
    }
    return false;
}

void AiSystem::rts_enter(Unit& p) {  // Rts_init 0x43fc90 (gameplay part)
    w_.stop(p);
    p.ai_controlled = true;
}

void AiSystem::rts_exit(Unit& p) {  // Rts_deinit 0x4404c0 (gameplay part)
    p.ai_controlled = false;
    stop_all_order(p);
    AiUnit& s = st(p);
    if (s.task.id != 0 || s.task.p1 == 0) { s.task2 = s.task; s.task = {}; }
    for (int m : std::vector<int>(p.members))
        if (Unit* mu = w_.unit(m)) { stop_all_order(*mu); st(*mu).task = {}; }
}

// Rts_dismiss_siege_crew 0x44a690: every reserved (+0x37c) then attached (+0x378) slot unit drops its task and
// orders and is released from the weapon; attached crew also stand (Unit_stop). Voice RTS_SIEGE_007 not played here.
void AiSystem::rts_dismiss_crew(int weapon) {
    Unit* wpn = w_.unit(weapon);
    if (!wpn || !wpn->m.can_attach) return;
    for (std::vector<int>* slots : {&wpn->m.reserved, &wpn->m.crew})
        for (std::size_t k = 0; k < slots->size(); ++k) {
            Unit* u = w_.unit((*slots)[k]);
            if (!u) continue;
            const bool attached = slots == &wpn->m.crew;
            clear_task(*u);
            stop_all_order(*u);
            if (k < wpn->m.reserved.size()) wpn->m.reserved[k] = 0;
            if (k < wpn->m.crew.size()) wpn->m.crew[k] = 0;
            u->m.vehicle = u->m.slot = 0;
            st(*u).push_slot = -1;
            if (attached) w_.stop(*u);
        }
}

}  // namespace joa::game
