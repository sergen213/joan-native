// Gamepad helpers around GameInput (game_input.hpp): menu navigation input, the RTS virtual cursor and rumble.
#include "engine/game_input.hpp"
#include "game/world_sim.hpp"

#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>

namespace joa::engine {

int joypad_index(SDL_GamepadButton b) {
    for (int n = 1; n <= 12; ++n)
        if (joypad_button(n) == b) return n;
    return 0;
}

UiPad PadUiTracker::update(const RawInput& raw, float dt, float deadzone) {
    UiPad p;
    p.connected = raw.pad_connected;
    const auto& b = raw.pad_buttons;
    const auto edge = [&](SDL_GamepadButton k) { return b[std::size_t(k)] && !prev_[std::size_t(k)]; };
    for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT && p.button < 0; ++k)
        if (edge(SDL_GamepadButton(k))) p.button = k;
    p.activate = edge(SDL_GAMEPAD_BUTTON_SOUTH);
    p.back = edge(SDL_GAMEPAD_BUTTON_EAST) || edge(SDL_GAMEPAD_BUTTON_START);
    p.page = edge(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) - edge(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    prev_ = b;

    // Direction: D-pad, else the left stick past half tilt (and the deadzone); vertical wins on diagonals.
    const float lx = raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTX], ly = raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY];
    const float t = std::max(0.5f, deadzone);
    glm::ivec2 d{int(b[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]) - int(b[SDL_GAMEPAD_BUTTON_DPAD_LEFT]),
                 int(b[SDL_GAMEPAD_BUTTON_DPAD_DOWN]) - int(b[SDL_GAMEPAD_BUTTON_DPAD_UP])};
    if (d == glm::ivec2(0) && std::max(std::abs(lx), std::abs(ly)) > t)
        d = std::abs(ly) >= std::abs(lx) ? glm::ivec2(0, ly > 0 ? 1 : -1) : glm::ivec2(lx > 0 ? 1 : -1, 0);
    if (d.y) d.x = 0;
    if (d != dir_) {
        dir_ = d, repeat_ = 0.4f;
        p.dir = d;
    } else if (d != glm::ivec2(0) && (repeat_ -= dt) <= 0.0f) {
        repeat_ += 0.1f;
        p.dir = d;
    }
    const bool stick = std::max(std::abs(lx), std::abs(ly)) > t;
    p.used = p.button >= 0 || p.dir != glm::ivec2(0) || (stick && !stick_);
    stick_ = stick;
    return p;
}

void PadPointer::update(const RawInput& raw, float dt, glm::ivec4 vp, const PadSettings& s) {
    if (raw.mouse_dx != 0.0f || raw.mouse_dy != 0.0f) active = false;  // the mouse took over
    float x = raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTX], y = raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTY];
    const float m = std::hypot(x, y);
    const bool buttons = raw.pad_buttons[SDL_GAMEPAD_BUTTON_SOUTH] || raw.pad_buttons[SDL_GAMEPAD_BUTTON_WEST];
    if (m > s.deadzone || buttons) active = true;
    const glm::vec2 lo{float(vp.x), float(vp.y)}, hi = lo + glm::vec2(float(std::max(vp.z, 1) - 1), float(std::max(vp.w, 1) - 1));
    if (pos.x < 0.0f) pos = (lo + hi) * 0.5f;
    if (m > s.deadzone) {
        // Rescaled past the deadzone, squared for fine aim near the centre.
        const float r = std::min(1.0f, (m - s.deadzone) / std::max(1e-6f, 1.0f - s.deadzone));
        pos += glm::vec2(x, y) / m * (r * r * 0.9f * float(vp.w) * s.sensitivity * dt);
    }
    pos = glm::clamp(pos, lo, hi);
}

void PadPointer::apply(RawInput& raw) const {
    if (!active) return;
    if (raw.pad_buttons[SDL_GAMEPAD_BUTTON_SOUTH]) raw.mouse_buttons.set(SDL_BUTTON_LEFT);
    if (raw.pad_buttons[SDL_GAMEPAD_BUTTON_WEST]) raw.mouse_buttons.set(SDL_BUTTON_RIGHT);
}

PadRumble pad_rumble(const std::vector<game::GameEvent>& events, int player) {
    PadRumble r;
    const auto add = [&](float low, float high, int ms) { r.low = std::max(r.low, low), r.high = std::max(r.high, high), r.ms = std::max(r.ms, ms); };
    for (const game::GameEvent& e : events) {
        if (e.type == game::Ev::Hit && e.unit == player) add(std::min(1.0f, 0.4f + float(e.value) / 100.0f), 0.3f, 200);  // taken
        else if (e.type == game::Ev::Knockdown && e.unit == player) add(1.0f, 0.6f, 350);
        else if (e.type == game::Ev::Hit && e.other == player) add(0.0f, 0.35f, 80);  // landed
    }
    return r;
}

}  // namespace joa::engine
