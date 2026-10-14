#include "engine/game_input.hpp"

#include "engine/menu_options_store.hpp"

#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace joa::engine {
namespace {

// Key_held 0x4cc3d0 (CONFIRMED): left/right Shift, Ctrl, Alt and both Enters are one key.
int norm_dik(int d) { return d == 0x36 ? 0x2a : d == 0x9d ? 0x1d : d == 0xb8 ? 0x38 : d == 0x9c ? 0x1c : d; }
bool never_held(int d) { return d == 0x3a || d == 0x45 || d == 0x46; }  // Caps / Num / Scroll Lock
// Input_poll 0x594f90: these only update the modifier word, they are not queued (CONFIRMED).
bool modifier(int d) { return d == 0x2a || d == 0x1d || d == 0x38 || never_held(d) || d == 0xd2; }
// Key-down events with a translated char (0xa51d5e != 0): Key_match_char 0x599240 (mask 0xe200: any modifiers) has a
// non-zero char for exactly these DIKs: Esc..Enter, A..`, \..slash, keypad * and the keypad block, Space, F1..F12,
// keypad = and Enter, the 0x91..0x93 / 0xb3 / 0xb5 keys and Home/arrows/PgUp/End/PgDn/Ins/Del (0x4800-style codes).
// Only the double taps (WASD in mode 3, arrows in mode 0) depend on it.
bool has_char(int d) {
    return (d >= 0x01 && d <= 0x1c) || (d >= 0x1e && d <= 0x29) || (d >= 0x2b && d <= 0x35) || d == 0x37 || d == 0x39 ||
           (d >= 0x3b && d <= 0x44) || (d >= 0x47 && d <= 0x53) || d == 0x57 || d == 0x58 || d == 0x8d || (d >= 0x91 && d <= 0x93) ||
           d == 0x9c || d == 0xb3 || d == 0xb5 || d == 0xc7 || d == 0xc8 || d == 0xc9 || d == 0xcb || d == 0xcd || (d >= 0xcf && d <= 0xd3);
}

// Radial deadzone, rescaled so the edge of the deadzone is 0 and full deflection 1.
float stick(float& x, float& y, float dz) {
    const float m = std::hypot(x, y);
    if (m <= dz || m <= 0) return x = y = 0, 0.0f;
    const float r = std::min(1.0f, (m - dz) / std::max(1e-6f, 1 - dz));
    x *= r / m, y *= r / m;
    return r;
}

}  // namespace

JoyStick joy_stick(float fx, float fy) {
    constexpr int kMin = -32768, kMax = 32767, kMid = (kMin + kMax) / 2;
    const auto raw = [](float v) { return int(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); };
    const int x = raw(fx), y = raw(fy);
    JoyStick s;
    const int m = std::max((std::abs(x - kMid) << 4) / (kMax - kMin), (std::abs(y - kMid) << 4) / (kMax - kMin));
    s.level = m >= 5 ? 3 : m >= 3 ? 2 : m >= 2 ? 1 : 0;
    const int dx = x - kMid, dy = y - kMid;
    if (dx == 0) {
        s.angle = dy < 0 ? 90.0f : dy > 0 ? 270.0f : -1.0f;
    } else {
        // 0x576da0: atan2 in double, stored as float; 0x576240: byte = ftol(rad * 256 / 2pi); 0x576300: byte * 1.40625.
        const float r = float(std::atan2(double(float(-dy)), double(float(dx))));
        s.angle = float(std::uint8_t(int(double(r) * 40.74366543152521))) * 1.40625f;
    }
    const auto dir = [](int v) {   // 0x597f70: MulDiv(min - mid, 13, 100) / MulDiv(max - mid, 13, 100)
        return v < kMid + (kMin - kMid) * 13 / 100 ? -1 : v > kMid + (kMax - kMid) * 13 / 100 ? 1 : 0;
    };
    const int h = dir(x), v = dir(y);
    s.move_code = h < 0 ? (v < 0 ? 10 : v > 0 ? 0xc : 0x1c) : h > 0 ? (v < 0 ? 0xb : v > 0 ? 0xd : 0x22) : (v < 0 ? 0x25 : v > 0 ? 0x14 : 0);
    return s;
}

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
    // Point & click (scheme 2): PlayerCtrl_mouse_click 0x438b30 reads only actions 0x11 / 0x13 / 0x14 through
    // Action_pressed(0, ..) (K_KEY); every other key handler asks the current scheme (Action_pressed(2, ..) = 0).
    // Port deviation: 34 Main Game Menu stays live so the menu remains reachable.
    const int scheme = key_scheme(opt.input_method), kc = scheme == 0 || scheme == 2 ? 0 : scheme == 1 ? 1 : 3;
    for (const KeyBindingRow& r : rows) {
        const int a = r.action;
        if (r.header || !valid(a)) continue;
        if (scheme == 2 && a != game::IN_INTERACT && a != game::IN_HEAL_ITEM && a != game::IN_EP_ITEM && a != 34) continue;
        const int k = norm_dik(r.code[std::size_t(kc)]);
        if (k > 0 && k < 256 && !never_held(k)) map_[std::size_t(k)].set(std::size_t(a));
        const int m = r.code[4];  // FPS_MOUSE 1 L 2 R 3 M
        if (scheme == 3 && m >= 1 && m <= 3)
            map_[std::size_t(kMouse + (m == 1 ? SDL_BUTTON_LEFT : m == 2 ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE))].set(std::size_t(a));
        if (const SDL_GamepadButton b = joypad_button(r.code[2]); scheme != 2 && b != SDL_GAMEPAD_BUTTON_INVALID)
            map_[std::size_t(kPad + b)].set(std::size_t(a));
    }
    // Fixed pad controls (no DEFAULTK column): Start = Main Game Menu, Back tap = RTS toggle, RT/LT = light/heavy
    // attack, left stick = move/strafe (partial tilt = walk), and the Back layer (game_input.hpp).
    map_[kPad + SDL_GAMEPAD_BUTTON_START].set(34);
    if (scheme == 2) return reset();  // no pad controls in point & click
    map_[kBackTap].set(game::IN_RTS_TOGGLE);
    static constexpr std::pair<int, int> kLayer[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 24},         {SDL_GAMEPAD_BUTTON_EAST, 43},       {SDL_GAMEPAD_BUTTON_WEST, 15},
        {SDL_GAMEPAD_BUTTON_NORTH, 46},         {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 53}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 54},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 48},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 49},  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 50},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 51},     {SDL_GAMEPAD_BUTTON_LEFT_STICK, 45}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 44},
        {SDL_GAMEPAD_BUTTON_START, 40},         {SDL_GAMEPAD_BUTTON_COUNT, 55},      {SDL_GAMEPAD_BUTTON_COUNT + 1, 41}};
    for (const auto& [b, a] : kLayer) map_[std::size_t(kChord + b)].set(std::size_t(a));
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

void GameInput::feed(const RawInput& raw, float dt, glm::vec2 pointer) {
    pointer_ = pointer;
    Inputs now;
    for (std::size_t s = 0; s < raw.keys.size(); ++s)
        if (raw.keys[s])
            if (const int d = norm_dik(sdl_to_dik(int(s))); d > 0 && !never_held(d)) now.set(std::size_t(d));
    for (int b = 1; b < 8; ++b)
        if (raw.mouse_buttons[std::size_t(b)]) now.set(std::size_t(kMouse + b));
    // Back layer: while Back is held the other buttons/triggers are chords; Back released without one = tap.
    const bool back = raw.pad_buttons[SDL_GAMEPAD_BUTTON_BACK];
    if (back && !back_down_) chord_used_ = false;
    const auto axis = [&](SDL_GamepadAxis a) { return raw.pad_axes[std::size_t(a)]; };
    const bool lt = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f, rt = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (raw.pad_buttons[std::size_t(b)] && b != SDL_GAMEPAD_BUTTON_BACK) now.set(std::size_t((back ? kChord : kPad) + b));
    now.set(back ? kChord + SDL_GAMEPAD_BUTTON_COUNT : kLT, lt);
    now.set(back ? kChord + SDL_GAMEPAD_BUTTON_COUNT + 1 : kRT, rt);
    if (back && (lt || rt || (raw.pad_buttons.count() > 1))) chord_used_ = true;
    now.set(kBackTap, !back && back_down_ && !chord_used_);
    back_down_ = back;
    joy_x_ = axis(SDL_GAMEPAD_AXIS_LEFTX), joy_y_ = axis(SDL_GAMEPAD_AXIS_LEFTY), joy_present_ = raw.pad_connected;
    float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    if (const float r = stick(lx, ly, pad_.deadzone); r > 0) {
        constexpr float s = 0.3827f;  // sin 22.5°: eight 45° sectors
        now.set(kStickUp, ly < -s * r), now.set(kStickDown, ly > s * r);
        now.set(kStickLeft, lx < -s * r), now.set(kStickRight, lx > s * r);
        now.set(kStickWalk, r < 0.5f);
    }
    float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    stick(rx, ry, pad_.deadzone);
    // Right stick yaw follows the in-game Mouse Sensitivity like mouse yaw (k / k(0), take()).
    pad_yaw_deg_ += rx * kPadYawDegPerSec * pad_.sensitivity * ((std::clamp(opt_.mouse_sensitivity, -1.0f, 1.0f) + 1.0f) * 0.7f + 0.2f) / 0.9f * dt;
    dy_ += ry * kPadPitchCountsPerSec * pad_.sensitivity * dt;
    dx_ += raw.mouse_dx, dy_ += raw.mouse_dy;

    const auto queued = [](std::size_t i) { return i < 256 ? !modifier(int(i)) : i == kMouse + SDL_BUTTON_LEFT || i == kMouse + SDL_BUTTON_RIGHT; };
    for (std::size_t i = 0; i < now.size(); ++i) {
        if (now[i] == down_[i]) continue;
        if (queued(i)) {
            if (queue_.size() < 20) queue_.push_back({int(i), bool(now[i]), pointer_});  // ring of 20 (+0x70d); full = dropped (HYPOTHESIS)
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
    shift_ = raw.keys[SDL_SCANCODE_LSHIFT] || raw.keys[SDL_SCANCODE_RSHIFT];  // DAT_00a51800 & 3 (commander hold keys)
}

game::InputFrame GameInput::take() {
    game::InputFrame f;
    if (!queue_.empty()) {
        const Event e = queue_.front();
        queue_.pop_front();
        if (e.down) {
            f.pressed = map_[std::size_t(e.id)];
            f.key_char_event = e.id < 256 && has_char(e.id);
            // Point & click: the popped LMB / RMB down (Mouse_click_in_rect 0x5945d0 button 0 / 1) and its position.
            if (key_scheme(opt_.input_method) == 2 && (e.id == kMouse + SDL_BUTTON_LEFT || e.id == kMouse + SDL_BUTTON_RIGHT)) {
                (e.id == kMouse + SDL_BUTTON_LEFT ? f.click_left : f.click_right) = true;
                click_pos_ = e.pos;
            }
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
    // Joystick controller (mode 1) stick inputs; the other controllers ignore them.
    if (joy_present_) {
        const JoyStick s = joy_stick(joy_x_, joy_y_);
        f.joy_present = true, f.joy_level = s.level, f.joy_angle = s.angle, f.joy_move_code = s.move_code;
    }
    // Mouse yaw (docs/CAMERA_AND_WIDESCREEN.md "Native mouse yaw"): the controller turns int(mouse_dx * k) degrees
    // (0x431790, k below). Accumulate the exact float yaw here, hand the game a mouse_dx whose product truncates to
    // the whole degrees and keep the sub-degree remainder for the next tick.
    const float k = (f.mouse_sensitivity + 1.0f) * 0.7f + 0.2f;
    yaw_rem_deg_ += dx_ * k * (camera_fix_ ? 1.0f : 0.2f) + pad_yaw_deg_;
    const float deg = std::trunc(yaw_rem_deg_);
    yaw_rem_deg_ -= deg;
    f.mouse_dx = deg == 0 ? 0.0f : (deg + std::copysign(0.5f, deg)) / (k * 0.2f);
    f.mouse_dy = dy_;
    if (key_scheme(opt_.input_method) == 2) f.mouse_dx = f.mouse_dy = yaw_rem_deg_ = 0;  // the mouse is a pointer: no look
    dx_ = dy_ = pad_yaw_deg_ = 0;
    return f;
}

void GameInput::reset() {
    queue_.clear();
    latched_.reset();
    wheel_acc_ = dx_ = dy_ = pad_yaw_deg_ = yaw_rem_deg_ = 0;
}

}  // namespace joa::engine
