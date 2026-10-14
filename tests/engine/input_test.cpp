// Action map evaluation, dead zones, sensitivity and remapping (no SDL init or devices needed).
#include "check.hpp"
#include "engine/input.hpp"

#include <SDL3/SDL_mouse.h>

using namespace joa::engine;

static ActionState eval(const InputMap& m, const RawInput& raw, float dt = 1.0f / 60.0f) {
    ActionState s;
    m.evaluate(raw, s, dt);
    return s;
}

int main() {
    const InputMap m = InputMap::defaults();

    {  // keys -> signed axes, opposing keys cancel, edges
        RawInput raw;
        raw.keys[SDL_SCANCODE_W] = true;
        raw.keys[SDL_SCANCODE_A] = true;
        ActionState s;
        m.evaluate(raw, s, 0.016f);
        CHECK_NEAR(s[Action::MoveY], 1.0f, 0.0f);
        CHECK_NEAR(s[Action::MoveX], -1.0f, 0.0f);
        raw.keys[SDL_SCANCODE_S] = true;
        raw.keys[SDL_SCANCODE_SPACE] = true;
        m.evaluate(raw, s, 0.016f);
        CHECK_NEAR(s[Action::MoveY], 0.0f, 0.0f);
        CHECK(s.pressed(Action::Jump));
        m.evaluate(raw, s, 0.016f);
        CHECK(s.down(Action::Jump) && !s.pressed(Action::Jump));
        raw.keys[SDL_SCANCODE_W] = raw.keys[SDL_SCANCODE_UP] = true;  // two bindings, clamped
        raw.keys[SDL_SCANCODE_S] = false;
        m.evaluate(raw, s, 0.016f);
        CHECK_NEAR(s[Action::MoveY], 1.0f, 0.0f);
    }
    {  // radial stick dead zone (0.2) with rescale; SDL stick-up is negative
        RawInput raw;
        raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.15f;
        CHECK_NEAR(eval(m, raw)[Action::MoveX], 0.0f, 0.0f);
        raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.0f;
        raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.6f;
        CHECK_NEAR(eval(m, raw)[Action::MoveY], 0.5f, 1e-6);
        raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -1.0f;
        CHECK_NEAR(eval(m, raw)[Action::MoveY], 1.0f, 1e-6);
        raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.15f;  // inside per-axis zone, outside radial: kept
        raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = 0.15f;
        CHECK(eval(m, raw)[Action::MoveX] > 0.0f);
        raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.05f;
        CHECK(!eval(m, raw).down(Action::Attack));
        raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 1.0f;
        CHECK(eval(m, raw).down(Action::Attack));
    }
    {  // look: mouse only while captured, scaled by sensitivity; stick is a rate * dt; invert
        RawInput raw;
        raw.mouse_dx = 100.0f;
        CHECK_NEAR(eval(m, raw)[Action::LookX], 0.0f, 0.0f);
        raw.mouse_look = true;
        CHECK_NEAR(eval(m, raw)[Action::LookX], 100.0f * m.mouse_sensitivity, 1e-6);
        raw.mouse_dx = 0.0f;
        raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTX] = 1.0f;
        CHECK_NEAR(eval(m, raw, 0.5f)[Action::LookX], m.pad_look_speed * 0.5f, 1e-5);
        raw.end_frame();
        CHECK_NEAR(raw.mouse_dx, 0.0f, 0.0f);
        InputMap inv = m;
        inv.invert_y = true;
        RawInput r2;
        r2.mouse_look = true;
        r2.mouse_dy = 10.0f;
        CHECK_NEAR(eval(inv, r2)[Action::LookY], -eval(m, r2)[Action::LookY], 1e-7);
        CHECK(eval(m, r2)[Action::LookY] > 0.0f);
    }
    {  // remap: replaces only mentioned actions, tunables via "set"
        InputMap r = m;
        std::string err;
        CHECK(r.parse("# custom\nJump key:J\nJump pad:y\nset stick_deadzone 0.3\nset invert_y 1\n", &err));
        CHECK(err.empty());
        CHECK_NEAR(r.stick_deadzone, 0.3f, 1e-7);
        CHECK(r.invert_y);
        RawInput raw;
        raw.keys[SDL_SCANCODE_SPACE] = true;
        CHECK(!eval(r, raw).down(Action::Jump));
        raw.keys[SDL_SCANCODE_J] = true;
        CHECK(eval(r, raw).down(Action::Jump));
        RawInput pad;
        pad.pad_buttons[SDL_GAMEPAD_BUTTON_NORTH] = true;
        CHECK(eval(r, pad).down(Action::Jump));
        raw = {};
        raw.keys[SDL_SCANCODE_W] = true;  // untouched action keeps its default
        CHECK_NEAR(eval(r, raw)[Action::MoveY], 1.0f, 0.0f);
        r.clear(Action::MoveY);
        CHECK_NEAR(eval(r, raw)[Action::MoveY], 0.0f, 0.0f);
    }
    {  // malformed input leaves the map untouched
        InputMap r = m;
        std::string err;
        CHECK(!r.parse("Jump key:NotAKey\n", &err));
        CHECK(!err.empty());
        CHECK(!r.parse("Fly key:F\n", &err));
        CHECK(!r.parse("Jump mouse:9\n", &err));
        CHECK(r.bindings().size() == m.bindings().size());
    }
    {  // serialize -> parse round trip reproduces every binding
        InputMap r;
        std::string err;
        CHECK(r.parse(m.serialize(), &err));
        CHECK(r.bindings().size() == m.bindings().size());
        for (std::size_t i = 0; i < r.bindings().size() && i < m.bindings().size(); ++i) {
            const auto& [a1, b1] = r.bindings()[i];
            const auto& [a2, b2] = m.bindings()[i];
            CHECK(a1 == a2 && b1.source == b2.source && b1.code == b2.code && b1.scale == b2.scale);
        }
        CHECK(r.serialize() == m.serialize());
    }
    return test_result();
}
