#pragma once
// In-game input: RawInput (SDL state) -> game::InputFrame on the original DEFAULTK action ids (1..55).
// Bindings = STD.SET DEFAULTK rows (remapped via remap_assign / options.ini), column by key_scheme(input_method):
// 0 keyboard K_KEY, 3 keyboard+mouse FPS_KEY | FPS_MOUSE, 1 gamepad J_KEY. Gamepad buttons (J_JOYPAD) and sticks
// are active in every scheme. Event model (GAMEPLAY_CHARACTER_COMBAT.md Part B §1.2, CONFIRMED 0x5932e0): key
// and LMB/RMB down/up events go through a FIFO (ring of 20) and take() pops ONE per sim tick, as Input_get_event
// pops one per frame; modifiers (Shift/Ctrl/Alt), Caps/Num/Scroll/Insert, MMB and every pad input are not queued
// (original: modifier bits / joystick held-vs-prev edges) — their rising edges are latched across feeds and
// reported by the next take(). A pressed action is also held on that tick, so a tap between two ticks is seen once.
// Keyboard & mouse scheme: wheel notches since the last take() report Zoom in (48, up) / out (49, down) pressed+held.
#include "engine/input.hpp"
#include "engine/ui_model.hpp"
#include "game/controller_player.hpp"

#include <bitset>
#include <deque>
#include <vector>

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
// Fixed: Start/Options = Main Game Menu (34), Back/Share/Create = RTS toggle (23), RT/R2 = light, LT/L2 = heavy
// attack, left stick = move/strafe (8 sectors, tilt < 50% walks), right stick X = yaw, Y = pitch (mouse_dy).
SDL_GamepadButton joypad_button(int joypad);

class GameInput {
public:
    static constexpr float kPadYawDegPerSec = 150.0f;    // right stick X at full deflection, sensitivity 1
    static constexpr float kPadPitchCountsPerSec = 600.0f;  // right stick Y -> mouse_dy counts

    void configure(const std::vector<KeyBindingRow>& rows, const GameOptions& opt, const PadSettings& pad, bool camera_fix);
    void feed(const RawInput& raw, float dt);  // once per rendered frame
    game::InputFrame take();                   // once per 33 ms sim tick
    void reset();                              // drop queued events, motion and yaw remainder (pause/resume, load)
    bool pressed(int action) const { return valid(action) && frame_pressed_[std::size_t(action)]; }
    bool held(int action) const { return valid(action) && frame_held_[std::size_t(action)]; }
    float wheel() const { return wheel_; }

private:
    using Actions = std::bitset<game::IN_ACTION_COUNT>;
    // Input ids: DIK 0..255, mouse 256 + SDL button, pad 264 + SDL button, then virtual pad inputs.
    static constexpr int kMouse = 256, kPad = 264, kLT = kPad + SDL_GAMEPAD_BUTTON_COUNT, kRT = kLT + 1, kStickUp = kRT + 1,
                         kStickDown = kStickUp + 1, kStickLeft = kStickDown + 1, kStickRight = kStickLeft + 1,
                         kStickWalk = kStickRight + 1, kInputs = kStickWalk + 1;
    using Inputs = std::bitset<kInputs>;
    struct Event {
        int id;
        bool down;
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
    float wheel_ = 0, wheel_acc_ = 0, dx_ = 0, dy_ = 0, pad_yaw_deg_ = 0, yaw_rem_deg_ = 0;
    bool alt_ = false;
};

}  // namespace joa::engine
