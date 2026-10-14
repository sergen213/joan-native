// Player controller dispatcher 0x42e1a0, keyboard+mouse controller 0x430480 and the helpers shared with the
// keyboard (controller_keyboard.cpp) and joystick (controller_joystick.cpp) controllers (Part B §3, Part C §2.2,
// §3.9-3.12).
#include "game/controller_player.hpp"

#include <algorithm>
#include <cmath>

#include "game/mount_ride.hpp"
#include "game/world_sim.hpp"

namespace joa::game {

namespace {
bool guarding(int a) { return a >= GUARD1 && a <= GUARDR; }
}  // namespace

bool PlayerController::process(World& w, const InputFrame& in) {
    Unit* up = w.unit(unit);
    if (!up) return false;
    Unit& u = *up;
    camera_yaw_out = in.camera_yaw_deg;
    camera_pitch_out = in.camera_pitch_deg;
    // Dispatcher 0x42e1a0: states that swallow input.
    if (w.dead(u) || u.action == DIE || u.action == STANDUP || is_knocked(u.action) || u.action == H_DROP) {
        run_ts = 0;
        return false;
    }
    bool r = false;
    switch (mode) {
        case 0: r = process_keyboard(w, u, in); break;
        case 1: r = process_joystick(w, u, in); break;
        case 2: return process_click(w, u, in);   // its ViewCtrl update (0x439090) runs from World::tick_units
        default: r = process_kbm(w, u, in); break;
    }
    // ViewCtrl_process mode updates 0x434f40 / 0x4338c0 / 0x431520 (modes 0 / 1 / 3) end in 0x4384e0 outside
    // range mode. Mode 3 never sets +0x74 here, so this is a no-op for it.
    if (!range_mode) center_camera_step();
    return r;
}

bool PlayerController::process_kbm(World& w, Unit& u, const InputFrame& in) {
    const int now = w.now();
    const bool A = in.pressed[IN_ATTACK_A], B = in.pressed[IN_ATTACK_B], C = in.pressed[IN_ENERGY];
    const bool Wh = in.held[IN_FORWARD], Sh = in.held[IN_BACKWARD], Ah = in.held[IN_STRAFE_LEFT], Dh = in.held[IN_STRAFE_RIGHT];
    const bool def = in.held[IN_DEFEND];
    int hold = 0;
    const bool jfire = jump_timer(w, in.held[IN_JUMP], hold);
    // Move code / relative angle (0x431680 / 0x431570); A+D cancel.
    const bool l = Ah && !Dh, r = Dh && !Ah;
    int move_code = 0, rel = -1;
    if (Wh && l) move_code = 10, rel = 135;
    else if (Wh && r) move_code = 0xb, rel = 45;
    else if (Wh) move_code = 0x25, rel = 90;
    else if (Sh && l) move_code = 0xc, rel = 225;
    else if (Sh && r) move_code = 0xd, rel = 315;
    else if (l) move_code = 0x1c, rel = 180;
    else if (r) move_code = 0x22, rel = 360;
    else if (Sh) move_code = 0x14, rel = 270;
    const int magnitude = (Wh || Sh || Ah || Dh) ? 3 : 0;
    // Double tap (check first, then record this frame's direction press).
    int dt = 0;
    if (dtap_action && unsigned(now - dtap_time) < 300u && in.pressed[size_t(dtap_action)]) dt = dtap_action;
    if (in.key_char_event) {
        dtap_action = in.pressed[IN_FORWARD] ? 4 : in.pressed[IN_BACKWARD] ? 5 : in.pressed[IN_STRAFE_LEFT] ? 6 : in.pressed[IN_STRAFE_RIGHT] ? 7 : 0;
        dtap_time = now;
    }
    if (in.pressed[IN_HEAL_ITEM]) w.use_recovery_item(u, false);
    if (in.pressed[IN_EP_ITEM] && !in.alt_held) w.use_recovery_item(u, true);
    if (range_mode || in.pressed[IN_RANGE_MODE]) {
        if (range_input(w, u, in)) return true;
    }
    // Riding (+0xc4): horse controller 0x43c5c0. Climbing / vehicle (+0xcd): 0x42d270.
    if (ride_mode && w.unit(u.mount)) return horse(w, u, in, move_code, rel, magnitude);
    if (u.m.climb && w.ladder_input && w.ladder_input(u, in)) return true;
    // On foot, step 1: interact handlers (R). 0x430480 ignores their result: the mount (0x43c7d0) and
    // ladder (0x42d300) steps below still see the same R press.
    if (in.pressed[IN_INTERACT] && w.interact) w.interact(u);
    if (down_attack(w, u, def)) return true;
    if (block(w, u, def)) return true;
    if (energy_attack(w, u, C || (A && B))) return true;
    if (attack_input(w, u, A, B, dt == 4)) return true;
    if (!(u.action >= MOVE_L && u.action <= MOVE_BR)) {
        if (mount_input(w, u, in.pressed[IN_INTERACT])) return true;   // 0x43c7d0
        if (w.ladder_input && w.ladder_input(u, in)) return true;   // 0x42d300
        if (jump(w, u, jfire, magnitude, rel, hold, in.camera_yaw_deg)) return true;
    }
    if (const int ev = dt == 5 ? JUMP_B : dt == 7 ? JUMP_R : dt == 6 ? JUMP_L : 0) return evade(w, u, ev, true);
    if (chain_input(w, u, in.pressed[IN_CHAIN])) return true;
    // Movement.
    if (l || r) strafe(w, u, Wh, Sh, l, r);
    else if (Ah && Dh) return true;
    else if (Sh && !Wh) strafe(w, u, false, true, false, false);
    else if (u.action >= MOVE_L && u.action <= MOVE_BR) w.stop(u);
    const bool forward_only = !Ah && !Dh && !Sh;
    if (forward_only) face_move_dir(w, u, rel, magnitude, in.camera_yaw_deg);
    // Mouse yaw 0x431790.
    if ((w.can_act(u) || u.action == H_ATTACK || u.action == ATTACK || u.action == JATTACKB) && u.posture != 1 && in.mouse_dx != 0) {
        const float s = std::clamp(in.mouse_sensitivity, -1.0f, 1.0f);
        int d = int(in.mouse_dx * ((s + 1.0f) * 0.7f + 0.2f) * 0.2f);
        if (u.action == ATTACK) d = std::clamp(d, -45, 45);
        int f = int(u.yaw_goal - float(d)) % 360;   // Unit_get_yaw_deg, then both setters (instant)
        if (f < 0) f += 360;
        w.set_yaw_now(u, float(f));
        camera_yaw_out -= float(d);
    }
    if (forward_only) locomotion(w, u, move_code, in.held[IN_WALK]);
    return false;
}

// Jump key timer (+0x9e, 0x430480 / 0x4343e0 / 0x432d30): fires at JUMPMAXD held or on release with the hold time.
bool PlayerController::jump_timer(const World& w, bool held, int& hold) {
    if (held) {
        jump_hold += World::kTickMs;
        if (jump_hold < w.data().c.jumpmaxd) return false;
    } else if (jump_hold <= 0) {
        return false;
    }
    hold = jump_hold, jump_hold = 0;
    return true;
}

// Double-tap evade (dt 5 JUMP_B, 7 JUMP_R, 6 JUMP_L): canAct, posture != 1, not IDLEUP/IDLEDN; 0x430480 and
// 0x432d30 also need the move in the list (0x416dc0), 0x4343e0 does not. Consumes the frame even when refused.
bool PlayerController::evade(World& w, Unit& u, int ev, bool need_move) {
    if (w.can_act(u) && u.posture != 1 && u.action != IDLEUP && u.action != IDLEDN && (!need_move || w.find_move(u, ev)))
        w.set_action(u, ev);
    return true;
}

// 0x43c7d0: R mounts a free horse within 70. Deviation: ride mode (+0xc4) is set only when the mount
// succeeded (the original sets it regardless and leaves a refused rider stuck).
bool PlayerController::mount_input(World& w, Unit& u, bool r) {
    if (!r || !w.can_act(u) || u.posture == 1 || u.mount) return false;
    const int h = find_mount(w, u, 2);
    const Unit* hu = w.unit(h);
    if (!hu || hu->m.rider != 0) return false;
    if (ride_horse(w, u, h) == 1) ride_mode = 1;
    jump_hold = 0;
    return true;
}

// 0x436190(on): on = save the facing (+0x75) and hold camera centring (+0x74 = 2); off = a held centring
// finishes (2 -> 1).
void PlayerController::center_camera(const Unit& u, bool on) {
    if (!on) {
        if (cam_center == 2) cam_center = 1;
        return;
    }
    if (cam_center == 0 || cam_center == 1) cam_center = 2;
    cam_center_yaw = u.yaw_deg;
}

// 0x4384e0 (0x577a20: the shorter way, at most 40 deg, snaps when within the step). HIGH.
void PlayerController::center_camera_step() {
    if (!cam_center) return;
    const float d = std::remainder(cam_center_yaw - camera_yaw_out, 360.0f);
    if (std::fabs(d) > 40.0f) {
        camera_yaw_out += d > 0 ? 40.0f : -40.0f;
        return;
    }
    camera_yaw_out = cam_center_yaw;
    if (cam_center == 1) cam_center = 0;
}

// Ctrl_strafe 0x438110 as modes 0/1 call it with raw direction flags: only single directions and the four
// diagonals act, forward alone walks (0x437ed0(0xc, walk = 1)), opposite directions together do nothing.
void PlayerController::strafe_exact(World& w, Unit& u, bool f, bool b, bool l, bool r, int fwd) {
    if (!w.can_act(u) || u.posture == 1 || (f && b) || (l && r)) return;
    if (f && !l && !r) locomotion(w, u, 0xc, true, fwd);
    else strafe(w, u, f, b, l, r);
}

// Player_attackInput 0x4367c0 (decomp 0x4367c0..0x437339 read for this port).
bool PlayerController::attack_input(World& w, Unit& u, bool a, bool b, bool rush) {
    const int now = w.now();
    const bool is_player = u.id == w.player_id();
    if (u.combo_idx < 1) fresh_a = fresh_b = fresh_ab = 1;
    auto edge = [&](int& ph, int& fresh, bool p) {
        if (!p) {
            if (ph == 1 || ph == 3 || u.combo_idx > 0) ph = 2;
            fresh = 1;
        } else if (ph == 0) ph = 1;
        else if (ph == 2) ph = 3;
    };
    edge(ph_a, fresh_a, a);
    edge(ph_b, fresh_b, b);
    if (!(a && b)) {
        if (!a && !b) {
            if (ph_ab == 1 || ph_ab == 3 || u.combo_idx > 0) ph_ab = 2;
            fresh_ab = 1;
        }
    } else if (ph_ab == 0) ph_ab = 1;
    else if (ph_ab == 2) ph_ab = 3;
    if (u.combo_idx == -1 && unsigned(now - u.combo_lock) > 250u) u.combo_lock = 0, u.combo_idx = 0;
    auto cost_of = [&](const MoveRecord& r) { return w.bonus(u, EP_CONSUME) * r.energy_pt / 100; };
    if (u.combo_lock == 0 && w.can_act(u) && u.posture != 1 && rush && w.can_use_special(u, RUSH)) {
        const MoveRecord* r = w.find_move(u, RUSH);
        const int cost = cost_of(*r);
        if (cost <= u.ep) {
            w.set_ep(u, u.ep - cost);
            w.set_action(u, RUSH);
            run_ts = 0;
            return true;
        }
    }
    const unsigned since = unsigned(now - attack_ts);
    auto queue = [&](int& ph, int& fresh, int letter) {
        if ((u.combo_idx >= 1 || chain > 1) && ph == 3 && fresh == 1) {
            fresh = 0;
            if (unsigned(u.win_open) <= since && since < unsigned(u.win_close)) {
                queued = letter, follow = 1, u.follow_queued = true;
                return true;
            }
        }
        return false;
    };
    if (a && u.combo_lock == 0) {
        if (!w.can_act(u)) {
            if (queue(ph_a, fresh_a, 1)) return true;
        } else if (u.posture == 1) {
            if (u.combo.empty() && u.action != UP && u.action != DOWN && unsigned(now - u.takeoff_ts) > 50u) {
                w.set_action(u, ATTACK, 'D');
                run_ts = 0, follow = 0;
                return true;
            }
        } else if (!rush) {
            if (is_player) w.crit_key('A', true);
            w.set_action(u, ATTACK, 'A');
            run_ts = 0, attack_ts = now, follow = 0;
            return true;
        } else if (const MoveRecord* r = w.find_move(u, RUSH); r && r->energy_pt <= u.ep) {
            w.set_ep(u, u.ep - r->energy_pt);   // fallback rush: no EP_CONSUME scaling (CONFIRMED)
            w.set_action(u, RUSH);
            run_ts = 0;
            return true;
        }
    }
    if (b && u.combo_lock == 0) {
        if (!w.can_act(u)) {
            if (queue(ph_b, fresh_b, 2)) return true;
        } else if (u.posture == 1) {
            if (const MoveRecord* r = w.find_move(u, JATTACKB, "E"); r && (r->combo_avail == 'Y' || w.skill_level(u, 10, {}) > 0)) {
                const int cost = cost_of(*r);
                if (cost <= u.ep) {
                    w.set_ep(u, u.ep - cost);
                    w.set_action(u, JATTACKB, 'E');
                    run_ts = 0, attack_ts = now, follow = 0;
                    return true;
                }
            }
        } else if (const MoveRecord* r = w.find_move(u, ATTACK, "B")) {
            const int cost = cost_of(*r);
            if (cost <= u.ep) {
                w.set_ep(u, u.ep - cost);
                if (is_player) w.crit_key('B', true);
                w.set_action(u, ATTACK, 'B');
                run_ts = 0, attack_ts = now, follow = 0;
                return true;
            }
        }
    }
    if (since < unsigned(u.win_close) || follow != 1) {
        if (unsigned(u.win_close) <= since && w.can_act(u) && u.combo_idx == 0) ph_a = ph_b = ph_ab = 0;
        return false;
    }
    // Fire the queued follow-up: a record (same posture/action/weapon/special) whose COMBO is the
    // current string + the queued letter, exactly one char longer, usable (free or learned).
    const MoveRecord* found = nullptr;
    const char letter = queued == 1 ? 'A' : queued == 2 ? 'B' : queued == 3 ? 'C' : 0;
    if (u.action == CHAIN) found = w.find_move(u, ATTACK, std::string(1, letter));
    else if (const MoveTable* mt = w.data().moves(u.cls); mt && letter) {
        const std::string want = (u.combo_idx > 0 ? u.combo : std::string()) + letter;
        for (auto it = mt->rows.rbegin(); it != mt->rows.rend(); ++it)
            if (it->position == u.posture && it->movement == u.action && it->item_using == u.item_using &&
                it->special == u.special && it->combo == want && w.combo_learned(u, *it)) { found = &*it; break; }
    }
    if (!found) {
        ph_a = ph_b = ph_ab = 0;
        follow = 0;
        u.follow_queued = false;
        return true;
    }
    if (letter == 'B') {
        const int cost = cost_of(*found);
        if (cost <= u.ep) {
            w.set_action(u, ATTACK, 'B');
            run_ts = 0;
            w.set_ep(u, u.ep - cost);
            if (is_player) w.crit_key('B', false);
        }
    } else {
        w.set_action(u, ATTACK, letter);
        run_ts = 0;
        if (is_player) w.crit_key(letter, false);
    }
    attack_ts = now, follow = 0;
    u.follow_queued = false;
    return true;
}

// Player_energyAttack 0x437910.
bool PlayerController::energy_attack(World& w, Unit& u, bool go) {
    if (!go) return false;
    const Consts& c = w.data().c;
    const int cost_ep = int(float(w.bonus(u, EP_CONSUME)) * (float(w.max_ep(u) * c.egatk_mp) / 100.0f) / 100.0f);
    const int cost_hp = int(float(w.max_hp(u) * c.egatk_hp) / 100.0f);
    if (!w.can_act(u) || u.posture == 1 || u.combo_lock != 0 || u.ep < cost_ep || u.hp <= cost_hp) return false;
    if (!w.can_use_special(u, ENGATK) && w.skill_level(u, 0xd, {}) == 0) return false;
    if (!w.set_action(u, ENGATK)) return false;
    run_ts = 0;
    if (u.id == w.player_id()) w.crit_key('E', true);
    w.set_ep(u, u.ep - cost_ep);
    w.set_hp(u, u.hp - cost_hp);
    u.ts_ep_regen = w.now();
    follow = 0;
    return true;
}

// Block 0x436250.
bool PlayerController::block(World& w, Unit& u, bool held) {
    if (held && w.can_act(u) && u.posture != 1) {
        if (!guarding(u.action) && !w.set_action(u, GUARD1)) return false;
        return true;
    }
    if (!held && (u.action == GUARD1 || u.action == GUARDR)) {
        w.stop(u);
        return true;
    }
    return false;
}

// Down attack 0x4363a0.
bool PlayerController::down_attack(World& w, Unit& u, bool held) {
    if (!held || !w.can_act(u) || guarding(u.action) || u.action == THROW || u.posture == 1) return false;
    const MoveRecord* r = w.find_move(u, DOWNATK);
    const AnimScript* s = w.script_of(r);
    if (!s) return false;
    const float a1 = float(std::min(s->h0, s->h1)), a2 = float(std::max(s->h0, s->h1));
    for (const Unit& e : w.units()) {
        if (e.deleted || e.id == u.id || !w.hostile(e.nation, u.nation)) continue;
        if (!(is_knocked(e.action) || e.action == DNATKED) || e.down_attacked) continue;
        const float dx = e.pos.x - u.pos.x, dy = e.pos.y - u.pos.y, dz = e.pos.z - u.pos.z;
        if (dx * dx + dy * dy + dz * dz > 3600.0f) continue;
        const float b = w.rel_angle(u, e.pos);
        if (!((b >= a1 && b <= a2) || (b - 360 >= a1 && b - 360 <= a2) || (b + 360 >= a1 && b + 360 <= a2))) continue;
        if (w.can_use_special(u, DOWNATK)) return w.set_action(u, DOWNATK);
        return false;
    }
    return false;
}

// Jump 0x437b60.
bool PlayerController::jump(World& w, Unit& u, bool fire, int magnitude, float rel_angle, int hold, float cam_yaw) {
    if (!fire || !w.can_act(u) || u.posture == 1 || u.action == IDLEUP || u.action == IDLEDN) return false;
    const Consts& c = w.data().c;
    const int world_dir = int(cam_yaw + float(rel_angle) - 90.0f) % 360;
    const int dir_byte = int(float(world_dir) * 0.7111111f);
    const float ang = float(dir_byte) * (6.2831853f / 256.0f);
    int base = 1400;
    if (const ITerrainQuery* t = w.terrain()) {
        const Vec3 n = t->normal(u.pos.x, u.pos.y);
        if (n.x * std::cos(ang) + n.y * std::sin(ang) < 0) base = int(1400.0f * n.z);
    }
    const float vx = std::cos(ang) * float(base * magnitude), vy = std::sin(ang) * float(base * magnitude);
    const float vz = float(std::min(hold, c.jumpmaxd)) * 5000.0f / float(c.jumpmaxd) + 5000.0f;
    u.posture = 1;
    if (!w.set_action(u, UP)) {
        u.posture = 0;
        return false;
    }
    // HYPOTHESIS: launch values are body units of 1/1000 m/s (gives 0.2..0.85 m hops at GravityLevel 6).
    const float k = World::kUnitsPerMetre / 1000.0f;
    u.vel = {vx * k, vy * k, vz * k};
    u.airborne = true;
    u.takeoff_ts = w.now();
    run_ts = 0;
    return true;
}

// Ctrl_face_move_dir 0x4352f0 (no A/D/S held; rel -1 = no input). Skipped while guarding or mounting.
// Landing DOWN and take-off UP face the input; during UP with > 180 ms left the jump's horizontal
// push is re-aimed to dir * magnitude * 1400 (zero without input). Airborne: first frame with
// vz <= 0 after rising (+0xa6 == 1) and canAct -> combo cleared, IDLEDN, +0xa6 = 2; then one air-steer
// step: the heading turns <= 30 deg toward the input and the horizontal velocity is rotated onto it
// keeping its magnitude (+0xa6 = 0). On the ground the heading is set to the input direction.
// Camera 0 follows each of these turns (on the ground only while +0x74 == 0): + frame ms * 0.002 deg/ms per
// degree of offset, offset = rel - 90 (rel < 180) else 270 - rel; zero for rel 90 (keyboard+mouse only calls this
// running forward), so it matters in keyboard-only / joystick mode (turn keys, stick).
// Deviation: the ATTACK re-aim (+-45 deg steps from the last direction +0x126a) is not modelled; the
// heading (and camera) is left alone during ATTACK.
void PlayerController::face_move_dir(World& w, Unit& u, float rel, int magnitude, float cam_yaw) {
    const int a = u.action;
    if (guarding(a) || (a >= H_MNT_L && a <= H_DMNT_R)) return;
    const bool input = magnitude != 0 && rel != -1;
    const float wd = float(int(cam_yaw + float(rel) - 90.0f) % 360);   // Ctrl_world_dir 0x42fe30
    const auto turn_camera = [&] {
        const float off = rel < 180.0f ? rel - 90.0f : 270.0f - rel;
        if (off != 0.0f) camera_yaw_out += float(World::kTickMs) * 0.002f * off;
    };
    if (a == DOWN && input) w.set_yaw(u, wd), turn_camera();
    if (a == UP && w.anim_remaining(u) > 180) {
        if (input) w.set_yaw(u, wd), turn_camera();
        const float ang = (input ? wd : u.yaw_goal) * 0.017453292f, f = float(magnitude * 1400) * World::kUnitsPerMetre / 1000.0f;
        u.vel.x = std::cos(ang) * f, u.vel.y = std::sin(ang) * f;
    }
    if (u.posture == 1) {
        if (u.vel.z <= 0) {
            if (air_state == 1 && w.can_act(u)) {
                u.combo.clear();
                if (u.combo_idx != -1) u.combo_idx = 0;
                u.follow_queued = false;
                w.set_action(u, IDLEDN);
                run_ts = 0;
                air_state = 2;
            }
        } else {
            air_state = 1;
        }
    }
    if (!input) return;
    if (u.posture == 1) {
        if (air_state != 2) return;
        float d = std::fmod(wd - u.yaw_goal + 540.0f, 360.0f) - 180.0f;
        d = std::clamp(d, -30.0f, 30.0f);
        w.set_yaw(u, u.yaw_goal + d);
        const float sp = std::hypot(u.vel.x, u.vel.y), g = u.yaw_goal * 0.017453292f;
        u.vel.x = std::cos(g) * sp, u.vel.y = std::sin(g) * sp;
        air_state = 0;
    } else if (a != ATTACK) {
        if (a != DOWN && a != UP) w.set_yaw(u, wd);
        if (cam_center == 0) turn_camera();
    }
}

// Player_chain 0x438820.
bool PlayerController::chain_input(World& w, Unit& u, bool go) {
    if (!go) return false;
    const MoveRecord* cur = w.move(u);
    if (!cur || cur->chain_avail == ' ' || !w.find_move(u, CHAIN)) return false;
    const float limit = float(w.data().c.chainpro) / 1000.0f * float(w.anim_length(u)) / float(std::max(1, chain));
    if (float(w.anim_remaining(u)) > limit || !w.set_action(u, CHAIN)) return false;
    ++chain;
    if (u.id == w.player_id()) w.controller().chain = chain;
    follow = 1;
    attack_ts = w.now();
    return true;
}

// Range mode: enter 0x42b3c0, draw 0x42ad50, target 0x42c260, fire 0x42ba40, exit 0x42b6e0.
bool PlayerController::range_input(World& w, Unit& u, const InputFrame& in) {
    const int now = w.now();
    const bool has_ammo = u.inv.ranged.item && u.inv.ammo.item && u.inv.ammo.quantity > 0;
    if (!range_mode) {
        if (!w.can_act(u) || u.posture == 1 || w.dead(u)) return false;
        w.stop(u);
        range_mode = true;
        range_ts = attack_ts = now;
        range_base_yaw = u.yaw_deg;
        range_base_pitch = in.camera_pitch_deg;
        if (has_ammo) w.set_action(u, SHTLOAD);
        return true;
    }
    if (in.pressed[IN_RANGE_MODE]) {
        range_mode = false;
        u.range_target = 0;
        w.stop(u);
        return true;
    }
    w.set_yaw_now(u, in.camera_yaw_deg);   // Camera_mouse_aim 0x42222f sets both headings
    range_fire(w, u, in);
    return true;
}

// Draw 0x42ad50 (SHTLOAD -> SHTIDLE after the RATE_OF_FIRE-scaled anim) and A: release 0x42ba40 in SHTIDLE, else
// start drawing.
void PlayerController::range_fire(World& w, Unit& u, const InputFrame& in) {
    const int now = w.now();
    const Consts& c = w.data().c;
    const bool has_ammo = u.inv.ranged.item && u.inv.ammo.item && u.inv.ammo.quantity > 0;
    if (u.action == SHTLOAD) {
        const int need = w.anim_length(u) * 100 / (w.bonus(u, RATE_OF_FIRE) + 100);
        if (now - attack_ts >= need) w.set_action(u, SHTIDLE);
    }
    if (!in.pressed[IN_ATTACK_A] || !has_ammo) return;
    if (u.action == SHTIDLE) {
        const UnitClass& cls = w.data().units[size_t(u.cls)];
        const Vec3 cam{u.pos.x, u.pos.y, u.pos.z + float(cls.height)};   // HYPOTHESIS: camera eye at class height
        const float yaw = in.camera_yaw_deg * 0.017453292f, pitch = in.camera_pitch_deg * 0.017453292f;
        const Vec3 f{std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)};
        float best = c.aim_mxd;
        int lock = 0;
        for (const Unit& e : w.units()) {
            if (e.deleted || w.dead(e) || !w.hostile(e.nation, u.nation)) continue;
            const UnitClass& ec = w.data().units[size_t(e.cls)];
            if (ec.special == 2 || ec.special == 7) continue;
            const Vec3 v{e.pos.x - cam.x, e.pos.y - cam.y, e.pos.z + float(ec.height) * 0.5f - cam.z};
            const float vv = v.x * v.x + v.y * v.y + v.z * v.z, vf = v.x * f.x + v.y * f.y + v.z * f.z;
            const float d = std::sqrt(vv), perp = std::sqrt(std::max(0.0f, vv - vf * vf));
            if (perp <= d / 100.0f + c.aim_r && d < best) best = d, lock = e.id;
        }
        Vec3 dir = f;
        if (const Unit* t = w.unit(lock)) {
            dir = {t->pos.x - cam.x, t->pos.y - cam.y, t->pos.z + 60.0f - cam.z};
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            dir = {dir.x / l, dir.y / l, dir.z / l};
        }
        u.range_target = lock;
        u.aim_dir = dir;
        w.set_action(u, SHTOUT);
    } else if (w.can_act(u)) {
        w.set_action(u, SHTLOAD);
        attack_ts = now;
    }
}

// Ctrl_range_update 0x42a810 in modes 0/1 (entered through range_input like mode 3): exit 0x42b6e0 (`exit`: mode 0
// Z pressed, mode 1 Z released = hold to aim; the exit also centres camera 0, 0x436190(1)+(0)), else release
// 0x42ba40, else Camera_mouse_aim 0x42bcd0 with keys/stick: yaw -= cos(angle) * level deg and pitch (up +) +=
// sin(angle) * level deg per frame (1/2/3 deg; keyboard always 3), kept only while yaw is within AIM_HA of the
// facing at entry (+0xb1) and pitch within AIM_HA of level (+0xb5 = 0 at entry); then both hero headings = yaw
// and the lock (+0xbb, unit +0x2db) is dropped. The camera reads camera_yaw_out / camera_pitch_out back.
bool PlayerController::range_update(World& w, Unit& u, const InputFrame& in, bool exit, float angle, int level) {
    if (exit) {
        range_mode = false;
        u.range_target = 0;
        w.stop(u);
        center_camera(u, true), center_camera(u, false);
        return true;
    }
    const bool has_ammo = u.inv.ranged.item && u.inv.ammo.item && u.inv.ammo.quantity > 0;
    const bool release = in.pressed[IN_ATTACK_A] && u.action == SHTIDLE && has_ammo;
    if (!release && angle != -1.0f && level != 0) {
        const float a = angle * 0.017453292f, k = float(level), ha = w.data().c.aim_ha;
        float yaw = in.camera_yaw_deg - std::cos(a) * k;
        const float pitch = in.camera_pitch_deg + std::sin(a) * k;
        if (yaw < 0) yaw += 360.0f;
        else if (yaw >= 360.0f) yaw -= 360.0f;
        if (std::fabs(std::remainder(yaw - range_base_yaw, 360.0f)) < ha && std::fabs(pitch) < ha) {
            w.set_yaw_now(u, yaw);
            u.range_target = 0;
            camera_yaw_out = yaw, camera_pitch_out = pitch;
        }
    }
    range_fire(w, u, in);
    return true;
}

// 0x437ed0: forward locomotion; fwd = Ctrl_forward_state 0x4350f0 (RUN; joystick: IDLE / WALK / RUN by stick level),
// the walk key turns RUN into WALK.
void PlayerController::locomotion(World& w, Unit& u, int move_code, bool walk, int fwd) {
    if (move_code != 0) {
        const int st = walk && fwd == RUN ? WALK : fwd;
        if (u.action != st && w.can_act(u) && u.posture != 1 && !guarding(u.action) && w.set_action(u, st) && st == RUN) run_ts = w.now();
    } else if (u.action == TIPTOE || u.action == WALK || u.action == RUN) {
        w.stop(u);
    }
}

// 0x438110: strafe / back-step states.
void PlayerController::strafe(World& w, Unit& u, bool f, bool b, bool l, bool r) {
    int st = 0;
    if (b && l) st = MOVE_BL;
    else if (b && r) st = MOVE_BR;
    else if (f && l) st = MOVE_FL;
    else if (f && r) st = MOVE_FR;
    else if (l) st = MOVE_L;
    else if (r) st = MOVE_R;
    else if (b) st = MOVE_B;
    if (st && u.action != st && w.can_act(u) && u.posture != 1 && !guarding(u.action)) w.set_action(u, st);
}

InputFrame DuelPad::next(World& w, const Unit& self) {
    InputFrame f;
    const Unit* t = w.unit(target);
    if (!t || w.dead(*t) || w.dead(self)) return f;
    const int now = w.now();
    const float dx = t->pos.x - self.pos.x, dy = t->pos.y - self.pos.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    f.camera_yaw_deg = std::atan2(dy, dx) / 0.017453292f;
    // Timed block: roll once per opponent attack start, like the hero AI (BLOCKPER x difficulty).
    if (is_attacking(t->action) && t->ts_state != last_seen_attack && dist < 200 && w.can_act(self) && !is_attacking(self.action)) {
        last_seen_attack = t->ts_state;
        const Consts& c = w.data().c;
        const int chance = int(float(c.blockper) * c.diff_a[std::clamp(w.difficulty, 0, 3)]);
        if (w.rng().next(100) <= chance && w.find_move(self, GUARD1)) block_until = now + 2000;
    }
    if (now < block_until && !(self.combo_idx > 0)) {
        f.held.set(IN_DEFEND);
        f.mouse_dx = w.rel_angle(self, t->pos) / 0.18f;
        return f;
    }
    if (dist > approach) {
        f.held.set(IN_FORWARD);
        f.press(IN_FORWARD);
        f.pressed.reset(IN_FORWARD);
        return f;
    }
    f.mouse_dx = w.rel_angle(self, t->pos) / 0.18f;
    press_phase ^= 1;
    if (press_phase) {
        const size_t i = self.combo_idx > 0 ? size_t(self.combo_idx) : 0;
        f.press(i < combo.size() && combo[i] == 'B' ? IN_ATTACK_B : IN_ATTACK_A);
    }
    return f;
}

}  // namespace joa::game
