// Horses: mount / dismount, rider sync, gears and the player's horse controller (Ctrl_horse 0x43c5c0).
// Decomp read for this port: 0x4fc220, 0x482930, 0x5174e0, 0x44f0b0, 0x4533e0, 0x470430, 0x466d50,
// 0x43c5c0, 0x43c920, 0x43d7a0 (+asm of its tail), 0x43dab0, 0x43ca90, anim-end 0x7c7ab0 cases 0x47..0x52.
#include "game/mount_ride.hpp"

#include <cmath>
#include <string>

#include "game/rng_lcg.hpp"
#include "game/world_sim.hpp"

namespace joa::game {

namespace {
constexpr float kDeg = 0.017453292f;
Unit* live(World& w, int id) {
    Unit* u = w.unit(id);
    return u && !u->deleted ? u : nullptr;
}
bool mounting(int a) { return a >= H_MNT_L && a <= H_DMNT_R; }
float wrap180(float d) {
    d = std::fmod(d, 360.0f);
    return d > 180 ? d - 360 : d < -180 ? d + 360 : d;
}
}  // namespace

int horse_set_rider(World& w, Unit& h, int rider) {
    if (rider == 0 && h.m.rider != 0) {
        h.m.rider = 0;
        h.gear_speed = 0;
        if (h.m.active) {
            w.stop(h);
            h.m.bounding = 1;
        }
        h.ai_controlled = true;
        return 1;
    }
    Unit* r = live(w, rider);
    if (!r || h.m.rider != 0 || !h.m.active) return 0;
    h.m.rider = rider;
    h.m.bounding = 1;
    w.stop(h);
    h.ai_controlled = r->ai_controlled;
    return 1;
}

// HIGH; the position/heading reads inside the mount branch use the horse (ECX not visible in the decomp).
int ride_horse(World& w, Unit& r, int horse) {
    if (horse == r.id) return 0;
    const std::string& code = w.data().units[r.cls].code;
    const float d = code == "LAHIRE" || code == "ALENCON" ? 45.0f : 30.0f;  // JEAN_DE_METZ 30 = default
    if (horse == 0 && r.mount != 0) {
        if (!r.m.active) return 0;
        // Left side (pos + side*d) probed first; always free here -> H_DMNT_L, position unchanged.
        r.m.bounding = 0;
        w.set_yaw(r, r.yaw_deg);
        if (Unit* h = live(w, r.mount)) {
            horse_set_rider(w, *h, 0);
            w.set_yaw(*h, h->yaw_deg);
        }
        w.set_action(r, H_DMNT_L);
        r.ride_ready = false;
        r.mount = 0;
        return 1;
    }
    Unit* h = live(w, horse);
    if (!h) return 2;
    if (!r.m.active) return 0;
    // FUN_0042a4d0(horse class special) on the rider: alive and able to ride that mount.
    const int special = w.data().units[h->cls].special;
    if (w.dead(r) || !((special == 2 && r.horse_riding) || (special == 3 && r.elephant_riding))) return 0;
    if (!horse_set_rider(w, *h, r.id)) return 0;
    if (r.mount != 0) return 0;  // original quirk: the horse keeps the rider link set above
    const float yaw = h->yaw_rad();
    const float fx = std::cos(yaw), fy = std::sin(yaw);  // row 0 (forward)
    const float sx = -fy, sy = fx;                       // row 1 (left)
    if (w.rel_angle(*h, r.pos) >= 0) {  // rider on the horse's right
        r.pos = {h->pos.x - sx * d + fx * 30, h->pos.y - sy * d + fy * 30, h->pos.z};
        w.set_yaw_now(r, h->yaw_deg + 90);
        w.set_action(r, H_MNT_R);
    } else {
        r.pos = {h->pos.x + sx * d + fx * 30, h->pos.y + sy * d + fy * 30, h->pos.z};
        w.set_yaw_now(r, h->yaw_deg - 90);
        w.set_action(r, H_MNT_L);
    }
    r.vel = {};
    r.ride_ready = false;
    r.mount = horse;
    return 1;
}

// Last-to-first scan for a rideable (+0x3c0) unit of class Special `special` within 70 (horse) or
// 150 (3-D distance). Scene-sector membership is not modelled: every unit is a candidate.
int find_mount(const World& w, const Unit& u, int special) {
    const float r = special == 2 ? 70.0f : 150.0f;
    const auto& units = w.units();
    for (int i = int(units.size()); i >= 1; --i) {
        const Unit& h = units[size_t(i - 1)];
        if (h.deleted || h.id == u.id || !h.m.can_rided || w.data().units[h.cls].special != special) continue;
        const float dx = h.pos.x - u.pos.x, dy = h.pos.y - u.pos.y, dz = h.pos.z - u.pos.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= r) return i;
    }
    return 0;
}

// FUN_005174e0 (phase 1 head). Rider: no body of its own; seated riders sit on the horse
// (HYPOTHESIS: actor attach = horse position). A dead rider or horse drops (H_DROP) with a 100 ms
// force pulse of -10000 along the facing. A horse follows its seated rider's heading.
// HYPOTHESIS: H_DROP ends the ride (the rider is unlinked and falls off).
void mount_sync(World& w, Unit& u) {
    if (Unit* h = live(w, u.mount)) {
        u.airborne = false;
        if (u.ride_ready && !mounting(u.action)) u.pos = h->pos, u.vel = {};
        if ((w.dead(u) || w.dead(*h)) && u.action != H_DROP && u.action != DIE) {
            w.set_action(u, H_DROP);
            const float k = -10000.0f * World::kUnitsPerMetre / 1000.0f, a = u.yaw_rad();
            u.vel = {std::cos(a) * k, std::sin(a) * k, 0};
        }
        if (u.action == H_DROP) {
            horse_set_rider(w, *h, 0);
            u.mount = 0;
            u.ride_ready = false;
        }
    }
    if (Unit* r = live(w, u.m.rider); r && r->mount == u.id && r->ride_ready && !mounting(r->action))
        u.yaw_deg = u.yaw_goal = r->yaw_deg;
}

void rider_stop(World& w, Unit& r) {
    Unit* h = live(w, r.mount);
    if (!h) return;
    const double roll = rand_unit_incl(w.rng());   // drawn even when the horse stands (CONFIRMED order)
    r.gear_ts = 0;
    const int ha = h->action;
    if (ha != RUN && ha != TIPTOE && ha != WALK && ha != MOVE_B) return;
    if (ha == WALK || ha == MOVE_B) {
        w.set_action(r, H_IDLE);
        w.set_action(*h, IDLE);
        h->vel.x = h->vel.y = 0;
        h->gear_speed = 0;
    } else if (roll > w.data().c.stop_cc) {
        w.set_action(r, H_STOP_B);
        w.set_action(*h, STOP_B);
    } else {
        w.set_action(r, H_STOP);
        w.set_action(*h, STOP);
    }
}

// Shared guard of 0x470430 / 0x466d50; body speed caps +0x2a4: walk/back 5, run 5 then 10 after
// 1000 ms in gear (m/s, HYPOTHESIS for the unit). Back keeps the class MOVE_B speed (+0x2a2 = 0).
bool horse_gear(World& w, Unit& r, int gear) {
    Unit* h = live(w, r.mount);
    if (!h || (r.action >= GUARD1 && r.action <= GUARDR) || mounting(r.action) || r.posture == 1 || h->posture == 1)
        return false;
    if (gear != 2 && !w.can_act(r)) return false;
    if (!w.can_act(*h)) return false;
    if (w.can_act(r) && r.action != H_ATTACK) w.set_action(r, gear == 0 ? H_BACK : H_WALK);
    if (gear == 0) {
        if (h->action != MOVE_B) w.set_action(*h, MOVE_B), h->gear_speed = 0;
        r.gear_ts = 0;
    } else if (gear == 1) {
        if (h->action != WALK) w.set_action(*h, WALK), h->gear_speed = 5;
        r.gear_ts = 0;
    } else {
        const bool fast = r.gear_ts != 0 && w.now() > r.gear_ts + 1000;
        h->gear_speed = fast ? 10.0f : 5.0f;
        if (h->action != WALK) w.set_action(*h, WALK);
        if (r.gear_ts == 0) r.gear_ts = w.now();
    }
    return true;
}

// Ctrl_horse 0x43c5c0 (keyboard+mouse, horse mode A 0xa5802c == 0). F3/F4 ran in process().
bool PlayerController::horse(World& w, Unit& u, const InputFrame& in, int move_code, int rel, int magnitude) {
    Unit* h = live(w, u.mount);
    if (!h) return false;
    // R: interact handlers first, then dismount (0x43c920) while the rider idles on a standing horse.
    if (in.pressed[IN_INTERACT]) {
        if (w.interact && w.interact(u)) return true;
        if (w.can_act(u) && u.action == H_IDLE && h->action >= IDLE && h->action <= IDLE3 && u.posture != 1) {
            ride_horse(w, u, 0);
            ride_mode = 0;
            jump_hold = 0;
            return true;
        }
    }
    horse_attack(w, u, in.pressed[IN_ATTACK_A], in.pressed[IN_ATTACK_B]);
    // Ctrl_horse_turn 0x43dab0, mode A: A/D steer relative to the horse, <= TURN_ANG per frame, only
    // while the horse moves (and not in STOP/STOP_B); S alone (270) is ignored.
    const int ha = h->action;
    if (u.ride_ready && !mounting(u.action) && ha != STOP && ha != STOP_B && !(ha >= IDLE && ha <= IDLE3) &&
        w.can_act(u) && magnitude && rel != -1 && rel != 270 && u.posture != 1) {
        const float r = std::fmod(float(rel), 360.0f);
        const float want = u.yaw_deg + (r <= 180 ? r - 90 : r - 270);
        const float t = w.data().c.turn_ang, d = wrap180(want - u.yaw_deg);
        w.set_yaw(u, u.yaw_deg + (d > t ? t : d < -t ? -t : d));
    }
    // Ctrl_horse_gear 0x43d7a0 (asm-checked branch structure). HYPOTHESIS: param_2 = walk key.
    const bool walk = in.held[IN_WALK];
    if (u.ride_ready && !mounting(u.action) && w.can_act(u)) {
        const bool back = move_code == 0x14 || move_code == 0xc || move_code == 0xd;
        if (move_code == 0 && u.action == H_ATTACK) {
            if (w.anim_remaining(u) < 100) rider_stop(w, u);
        } else if (move_code != 0 && !walk && !back) {
            horse_gear(w, u, 2);
            horse_heading = u.yaw_goal;
        } else if (move_code != 0 && move_code != 0x1c && move_code != 0x22) {
            horse_gear(w, u, back ? 0 : 1);
            horse_heading = u.yaw_goal;
        } else {
            rider_stop(w, u);   // no key, or A/D alone: the horse stops (asm 0x43d9b9)
        }
    }
    return true;
}

// Ctrl_horse_attack 0x43ca90: A -> H_ATTACK 'A'; B only if the "B" record is free or HORSE_ATTACK_B
// (skill 0x10) is learned, EP cost ENERGY_PT x EP_CONSUME / 100. Follow-ups use the foot attack's
// queue window; a combo that cannot grow wraps back to one hit.
void PlayerController::horse_attack(World& w, Unit& u, bool a, bool b) {
    const int now = w.now();
    if (u.combo_idx == -1 && unsigned(now - u.combo_lock) > 250u) u.combo_lock = 0, u.combo_idx = 0;
    if ((!a && !b) || u.combo_lock != 0) return;
    const char letter = a ? 'A' : 'B';
    auto start = [&](char l) {
        if (l == 'B') {
            const MoveRecord* r = w.find_move(u, H_ATTACK, "B");
            if (!r || !(r->combo_avail == 'Y' || w.skill_level(u, 0x10, {}) > 0)) return;
            const int cost = w.bonus(u, EP_CONSUME) * r->energy_pt / 100;
            if (cost > u.ep) return;
            w.set_ep(u, u.ep - cost);
        }
        if (w.set_action(u, H_ATTACK, l)) attack_ts = now;
    };
    if (w.can_act(u)) {
        start(letter);
        return;
    }
    if (u.action != H_ATTACK || u.combo_idx < 1) return;
    const unsigned since = unsigned(now - attack_ts);
    if (since < unsigned(u.win_open) || since >= unsigned(u.win_close)) return;
    if (!w.find_move(u, H_ATTACK, u.combo + letter)) u.combo_idx = 0, u.combo.clear();   // wrap
    start(letter);
}

}  // namespace joa::game
