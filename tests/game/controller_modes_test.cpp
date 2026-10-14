// Non-default controller modes (World::game_mode -> PlayerController::mode): keyboard-only 0x4343e0 and joystick
// 0x432d30 (GAMEPLAY_CHARACTER_COMBAT.md Part B §3.3-§3.7) with Joan's move table. One frame = one 33 ms tick.
#include "sim_fixture.hpp"

using namespace joa::game;

namespace {
struct Rig {
    FlatTerrain flat{0};
    World w;
    int joan;
    Rig(GameData& gd, int mode) : w(gd, &flat, 0) {
        joan = w.spawn_unit("JOAN_OF_ARC", gd.nation_id("FRENCH"), {0, 0, 0}, 0);
        w.set_player(joan);
        w.game_mode = mode;
        idle(99);
    }
    Unit& J() { return *w.unit(joan); }
    void tick(const InputFrame& f) { w.tick(f); }
    void idle(int ms) { for (int t = 0; t < ms; t += World::kTickMs) tick(InputFrame{}); }
    // Keyboard frame: `held` actions held, `tap` (0 = none) pressed by a key event with a char.
    InputFrame key(std::initializer_list<int> held, int tap = 0) {
        InputFrame f;
        for (int a : held) f.held.set(size_t(a));
        if (tap) f.press(tap), f.key_char_event = true;
        return f;
    }
    InputFrame stick(int level, float angle, int code) {
        InputFrame f;
        f.joy_present = true, f.joy_level = level, f.joy_angle = angle, f.joy_move_code = code;
        return f;
    }
};
}  // namespace

int main() {
    auto gd = load_data_or_skip();
    // --- keyboard-only (mode 0) ---
    {   // Right = turn to face camera-right and run (no strafe); camera 0 follows at 0.002 deg/ms per degree.
        Rig r(*gd, 0);
        CHECK(r.w.controller().mode == 0, "mode %d", r.w.controller().mode);
        r.tick(r.key({IN_TURN_RIGHT}));
        CHECK(r.J().action == RUN, "Right: %s", action_name(r.J().action));
        CHECK(NEAR(r.J().yaw_goal, 270, 1e-3), "Right heading %.2f", r.J().yaw_goal);   // (0 + 360 - 90) % 360
        CHECK(NEAR(r.w.controller().camera_yaw_out, -33 * 0.002 * 90, 1e-3), "camera %.3f", r.w.controller().camera_yaw_out);
        r.tick(r.key({IN_TURN_LEFT}));
        CHECK(r.J().action == RUN && NEAR(r.J().yaw_goal, 90, 1e-3), "Left: %s %.2f", action_name(r.J().action), r.J().yaw_goal);
        r.tick(r.key({IN_BACKWARD}));   // Down runs toward the camera, the camera does not turn
        CHECK(r.J().action == RUN && NEAR(r.J().yaw_goal, 180, 1e-3) && r.w.controller().camera_yaw_out == 0.0f,
              "Down: %s %.2f cam %.3f", action_name(r.J().action), r.J().yaw_goal, r.w.controller().camera_yaw_out);
        r.tick(InputFrame{});
        CHECK(r.J().action != RUN, "release: %s", action_name(r.J().action));
    }
    {   // Shift + Right = strafe (MOVE_R) and camera centring held; Shift + Up walks.
        Rig r(*gd, 0);
        r.tick(r.key({IN_SHIFT, IN_TURN_RIGHT}));
        CHECK(r.J().action == MOVE_R, "Shift+Right: %s", action_name(r.J().action));
        CHECK(r.w.controller().cam_center == 2, "centre %d", r.w.controller().cam_center);
        r.tick(r.key({IN_SHIFT, IN_FORWARD}));
        CHECK(r.J().action == WALK, "Shift+Up: %s", action_name(r.J().action));
        r.tick(InputFrame{});
        CHECK(r.w.controller().cam_center == 0, "centre after release %d", r.w.controller().cam_center);
    }
    {   // Up-Up double tap = rush (fallback path with A, as the KBM W-W+A test: RUSH not learned at level 1).
        Rig r(*gd, 0);
        r.tick(r.key({}, IN_FORWARD));
        r.tick(InputFrame{});
        InputFrame f = r.key({}, IN_FORWARD);
        f.press(IN_ATTACK_A);
        r.tick(f);
        CHECK(r.J().action == RUSH && r.J().ep == 25, "Up Up+Q: %s ep %d", action_name(r.J().action), r.J().ep);
    }
    {   // Shift+Down-Down = JUMP_B; Down-Down without Shift is no tap.
        Rig r(*gd, 0);
        r.tick(r.key({IN_SHIFT}, IN_BACKWARD));
        r.tick(r.key({IN_SHIFT}));
        r.tick(r.key({IN_SHIFT}, IN_BACKWARD));
        CHECK(r.J().action == JUMP_B, "Shift+Down Down: %s", action_name(r.J().action));
        Rig q(*gd, 0);
        q.tick(q.key({}, IN_BACKWARD));
        q.tick(InputFrame{});
        q.tick(q.key({}, IN_BACKWARD));
        CHECK(q.J().action != JUMP_B, "Down Down: %s", action_name(q.J().action));
    }
    {   // Jump never fires in keyboard-only mode (asm 0x434a7b: sum of 0x80 key states == 1); KBM jumps.
        for (int mode : {0, 3}) {
            Rig r(*gd, mode);
            bool up = false;
            for (int held : {0, 1, 2}) {   // no direction, Up, Up+Right
                for (int i = 0; i < 6; ++i) {
                    InputFrame f = r.key({IN_JUMP});
                    if (held >= 1) f.held.set(IN_FORWARD);
                    if (held == 2) f.held.set(IN_TURN_RIGHT);
                    r.tick(f);
                    up = up || r.J().action == UP || r.J().posture == 1;
                }
                r.idle(2000);
            }
            CHECK(up == (mode == 3), "mode %d jumped %d", mode, int(up));
        }
    }
    // --- joystick (mode 1) ---
    {   // No pad: nothing happens.
        Rig r(*gd, 1);
        InputFrame f = r.stick(3, 90, 0x25);
        f.joy_present = false;
        r.tick(f);
        CHECK(r.J().action != RUN, "no pad: %s", action_name(r.J().action));
    }
    {   // Stick level picks the state (Ctrl_forward_state 0x4350f0): 1..2 WALK, 3 RUN, 0 (inside the move deadzone) IDLE.
        Rig r(*gd, 1);
        r.tick(r.stick(1, 90, 0x25));
        CHECK(r.J().action == WALK, "level 1: %s", action_name(r.J().action));
        r.tick(r.stick(2, 90, 0x25));
        CHECK(r.J().action == WALK, "level 2: %s", action_name(r.J().action));
        r.tick(r.stick(3, 90, 0x25));
        CHECK(r.J().action == RUN, "level 3: %s", action_name(r.J().action));
        r.tick(r.stick(0, 90, 0x25));
        CHECK(r.J().action == IDLE, "level 0: %s", action_name(r.J().action));
        r.tick(r.stick(3, 180, 0x1c));   // stick left = camera-relative turn, like keyboard Left
        CHECK(r.J().action == RUN && NEAR(r.J().yaw_goal, 90, 1e-3), "left: %s %.2f", action_name(r.J().action), r.J().yaw_goal);
    }
    {   // Jump push = stick level (Ctrl_jump magnitude): horizontal speed 0 at level 0, x3 between levels 1 and 3.
        float speed[4] = {};
        for (int level : {0, 1, 3}) {
            Rig r(*gd, 1);
            InputFrame hold = r.stick(level, 90, level ? 0x25 : 0);
            hold.held.set(IN_JUMP);
            r.tick(hold);
            r.tick(r.stick(level, 90, level ? 0x25 : 0));   // released after 33 ms: fires
            CHECK(r.J().action == UP, "level %d jump: %s", level, action_name(r.J().action));
            speed[level] = std::hypot(r.J().vel.x, r.J().vel.y);
        }
        CHECK(speed[0] < 1e-3f && speed[1] > 1.0f && NEAR(speed[3], 3 * speed[1], 0.05 * speed[1]), "speeds %.2f %.2f %.2f", speed[0],
              speed[1], speed[3]);
    }
    {   // Stick flick up, back below level 2, flick up + A within 300 ms = rush (fallback with A).
        Rig r(*gd, 1);
        r.tick(r.stick(3, 90, 0x25));
        r.tick(r.stick(0, -1, 0));
        CHECK(r.w.controller().dtap_armed == 1, "armed %d", r.w.controller().dtap_armed);
        InputFrame f = r.stick(3, 88.59375f, 0x25);
        f.press(IN_ATTACK_A);
        r.tick(f);
        CHECK(r.J().action == RUSH, "flick flick+A: %s", action_name(r.J().action));
    }
    {   // Mode 3 is untouched by the joystick fields.
        Rig r(*gd, 3);
        r.tick(r.stick(3, 90, 0x25));
        CHECK(r.J().action != RUN, "KBM ignores the stick fields: %s", action_name(r.J().action));
    }
    return finish();
}
