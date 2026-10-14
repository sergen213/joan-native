// Joystick player controller PlayerCtrl_joystick 0x432d30 (mode 1; GAMEPLAY_CHARACTER_COMBAT.md Part B §3.3-§3.7).
// The platform fills InputFrame::joy_* from the pad's left stick (Joy_stick_level 0x433930, Joy_stick_angle
// 0x433af0, digital direction 0x598040) and maps the DEFAULTK J_JOYPAD buttons (J_KEY when an action has none,
// Action_held 0x4cb6d0). Decomp 0x432d30..0x43366f read for this port.
#include "game/controller_player.hpp"

#include "game/world_sim.hpp"

namespace joa::game {

// Differences to 0x430480: the stick angle is the camera-relative direction and its level (0..3) the magnitude:
// level picks IDLE / WALK / RUN (Ctrl_forward_state 0x4350f0) and the jump push (Ctrl_jump magnitude = level);
// range mode is held (button 9) instead of toggled and aimed with the stick; A+B pressed together = energy attack
// (neither then counts as A/B); the centre-camera button (2, pad 10) is the strafe/evade modifier and centres the
// camera; double taps are stick flicks to full tilt (level 3): up (85..95 deg) = rush, with the modifier left
// (175..185) / back (265..275) / right (<= 5 or >= 355) = JUMP_L / JUMP_B / JUMP_R, armed by the stick dropping
// below level 2 (+0x8a) and fired within 300 ms of the last full-tilt frame in that direction. No mouse yaw.
bool PlayerController::process_joystick(World& w, Unit& u, const InputFrame& in) {
    if (!in.joy_present) return false;   // *0xa51806 == 0: no pad, nothing happens
    const int now = w.now();
    const bool z = in.held[IN_RANGE_MODE], shift = in.held[IN_SHIFT_CAMERA], def = in.held[IN_DEFEND];
    bool A = in.pressed[IN_ATTACK_A], B = in.pressed[IN_ATTACK_B];
    const bool energy = A && B;
    if (energy) A = B = false;
    int hold = 0;
    const bool jfire = jump_timer(w, in.held[IN_JUMP], hold);
    const int move_code = in.joy_move_code, level = in.joy_level;
    const float ang = in.joy_angle;
    if (dtap_action != 0 && level < 2) dtap_armed = 1;
    int dir = 0;
    if (level == 3) {
        if (ang >= 85.0f && ang <= 95.0f) dir = 4;
        else if (shift && ang >= 175.0f && ang <= 185.0f) dir = 6;
        else if (shift && ang >= 265.0f && ang <= 275.0f) dir = 5;
        else if (shift && (ang <= 5.0f || ang >= 355.0f)) dir = 7;
    }
    const int dt = dir != 0 && dir == dtap_action && dtap_armed != 0 && unsigned(now - dtap_time) < 300u ? dir : 0;
    if (dir != 0) dtap_action = dir, dtap_time = now, dtap_armed = 0;
    if (in.pressed[IN_HEAL_ITEM]) w.use_recovery_item(u, false);
    if (in.pressed[IN_EP_ITEM] && !in.alt_held) w.use_recovery_item(u, true);
    if (range_mode) return range_update(w, u, in, !z, ang, level);   // 0x42b6e0 mode 1: exit on release
    if (ride_mode && w.unit(u.mount)) return horse(w, u, in, move_code, ang, level);
    if (u.m.climb && w.ladder_input && w.ladder_input(u, in)) return true;
    if (in.pressed[IN_INTERACT] && w.interact) w.interact(u);
    if (z && range_input(w, u, in)) return true;   // Ctrl_range_enter 0x42b3c0 while held
    if (down_attack(w, u, def)) return true;
    if (block(w, u, def)) return true;
    if (attack_input(w, u, A, B, dt == 4)) return true;
    if (energy_attack(w, u, energy)) return true;
    if (!(u.action >= MOVE_L && u.action <= MOVE_BR)) {
        if (mount_input(w, u, in.pressed[IN_INTERACT])) return true;   // 0x43c7d0
        if (w.ladder_input && w.ladder_input(u, in)) return true;     // 0x42d300
        if (jump(w, u, jfire, level, ang, hold, in.camera_yaw_deg)) return true;
    }
    if (dt == 5 || dt == 6 || dt == 7) return evade(w, u, dt == 5 ? JUMP_B : dt == 7 ? JUMP_R : JUMP_L, true);
    center_camera(u, shift);
    // Ctrl_forward_state 0x4350f0, mode 1 on foot: level 0 IDLE, 1..2 WALK, 3 RUN (C_IDLE variants: no carts here).
    const int fwd = level == 0 ? IDLE : level == 3 ? RUN : WALK;
    if (!shift || level == 0) {
        if (u.action >= MOVE_L && u.action <= MOVE_BR) w.stop(u);
        face_move_dir(w, u, ang, level, in.camera_yaw_deg);
        locomotion(w, u, move_code, shift, fwd);
        return false;
    }
    // Modifier + stick: the digital direction (move code) as strafe flags.
    const int m = move_code;
    strafe_exact(w, u, m == 0x25 || m == 10 || m == 0xb, m == 0x14 || m == 0xc || m == 0xd, m == 0x1c || m == 10 || m == 0xc,
                 m == 0x22 || m == 0xb || m == 0xd, fwd);
    return true;
}

}  // namespace joa::game
