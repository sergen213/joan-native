#include "engine/input.hpp"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace joa::engine {
namespace {

constexpr const char* kActionNames[kActionCount] = {
    "MoveX", "MoveY", "LookX", "LookY", "Zoom", "Attack", "Block", "Jump",
    "Use", "Run", "MouseLook", "Pause", "ToggleFullscreen",
};

bool action_from_name(std::string_view n, Action& out) {
    for (std::size_t i = 0; i < kActionCount; ++i)
        if (n == kActionNames[i]) return out = Action(i), true;
    return false;
}

float radial_deadzone(float& x, float& y, float dz) {
    const float len = std::sqrt(x * x + y * y);
    if (len <= dz) return x = y = 0.0f;
    const float s = std::min(1.0f, (len - dz) / (1.0f - dz)) / len;
    x *= s;
    y *= s;
    return s;
}

bool is_look(Action a) { return a == Action::LookX || a == Action::LookY; }

}  // namespace

const char* action_name(Action a) { return std::size_t(a) < kActionCount ? kActionNames[std::size_t(a)] : "?"; }

InputMap InputMap::defaults() {
    InputMap m;
    auto key = [&](Action a, SDL_Scancode s, float scale = 1.0f) { m.bind(a, {Source::Key, int(s), scale}); };
    auto pad = [&](Action a, SDL_GamepadButton b) { m.bind(a, {Source::PadButton, int(b), 1.0f}); };
    auto axis = [&](Action a, SDL_GamepadAxis x, float scale = 1.0f) { m.bind(a, {Source::PadAxis, int(x), scale}); };
    // Keyboard/mouse: provisional WASD layout until the original key table (CONFIG.DAT /
    // joan.exe DirectInput code) is recovered.
    key(Action::MoveY, SDL_SCANCODE_W);
    key(Action::MoveY, SDL_SCANCODE_S, -1.0f);
    key(Action::MoveX, SDL_SCANCODE_D);
    key(Action::MoveX, SDL_SCANCODE_A, -1.0f);
    key(Action::MoveY, SDL_SCANCODE_UP);
    key(Action::MoveY, SDL_SCANCODE_DOWN, -1.0f);
    key(Action::LookX, SDL_SCANCODE_RIGHT);
    key(Action::LookX, SDL_SCANCODE_LEFT, -1.0f);
    key(Action::Jump, SDL_SCANCODE_SPACE);
    key(Action::Use, SDL_SCANCODE_E);
    key(Action::Run, SDL_SCANCODE_LSHIFT);
    key(Action::Pause, SDL_SCANCODE_ESCAPE);
    m.bind(Action::Attack, {Source::MouseButton, SDL_BUTTON_LEFT, 1.0f});
    m.bind(Action::MouseLook, {Source::MouseButton, SDL_BUTTON_RIGHT, 1.0f});
    m.bind(Action::Block, {Source::MouseButton, SDL_BUTTON_MIDDLE, 1.0f});
    m.bind(Action::LookX, {Source::MouseMotionX, 0, 1.0f});
    m.bind(Action::LookY, {Source::MouseMotionY, 0, 1.0f});
    m.bind(Action::Zoom, {Source::MouseWheel, 0, 1.0f});
    // Gamepad
    axis(Action::MoveX, SDL_GAMEPAD_AXIS_LEFTX);
    axis(Action::MoveY, SDL_GAMEPAD_AXIS_LEFTY, -1.0f);  // SDL: stick up = negative
    axis(Action::LookX, SDL_GAMEPAD_AXIS_RIGHTX);
    axis(Action::LookY, SDL_GAMEPAD_AXIS_RIGHTY);
    axis(Action::Attack, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    axis(Action::Block, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    pad(Action::Jump, SDL_GAMEPAD_BUTTON_SOUTH);
    pad(Action::Use, SDL_GAMEPAD_BUTTON_WEST);
    pad(Action::Run, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    pad(Action::Pause, SDL_GAMEPAD_BUTTON_START);
    return m;
}

void InputMap::clear(Action a) {
    std::erase_if(bindings_, [a](const auto& p) { return p.first == a; });
}

void InputMap::evaluate(const RawInput& raw, ActionState& state, float dt) const {
    state.previous = state.value;
    state.value.fill(0.0f);

    auto axes = raw.pad_axes;
    radial_deadzone(axes[SDL_GAMEPAD_AXIS_LEFTX], axes[SDL_GAMEPAD_AXIS_LEFTY], stick_deadzone);
    radial_deadzone(axes[SDL_GAMEPAD_AXIS_RIGHTX], axes[SDL_GAMEPAD_AXIS_RIGHTY], stick_deadzone);
    for (auto t : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER}) {
        float& v = axes[t];
        v = v <= trigger_deadzone ? 0.0f : (v - trigger_deadzone) / (1.0f - trigger_deadzone);
    }

    for (const auto& [action, b] : bindings_) {
        float v = 0.0f;
        switch (b.source) {
        case Source::Key: v = b.code >= 0 && b.code < SDL_SCANCODE_COUNT && raw.keys[b.code] ? 1.0f : 0.0f; break;
        case Source::MouseButton: v = b.code >= 0 && b.code < 8 && raw.mouse_buttons[b.code] ? 1.0f : 0.0f; break;
        case Source::MouseMotionX: v = raw.mouse_look ? raw.mouse_dx * mouse_sensitivity : 0.0f; break;
        case Source::MouseMotionY: v = raw.mouse_look ? raw.mouse_dy * mouse_sensitivity : 0.0f; break;
        case Source::MouseWheel: v = raw.wheel; break;
        case Source::PadButton: v = b.code >= 0 && b.code < SDL_GAMEPAD_BUTTON_COUNT && raw.pad_buttons[b.code] ? 1.0f : 0.0f; break;
        case Source::PadAxis: v = b.code >= 0 && b.code < SDL_GAMEPAD_AXIS_COUNT ? axes[b.code] : 0.0f; break;
        }
        // Look actions are radians this frame: digital/stick sources are rates, mouse is a delta.
        const bool mouse = b.source == Source::MouseMotionX || b.source == Source::MouseMotionY;
        if (is_look(action) && !mouse) v *= pad_look_speed * dt;
        state.value[std::size_t(action)] += v * b.scale;
    }
    for (std::size_t i = 0; i < kActionCount; ++i)
        if (!is_look(Action(i)) && Action(i) != Action::Zoom) state.value[i] = std::clamp(state.value[i], -1.0f, 1.0f);
    if (invert_y) state.value[std::size_t(Action::LookY)] = -state.value[std::size_t(Action::LookY)];
}

std::string InputMap::serialize() const {
    std::ostringstream o;
    o << "set stick_deadzone " << stick_deadzone << "\nset trigger_deadzone " << trigger_deadzone
      << "\nset mouse_sensitivity " << mouse_sensitivity << "\nset pad_look_speed " << pad_look_speed
      << "\nset invert_y " << (invert_y ? 1 : 0) << '\n';
    for (const auto& [a, b] : bindings_) {
        o << action_name(a) << ' ';
        switch (b.source) {
        case Source::Key: {
            std::string name = SDL_GetScancodeName(SDL_Scancode(b.code));
            std::replace(name.begin(), name.end(), ' ', '_');  // "Left Shift" -> "Left_Shift"
            o << "key:" << name;
            break;
        }
        case Source::MouseButton: o << "mouse:" << b.code; break;
        case Source::MouseMotionX: o << "mousex"; break;
        case Source::MouseMotionY: o << "mousey"; break;
        case Source::MouseWheel: o << "wheel"; break;
        case Source::PadButton: o << "pad:" << SDL_GetGamepadStringForButton(SDL_GamepadButton(b.code)); break;
        case Source::PadAxis: o << "axis:" << SDL_GetGamepadStringForAxis(SDL_GamepadAxis(b.code)); break;
        }
        o << ' ' << b.scale << '\n';
    }
    return o.str();
}

bool InputMap::parse(std::string_view text, std::string* error) {
    InputMap out = *this;
    std::vector<Action> replaced;
    std::istringstream in{std::string(text)};
    std::string line;
    int line_no = 0;
    auto fail = [&](const std::string& msg) {
        if (error) *error = "line " + std::to_string(line_no) + ": " + msg;
        return false;
    };
    while (std::getline(in, line)) {
        ++line_no;
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string first, spec;
        if (!(ls >> first)) continue;
        if (first == "set") {
            std::string name;
            float v = 0.0f;
            if (!(ls >> name >> v)) return fail("expected: set <name> <value>");
            if (name == "stick_deadzone") out.stick_deadzone = std::clamp(v, 0.0f, 0.95f);
            else if (name == "trigger_deadzone") out.trigger_deadzone = std::clamp(v, 0.0f, 0.95f);
            else if (name == "mouse_sensitivity") out.mouse_sensitivity = v;
            else if (name == "pad_look_speed") out.pad_look_speed = v;
            else if (name == "invert_y") out.invert_y = v != 0.0f;
            else return fail("unknown setting " + name);
            continue;
        }
        Action a;
        if (!action_from_name(first, a)) return fail("unknown action " + first);
        if (!(ls >> spec)) return fail("missing binding");
        float scale = 1.0f;
        ls >> scale;
        const auto colon = spec.find(':');
        const std::string kind = spec.substr(0, colon);
        const std::string arg = colon == std::string::npos ? "" : spec.substr(colon + 1);
        Binding b{Source::Key, 0, scale};
        if (kind == "key") {
            std::string name = arg;
            std::replace(name.begin(), name.end(), '_', ' ');
            b.code = SDL_GetScancodeFromName(name.c_str());
            if (b.code == SDL_SCANCODE_UNKNOWN) return fail("unknown key " + arg);
        } else if (kind == "mouse") {
            b.source = Source::MouseButton;
            b.code = std::atoi(arg.c_str());
            if (b.code < 1 || b.code > 5) return fail("mouse button must be 1..5");
        } else if (kind == "mousex") b.source = Source::MouseMotionX;
        else if (kind == "mousey") b.source = Source::MouseMotionY;
        else if (kind == "wheel") b.source = Source::MouseWheel;
        else if (kind == "pad") {
            b.source = Source::PadButton;
            b.code = SDL_GetGamepadButtonFromString(arg.c_str());
            if (b.code == SDL_GAMEPAD_BUTTON_INVALID) return fail("unknown pad button " + arg);
        } else if (kind == "axis") {
            b.source = Source::PadAxis;
            b.code = SDL_GetGamepadAxisFromString(arg.c_str());
            if (b.code == SDL_GAMEPAD_AXIS_INVALID) return fail("unknown pad axis " + arg);
        } else return fail("unknown binding kind " + kind);
        if (std::find(replaced.begin(), replaced.end(), a) == replaced.end()) {
            out.clear(a);
            replaced.push_back(a);
        }
        out.bind(a, b);
    }
    *this = std::move(out);
    return true;
}

}  // namespace joa::engine
