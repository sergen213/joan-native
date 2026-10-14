// Ladder climbing. Decomp: FUN_0042d300 / 0042d5c0 (controller), 005183d0 (find), 004fab10 (mount /
// dismount), 004ac910 (mount placement), 00518890 / 00445000 (rung steps), Unit_updateAction 007c7ab0
// (climb states), Unit_stop 00508b90 (climb idles). Positions use the actor forward axis = (cos, sin) of
// the yaw (HIGH: the climber is turned to ladder yaw - pi, i.e. facing the ladder's forward side).
#include "game/ladder_climb.hpp"

#include <cmath>
#include <string_view>

#include "game/mission.hpp"

namespace joa::game {

namespace {

constexpr float kPi = 3.1415927f;

// Per-model offsets: the hero models LAHIRE / ALENCON / JEAN_DE_METZ have their own constants, every other
// class the default column (strcmp chains in 0x4ac910, 0x4fab10, 0x518890, 0x7c7ab0).
struct Dims {
    float mount;          // 0x4ac910: distance in front of the ladder
    float top_a, top_b[2];// 0x4ac910 mode 0: z = ladder z + height - top_a - top_b[height/22 odd]
    float off_fwd, off_up;// 0x4fab10 top dismount: forward / up
    float rung_k;         // 0x518890: rungs left before the top exit
    float ab, ba, ca, cb; // climb segment per state family (C_DN_AB/C_UP_BA, C_DN_BA/C_UP_AB, ..CA/AC, ..CB/BC)
};
constexpr Dims kDefault{22.405f, 66.877f, {10.668f, 10.668f}, 45.201f, 66.886f, 5.0f, 21.998f, 21.991f, 10.668f, 10.668f};
constexpr Dims kLahire{33.754f, 78.083f, {5.572f, 5.946f}, 43.754f, 80.073f, 4.5f, 22.426f, 21.589f, 5.572f, 5.946f};
constexpr Dims kAlencon{22.761f, 83.589f, {3.097f, 2.949f}, 42.761f, 83.589f, 4.5f, 23.452f, 20.548f, 1.497f, 2.949f};
constexpr Dims kMetz{22.761f, 85.0f, {1.638f, 1.638f}, 37.687f, 85.0f, 4.5f, 21.999f, 21.999f, 1.638f, 1.638f};

const Dims& dims(Mission& m, const Unit& u) {
    const std::string_view c = m.data().units[std::size_t(u.cls)].code;
    return c == "LAHIRE" ? kLahire : c == "ALENCON" ? kAlencon : c == "JEAN_DE_METZ" ? kMetz : kDefault;
}

const MapItem* ladder(Mission& m, int recno, int* height = nullptr) {
    const MapItem* it = m.objects().item(recno);
    const ObjRes* r = it ? m.objects().res(it->res) : nullptr;
    if (!r || r->cls != 11) return nullptr;
    if (height) *height = r->height;   // ItemRes +0x26
    return it;
}

float d3(Vec3 a, Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

// 0x4fab10(ladder, 0, top): leave the ladder at the bottom (feet at ladder z) or the top (up + forward).
// The let-go variant (how 2, 30 units back) is only reachable from 0x42d450, which has no caller.
void dismount(Mission& m, Unit& u, bool top) {
    World& w = m.world();
    const MapItem* l = ladder(m, u.m.climb);
    if (!l) return;
    const Dims& d = dims(m, u);
    Vec3 p = u.pos;
    if (top) p.z += d.off_up, p.x += std::cos(u.yaw_rad()) * d.off_fwd, p.y += std::sin(u.yaw_rad()) * d.off_fwd;
    else p.z = l->pos.z;
    u.pos = p;
    u.vel = {};
    u.m.climb = 0;
    u.ts_state = w.now();   // +0x20f
}

// Climb segment along z for the current state, capped at `seg` from the anchor (+0x203).
void slide(World& w, Unit& u, float seg, bool up) {
    const int len = w.anim_length(u);
    if (len <= 0) return;
    const float step = seg / float(len) * float(World::kTickMs);
    float z = u.pos.z + (up ? step : -step);
    if (up && seg < z - u.m.climb_from.z) z = u.m.climb_from.z + seg;
    if (!up && seg < u.m.climb_from.z - z) z = u.m.climb_from.z - seg;
    u.pos.z = z;
}

void climb_state(Mission& m, Unit& u) {   // Unit_updateAction 0x7c7ab0, climbing cases
    World& w = m.world();
    const Dims& d = dims(m, u);
    const int a = u.action;
    switch (a) {
    case C_DN_AB: slide(w, u, d.ab, false); break;
    case C_DN_BA: slide(w, u, d.ba, false); break;
    case C_DN_CA: slide(w, u, d.ca, false); break;
    case C_DN_CB: slide(w, u, d.cb, false); break;
    case C_UP_AB: slide(w, u, d.ba, true); break;
    case C_UP_BA: slide(w, u, d.ab, true); break;
    case C_UP_AC: slide(w, u, d.ca, true); break;
    case C_UP_BC: slide(w, u, d.cb, true); break;
    default: break;
    }
    if (w.anim_remaining(u) > 0) return;
    switch (a) {
    case C_DN_AB: case C_DN_BA: case C_DN_CA: case C_DN_CB: case C_UP_AB: case C_UP_BA: case C_UP_SA:
        w.stop(u);   // Unit_stop: rung idle of the state family while climbing
        break;
    case C_DN_AS:
        dismount(m, u, false);
        w.stop(u);
        break;
    case C_DN_SC: {
        int h = 0;
        if (ladder(m, u.m.climb, &h)) w.set_action(u, (h / 22) % 2 ? C_DN_CB : C_DN_CA);
        break;
    }
    case C_UP_AC: case C_UP_BC:
        w.set_action(u, C_UP_CS);
        u.m.climb_from = u.pos;
        break;
    case C_UP_CS:
        dismount(m, u, true);
        w.stop(u);
        break;
    default: break;
    }
}

// 0x518890 (up) / 0x445000 (down) from a rung idle.
void step(Mission& m, Unit& u, bool up) {
    World& w = m.world();
    int h = 0;
    const MapItem* l = ladder(m, u.m.climb, &h);
    if (!l) return;
    u.m.climb_from = u.pos;
    const bool a = u.action == C_IDLE_A, b = u.action == C_IDLE_B;
    if (up) {
        const bool far = dims(m, u).rung_k * 22.0f + u.pos.z < l->pos.z + float(h);
        if (a) w.set_action(u, far ? C_UP_AB : C_UP_AC);
        else if (b) w.set_action(u, far ? C_UP_BA : C_UP_BC);
    } else if (l->pos.z + 0.0001f < u.pos.z) {
        if (a) w.set_action(u, C_DN_AB);
        else if (b) w.set_action(u, C_DN_BA);
    } else {
        w.set_action(u, C_DN_AS);
    }
}

}  // namespace

int ladder_find(Mission& m, const Unit& u, int& mode) {   // 0x5183d0 (from the top of the item array)
    mode = -1;
    for (int r = m.objects().size(); r >= 1; --r) {
        int h = 0;
        const MapItem* l = ladder(m, r, &h);
        if (!l) continue;
        if (d3(l->pos, u.pos) <= 100.0f) return mode = 1, r;
        if (d3({l->pos.x, l->pos.y, l->pos.z + float(h)}, u.pos) <= 100.0f) return mode = 0, r;
    }
    return 0;
}

bool ladder_start(Mission& m, Unit& u, int recno, int mode) {
    World& w = m.world();
    int h = 0;
    const MapItem* l = ladder(m, recno, &h);
    if (!l || u.m.climb) return false;
    const float Fx = std::cos(l->dir), Fy = std::sin(l->dir);          // ladder forward axis
    const float side = Fx * (u.pos.x - l->pos.x) + Fy * (u.pos.y - l->pos.y);
    const float facing = Fx * std::cos(u.yaw_rad()) + Fy * std::sin(u.yaw_rad());
    // Bottom: in front of the ladder, facing it. Top: behind its plane (on the walkway), facing out.
    if (mode != 0 ? !(side >= 0 && facing <= 0) : !(side <= 0 && facing > 0)) return false;
    const Dims& d = dims(m, u);
    w.set_yaw_now(u, (l->dir - kPi) / 0.017453292f);   // 0x4ac910
    Vec3 p{l->pos.x + Fx * d.mount, l->pos.y + Fy * d.mount, l->pos.z};
    if (mode == 0) p.z += float(h) - d.top_a - d.top_b[(h / 22) % 2];
    u.pos = p;
    u.vel = {};
    u.m.climb = recno;
    u.m.climb_from = p;
    w.set_action(u, mode == 0 ? C_DN_SC : C_UP_SA);
    return true;
}

bool ladder_input(Mission& m, Unit& u, const InputFrame& in) {
    World& w = m.world();
    if (u.m.climb) {   // controller +0xcd: all movement goes to 0x42d5c0
        const bool up = in.held[IN_FORWARD], down = in.held[IN_BACKWARD];
        // HYPOTHESIS: turn keys (also routed there) map through Input_move_angle; only W/S step here.
        if ((up || down) && w.can_act(u)) step(m, u, up);
        return true;
    }
    if (!in.pressed[IN_INTERACT] || !w.can_act(u) || u.posture == 1) return false;   // 0x42d300
    int mode = 0;
    const int l = ladder_find(m, u, mode);
    if (!l || !ladder_start(m, u, l, mode)) return false;
    w.controller().jump_hold = 0;   // +0x9e
    return true;
}

void ladder_tick(Mission& m) {
    for (Unit& u : m.world().units())
        if (!u.deleted && u.m.climb) climb_state(m, u);
}

}  // namespace joa::game
