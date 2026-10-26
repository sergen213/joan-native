// AiSystem core: per-unit state, IUnitBrain phases, tasks, give_order, dispatch, persistence.
#include "game/ai_system.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <type_traits>

#include "game/ai_routines.hpp"

namespace joa::game {

namespace {
struct Writer {
    std::vector<std::uint8_t>& o;
    template <class T> void operator()(T& x) {
        if constexpr (std::is_arithmetic_v<T>) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
            o.insert(o.end(), p, p + sizeof(T));
        } else if constexpr (requires { x.size(); x.resize(0); x[0]; }) {
            std::uint32_t n = std::uint32_t(x.size());
            (*this)(n);
            for (auto& e : x) (*this)(e);
        } else {
            x.visit(*this);
        }
    }
};
struct Reader {
    std::span<const std::uint8_t> in;
    std::size_t pos = 0;
    bool ok = true;
    template <class T> void operator()(T& x) {
        if constexpr (std::is_arithmetic_v<T>) {
            if (pos + sizeof(T) > in.size()) { ok = false; return; }
            std::memcpy(&x, in.data() + pos, sizeof(T));
            pos += sizeof(T);
        } else if constexpr (requires { x.size(); x.resize(0); x[0]; }) {
            std::uint32_t n = 0;
            (*this)(n);
            if (!ok || n > in.size()) { ok = false; return; }
            x.resize(n);
            for (auto& e : x) (*this)(e);
        } else {
            x.visit(*this);
        }
    }
};
template <class V, class K, class T> void visit_map(V& v, std::map<K, T>& m) {
    std::uint32_t n = std::uint32_t(m.size());
    v(n);
    if constexpr (std::is_same_v<V, Reader>) {
        m.clear();
        for (std::uint32_t i = 0; i < n && v.ok; ++i) {
            K k{};
            T t{};
            v(k), v(t);
            m.emplace(k, std::move(t));
        }
    } else {
        for (auto& [k, t] : m) { K kk = k; v(kk), v(t); }
    }
}
}  // namespace

AiSystem::AiSystem(World& w, const NavWorld* nav) : w_(w), nav_(nav) {
    item = [this](int id) -> std::optional<AiItem> {
        WorldItem* it = w_.item(id);
        if (!it || it->deleted) return std::nullopt;
        AiItem a;
        a.pos = it->pos;
        a.code = w_.data().items[std::size_t(it->item)].code;
        return a;
    };
    item_ids = [this] {
        std::vector<int> ids;
        for (const auto& it : w_.items())
            if (!it.deleted) ids.push_back(it.id);
        return ids;
    };
    w_.set_brain(this);
}

AiSystem::~AiSystem() { w_.set_brain(nullptr); }

int AiSystem::ai_class_for(const Unit& u) const {  // UnitAI_class_from_name 0x546520 (via unit.res ScriptFile)
    std::string s = w_.data().units[std::size_t(u.cls)].script_file;
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    if (auto p = s.rfind(".scr"); p != std::string::npos) s.resize(p);
    static const std::pair<const char*, int> k[] = {
        {"human_light_infantry", 1}, {"human_light_general", 2}, {"special_general", 3}, {"human_heavy_infantry", 4},
        {"human_heavy_general", 5}, {"human_archer", 6}, {"human_archer_ex", 6}, {"archer_gen", 7},
        {"archer_ex_gen", 7}, {"human_cannon", 8}, {"autobow", 9}, {"batterram", 10}, {"camp_horse", 11},
        {"cart", 12}, {"citizen", 13}, {"human_trebuchet", 14}, {"pig", 15}};
    for (auto& [n, id] : k)
        if (s == n) return id;
    return 0;  // NONE, human_horse: no native AI
}

void AiSystem::ensure_init(Unit& u) {
    AiUnit& s = units_[u.id];
    if (s.initialised) return;
    s.initialised = true;
    s.wait_len = s.wait_cnt = 5;  // INIT_W_C (GAMEPLAY.RES)
    s.last_pos = u.pos;
    if (u.ai_class == 0) u.ai_class = ai_class_for(u);
    if (u.threat == 0) u.threat = base_attack_chance(u);
    if (u.rank > 2 && u.leader == 0 && !teams_.count(u.id)) create_team(u);  // set_rank 0x5089e0
}

AiUnit& AiSystem::st(const Unit& u) {
    AiUnit& s = units_[u.id];
    if (!s.initialised) ensure_init(const_cast<Unit&>(u));
    return s;
}

bool AiSystem::st_settled() {
    if (settled_gen_ != w_.load_generation()) settled_gen_ = w_.load_generation(), settled_ = 0;
    const std::vector<Unit>& us = w_.units();
    for (; settled_ < us.size(); ++settled_) {
        const Unit& o = us[settled_];
        if (o.deleted) continue;
        const auto it = units_.find(o.id);
        if (it == units_.end() || !it->second.initialised) return false;
    }
    return true;
}

const AiUnit* AiSystem::find(int recno) const {
    auto it = units_.find(recno);
    return it == units_.end() ? nullptr : &it->second;
}

AiTeam* AiSystem::team_of_leader(int recno) {
    auto it = teams_.find(recno);
    return it == teams_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------- phases

void AiSystem::execute_orders(World& w, Unit& u) {
    AiUnit& s = st(u);
    if (w.dead(u)) {  // die 0x7c6240 clears the task; corpses keep their team slot
        if (s.task.id) clear_task(u);
        return;
    }
    if (s.task.id == task::RETREAT && s.task.p3 > 0) s.task.p3 -= World::kTickMs;  // phase 0 timer
    update_movement(u);                                   // phase 1 Unit_update_movement
    if (is_leader(u)) {                                   // phase 2: 0x4655a0, 0x4fa0f0(0,1)
        auto_join(u);
        update_formation(u, false, true);
    }
    const int id = u.id;
    execute_order(u);
    Unit& me = *w.unit(id);                               // an order that spawns units (RECRUIT_TROOP) may reallocate them
    me.m.task_id = s.task.id;                             // mirror for scripts / UI
    me.m.task_state = s.task.state;
    me.m.task_params[0] = s.task.p1, me.m.task_params[1] = s.task.p2, me.m.task_params[2] = s.task.p3;
    me.m.order_id = s.orders.cur.action;
}

void AiSystem::think(World& w, Unit& u) {  // Unit_ai_think 0x49d810
    // 0x49d810 head: the player (+0x102 == 0) and a horse with a rider (+0x1e6) go to Unit_player_control.
    if ((u.id == w.player_id() && !u.ai_controlled) || u.m.rider) return;
    AiUnit& s = st(u);
    if (!s.orders.empty()) return;  // AI only plans when the queue is empty
    const int now = w.now();
    if (u.think_ts < 0) { u.think_ts = now + rand(500); return; }
    if (now - u.think_ts < w.data().units[std::size_t(u.cls)].script_duration) return;
    expire_task(u);
    dispatch(u);
    u.think_ts = now;
}

bool AiSystem::dispatch(Unit& u) {
    switch (u.ai_class) {
    case 1: return ai::light_infantry(*this, u);
    case 2: return ai::light_general(*this, u);
    case 3: return ai::special_general(*this, u);
    case 4: return ai::heavy_infantry(*this, u);
    case 5: return ai::heavy_general(*this, u);
    case 6: return ai::archer(*this, u);
    case 7: return ai::archer_general(*this, u);
    case 8: return ai::cannon(*this, u);
    case 9: return ai::autobow(*this, u);
    case 10: return ai::batterram(*this, u);
    case 11: return ai::camp_horse(*this, u);
    case 12: return ai::cart(*this, u);
    case 13: return ai::citizen(*this, u);
    case 14: return ai::trebuchet(*this, u);
    case 15: return ai::pig(*this, u);
    default: return false;
    }
}

// ---------------------------------------------------------------- tasks

bool AiSystem::set_task(Unit& u, const Task& t) {  // 0x527fb0
    AiUnit& s = st(u);
    if (u.no_react) return false;  // +0x2e1 task locked
    const int now = w_.now();
    if (!(s.task.id == 0 || s.task.prio < t.prio || (s.task.expire && s.task.expire < now))) return false;
    // ponytail: the news-log entries (msg 0x30-0x32) are not generated; nothing gameplay reads them.
    clear_task(u);
    s.task = t;
    s.task.state = 0;
    s.task2 = {};
    if (t.id == task::LARGE_WEAPON_PUSH) {
        u.think_ts = 1;
        think(w_, u);
    }
    return true;
}

void AiSystem::clear_task(Unit& u) {  // 0x528300
    AiUnit& s = st(u);
    if (s.task.id == task::LARGE_WEAPON_PUSH && s.push_slot >= 0) {
        if (Unit* wpn = w_.unit(s.task.p1)) {
            const auto k = std::size_t(s.push_slot);
            if (k < wpn->m.reserved.size()) wpn->m.reserved[k] = 0;
            if (k < wpn->m.crew.size()) wpn->m.crew[k] = 0;
        }
        u.m.vehicle = 0;
        u.m.slot = 0;
        s.push_slot = -1;
    }
    s.task = {};
    s.task2 = {};
}

void AiSystem::expire_task(Unit& u) {  // 0x528440
    AiUnit& s = st(u);
    if (s.task.expire && s.task.expire < w_.now()) s.task = {};
}

int AiSystem::curr_task(const Unit& u) {  // 0x6520d0
    if (w_.dead(u)) return 0;
    AiUnit& s = st(u);
    if (s.task.id) return s.task.id;
    const int l = leader_recno(u);
    if (!l) return 0;
    Unit* L = w_.unit(l);
    const int lt = st(*L).task.id;
    if (lt == task::RTS_MOVE_TO) return 0;
    const_cast<Unit&>(u).m.track = L->m.track;
    return lt;
}

int AiSystem::curr_task_para(const Unit& u, int i) {  // 0x652190
    AiUnit& s = st(u);
    const int l = leader_recno(u);
    if (s.task.id == 0 && l) return st(*w_.unit(l)).task.para(i);
    return s.task.para(i);
}

int AiSystem::curr_task_state(const Unit& u) {
    AiUnit& s = st(u);
    const int l = leader_recno(u);
    if (s.task.id == 0 && l) return st(*w_.unit(l)).task.state;
    return s.task.state;
}

void AiSystem::set_curr_task_state(Unit& u, int v) {
    AiUnit& s = st(u);
    if (s.task.id || leader_recno(u)) s.task.state = v;
}

bool AiSystem::give_task(Unit& u, int id, int prio, int p1, int p2, int p3, int duration_ms) {  // 0x6619d0
    const Task t{id, prio, p1, p2, p3, duration_ms ? w_.now() + duration_ms : 0, 0};
    const bool ok = set_task(u, t);
    if (id == task::RETREAT && p2 == 1)
        if (Unit* L = w_.unit(leader_of(u)))
            for (int m : std::vector<int>(L->members))
                if (Unit* mu = w_.unit(m); mu && m != u.id) set_task(*mu, t);
    return ok;
}

bool AiSystem::give_team_task(Unit& u, int id, int prio, int p1, int p2, int p3, int duration_ms) {  // 0x661b40
    const Task t{id, prio, p1, p2, p3, duration_ms ? w_.now() + duration_ms : 0, 0};
    const bool ok = set_task(u, t);
    if (Unit* L = w_.unit(leader_of(u))) {
        if (L != &u) set_task(*L, t);
        for (int m : std::vector<int>(L->members))
            if (Unit* mu = w_.unit(m); mu && m != u.id) set_task(*mu, t);
    }
    return ok;
}

void AiSystem::stop_all_team_task(int team_id) {
    for (auto& o : w_.units())
        if (!o.deleted && o.team == team_id) clear_task(o);
}

void AiSystem::set_idle(Unit& u) {  // lose/win freeze 0x6b8af0
    stop_all_order(u);
    clear_task(u);
    st(u).task = {task::IDLE, 0x7f, 1, 0, 0, 0, 0};
}

// ---------------------------------------------------------------- orders

int AiSystem::give_order(Unit& u, int action, int prio, bool team, int p1, int p2, int p3, int p4, int duration_ms) {
    if (u.deleted) return 0;
    const int expire = w_.now() + duration_ms;
    auto add = [&](Unit& t, int pr, int from_leader, int issuer) {
        bool retarget = false;
        st(t).orders.add({action, pr, {p1, p2, p3, p4}, from_leader, issuer, expire}, &retarget);
        if (retarget) { stop(t); }  // cleanup_12 then the record is re-armed (OrderQueue::add)
    };
    Unit* L = w_.unit(leader_of(u));
    if (action == order::FOLLOW_FORMATION && team && L) {
        for (auto it = L->members.rbegin(); it != L->members.rend(); ++it)
            if (Unit* m = w_.unit(*it); m && st(*m).orders.has(order::FOLLOW_FORMATION)) { team = false; break; }
    }
    add(u, prio, 0, 0);
    if (!(team || action == order::APPROACH_ENEMY) || !L) return 1;
    const int lt = st(*L).task.id;
    for (auto it = L->members.rbegin(); it != L->members.rend(); ++it) {
        Unit* m = w_.unit(*it);
        if (!m) continue;
        AiUnit& ms = st(*m);
        if (!(lt == 0 || ms.task.id == 0 || lt == ms.task.id)) continue;
        if (action == order::FOLLOW_FORMATION && class_of(*m) == 8) continue;
        if (action == order::APPROACH_ENEMY && (m->target != 0 || ms.orders.cur.action == order::APPROACH_ENEMY)) continue;
        add(*m, prio - 1, 1, u.id);
    }
    const int ut = st(u).task.id;
    if (L != &u && action != order::FOLLOW_FORMATION && (lt == 0 || ut == 0 || lt == ut)) {
        if (!(action == order::APPROACH_ENEMY && (L->target != 0 || st(*L).orders.cur.action == order::APPROACH_ENEMY)))
            add(*L, prio - 1, 1, u.id);
    }
    return 1;
}

void AiSystem::stop_all_order(Unit& u) {  // 0x6618f0: end current with cleanup, free the queue
    AiUnit& s = st(u);
    if (s.orders.cur.action) finish_current(u);
    s.orders = {};
}

int AiSystem::curr_order(const Unit& u) { return st(u).orders.cur.action; }
bool AiSystem::has_order(const Unit& u, int action) { return st(u).orders.has(action); }

// ---------------------------------------------------------------- misc helpers

void AiSystem::set_unit_dead(Unit& u) {
    // Script set_unit_dead: the unit fades out ("escaped"); no death event, no kill credit.
    stop_all_order(u);
    clear_task(u);
    u.retreat = -1;
    w_.delete_unit(u.id);
}

// ---------------------------------------------------------------- persistence

void AiSystem::serialize(std::vector<std::uint8_t>& out) const {
    Writer wr{out};
    auto& self = const_cast<AiSystem&>(*this);
    visit_map(wr, self.units_);
    visit_map(wr, self.teams_);
    wr(self.chapter);
}

bool AiSystem::deserialize(std::span<const std::uint8_t> in) {
    Reader rd{in};
    settled_ = 0;
    visit_map(rd, units_);
    visit_map(rd, teams_);
    rd(chapter);
    return rd.ok && rd.pos == in.size();
}

std::uint64_t AiSystem::hash() const {
    std::vector<std::uint8_t> b;
    serialize(b);
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint8_t c : b) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

}  // namespace joa::game
