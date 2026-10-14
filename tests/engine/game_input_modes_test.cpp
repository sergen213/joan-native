// Input methods other than keyboard & mouse: the joystick stick reading (Joy_stick_level 0x433930, Joy_stick_angle
// 0x433af0, digital direction 0x598040) fed into InputFrame::joy_*, arrow keys as char events (Key_match_char
// 0x599240, keyboard-only double taps) and Options -> Input Method in a running mission (0x73ca80).
#include "check.hpp"
#include "engine/game_input.hpp"
#include "engine/menu_options_store.hpp"
#include "engine/session.hpp"
#include "game/mission.hpp"

#include <cstdlib>
#include <string>

using namespace joa;
using namespace joa::engine;

int main() {
    {   // Stick levels: |axis| * 16 / 65535 -> < 2: 0, 2: 1, 3..4: 2, >= 5: 3.
        CHECK(joy_stick(0, 0).level == 0 && joy_stick(0, 0).angle == -1.0f && joy_stick(0, 0).move_code == 0);
        CHECK(joy_stick(0.12f, 0).level == 0);
        CHECK(joy_stick(0.25f, 0).level == 1);
        CHECK(joy_stick(0.4f, 0).level == 2);
        CHECK(joy_stick(0.7f, 0).level == 3 && joy_stick(0, -1).level == 3);
        // Angle: CCW from right, 90 = up (y down negative), 1.40625 deg steps; move code past 13 % of the half range.
        const JoyStick up = joy_stick(0, -1), right = joy_stick(1, 0), left = joy_stick(-1, 0), down = joy_stick(0, 1);
        CHECK(up.angle == 90.0f && up.move_code == 0x25);
        CHECK(down.angle == 270.0f && down.move_code == 0x14);
        CHECK(right.angle == 0.0f && right.move_code == 0x22);
        CHECK(left.angle == 180.0f && left.move_code == 0x1c);
        const JoyStick ur = joy_stick(0.7071f, -0.7071f), dl = joy_stick(-0.7071f, 0.7071f);
        CHECK(ur.angle == 45.0f && ur.move_code == 0xb);
        CHECK(dl.angle == 225.0f && dl.move_code == 0xc);
        CHECK(joy_stick(0.3f, -0.05f).angle == 8.4375f);   // atan2 * 256 / 2pi = 6.73 -> byte 6 (ftol truncates)
        CHECK(joy_stick(0.1f, 0).move_code == 0 && joy_stick(0.2f, 0).move_code == 0x22);
        CHECK(joy_stick(0.1f, 0).level == 0 && joy_stick(0.1f, 0).angle == 0.0f);   // tilted but level 0
    }
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !formats::Vfs(game).exists("Resource/STD.SET")) return g_failures ? 1 : 77;
    const formats::Vfs vfs(game);
    const std::vector<KeyBindingRow>& def = default_bindings(vfs);
    {   // Gamepad/Joystick (input method 2 -> scheme 1): left stick -> joy fields, pad buttons by J_JOYPAD.
        GameOptions o;
        o.input_method = 2;
        GameInput in;
        in.configure(def, o, {}, false);
        RawInput raw;
        raw.pad_connected = true;
        raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -1.0f;
        raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_SOUTH);   // J_JOYPAD 1 = Jump
        in.feed(raw, 1.0f / 30);
        game::InputFrame f = in.take();
        CHECK(f.joy_present && f.joy_level == 3 && f.joy_angle == 90.0f && f.joy_move_code == 0x25);
        CHECK(f.held[game::IN_JUMP]);
        raw.pad_connected = false;
        in.feed(raw, 1.0f / 30);
        CHECK(!in.take().joy_present);
    }
    {   // Keyboard-only: arrow keys are key events with a char (0x4800-style codes), so Up-Up double taps work.
        GameOptions o;
        o.input_method = 0;
        GameInput in;
        in.configure(def, o, {}, false);
        RawInput raw;
        raw.keys.set(SDL_SCANCODE_UP);
        in.feed(raw, 1.0f / 30);
        const game::InputFrame f = in.take();
        CHECK(f.pressed[game::IN_FORWARD] && f.key_char_event);
    }
    {   // Options -> Input Method while playing: outside RTS the controller mode switches (old mode -> +0x14); in
        // RTS only the remembered mode changes and leaving RTS restores it.
        Session s(vfs);
        s.seed = 1;
        std::string err;
        if (s.new_game(1, 1, &err)) {
            game::World& w = s.mission()->world();
            game::PlayerController& pc = w.controller();
            CHECK(w.game_mode == 3);
            s.set_control_mode(0);
            CHECK(w.game_mode == 0 && pc.prev_mode == 3 && s.control_mode == 0);
            s.rts_unlocked = true;
            CHECK(s.toggle_rts());
            CHECK(w.game_mode == 4 && pc.prev_mode == 0);
            s.set_control_mode(1);
            CHECK(w.game_mode == 4 && pc.prev_mode == 1);
            CHECK(s.toggle_rts());
            CHECK(w.game_mode == 1);
        } else {
            std::fprintf(stderr, "new_game: %s (session part skipped)\n", err.c_str());
        }
    }
    return test_result();
}
