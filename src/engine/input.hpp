#pragma once
// Device-agnostic input action map. Raw device state is filled from SDL events by the platform
// layer; evaluation is pure so it is testable without a window or devices.
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace joa::engine {

// Axes are in [-1,1]; LookX/LookY are radians to turn this frame; Zoom is wheel steps. Buttons are 0/1.
enum class Action : std::uint8_t {
    MoveX, MoveY,        // strafe right+, forward+
    LookX, LookY,        // camera yaw/pitch rate
    Zoom,
    Attack, Block, Jump, Use, Run,
    MouseLook,           // hold to steer the camera with the mouse
    Pause, ToggleFullscreen,
    Count
};
inline constexpr std::size_t kActionCount = std::size_t(Action::Count);

const char* action_name(Action a);

enum class Source : std::uint8_t { Key, MouseButton, MouseMotionX, MouseMotionY, MouseWheel, PadButton, PadAxis };

struct Binding {
    Source source = Source::Key;
    int code = 0;        // SDL_Scancode, SDL mouse button index, SDL_GamepadButton or SDL_GamepadAxis
    float scale = 1.0f;  // sign/weight; a key bound with -1 to MoveX means "left"
};

struct RawInput {
    std::bitset<SDL_SCANCODE_COUNT> keys;
    std::bitset<8> mouse_buttons;  // SDL_BUTTON_LEFT=1 .. X2=5
    float mouse_dx = 0.0f, mouse_dy = 0.0f, wheel = 0.0f;  // accumulated since last frame
    bool mouse_look = false;  // mouse motion drives Look* only while captured (MouseLook held)
    std::bitset<SDL_GAMEPAD_BUTTON_COUNT> pad_buttons;
    std::array<float, SDL_GAMEPAD_AXIS_COUNT> pad_axes{};  // normalised [-1,1], triggers [0,1]
    std::string text;    // UTF-8 typed since last frame (SDL text input, only while a text field is active)
    int key_repeat = 0;  // scancode of a key-repeat event while text input is active (typematic editing), 0 = none
    bool pad_connected = false;  // GamepadPlay: a gamepad is open (Options -> Input lists Gamepad/Joystick, 0x7435e0)

    void end_frame() { mouse_dx = mouse_dy = wheel = 0.0f, text.clear(), key_repeat = 0; }
};

struct ActionState {
    std::array<float, kActionCount> value{};
    std::array<float, kActionCount> previous{};

    float operator[](Action a) const { return value[std::size_t(a)]; }
    bool down(Action a) const { return value[std::size_t(a)] > 0.5f; }
    bool pressed(Action a) const { return down(a) && previous[std::size_t(a)] <= 0.5f; }
};

class InputMap {
public:
    float stick_deadzone = 0.2f;     // radial, applied per stick
    float trigger_deadzone = 0.1f;
    float mouse_sensitivity = 0.0025f;  // radians per mouse count for LookX/LookY
    float pad_look_speed = 2.5f;        // radians per second at full stick deflection / held key
    bool invert_y = false;

    static InputMap defaults();

    void bind(Action a, Binding b) { bindings_.push_back({a, b}); }
    void clear(Action a);
    const std::vector<std::pair<Action, Binding>>& bindings() const { return bindings_; }

    // Updates `state` (shifting value->previous) from raw device state.
    void evaluate(const RawInput& raw, ActionState& state, float dt) const;

    // Text format, one binding per line: "<Action> key:Left_Shift -1" (SDL scancode name, spaces as '_') /
    // "mouse:1" / "mousex" / "mousey" / "wheel" / "pad:a" (SDL gamepad button name) / "axis:leftx",
    // plus "set <name> <value>" for tunables. '#' starts a comment.
    std::string serialize() const;
    // Replaces bindings for every action mentioned; returns false (with error) on malformed input.
    bool parse(std::string_view text, std::string* error = nullptr);

private:
    std::vector<std::pair<Action, Binding>> bindings_;
};

}  // namespace joa::engine
