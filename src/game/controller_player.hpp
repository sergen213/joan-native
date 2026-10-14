// Player controller (g_PlayerCtrl 0xa5b9e8, dispatcher PlayerCtrl_process 0x42e1a0, keyboard+mouse
// mode 3 controller 0x430480). GAMEPLAY_CHARACTER_COMBAT.md Part B §2..§3, Part C §2.2.
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
    void press(int a) { pressed.set(a); held.set(a); }
};

// Controller object state (offsets of g_PlayerCtrl). Plain data, serialized with the World.
struct PlayerController {
    int unit = 0;            // +0x00 controlled unit recno
    int mode = 3;            // +0x10 input scheme (only 3 = keyboard+mouse is implemented); RTS = World::game_mode
    int unit2 = 0;           // +0x04 (set_main_character mode 2)
    int prev_mode = 3;       // +0x14 mode before RTS
    int ride_mode = 0;       // +0xc4 riding (horse controller not implemented)
    int dtap_action = 0;     // +0x82
    int dtap_time = 0;       // +0x86
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

    // One drawn frame. Returns true if input was consumed.
    bool process(World& w, const InputFrame& in);

    template <class V> void visit(V& v) {
        v(unit), v(mode), v(unit2), v(prev_mode), v(ride_mode), v(dtap_action), v(dtap_time), v(ph_a), v(ph_b), v(ph_ab), v(fresh_a), v(fresh_b),
            v(fresh_ab), v(follow), v(attack_ts), v(queued), v(chain), v(jump_hold), v(air_state), v(run_ts),
            v(range_mode), v(range_base_yaw), v(range_base_pitch), v(range_ts), v(camera_yaw_out);
    }

private:
    bool attack_input(World& w, Unit& u, bool a, bool b, bool rush);
    bool energy_attack(World& w, Unit& u, bool go);
    bool block(World& w, Unit& u, bool held);
    bool down_attack(World& w, Unit& u, bool held);
    bool jump(World& w, Unit& u, bool fire, int magnitude, int rel_angle, int hold, float cam_yaw);
    bool chain_input(World& w, Unit& u, bool go);
    bool range_input(World& w, Unit& u, const InputFrame& in);
    void locomotion(World& w, Unit& u, int move_code, bool walk);
    void strafe(World& w, Unit& u, bool f, bool b, bool l, bool r);
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
