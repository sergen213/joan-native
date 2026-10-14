// Keyboard-only player controller PlayerCtrl_keyboard 0x4343e0 (mode 0; GAMEPLAY_CHARACTER_COMBAT.md Part B §3.3).
// Default keys (DEFAULTK K_KEY): arrows = Up/Down/Left/Right (actions 4/5/8/9), LShift = action 3, E defend, Q/W
// light/heavy attack, S energy, Space jump, R interact, Z range mode. Decomp 0x4343e0..0x434cd9 read for this port.
#include "game/controller_player.hpp"

#include "game/world_sim.hpp"

namespace joa::game {

namespace {
// Input_move_angle 0x431570: left+right cancel, forward before back; -1 = no direction.
float move_angle(bool f, bool b, bool l, bool r) {
    if (l && r) l = r = false;
    if (f) return l ? 135.0f : r ? 45.0f : 90.0f;
    if (b && l) return 225.0f;
    if (b && r) return 315.0f;
    return l ? 180.0f : r ? 360.0f : b ? 270.0f : -1.0f;
}
}  // namespace

// Differences to 0x430480: movement is camera-relative (Left/Right turn the hero to face camera-left/right and run,
// Down runs toward the camera; camera 0 follows in face_move_dir), move code 0x25 for Up/Left/Right else 0x14 for
// Down; Shift + direction = strafe/back-step (0x438110) with camera centring (0x436190); Defend is pressed or held;
// energy is S only (no A+B); attacks are tried before energy; no mouse yaw.
bool PlayerController::process_keyboard(World& w, Unit& u, const InputFrame& in) {
    const int now = w.now();
    const bool z = in.pressed[IN_RANGE_MODE], A = in.pressed[IN_ATTACK_A], B = in.pressed[IN_ATTACK_B];
    const bool C = in.pressed[IN_ENERGY], def = in.pressed[IN_DEFEND] || in.held[IN_DEFEND], shift = in.held[IN_SHIFT];
    int hold = 0;
    const bool jfire = jump_timer(w, in.held[IN_JUMP], hold);
    const bool up = in.held[IN_FORWARD], down = in.held[IN_BACKWARD], left = in.held[IN_TURN_LEFT], right = in.held[IN_TURN_RIGHT];
    const int move_code = up || left || right ? 0x25 : down ? 0x14 : 0;
    const float rel = move_angle(up, down, left, right);
    const int magnitude = up || down || left || right ? 3 : 0;
    // Double tap: on a key event with a char (0xa51d5e) the tap is Up (4), or with Shift held Down (5) / Left (6) /
    // Right (7); it fires when it equals the previous tap (+0x82) within 300 ms, then this event is recorded.
    int tap = 0;
    if (in.key_char_event)
        tap = in.pressed[IN_FORWARD] ? 4 : !shift ? 0 : in.pressed[IN_BACKWARD] ? 5 : in.pressed[IN_TURN_LEFT] ? 6 : in.pressed[IN_TURN_RIGHT] ? 7 : 0;
    const int dt = dtap_action != 0 && unsigned(now - dtap_time) < 300u && dtap_action == tap ? tap : 0;
    if (in.key_char_event) dtap_action = tap, dtap_time = now;
    if (in.pressed[IN_HEAL_ITEM]) w.use_recovery_item(u, false);
    if (in.pressed[IN_EP_ITEM] && !in.alt_held) w.use_recovery_item(u, true);
    if (range_mode) return range_update(w, u, in, z, rel, magnitude);   // 0x42a810: no movement, keys aim
    if (ride_mode && w.unit(u.mount)) return horse(w, u, in, move_code, rel, magnitude);
    if (u.m.climb && w.ladder_input && w.ladder_input(u, in)) return true;
    if (in.pressed[IN_INTERACT] && w.interact) w.interact(u);   // result ignored, as in 0x430480
    if (z && range_input(w, u, in)) return true;                // Ctrl_range_enter 0x42b3c0
    center_camera(u, shift);
    if (down_attack(w, u, def)) return true;
    if (block(w, u, def)) return true;
    if (attack_input(w, u, A, B, dt == 4)) return true;
    if (energy_attack(w, u, C)) return true;
    if (!(u.action >= MOVE_L && u.action <= MOVE_BR)) {
        if (mount_input(w, u, in.pressed[IN_INTERACT])) return true;   // 0x43c7d0
        if (w.ladder_input && w.ladder_input(u, in)) return true;     // 0x42d300
        // Jump guard (asm 0x434a7b..0x434a87): Key_held(Up)+Key_held(Down)+Key_held(Left)+Key_held(Right) == 1 on
        // the raw values, and Key_held 0x4cc3d0 returns the DirectInput state byte 0x80 per held key: the sum is
        // never 1, so keyboard-only mode never jumps (original behaviour, kept). (A remap of a direction to
        // Enter/Ctrl/Shift/Alt, for which Key_held returns 1, is not modelled.)
        const int raw_held = 0x80 * (int(up) + int(down) + int(left) + int(right));
        if (raw_held == 1 && jump(w, u, jfire, magnitude, rel, hold, in.camera_yaw_deg)) return true;
    }
    if (dt == 5 || dt == 6 || dt == 7) return evade(w, u, dt == 5 ? JUMP_B : dt == 7 ? JUMP_R : JUMP_L, false);
    if (!shift || move_code == 0) {
        if (u.action >= MOVE_L && u.action <= MOVE_BR) w.stop(u);
        face_move_dir(w, u, rel, magnitude, in.camera_yaw_deg);
        locomotion(w, u, move_code, shift);
        return false;
    }
    center_camera(u, true);
    strafe_exact(w, u, up, down, left, right, RUN);
    return true;
}

}  // namespace joa::game
