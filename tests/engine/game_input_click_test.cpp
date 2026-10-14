// Point & click input (Options "Point & Click", input_method 3 -> mode 2): key_scheme / CONFIG.DAT import, the
// K_KEY column of actions 17/19/20/34 only (PlayerCtrl_mouse_click 0x438b30 reads Action_pressed(0, ..); everything
// else asks scheme 2 = nothing), the popped LMB / RMB down -> InputFrame::click_* with its pointer, no mouse look.
#include "check.hpp"
#include "engine/game_input.hpp"
#include "engine/menu_options_store.hpp"
#include "formats/config_dat.hpp"

#include <SDL3/SDL_mouse.h>

#include <cstdlib>

using namespace joa::engine;
using namespace joa::game;

namespace {
// SDL scancode of the K_KEY binding of DEFAULTK row `action` (0 = none).
SDL_Scancode kkey(const std::vector<KeyBindingRow>& rows, int action) {
    for (int s = 1; s < SDL_SCANCODE_COUNT; ++s)
        if (sdl_to_dik(s) != 0 && sdl_to_dik(s) == rows[std::size_t(action - 1)].code[0]) return SDL_Scancode(s);
    return SDL_SCANCODE_UNKNOWN;
}
}  // namespace

int main() {
    CHECK(key_scheme(0) == 0 && key_scheme(1) == 3 && key_scheme(2) == 1 && key_scheme(3) == 2);
    {   // CONFIG.DAT 0x6B = 2 (the only way the original reaches mode 2) imports as Point & Click.
        joa::formats::GameConfig c;
        GameOptions o;
        std::vector<KeyBindingRow> none;
        c.control_scheme = 2;
        options_from_config(c, o, none);
        CHECK(o.input_method == 3);
        c.control_scheme = 3;
        options_from_config(c, o, none);
        CHECK(o.input_method == 1);
    }

    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/STD.SET")) return 77;
    const joa::formats::Vfs vfs(game);
    const std::vector<KeyBindingRow>& def = default_bindings(vfs);
    GameOptions opt;
    opt.input_method = 3;
    GameInput in;
    in.configure(def, opt, {}, false);
    RawInput raw;
    auto tick = [&](glm::vec2 p = glm::vec2(-1.0f)) { in.feed(raw, 1.0f / 60, p), in.feed(raw, 1.0f / 60, p); return in.take(); };

    // Keys: K_KEY of the HP / EP item, Interact and the menu work; movement / attack keys do nothing.
    for (const int a : {int(IN_HEAL_ITEM), int(IN_EP_ITEM), int(IN_INTERACT), 34}) {
        const SDL_Scancode s = kkey(def, a);
        CHECK(s != SDL_SCANCODE_UNKNOWN);
        raw.keys.set(s);
        const InputFrame f = tick();
        CHECK(f.pressed[std::size_t(a)] && f.pressed.count() == 1);
        raw.keys.reset(s);
        tick();
    }
    for (const int a : {int(IN_FORWARD), int(IN_ATTACK_A), int(IN_JUMP), int(IN_RTS_TOGGLE)})
        if (const SDL_Scancode s = kkey(def, a); s != SDL_SCANCODE_UNKNOWN) {
            raw.keys.set(s);
            CHECK(tick().held.none());
            raw.keys.reset(s);
            tick();
        }

    // Clicks: the popped LMB / RMB down carries the pointer of its feed; no actions, no mouse look.
    raw.mouse_buttons.set(SDL_BUTTON_LEFT);
    raw.mouse_dx = 40, raw.mouse_dy = 25;
    InputFrame f = tick({320, 200});
    CHECK(f.click_left && !f.click_right && f.pressed.none() && f.mouse_dx == 0 && f.mouse_dy == 0);
    CHECK(in.click_pos() == glm::vec2(320, 200));
    raw.mouse_dx = raw.mouse_dy = 0;
    raw.mouse_buttons.reset();
    f = tick({330, 210});  // button up event: no click
    CHECK(!f.click_left && !f.click_right);
    raw.mouse_buttons.set(SDL_BUTTON_RIGHT);
    f = tick({100, 50});
    CHECK(f.click_right && !f.click_left && in.click_pos() == glm::vec2(100, 50));
    raw.mouse_buttons.reset();
    tick();

    // Keyboard & mouse keeps LMB = light attack and never reports clicks.
    opt.input_method = 1;
    in.configure(def, opt, {}, false);
    raw.mouse_buttons.set(SDL_BUTTON_LEFT);
    f = tick({320, 200});
    CHECK(f.pressed[IN_ATTACK_A] && !f.click_left);
    return test_result();
}
