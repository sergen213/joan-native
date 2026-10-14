#pragma once
// In-game input: RawInput (SDL state) -> game::InputFrame on the original DEFAULTK action ids (1..55).
// Bindings = STD.SET DEFAULTK rows (remapped via remap_assign / options.ini), column by key_scheme(input_method):
// 0 keyboard K_KEY, 3 keyboard+mouse FPS_KEY | FPS_MOUSE, 1 gamepad J_KEY, 2 point & click K_KEY of 17/19/20/34 only
// (popped LMB / RMB downs -> InputFrame::click_*, no mouse look, pad Start only). Gamepad buttons (J_JOYPAD) and sticks are
// active in the other schemes. Event model (GAMEPLAY_CHARACTER_COMBAT.md Part B §1.2, CONFIRMED 0x5932e0): key
// and LMB/RMB down/up events go through a FIFO (ring of 20) and take() pops ONE per sim tick, as Input_get_event
// pops one per frame; modifiers (Shift/Ctrl/Alt), Caps/Num/Scroll/Insert, MMB and every pad input are not queued
// (original: modifier bits / joystick held-vs-prev edges) — their rising edges are latched across feeds and
// reported by the next take(). A pressed action is also held on that tick, so a tap between two ticks is seen once.
// Keyboard & mouse scheme: wheel notches since the last take() report Zoom in (48, up) / out (49, down) pressed+held.
#include "engine/input.hpp"
#include "engine/ui_model.hpp"
#include "engine/ui_core.hpp"  // UiPad
#include "game/controller_player.hpp"

#include <array>
#include <bitset>
#include <deque>
#include <vector>

#include <glm/glm.hpp>

namespace joa::game {
struct GameEvent;
}

namespace joa::engine {

struct PadSettings {
    float deadzone = 0.15f, sensitivity = 1.0f;  // settings.ini [controller]
};

// J_JOYPAD n (pad button n-1, KEYDESC JOYPAD1..12) -> SDL positional button. SDL's south/east/west/north are
// positions, so Xbox (A B X Y) and PlayStation (Cross Circle Square Triangle) share one mapping. With DEFAULTK:
//   1 South  A / Cross     Jump             7 D-pad up    Character details   (Xbox DInput order was Back,Start,
//   2 East   B / Circle    Defend           8 D-pad down  Interact             LS,RS for 7..10; Start/Back are
//   3 West   X / Square    Heavy attack     9 L3          Range mode           taken by the fixed menu/RTS keys)
//   4 North  Y / Triangle  Light attack    10 R3          Center camera       11/12 D-pad left/right (unbound)
//   5 LB / L1              HP item          6 RB / R1     EP item
// Fixed: Start/Options = Main Game Menu (34), Back/Share/Create tapped alone = RTS toggle (23), RT/R2 = light,
// LT/L2 = heavy attack, left stick = move/strafe (8 sectors, tilt < 50% walks), right stick X = yaw (scaled like
// the in-game Mouse Sensitivity), Y = pitch (mouse_dy, so Invert Mouse applies).
// Back layer (port extension, fixed; reaches the DEFAULTK actions the original J_JOYPAD column leaves unbound):
// while Back is held the other buttons do not act as themselves but as
//   South 24 Switch character    East 43 Close message     West 15 Energy attack   North 46 Objective
//   LB 53 Quick save             RB 54 Quick load          D-pad up/down 48/49 zoom in/out (held)
//   D-pad right/left 50/51 rotate camera CW/CCW (held)     L3 45 Zoom minimap      R3 44 Hit point bars
//   Start 40 Mission brief       LT 55 Capture screen      RT 41 Message log
// Save/Load/Options (37..39) are in the Start menu; 25..32 via Switch character / the RTS cursor.
SDL_GamepadButton joypad_button(int joypad);
int joypad_index(SDL_GamepadButton b);  // inverse: J_JOYPAD 1..12, 0 = not a J_JOYPAD button

// Menu/overlay navigation from the pad (UiInput::pad): button edges, D-pad / left stick direction with auto-repeat
// (first step on press, then every 100 ms after 400 ms held). game_input_pad.cpp.
class PadUiTracker {
public:
    UiPad update(const RawInput& raw, float dt, float deadzone);

private:
    std::bitset<SDL_GAMEPAD_BUTTON_COUNT> prev_;
    glm::ivec2 dir_{0};
    float repeat_ = 0;
    bool stick_ = false;
};

// RTS virtual cursor: the right stick moves a pointer (0.9 viewport heights per second at full tilt x
// sensitivity), South = left button (click / box select), West = right button (order). Active from the first
// pad pointer input until the mouse moves. game_input_pad.cpp.
struct PadPointer {
    glm::vec2 pos{-1.0f};
    bool active = false;
    void update(const RawInput& raw, float dt, glm::ivec4 viewport, const PadSettings& s);
    void apply(RawInput& raw) const;  // adds the synthesized left/right buttons while active
};

// Rumble for the player's own hits/damage this tick (Ev::Hit / Knockdown); zero = none.
struct PadRumble {
    float low = 0, high = 0;  // motor strength 0..1
    int ms = 0;
};
PadRumble pad_rumble(const std::vector<game::GameEvent>& events, int player);

// Joystick input method (key_scheme 1, controller mode 1 = PlayerCtrl_joystick 0x432d30): the left stick as the
// original reads its DirectInput axes, on SDL's int16 range [-32768, 32767] (centre (min + max) / 2 = 0):
// level = Joy_stick_level 0x433930 (max over x/y of |axis - centre| * 16 / (max - min): < 2 -> 0, 2 -> 1, 3..4 -> 2,
// >= 5 -> 3), angle = Joy_stick_angle 0x433af0 (atan2(-dy, dx) stored as a byte of 256 steps, 1.40625 deg each;
// 90 / 270 straight up / down, -1 exactly centred), move_code = 0x598040 (per axis -1/0/1 past 13 % of the half
// range, 0x597f70, as Input_move_code 0x431680). The POV-hat fallback of 0x598040 is not fed: the port maps the
// D-pad to J_JOYPAD buttons 7, 8, 11, 12. Axes are normalised [-1, 1] (platform: int16 / 32767).
struct JoyStick {
    int level = 0;
    float angle = -1;
    int move_code = 0;
};
JoyStick joy_stick(float x, float y);

class GameInput {
public:
    static constexpr float kPadYawDegPerSec = 150.0f;    // right stick X at full deflection, sensitivity 1
    static constexpr float kPadPitchCountsPerSec = 600.0f;  // right stick Y -> mouse_dy counts

    void configure(const std::vector<KeyBindingRow>& rows, const GameOptions& opt, const PadSettings& pad, bool camera_fix);
    // Once per rendered frame. `pointer` = the window mouse position (pixels) that mouse events of this frame carry
    // (point & click picks at the position of the popped click; -1 = unknown).
    void feed(const RawInput& raw, float dt, glm::vec2 pointer = glm::vec2(-1.0f));
    game::InputFrame take();                   // once per 33 ms sim tick
    void reset();                              // drop queued events, motion and yaw remainder (pause/resume, load)
    bool pressed(int action) const { return valid(action) && frame_pressed_[std::size_t(action)]; }
    bool held(int action) const { return valid(action) && frame_held_[std::size_t(action)]; }
    float wheel() const { return wheel_; }
    bool shift_held() const { return shift_; }
    // Point & click (scheme 2): position of the last take()'s popped LMB / RMB down event (InputFrame::click_*).
    glm::vec2 click_pos() const { return click_pos_; }

private:
    using Actions = std::bitset<game::IN_ACTION_COUNT>;
    // Input ids: DIK 0..255, mouse 256 + SDL button, pad 264 + SDL button, then virtual pad inputs: triggers,
    // stick sectors, Back-layer chords (kChord + SDL button, + COUNT LT, + COUNT + 1 RT) and the Back tap.
    static constexpr int kMouse = 256, kPad = 264, kLT = kPad + SDL_GAMEPAD_BUTTON_COUNT, kRT = kLT + 1, kStickUp = kRT + 1,
                         kStickDown = kStickUp + 1, kStickLeft = kStickDown + 1, kStickRight = kStickLeft + 1,
                         kStickWalk = kStickRight + 1, kChord = kStickWalk + 1, kBackTap = kChord + SDL_GAMEPAD_BUTTON_COUNT + 2,
                         kInputs = kBackTap + 1;
    using Inputs = std::bitset<kInputs>;
    struct Event {
        int id;
        bool down;
        glm::vec2 pos{-1.0f};  // pointer at the feed that queued it
    };

    static bool valid(int a) { return a > 0 && a < game::IN_ACTION_COUNT; }
    Actions actions(const Inputs& in) const;

    std::vector<Actions> map_ = std::vector<Actions>(kInputs);
    GameOptions opt_;
    PadSettings pad_;
    bool camera_fix_ = false;
    Inputs down_, latched_;
    std::deque<Event> queue_;
    Actions frame_held_, frame_pressed_;
    float wheel_ = 0, wheel_acc_ = 0, dx_ = 0, dy_ = 0, pad_yaw_deg_ = 0, yaw_rem_deg_ = 0, joy_x_ = 0, joy_y_ = 0;
    bool alt_ = false, shift_ = false, back_down_ = false, chord_used_ = false, joy_present_ = false;
    glm::vec2 pointer_{-1.0f}, click_pos_{-1.0f};
};

}  // namespace joa::engine
