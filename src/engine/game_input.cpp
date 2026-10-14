#include "engine/game_input.hpp"

#include "engine/menu_options_store.hpp"

#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>

namespace joa::engine {
namespace {

// Key_held 0x4cc3d0 (CONFIRMED): left/right Shift, Ctrl, Alt and both Enters are one key.
int norm_dik(int d) { return d == 0x36 ? 0x2a : d == 0x9d ? 0x1d : d == 0xb8 ? 0x38 : d == 0x9c ? 0x1c : d; }
bool never_held(int d) { return d == 0x3a || d == 0x45 || d == 0x46; }  // Caps / Num / Scroll Lock
// Input_poll 0x594f90: these only update the modifier word, they are not queued (CONFIRMED).
bool modifier(int d) { return d == 0x2a || d == 0x1d || d == 0x38 || never_held(d) || d == 0xd2; }
// Key-down events with a translated char (0xa51d5e != 0): printable keys, Esc/BS/Tab/Enter, keypad (NumLock on).
// HYPOTHESIS: exact ToAscii set of 0x599240 not traced; only the WASD/arrow double tap depends on it.
bool has_char(int d) { return (d >= 0x01 && d <= 0x1c) || (d >= 0x1e && d <= 0x29) || (d >= 0x2b && d <= 0x35) || d == 0x37 || d == 0x39 || (d >= 0x47 && d <= 0x53); }

// Radial deadzone, rescaled so the edge of the deadzone is 0 and full deflection 1.
float stick(float& x, float& y, float dz) {
    const float m = std::hypot(x, y);
    if (m <= dz || m <= 0) return x = y = 0, 0.0f;
    const float r = std::min(1.0f, (m - dz) / std::max(1e-6f, 1 - dz));
    x *= r / m, y *= r / m;
    return r;
}

}  // namespace

SDL_GamepadButton joypad_button(int n) {
    static constexpr SDL_GamepadButton b[12] = {
        SDL_GAMEPAD_BUTTON_SOUTH,         SDL_GAMEPAD_BUTTON_EAST,           SDL_GAMEPAD_BUTTON_WEST,     SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,  SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_LEFT_STICK,    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    return n >= 1 && n <= 12 ? b[n - 1] : SDL_GAMEPAD_BUTTON_INVALID;
}

void GameInput::configure(const std::vector<KeyBindingRow>& rows, const GameOptions& opt, const PadSettings& pad, bool camera_fix) {
    opt_ = opt, pad_ = pad, camera_fix_ = camera_fix;
    std::fill(map_.begin(), map_.end(), Actions{});
    const int scheme = key_scheme(opt.input_method), kc = scheme == 0 ? 0 : scheme == 1 ? 1 : 3;
    for (const KeyBindingRow& r : rows) {
        const int a = r.action;
        if (r.header || !valid(a)) continue;
        const int k = norm_dik(r.code[std::size_t(kc)]);
        if (k > 0 && k < 256 && !never_held(k)) map_[std::size_t(k)].set(std::size_t(a));
        const int m = r.code[4];  // FPS_MOUSE 1 L 2 R 3 M
        if (scheme == 3 && m >= 1 && m <= 3)
            map_[std::size_t(kMouse + (m == 1 ? SDL_BUTTON_LEFT : m == 2 ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE))].set(std::size_t(a));
        if (const SDL_GamepadButton b = joypad_button(r.code[2]); b != SDL_GAMEPAD_BUTTON_INVALID) map_[std::size_t(kPad + b)].set(std::size_t(a));
    }
    // Fixed pad controls (no DEFAULTK column): Start = Main Game Menu, Back = RTS toggle, RT/LT = light/heavy attack,
    // left stick = move/strafe (partial tilt = walk).
    map_[kPad + SDL_GAMEPAD_BUTTON_START].set(34);
    map_[kPad + SDL_GAMEPAD_BUTTON_BACK].set(game::IN_RTS_TOGGLE);
    map_[kRT].set(game::IN_ATTACK_A);
    map_[kLT].set(game::IN_ATTACK_B);
    map_[kStickUp].set(game::IN_FORWARD);
    map_[kStickDown].set(game::IN_BACKWARD);
    map_[kStickLeft].set(game::IN_STRAFE_LEFT);
    map_[kStickRight].set(game::IN_STRAFE_RIGHT);
    map_[kStickWalk].set(game::IN_WALK);
    reset();
}

GameInput::Actions GameInput::actions(const Inputs& in) const {
    Actions a;
    for (std::size_t i = 0; i < in.size(); ++i)
        if (in[i]) a |= map_[i];
    return a;
}

void GameInput::feed(const RawInput& raw, float dt) {
    Inputs now;
    for (std::size_t s = 0; s < raw.keys.size(); ++s)
        if (raw.keys[s])
            if (const int d = norm_dik(sdl_to_dik(int(s))); d > 0 && !never_held(d)) now.set(std::size_t(d));
    for (int b = 1; b < 8; ++b)
        if (raw.mouse_buttons[std::size_t(b)]) now.set(std::size_t(kMouse + b));
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (raw.pad_buttons[std::size_t(b)]) now.set(std::size_t(kPad + b));
    const auto axis = [&](SDL_GamepadAxis a) { return raw.pad_axes[std::size_t(a)]; };
    now.set(kLT, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f);
    now.set(kRT, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f);
    float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    if (const float r = stick(lx, ly, pad_.deadzone); r > 0) {
        constexpr float s = 0.3827f;  // sin 22.5°: eight 45° sectors
        now.set(kStickUp, ly < -s * r), now.set(kStickDown, ly > s * r);
        now.set(kStickLeft, lx < -s * r), now.set(kStickRight, lx > s * r);
        now.set(kStickWalk, r < 0.5f);
    }
    float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    stick(rx, ry, pad_.deadzone);
    pad_yaw_deg_ += rx * kPadYawDegPerSec * pad_.sensitivity * dt;
    dy_ += ry * kPadPitchCountsPerSec * pad_.sensitivity * dt;
    dx_ += raw.mouse_dx, dy_ += raw.mouse_dy;

    const auto queued = [](std::size_t i) { return i < 256 ? !modifier(int(i)) : i == kMouse + SDL_BUTTON_LEFT || i == kMouse + SDL_BUTTON_RIGHT; };
    for (std::size_t i = 0; i < now.size(); ++i) {
        if (now[i] == down_[i]) continue;
        if (queued(i)) {
            if (queue_.size() < 20) queue_.push_back({int(i), bool(now[i])});  // ring of 20 (+0x70d); full = dropped (HYPOTHESIS)
        } else if (now[i]) {
            latched_.set(i);
        }
    }
    down_ = now;
    const Actions held = actions(now);
    frame_pressed_ = held & ~frame_held_;
    frame_held_ = held;
    wheel_ = raw.wheel;
    wheel_acc_ += raw.wheel;
    alt_ = raw.keys[SDL_SCANCODE_LALT] || raw.keys[SDL_SCANCODE_RALT];
}

game::InputFrame GameInput::take() {
    game::InputFrame f;
    if (!queue_.empty()) {
        const Event e = queue_.front();
        queue_.pop_front();
        if (e.down) {
            f.pressed = map_[std::size_t(e.id)];
            f.key_char_event = e.id < 256 && has_char(e.id);
        }
    }
    f.pressed |= actions(latched_);
    if (key_scheme(opt_.input_method) == 3 && wheel_acc_ != 0) f.pressed.set(wheel_acc_ > 0 ? 48 : 49);  // KBM: wheel = zoom
    wheel_acc_ = 0;
    latched_.reset();
    f.held = actions(down_) | f.pressed;
    f.alt_held = alt_;
    f.mouse_sensitivity = std::clamp(opt_.mouse_sensitivity, -1.0f, 1.0f);
    f.invert_mouse = opt_.invert_mouse;
    // Mouse yaw (docs/CAMERA_AND_WIDESCREEN.md "Native mouse yaw"): the controller turns int(mouse_dx * k) degrees
    // (0x431790, k below). Accumulate the exact float yaw here, hand the game a mouse_dx whose product truncates to
    // the whole degrees and keep the sub-degree remainder for the next tick.
    const float k = (f.mouse_sensitivity + 1.0f) * 0.7f + 0.2f;
    yaw_rem_deg_ += dx_ * k * (camera_fix_ ? 1.0f : 0.2f) + pad_yaw_deg_;
    const float deg = std::trunc(yaw_rem_deg_);
    yaw_rem_deg_ -= deg;
    f.mouse_dx = deg == 0 ? 0.0f : (deg + std::copysign(0.5f, deg)) / (k * 0.2f);
    f.mouse_dy = dy_;
    dx_ = dy_ = pad_yaw_deg_ = 0;
    return f;
}

void GameInput::reset() {
    queue_.clear();
    latched_.reset();
    wheel_acc_ = dx_ = dy_ = pad_yaw_deg_ = yaw_rem_deg_ = 0;
}

}  // namespace joa::engine
