// Player controller (g_PlayerCtrl 0xa5b9e8, dispatcher PlayerCtrl_process 0x42e1a0, mode controllers keyboard 0
// 0x4343e0, joystick 1 0x432d30, point&click 2 0x438b30, keyboard+mouse 3 0x430480). GAMEPLAY_CHARACTER_COMBAT.md
// Part B §2..§3, Part C §2.2.
// Input is API-neutral: an InputFrame carries the action ids of the original 55-entry binding table
// (DEFAULTK.csv, Part B §1.4) as held/pressed bits — the platform layer maps keys/buttons to them
// and pops ONE input event per frame like Input_get_event 0x5932e0.
#pragma once

#include <bitset>
#include <string>
#include <cstdint>

namespace joa::game {

class World;
struct Unit;
class AiSystem;

// Action ids (1-based DEFAULTK rows; CONFIRMED).
enum InputAction : int {
    IN_SHIFT_CAMERA = 2, IN_SHIFT = 3, IN_FORWARD = 4, IN_BACKWARD = 5, IN_STRAFE_LEFT = 6, IN_STRAFE_RIGHT = 7,
    IN_TURN_LEFT = 8, IN_TURN_RIGHT = 9, IN_WALK = 10, IN_DEFEND = 12, IN_ATTACK_A = 13, IN_ATTACK_B = 14,
    IN_ENERGY = 15, IN_JUMP = 16, IN_INTERACT = 17, IN_RANGE_MODE = 18, IN_HEAL_ITEM = 19, IN_EP_ITEM = 20,
    IN_CHAIN = 21, IN_RTS_TOGGLE = 23, IN_SWITCH_CYCLE = 24, IN_SWITCH_JOAN = 25, IN_SWITCH_JEAN = 26,
    IN_SWITCH_LAHIRE = 27, IN_SWITCH_ALENCON = 28, IN_ACTION_COUNT = 56
};

struct InputFrame {
    std::bitset<IN_ACTION_COUNT> held, pressed;  // pressed = the popped event of this frame (edge)
    bool key_char_event = false;  // popped event was a key-down with a translated char (0xa51d5e != 0)
    bool alt_held = false;
    float mouse_dx = 0, mouse_dy = 0;             // accumulated mickeys since the last frame
    float camera_yaw_deg = 0;                     // camera 0 yaw (owned by the camera system)
    float camera_pitch_deg = 0;
    float mouse_sensitivity = 0;                  // config [-1,1] (controller +0xe1)
    bool invert_mouse = false;
    // Joystick (mode 1): the platform fills these from the pad's left stick like Joy_stick_level 0x433930,
    // Joy_stick_angle 0x433af0 and the digital direction 0x598040 (13 % deadzone per axis).
    bool joy_present = false;  // *0xa51806 != 0; without a pad the joystick controller does nothing
    int joy_level = 0;         // 0..3
    float joy_angle = -1;      // degrees CCW (90 = up) in 1.40625 steps; -1 = exactly centred
    int joy_move_code = 0;     // Input_move_code 0x431680 code of the digital direction (0x25 up, 0x14 down, ...)
    // Point & click (mode 2, PlayerCtrl_mouse_click 0x438b30): this frame's popped LMB / RMB down event inside the
    // 3-D view (Mouse_click_in_rect 0x5945d0) and what the engine's pick ray found under it: 0x439110 = the nearest
    // unit / map item (MapObjects recno) / floor drop (World::items id), hero excluded, at most one set; else
    // 0x43a3b0 = the terrain point. The engine fills these; game/ never sees the camera.
    bool click_left = false, click_right = false;
    int pick_unit = 0, pick_item = 0, pick_drop = 0;
    bool pick_ground = false;
    float ground_x = 0, ground_y = 0, ground_z = 0;
    void press(int a) { pressed.set(a); held.set(a); }
};

// Controller object state (offsets of g_PlayerCtrl). Plain data, serialized with the World.
struct PlayerController {
    int unit = 0;            // +0x00 controlled unit recno
    int mode = 3;            // +0x10 input scheme 0 kbd, 1 joystick, 2 point&click, 3 kbd+mouse (World::tick_units copies game_mode)
    int unit2 = 0;           // +0x04 (set_main_character mode 2)
    int prev_mode = 3;       // +0x14 mode before RTS
    int ride_mode = 0;       // +0xc4 riding: the horse controller (Ctrl_horse 0x43c5c0) runs
    float horse_heading = 0; // +0xc9 heading at the last gear change
    int dtap_action = 0;     // +0x82
    int dtap_time = 0;       // +0x86
    int dtap_armed = 0;      // +0x8a joystick: stick fell below level 2 since the last full-tilt direction
    int cam_center = 0;      // +0x74 camera centring 0 off, 1 finishing, 2 held (0x436190 / 0x4384e0)
    float cam_center_yaw = 0;  // +0x75 facing saved by 0x436190(1)
    int ph_a = 0, ph_b = 0, ph_ab = 0;          // +0x8e/+0x8f/+0x90 button phase 0 idle 1 down 2 up 3 re-pressed
    int fresh_a = 0, fresh_b = 0, fresh_ab = 0; // +0x92/+0x93/+0x94
    int follow = 0;          // +0x96 queued follow-up
    int attack_ts = 0;       // +0x97 time current attack started
    int queued = 0;          // +0x9b 1 A 2 B 3 C 4 cancel
    int chain = 1;           // +0x9d chain level (melee damage multiplier for the player unit)
    int jump_hold = 0;       // +0x9e
    int air_state = 0;       // +0xa6
    int run_ts = 0;          // +0xa8
    bool range_mode = false; // +0xac
    float range_base_yaw = 0, range_base_pitch = 0;  // +0xb1 / +0xb5
    int range_ts = 0;        // +0xc0
    float camera_yaw_out = 0;  // camera yaw after controller-driven turns (camera system reads it back)
    float camera_pitch_out = 0;  // camera pitch (up +) after controller aim, modes 0/1 range mode (camera reads it back); not state
    // Point & click (mode 2): auto action toward the clicked target, run by the mode update 0x439090.
    int auto_target = 0;     // +0xd3 unit or map-item recno
    int auto_kind = 0;       // +0xd7 0 unit, 1 map item
    int auto_state = 0;      // +0xdd 0 none, 1 attack, 2 fury (EGATK_MP paid), 3 energy attack, 4 approach own item
    int auto_fury = 0;       // +0xdb fury round: an attack started

    // One drawn frame. Returns true if input was consumed.
    bool process(World& w, const InputFrame& in);
    // 0x4384e0, shared tail of the ViewCtrl_process mode updates (outside range mode): while +0x74 != 0 camera 0
    // turns toward the facing saved by 0x436190 at <= 40 deg per frame; state 1 ends when it arrives.
    void center_camera_step();
    // Mode 2 update 0x439090 (ViewCtrl_process, once per tick after the units): Player_autoAttackUpdate 0x43acb0
    // (states 1..3) / 0x43b2d0 (state 4), then 0x4384e0. controller_click.cpp.
    void update_click(World& w);

    template <class V> void visit(V& v) {
        v(unit), v(mode), v(unit2), v(prev_mode), v(ride_mode), v(horse_heading), v(dtap_action), v(dtap_time), v(ph_a), v(ph_b),
            v(ph_ab), v(fresh_a), v(fresh_b), v(fresh_ab), v(follow), v(attack_ts), v(queued), v(chain), v(jump_hold), v(air_state),
            v(run_ts), v(range_mode), v(range_base_yaw), v(range_base_pitch), v(range_ts), v(camera_yaw_out), v(dtap_armed),
            v(cam_center), v(cam_center_yaw);
        v(auto_target), v(auto_kind), v(auto_state), v(auto_fury);
    }

private:
    bool process_kbm(World& w, Unit& u, const InputFrame& in);       // mode 3 0x430480
    bool process_keyboard(World& w, Unit& u, const InputFrame& in);  // mode 0 0x4343e0, controller_keyboard.cpp
    bool process_joystick(World& w, Unit& u, const InputFrame& in);  // mode 1 0x432d30, controller_joystick.cpp
    bool process_click(World& w, Unit& u, const InputFrame& in);     // mode 2 0x438b30, controller_click.cpp
    static AiSystem* click_ai(World& w);                             // World's brain if it is the AiSystem
    bool auto_attack_set(World& w, Unit& u, int id, int kind);       // Player_autoAttackSetTarget 0x43a7d0
    bool auto_energy_set(World& w, Unit& u, int id, int kind);       // 0x43a9e0
    bool jump_timer(const World& w, bool held, int& hold);           // +0x9e, shared by modes 0/1/3
    bool evade(World& w, Unit& u, int action, bool need_move);       // dt 5/7/6 -> JUMP_B/R/L, returns true
    bool mount_input(World& w, Unit& u, bool r);                     // 0x43c7d0: R mounts a free horse within 70
    void center_camera(const Unit& u, bool on);                      // 0x436190
    void range_fire(World& w, Unit& u, const InputFrame& in);       // draw 0x42ad50 / release 0x42ba40
    bool range_update(World& w, Unit& u, const InputFrame& in, bool exit, float angle, int level);  // 0x42a810 modes 0/1
    void strafe_exact(World& w, Unit& u, bool f, bool b, bool l, bool r, int fwd);  // Ctrl_strafe 0x438110 modes 0/1
    bool attack_input(World& w, Unit& u, bool a, bool b, bool rush);
    bool energy_attack(World& w, Unit& u, bool go);
    bool block(World& w, Unit& u, bool held);
    bool down_attack(World& w, Unit& u, bool held);
    bool jump(World& w, Unit& u, bool fire, int magnitude, float rel_angle, int hold, float cam_yaw);
    bool chain_input(World& w, Unit& u, bool go);
    bool range_input(World& w, Unit& u, const InputFrame& in);
    void locomotion(World& w, Unit& u, int move_code, bool walk, int fwd = 11 /* RUN */);  // 0x437ed0, fwd = 0x4350f0
    void strafe(World& w, Unit& u, bool f, bool b, bool l, bool r);
    void face_move_dir(World& w, Unit& u, float rel, int magnitude, float cam_yaw);   // 0x4352f0
    bool horse(World& w, Unit& u, const InputFrame& in, int move_code, float rel, int magnitude);  // mount_ride.cpp
    void horse_attack(World& w, Unit& u, bool a, bool b);                                     // 0x43ca90
};

// Stand-in melee opponent for duels/tests: produces the InputFrames a player would (approach, combo
// presses, timed blocks rolled against BLOCKPER x difficulty). NOT the original AI (that is engine
// code owned by AiTroops); it exists so combat can be exercised end to end headless.
struct DuelPad {
    int target = 0;
    int block_until = 0, last_seen_attack = 0;
    int press_phase = 0;
    float approach = 70;        // close in until this 2-D distance
    std::string combo = "A";    // letter per combo step (e.g. "AAAB")
    InputFrame next(World& w, const Unit& self);
};

}  // namespace joa::game
