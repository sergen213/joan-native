// Team formation slots: Team::update_formation 0x4fa0f0, slot picker 0x45de50, formation_pos 0x448870,
// get_follow_unit_distance 0x6592c0 (GAMEPLAY_AI_TROOPS.md Part B §4.7).
#include <cmath>

#include "game/ai_system.hpp"

namespace joa::game {

namespace {
constexpr float kLdFmDt = 160000.0f;  // LD_FM_DT (GAMEPLAY.RES), 400^2
float cos256(int a) { return std::cos(float(a & 255) * 3.14159265f / 128.0f); }
float sin256(int a) { return std::sin(float(a & 255) * 3.14159265f / 128.0f); }
int byte_angle(float rad) { return int(std::uint8_t(int(double(rad) * 40.74366543152521))); }
int cols_for(int n) { return n <= 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : n <= 16 ? 4 : 5; }
struct V2 { float x = 0, y = 0; };
V2 centre(Vec3 anchor, float angle, float width) {  // 0x451dc0 / 0x44a950
    const int a = byte_angle(angle);
    const float d = -100.0f - float(int(width) / 2);
    return {anchor.x + cos256(a) * d, anchor.y + sin256(a) * d};
}
}  // namespace

void AiSystem::update_formation(Unit& L, bool force, bool /*use_delta: only feeds the unused picker arg*/) {
    AiTeam* T = team_of_leader(L.id);
    if (!T) return;
    AiUnit& ls = st(L);
    if (ls.task.id == task::RTS_MOVE_TO) return;  // anchor owned by the RTS move
    V2 old_c, new_c;
    auto pl = places.find(ls.place);
    if (ls.place && ls.place_slot && (ls.task.id == task::ATTACK_PLACE || ls.task.id == task::DEFEND_PLACE) &&
        ls.task.p1 == ls.place && pl != places.end() && std::size_t(ls.place_slot - 1) < pl->second.slots.size()) {
        if (!force) return;
        old_c = centre(T->prev_anchor, T->prev_angle, ls.width);
        T->anchor = pl->second.slots[std::size_t(ls.place_slot - 1)];
        if (std::size_t(ls.place_slot - 1) < pl->second.slot_facing.size())
            T->angle = pl->second.slot_facing[std::size_t(ls.place_slot - 1)];
        new_c = centre(T->anchor, T->angle, ls.width);
    } else {
        const float d2 = (L.pos.x - T->anchor.x) * (L.pos.x - T->anchor.x) + (L.pos.y - T->anchor.y) * (L.pos.y - T->anchor.y);
        if (d2 < kLdFmDt) {
            if (!force) return;
        } else if (!force) {
            old_c = centre(T->prev_anchor, T->prev_angle, ls.width);
            T->anchor = L.pos;
            T->angle = L.yaw_rad();
            new_c = centre(T->anchor, T->angle, ls.width);
        }
    }
    if (T->ftype != 1) return;
    const V2 delta{new_c.x - old_c.x, new_c.y - old_c.y};
    const int n = int(L.members.size());
    const int cols = cols_for(n);
    const int W = (cols - 1) * kFormationSpacing;
    const int a = byte_angle(T->angle);
    const float c = cos256(a), s = sin256(a);
    struct Slot { V2 world, local; };
    std::vector<Slot> slots;
    slots.resize(std::size_t(n));
    int gA = 0, gB = 0, gC = 0;
    for (int i = 0; i < n; ++i) {
        const float fwd = float(-100 - (i / cols) * kFormationSpacing);
        const float side = float(W / 2 - (i % cols) * kFormationSpacing);
        slots[std::size_t(i)] = {{c * fwd - s * side, s * fwd + c * side}, {fwd, side}};
        const int k = class_of(L.members[std::size_t(i)]);
        if (k == 1 || k == 4) ++gA;
        else if (k == 2 || k == 3) ++gB;
        else if (k == 5 || k == 6 || k == 8) ++gC;
    }
    auto in_mode = [](int k, int mode) {
        return mode == 1 ? (k == 1 || k == 4) : mode == 2 ? (k == 5 || k == 6 || k == 8) : (k == 2 || k == 3);
    };
    std::vector<char> used(std::size_t(n), 0);
    int k = 0;
    auto assign = [&](int mode) {  // 0x45de50
        const Slot& sl = slots[std::size_t(k++)];
        const V2 sw{T->anchor.x + sl.world.x, T->anchor.y + sl.world.y};
        float best = 1e10f, keep_d = 0;
        int bi = 0, keep = 0;
        for (int j = 0; j < n; ++j) {
            Unit* m = w_.unit(L.members[std::size_t(j)]);
            if (used[std::size_t(j)] || !m || !in_mode(class_of(*m), mode)) continue;
            V2 p{m->pos.x, m->pos.y};
            if ((p.x - T->prev_anchor.x) * (p.x - T->prev_anchor.x) + (p.y - T->prev_anchor.y) * (p.y - T->prev_anchor.y) < kLdFmDt)
                p = {p.x + delta.x, p.y + delta.y};
            const float d = std::sqrt((sw.x - p.x) * (sw.x - p.x) + (sw.y - p.y) * (sw.y - p.y));
            if (d <= best) { best = d; bi = j + 1; }
            const AiUnit& ms = st(*m);
            if (sl.local.x == ms.local_fwd && sl.local.y == ms.local_side) { keep = j + 1; keep_d = d; }
        }
        if (keep && keep != bi && keep_d < float(kFormationSpacing / 10) + best) bi = keep;
        if (!bi) return;
        used[std::size_t(bi - 1)] = 1;
        AiUnit& ms = st(*w_.unit(L.members[std::size_t(bi - 1)]));
        ms.slot.x = sl.world.x;
        ms.slot.y = sl.world.y;
        ms.width = float(W);
        ms.local_fwd = sl.local.x;
        ms.local_side = sl.local.y;
    };
    for (int j = 0; j < gA; ++j) assign(1);
    for (int j = 0; j < gB; ++j) assign(3);
    for (int j = 0; j < gC; ++j) assign(2);
    ls.width = float(W);
    ls.local_fwd = ls.local_side = 0;
}

Vec3 AiSystem::formation_pos(Unit& u) {  // 0x448870
    const int l = leader_recno(u);
    AiTeam* T = l ? team_of_leader(l) : nullptr;
    if (!T || T->ftype == 0) return u.pos;
    const AiUnit& s = st(u);
    return {T->anchor.x + s.slot.x, T->anchor.y + s.slot.y, T->anchor.z + s.slot.z};
}

int AiSystem::follow_unit_distance(int recno) {  // 0x6592c0
    const Unit* u = w_.unit(recno);
    if (!recno || !u) return 0;
    AiTeam* T = team_of_leader(recno);
    if (!T) return 200;
    if (T->ftype != 1) return 200;
    const int n = int(u->members.size());
    const int k = n < 2 ? 1 : n < 5 ? 2 : n < 10 ? 3 : n < 17 ? 4 : 5;
    return 200 + 100 * k;
}

void AiSystem::set_formation_pos(Unit& u, Vec3 anchor, float deg) {  // 0x658810
    Unit* L = w_.unit(leader_of(u));
    AiTeam* T = L ? team_of_leader(L->id) : nullptr;
    if (!T) return;
    T->prev_anchor = T->anchor;
    T->prev_angle = T->angle;
    T->anchor = anchor;
    T->angle = deg * 0.017453292f;
    update_formation(*L, true, false);
}

void AiSystem::set_team_member_pos(Unit& u) {  // 0x658920
    Unit* L = w_.unit(leader_of(u));
    if (!L) return;
    for (int id : L->members)
        if (Unit* m = w_.unit(id)) {
            const AiUnit& s = st(*m);
            m->pos = {L->pos.x + s.slot.x, L->pos.y + s.slot.y, L->pos.z + s.slot.z};
            w_.set_yaw(*m, L->yaw_deg);
        }
}

}  // namespace joa::game
