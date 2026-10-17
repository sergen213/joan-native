// Gamepad play without a device or GPU: pad focus navigation through the real menus (UiRunner + NullRenderer),
// activation commands, slider/combo adjust, the remap page's joystick capture, the Back layer / deadzone of
// GameInput, the UI direction repeat, the RTS virtual cursor, rumble and the autoplay pad verbs.
#include "check.hpp"
#include "engine/game_input.hpp"
#include "engine/menu_options_store.hpp"
#include "engine/session_autoplay.hpp"
#include "engine/ui_runner.hpp"
#include "game/world_sim.hpp"

#include <SDL3/SDL_mouse.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace joa::engine;
using namespace joa::game;

namespace {
struct NullRenderer final : IRenderer {
    const char* name() const override { return "null"; }
    MeshHandle create_mesh(const MeshData&) override { return {1}; }
    void destroy_mesh(MeshHandle) override {}
    TextureHandle create_texture(const TextureData&) override { return {1}; }
    void destroy_texture(TextureHandle) override {}
    void begin_frame(glm::ivec2) override {}
    void submit(const FrameData&) override {}
    void end_frame() override {}
    Image read_back() override { return {}; }
};

constexpr float kDt = 1.0f / 30.0f;

struct Drive {
    UiRunner& u;
    std::vector<UiQuad> q;
    void frame(UiPad p = {}, glm::vec2 mouse = {-1, -1}, bool click = false) {
        UiInput in;
        in.mouse = mouse;
        in.mouse_released = click;
        in.pad = p;
        q.clear();
        u.frame({0, 0, 1024, 768}, in, kDt, q);
    }
    void idle(int n) {
        for (int i = 0; i < n; ++i) frame();
    }
    void dir(int x, int y) {
        UiPad p;
        p.used = true, p.dir = {x, y};
        frame(p);
    }
    void button(SDL_GamepadButton b, bool connected = false) {
        UiPad p;
        p.used = true, p.button = b, p.connected = connected;
        p.activate = b == SDL_GAMEPAD_BUTTON_SOUTH;
        p.back = b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START;
        p.page = (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) - (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        frame(p);
    }
    template <class Done>
    bool until(Done done) {
        for (int i = 0; i < 300; ++i) {
            if (done()) return true;
            frame();
        }
        return done();
    }
};

int count(const UiRunner& u, UiCommand::Type t) {
    int n = 0;
    for (const UiCommand& c : u.commands) n += c.type == t;
    return n;
}

struct Rig {
    GameInput in;
    RawInput raw;
    InputFrame tick() { in.feed(raw, 1.0f / 60), in.feed(raw, 1.0f / 60); return in.take(); }
};
}  // namespace

int main() {
    // The Options page saves options.ini (OptionsChanged): a fresh private config dir, never the user's, so the slider
    // starts from the defaults on every run instead of creeping up to the 100 clamp.
    const char* out = std::getenv("JOA_TEST_OUT");
    const std::filesystem::path xdg = std::filesystem::path(out ? out : ".") / "gamepad-xdg";
    std::filesystem::remove_all(xdg);
    setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
    // ---- GameInput: Back layer, Back tap, deadzone (no game data: empty DEFAULTK) ----
    {
        Rig r;
        PadSettings pad;
        pad.deadzone = 0.3f;
        r.in.configure({}, GameOptions{}, pad, false);
        const std::pair<SDL_GamepadButton, int> layer[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, 24}, {SDL_GAMEPAD_BUTTON_EAST, 43},          {SDL_GAMEPAD_BUTTON_WEST, 15},
            {SDL_GAMEPAD_BUTTON_NORTH, 46}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 53}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 54},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, 48}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 49},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 50},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 51}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, 45}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 44},
            {SDL_GAMEPAD_BUTTON_START, 40}};
        for (const auto& [b, a] : layer) {
            r.raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_BACK);
            r.tick();
            r.raw.pad_buttons.set(b);
            const InputFrame f = r.tick();
            CHECK(f.pressed[std::size_t(a)] && !f.pressed[34] && !f.pressed[IN_RTS_TOGGLE]);
            r.raw.pad_buttons.reset();
            CHECK(!r.tick().pressed[IN_RTS_TOGGLE]);  // a chord is not a tap
        }
        for (const auto& [axis, a] : {std::pair{SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 55}, std::pair{SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 41}}) {
            r.raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_BACK), r.raw.pad_axes[axis] = 1;
            const InputFrame f = r.tick();
            CHECK(f.pressed[std::size_t(a)] && !f.held[IN_ATTACK_A] && !f.held[IN_ATTACK_B]);
            r.raw.pad_buttons.reset(), r.raw.pad_axes = {};
            r.tick();
        }
        r.raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_BACK);
        CHECK(!r.tick().pressed[IN_RTS_TOGGLE]);
        r.raw.pad_buttons.reset();
        CHECK(r.tick().pressed[IN_RTS_TOGGLE]);
        CHECK(!r.tick().pressed[IN_RTS_TOGGLE]);  // once
        // Deadzone 0.3 from settings.ini: 0.25 tilt does nothing, 0.4 walks forward, full tilt runs.
        r.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.25f;
        CHECK(r.tick().held.none());
        r.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.4f;
        InputFrame f = r.tick();
        CHECK(f.held[IN_FORWARD] && f.held[IN_WALK]);
        r.raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTY] = -1.0f;
        f = r.tick();
        CHECK(f.held[IN_FORWARD] && !f.held[IN_WALK]);
        r.raw.pad_axes = {}, r.raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTX] = 0.25f;
        r.tick();
        CHECK(r.tick().mouse_dx == 0.0f);
        // Mouse Sensitivity +1 turns the right stick faster (k 1.6 / 0.9).
        GameOptions fast;
        fast.mouse_sensitivity = 1.0f;
        Rig s, n;
        s.in.configure({}, fast, {}, false), n.in.configure({}, GameOptions{}, {}, false);
        s.raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTX] = n.raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTX] = 1.0f;
        float ys = 0, yn = 0;
        for (int i = 0; i < 30; ++i) {
            const InputFrame a = s.tick(), b = n.tick();
            ys += a.mouse_dx * ((a.mouse_sensitivity + 1) * 0.7f + 0.2f) * 0.2f;
            yn += b.mouse_dx * ((b.mouse_sensitivity + 1) * 0.7f + 0.2f) * 0.2f;
        }
        CHECK(ys > yn * 1.6f);
    }

    // ---- PadUiTracker: edges and the D-pad / stick repeat ----
    {
        PadUiTracker t;
        RawInput raw;
        raw.pad_connected = true;
        CHECK(!t.update(raw, kDt, 0.15f).used);
        raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        UiPad p = t.update(raw, kDt, 0.15f);
        CHECK(p.used && p.dir == glm::ivec2(0, 1) && p.connected && p.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        int steps = 0;
        for (float s = 0; s < 0.38f; s += 0.02f) steps += t.update(raw, 0.02f, 0.15f).dir.y;
        CHECK(steps == 0);  // 400 ms before the repeat
        for (float s = 0; s < 0.5f; s += 0.02f) steps += t.update(raw, 0.02f, 0.15f).dir.y;
        CHECK(steps >= 5 && steps <= 6);  // then every 100 ms
        raw.pad_buttons.reset();
        t.update(raw, kDt, 0.15f);
        raw.pad_axes[SDL_GAMEPAD_AXIS_LEFTX] = -0.8f;
        CHECK(t.update(raw, kDt, 0.15f).dir == glm::ivec2(-1, 0));
        raw.pad_axes = {};
        raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_SOUTH);
        p = t.update(raw, kDt, 0.15f);
        CHECK(p.activate && !p.back);
        raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_EAST);
        p = t.update(raw, kDt, 0.15f);
        CHECK(!p.activate && p.back);
        raw.pad_buttons.reset(), raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        CHECK(t.update(raw, kDt, 0.15f).page == -1);
    }

    // ---- RTS virtual cursor, rumble, autoplay verbs ----
    {
        PadPointer pp;
        RawInput raw;
        const glm::ivec4 vp{0, 0, 1280, 720};
        pp.update(raw, kDt, vp, {});
        CHECK(!pp.active && pp.pos == glm::vec2(639.5f, 359.5f));
        raw.pad_axes[SDL_GAMEPAD_AXIS_RIGHTX] = 1;
        for (int i = 0; i < 15; ++i) pp.update(raw, kDt, vp, {});
        CHECK(pp.active);
        CHECK_NEAR(pp.pos.x, 639.5f + 0.9f * 720 * 0.5f, 1.0f);
        raw.pad_axes = {}, raw.pad_buttons.set(SDL_GAMEPAD_BUTTON_SOUTH);
        RawInput out = raw;
        pp.apply(out);
        CHECK(out.mouse_buttons[SDL_BUTTON_LEFT] && !out.mouse_buttons[SDL_BUTTON_RIGHT]);
        raw.mouse_dx = 3;  // the mouse takes over
        raw.pad_buttons.reset();
        pp.update(raw, kDt, vp, {});
        CHECK(!pp.active);

        std::vector<GameEvent> ev(3);
        ev[0].type = Ev::Hit, ev[0].unit = 7, ev[0].other = 1, ev[0].value = 30;
        ev[1].type = Ev::Hit, ev[1].unit = 9, ev[1].other = 1;
        CHECK(pad_rumble(ev, 1).ms == 80 && pad_rumble(ev, 1).low == 0.0f);  // landed hits: a short high-motor tick
        CHECK(pad_rumble(ev, 7).ms == 200 && pad_rumble(ev, 7).low > 0.6f);  // damage taken
        CHECK(pad_rumble(ev, 5).ms == 0);

        Autoplay a;
        std::string err;
        CHECK(!a.parse("3 pad nonsense down", &err));
        Autoplay b;
        CHECK(b.parse("1 pad south tap 3\n1 axis leftx -0.5\n2 pad dpup down\n6 pad dpup up\n", &err));
        RawInput r;
        b.apply(0, r);
        CHECK(!r.pad_connected && r.pad_buttons.none());
        b.apply(1, r);
        CHECK(r.pad_connected && r.pad_buttons[SDL_GAMEPAD_BUTTON_SOUTH] && r.pad_axes[SDL_GAMEPAD_AXIS_LEFTX] == -0.5f);
        b.apply(2, r);
        CHECK(r.pad_buttons[SDL_GAMEPAD_BUTTON_DPAD_UP]);
        b.apply(4, r);
        CHECK(!r.pad_buttons[SDL_GAMEPAD_BUTTON_SOUTH] && r.pad_buttons[SDL_GAMEPAD_BUTTON_DPAD_UP]);
        b.apply(6, r);
        CHECK(r.pad_buttons.none());
        Autoplay c;  // cmd: text2 is the rest of the line (save description with spaces)
        CHECK(c.parse("0 cmd Save 0 Test_S1L1_00_start.jsav TEST S1L1  start \n", &err));
        const Autoplay::FrameOut co = c.apply(0, r);
        CHECK(co.commands.size() == 1 && co.commands[0].text == "Test_S1L1_00_start.jsav" && co.commands[0].text2 == "TEST S1L1  start");
    }

    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP menu part: no original data\n");
        return test_result();
    }
    joa::formats::Vfs vfs(game);
    NullRenderer nr;

    // ---- Main menu -> New Campaign -> Normal by pad ----
    {
        UiRunner u(vfs, nr, "main");
        Drive d{u};
        d.idle(2);
        d.dir(0, 1);  // wakes the focus on New Campaign, no step
        d.dir(0, 1);  // Load a Campaign
        d.dir(0, -1); // New Campaign
        d.button(SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(u.menu.frontend_exit == 0);
        CHECK(d.until([&] { return u.menu.screen == MenuScreen::Difficulty; }));
        d.idle(40);  // buttons slide in
        d.dir(0, 1);  // Easy -> Normal
        d.button(SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(d.until([&] { return count(u, UiCommand::Type::NewGame) > 0; }));
        CHECK(count(u, UiCommand::Type::NewGame) == 1 && u.commands.back().value == 1);
    }
    {  // East = Escape: Difficulty CANCEL; then the mouse takes over again (last-used device wins).
        UiRunner u(vfs, nr, "difficulty");
        Drive d{u};
        d.idle(2);
        d.button(SDL_GAMEPAD_BUTTON_EAST);
        CHECK(u.menu.frontend_exit == 4);
        CHECK(d.until([&] { return u.menu.screen == MenuScreen::Main; }));
        d.idle(240);  // the sample's main_entry 0: full opening sequence
        d.frame({}, {258, 559});
        d.frame({}, {258, 560}, true);  // mouse click on Quit
        CHECK(u.menu.frontend_exit == 4);
    }

    // ---- Options: page by pad, slider adjust, East leaves the page ----
    {
        UiRunner u(vfs, nr, "options");
        Drive d{u};
        d.idle(2);
        d.dir(0, 1);  // wake (Video)
        d.dir(0, 1);  // Audio
        d.button(SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(u.menu.options_tab == OptionsTab::Audio);
        d.idle(60);
        const int music = u.menu.options.music_volume;
        d.dir(1, 0), d.dir(1, 0), d.dir(1, 0);
        CHECK(u.menu.options.music_volume == music + 3);
        d.dir(0, 1);  // Environmental Volume
        d.dir(-1, 0);
        CHECK(u.menu.options.music_volume == music + 3);
        d.button(SDL_GAMEPAD_BUTTON_EAST);
        CHECK(u.menu.options_tab == OptionsTab::Root && count(u, UiCommand::Type::OptionsChanged) == 1);
    }
    {  // Input page: Gamepad/Joystick is listed only with a pad connected.
        for (const bool connected : {false, true}) {
            UiRunner u(vfs, nr, "input");
            Drive d{u};
            d.idle(40);
            d.button(SDL_GAMEPAD_BUTTON_NORTH, connected);  // wake on row 0 (Input Method)
            d.frame();
            UiPad p;
            p.used = true, p.dir = {1, 0}, p.connected = connected;
            d.frame(p);
            CHECK(u.menu.options.input_method == (connected ? 2 : 3));  // Keyboard & Mouse -> next (Gamepad, else Point & Click)
        }
    }

    // ---- Remap: pad scheme shows the joypad column; capture binds a pad button ----
    {
        UiRunner u(vfs, nr, "remap");
        u.menu.options.input_method = 2;
        Drive d{u};
        d.idle(40);
        const KeyBindingRow* first = nullptr;  // the first listed Movement row (category 0)
        for (const KeyBindingRow& r : u.menu.bindings) {
            if (r.action < 1 || r.action > 10) continue;
            const std::string* n = binding_name(vfs, r, 2);
            if (!n || (*n != "Not Available" && *n != "Escape")) {
                first = &r;
                break;
            }
        }
        CHECK(first != nullptr);
        if (first) {
            const int action = first->action;
            d.button(SDL_GAMEPAD_BUTTON_NORTH);  // wake: focus on the category combo
            d.dir(0, 1);                          // first action slot
            d.button(SDL_GAMEPAD_BUTTON_SOUTH);
            CHECK(u.menu.remap_selected == 0);
            d.frame();
            d.button(SDL_GAMEPAD_BUTTON_NORTH);  // J_JOYPAD 4
            CHECK(u.menu.remap_selected < 0);
            CHECK(u.menu.bindings[std::size_t(action - 1)].code[2] == 4);
            const std::string* n = binding_name(vfs, u.menu.bindings[std::size_t(action - 1)], 2);
            CHECK(n && !n->empty());
            std::fprintf(stderr, "remap: action %d -> %s\n", action, n ? n->c_str() : "?");
            // Start cancels a capture; LB/RB change the category.
            d.button(SDL_GAMEPAD_BUTTON_SOUTH);
            CHECK(u.menu.remap_selected == 0);
            d.frame();
            d.button(SDL_GAMEPAD_BUTTON_START);
            CHECK(u.menu.remap_selected < 0 && u.menu.bindings[std::size_t(action - 1)].code[2] == 4);
            d.frame();
            d.button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            CHECK(u.menu.remap_category == 1);
            d.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            d.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            CHECK(u.menu.remap_category == 6);
        }
    }

    // ---- In-game menu: East resumes (BACK) ----
    {
        UiRunner u(vfs, nr, "ingame");
        Drive d{u};
        d.idle(40);
        d.button(SDL_GAMEPAD_BUTTON_EAST);
        CHECK(u.menu.ingame_exit == 8);
        CHECK(d.until([&] { return u.menu.screen == MenuScreen::None; }));
    }

    // ---- nav_seal: widgets after a modal window (the HUD over the character window) stay out of its ring ----
    {
        Ui ui(vfs, nr);
        std::vector<UiQuad> q;
        const glm::vec4 a{100, 100, 50, 50}, b{100, 300, 50, 50};  // b below a: D-pad down would reach it
        bool hover_a = false, hover_b = true;
        for (int i = 0; i < 4; ++i) {
            UiInput in;
            in.pad.used = true;
            if (i == 2) in.pad.dir = {0, 1};
            q.clear();
            ui.begin({0, 0, 1024, 768}, q, in, kDt);
            ui.nav_modal(42);
            hover_a = ui.hover(a);
            ui.nav_seal();
            hover_b = ui.hover(b);
        }
        CHECK(ui.pad_focus() && hover_a && !hover_b);
    }
    return test_result();
}
