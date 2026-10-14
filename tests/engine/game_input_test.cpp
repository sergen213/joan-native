// GameInput: DEFAULTK bindings -> InputFrame bits, one-event-per-tick queue, mouse yaw remainder, gamepad.
#include "check.hpp"
#include "engine/game_input.hpp"
#include "engine/menu_options_store.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>

#include <cstdlib>

using namespace joa::engine;
using namespace joa::game;

namespace {

// Controller yaw step 0x431790 (controller_player.cpp): int(mouse_dx * ((s+1)*0.7+0.2) * 0.2).
int game_yaw(const InputFrame& f) { return int(f.mouse_dx * ((f.mouse_sensitivity + 1.0f) * 0.7f + 0.2f) * 0.2f); }

struct Rig {
    GameInput in;
    RawInput raw;
    InputFrame tick() { in.feed(raw, 1.0f / 60), in.feed(raw, 1.0f / 60); return in.take(); }  // two frames per tick
    InputFrame press(SDL_Scancode s) {  // key down for a tick, then released
        raw.keys.set(s);
        InputFrame f = tick();
        raw.keys.reset(s);
        return f;
    }
};

}  // namespace

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/STD.SET")) return 77;
    const joa::formats::Vfs vfs(game);
    const std::vector<KeyBindingRow>& def = default_bindings(vfs);
    CHECK(def.size() == 55);
    GameOptions opt;  // input_method 1 = keyboard & mouse (scheme 3)
    Rig r;
    r.in.configure(def, opt, {}, false);

    // Held bits of the FPS defaults.
    const std::pair<SDL_Scancode, int> keys[] = {{SDL_SCANCODE_W, IN_FORWARD},    {SDL_SCANCODE_S, IN_BACKWARD}, {SDL_SCANCODE_A, IN_STRAFE_LEFT},
                                                 {SDL_SCANCODE_D, IN_STRAFE_RIGHT}, {SDL_SCANCODE_LSHIFT, IN_WALK}, {SDL_SCANCODE_Q, IN_DEFEND},
                                                 {SDL_SCANCODE_C, IN_ENERGY},     {SDL_SCANCODE_SPACE, IN_JUMP}, {SDL_SCANCODE_F2, IN_RTS_TOGGLE},
                                                 {SDL_SCANCODE_F5, 53},           {SDL_SCANCODE_F6, 54}};
    for (const auto& [sc, a] : keys) {
        r.raw.keys.set(sc);
        const InputFrame f = r.tick();
        CHECK(f.held[std::size_t(a)] && f.pressed[std::size_t(a)] && f.held.count() == 1);
        CHECK(r.in.held(a));
        r.raw.keys.reset(sc);
        r.tick();  // key-up event
    }
    CHECK(r.tick().held.none());
    // Mouse buttons: LMB light, RMB heavy.
    r.raw.mouse_buttons.set(SDL_BUTTON_LEFT);
    InputFrame f = r.tick();
    CHECK(f.held[IN_ATTACK_A] && f.pressed[IN_ATTACK_A] && !f.held[IN_ATTACK_B]);
    r.raw.mouse_buttons.reset(SDL_BUTTON_LEFT), r.raw.mouse_buttons.set(SDL_BUTTON_RIGHT);
    r.tick();  // LMB up
    f = r.tick();
    CHECK(f.held[IN_ATTACK_B] && f.pressed[IN_ATTACK_B] && !f.held[IN_ATTACK_A]);
    r.raw.mouse_buttons.reset();
    r.tick();
    // Frame-level edges (menu / quick-save ids) follow the last feed only.
    r.raw.keys.set(SDL_SCANCODE_F5);
    r.in.feed(r.raw, 0.016f);
    CHECK(r.in.pressed(53) && r.in.held(53));
    r.in.feed(r.raw, 0.016f);
    CHECK(!r.in.pressed(53) && r.in.held(53));
    r.raw.keys.reset();
    r.in.feed(r.raw, 0.016f);
    r.in.reset();

    // Tap between two takes: exactly one pressed edge (held on that tick so jump/attack phases see it).
    r.raw.keys.set(SDL_SCANCODE_SPACE);
    r.in.feed(r.raw, 0.016f);
    r.raw.keys.reset();
    r.in.feed(r.raw, 0.016f);
    f = r.in.take();
    CHECK(f.pressed[IN_JUMP] && f.held[IN_JUMP] && f.pressed.count() == 1 && f.key_char_event);
    f = r.in.take();  // the key-up event
    CHECK(f.pressed.none() && f.held.none());
    // Original pops one event per frame: two keys going down together are reported on consecutive ticks.
    r.raw.keys.set(SDL_SCANCODE_W), r.raw.keys.set(SDL_SCANCODE_R);
    r.in.feed(r.raw, 0.016f);
    InputFrame f1 = r.in.take(), f2 = r.in.take(), f3 = r.in.take();
    CHECK(f1.pressed.count() == 1 && f2.pressed.count() == 1 && f3.pressed.none());
    CHECK((f1.pressed | f2.pressed)[IN_FORWARD] && (f1.pressed | f2.pressed)[IN_INTERACT]);
    CHECK(f3.held[IN_FORWARD] && f3.held[IN_INTERACT]);
    r.raw.keys.reset();
    r.in.feed(r.raw, 0.016f);
    // Mouse wheel between ticks: zoom in (48) / out (49) on the next tick, wheel() = last feed.
    r.raw.wheel = 1;
    r.in.feed(r.raw, 0.016f);
    r.raw.wheel = 0;
    r.in.feed(r.raw, 0.016f);
    CHECK(r.in.wheel() == 0);
    f = r.in.take();
    CHECK(f.pressed[48] && f.held[48] && !f.pressed[49]);
    r.raw.wheel = -2;
    f = r.tick();
    r.raw.wheel = 0;
    CHECK(f.pressed[49] && !f.pressed[48] && r.in.wheel() == -2);
    CHECK(r.tick().pressed.none());
    r.in.reset();
    // Modifiers are not queued: Shift edge comes with the next tick, without a char event.
    f = r.press(SDL_SCANCODE_LSHIFT);
    CHECK(f.pressed[IN_WALK] && !f.key_char_event);
    // Keyboard-only scheme 0: K_KEY column (Up = forward, Q = light attack, W = heavy).
    {
        GameOptions k = opt;
        k.input_method = 0;
        Rig q;
        q.in.configure(def, k, {}, false);
        CHECK(q.press(SDL_SCANCODE_UP).held[IN_FORWARD]);
        q.tick();
        const InputFrame w = q.press(SDL_SCANCODE_W);
        CHECK(w.held[IN_ATTACK_B] && !w.held[IN_FORWARD]);
    }

    // Remap: Jump := J (DIK 0x24) in scheme 3.
    {
        std::vector<KeyBindingRow> rows = def;
        CHECK(remap_assign(vfs, rows, 3, IN_JUMP, 0, 0x24));
        Rig q;
        q.in.configure(rows, opt, {}, false);
        CHECK(q.press(SDL_SCANCODE_J).held[IN_JUMP]);
        q.tick();
        CHECK(!q.press(SDL_SCANCODE_SPACE).held[IN_JUMP]);
    }

    // Slow mouse: 1 count per tick for 60 ticks -> the game still turns, total within 1° of the float yaw.
    for (const bool fix : {false, true}) {
        Rig q;
        q.in.configure(def, opt, {}, fix);
        int total = 0;
        for (int i = 0; i < 60; ++i) {
            q.raw.mouse_dx = 1;
            q.in.feed(q.raw, 0.033f);
            total += game_yaw(q.in.take());
        }
        const float expect = 60 * 0.18f * (fix ? 5 : 1);  // k = 0.9*0.2 = 0.18°/count at sensitivity 0
        CHECK(total != 0 && std::fabs(float(total) - expect) <= 1.0f);
        // Large and negative motion, non-default sensitivity: per-tick exact truncation.
        GameOptions o = opt;
        o.mouse_sensitivity = 0.4f;
        q.in.configure(def, o, {}, fix);
        q.raw.mouse_dx = -137;
        q.in.feed(q.raw, 0.033f);
        const float kk = ((0.4f + 1) * 0.7f + 0.2f) * (fix ? 1.0f : 0.2f);
        CHECK(game_yaw(q.in.take()) == int(-137 * kk));
    }

    // Gamepad: positional buttons (Xbox A / PlayStation Cross are both SOUTH).
    {
        Rig q;
        q.in.configure(def, opt, {0.2f, 1.0f}, false);
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.15f;  // inside the deadzone
        CHECK(q.tick().held.none());
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -1.0f;
        f = q.tick();
        CHECK(f.held[IN_FORWARD] && f.pressed[IN_FORWARD] && !f.held[IN_WALK] && f.held.count() == 1);
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.45f;  // partial tilt walks
        CHECK(q.tick().held[IN_WALK]);
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = 0;
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTX] = 1;
        CHECK(q.tick().held[IN_STRAFE_RIGHT]);
        q.raw.pad_axes = {};
        CHECK(SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_XBOXONE, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_A);
        CHECK(SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_CROSS);
        CHECK(joypad_button(def[IN_JUMP - 1].code[2]) == SDL_GAMEPAD_BUTTON_SOUTH);
        q.raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_SOUTH);
        f = q.tick();
        CHECK(f.held[IN_JUMP] && f.pressed[IN_JUMP]);
        q.raw.pad_buttons.reset();
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 1;
        CHECK(q.tick().held[IN_ATTACK_A]);
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0, q.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 1;
        CHECK(q.tick().held[IN_ATTACK_B]);
        q.raw.pad_axes = {};
        q.raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_START);
        q.in.feed(q.raw, 0.016f);
        CHECK(q.in.pressed(34));
        q.raw.pad_buttons.reset(), q.raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_BACK);
        CHECK(q.tick().pressed[IN_RTS_TOGGLE]);
        q.raw.pad_buttons.reset();
        q.tick();
        // Right stick X turns like the mouse: 150°/s at full deflection.
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTX] = 1;
        int total = 0;
        for (int i = 0; i < 30; ++i) {
            q.in.feed(q.raw, 1.0f / 30);
            total += game_yaw(q.in.take());
        }
        CHECK(std::abs(total - 150) <= 1);
        q.raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTX] = 0, q.raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTY] = 1;
        q.in.feed(q.raw, 0.1f);
        CHECK(q.in.take().mouse_dy > 0);
    }
    return test_result();
}
