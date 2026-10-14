// Character details window + shop, headless (NullRenderer, mission 1): F1 (action 35) opens the window and pauses
// the flow; clicks through the real Ui hit tests spend an attribute point (ADD on the Strength row), equip a sword
// dragged from the bag onto the weapon grid, eat a bread (right click), switch to the combo page; F1 closes it.
// Then a shop (start_buy_interface state on an S3L1_POTION facility): click-buy changes gold and the bag, the
// second buy fails on gold (BUY 8 box, Enter), Esc closes (the script WAIT predicate clears).
// PadWindows: the same by pad only: D-pad up (J_JOYPAD 7 -> action 35 through GameInput) opens it, the D-pad steers
// the focus (rows, cells, slots), South = ADD / pick up / drop, West = use, LB / RB = page, East closes; South buys
// in the shop.
#include "check.hpp"
#include "engine/hud_character.hpp"
#include "engine/renderer_null.hpp"
#include "engine/game_input.hpp"
#include "engine/menu_options_store.hpp"
#include "engine/session_flow.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <cstdlib>
#include <filesystem>

using namespace joa;
using namespace joa::engine;

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !formats::Vfs(game).exists("scenario/scenario_01.scr")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    const char* out = std::getenv("JOA_TEST_OUT");
    const std::filesystem::path xdg = std::filesystem::path(out ? out : ".") / "chardetails-xdg";
    setenv("XDG_DATA_HOME", xdg.c_str(), 1);
    setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);

    const formats::Vfs vfs(game);
    Session s(vfs);
    s.seed = 1;
    MenuModel menu;
    HudModel hud;
    GameFlow flow(s, menu, hud);
    flow.persist_options = false;
    flow.new_game(1, 1);
    CHECK(flow.run_pending() && s.mission());
    if (g_failures) return test_result();
    game::Mission& m = *s.mission();
    m.autosave = false;
    for (int t = 0; t < 30; ++t) s.tick({}), s.take_world_events(), s.take_mission_events();
    m.ui().cs.active = false, m.ui().game_cut_scene = false;   // opening cut scene: F1 is ignored while it runs
    game::World& w = m.world();
    game::Unit& joan = *w.unit(w.player_id());

    NullRenderer r;
    Ui ui(vfs, r);
    CharacterWindow win;
    std::vector<UiQuad> quads;
    auto frame = [&](glm::vec2 mouse, bool press = false, bool release = false, bool right = false, int key = 0) {
        UiInput in;
        in.mouse = mouse, in.mouse_pressed = press, in.mouse_down = press, in.mouse_released = release, in.right_pressed = right;
        in.key_pressed = key;
        quads.clear();
        ui.begin({0, 0, 1024, 768}, quads, in, 0.033f);
        win.draw(ui, m, hud);
    };
    auto click = [&](glm::vec2 p) { frame(p), frame(p, true), frame(p, false, true); };

    // F1 opens and pauses (FUN_006f7d80 game speed 0).
    flow.action(35);
    CHECK(hud.show_character && flow.paused());
    frame({500, 700});
    CHECK(m.ui().inventory_open && win.unit == joan.id);
    CHECK(!quads.empty());

    // Attribute point: level up to get points, hover Strength (row 2 at (16, 300)), ADD at (266, 306).
    w.add_xp(joan, w.data().level_row(1)->exp - joan.xp, false, true);
    CHECK(joan.level >= 2 && joan.attr_points > 0);
    const int str = w.stat_str(joan), pts = joan.attr_points;
    click({275, 315});
    std::fprintf(stderr, "strength %d -> %d, points %d -> %d\n", str, w.stat_str(joan), pts, joan.attr_points);
    CHECK(w.stat_str(joan) > str && joan.attr_points == pts - 1);

    // Equip: a JOAN_SWORD_LV2 in the bag lands at personal cell (0,0) = (0x21a, 0x3e); press + release picks it up,
    // the next press on the weapon grid drops it (World::equip) and the old sword goes to the bag.
    const int old_sword = joan.inv.melee[0].item;
    joan.inv.bag.clear();   // known layout
    CHECK(w.give_item(joan, "JOAN_SWORD_LV2", 0, 1));
    const int sword = w.data().item_index("JOAN_SWORD_LV2");
    frame({600, 600});
    click({0x21a + 20, 0x3e + 20});
    frame({0x15f + 80, 0x3c + 80}, true);
    frame({0x15f + 80, 0x3c + 80}, false, true);
    std::fprintf(stderr, "melee0 %d (sword %d, old %d), bag %zu\n", joan.inv.melee[0].item, sword, old_sword, joan.inv.bag.size());
    CHECK(sword && joan.inv.melee[0].item == sword);
    CHECK(!joan.inv.bag.empty() && joan.inv.bag.back().item == old_sword);

    // Bread (Red Potion, +Damage HP): right click on it in the personal grid uses it (World::use_item).
    w.set_hp(joan, 10);
    CHECK(w.give_item(joan, "fd_bread_s", 0, 1));
    frame({600, 600});
    CHECK(win.cells().size() == 2 && win.cells()[1].grid == 0);
    if (win.cells().size() == 2) {   // first-fit next to the old sword
        const auto& c = win.cells()[1];
        frame({float(0x21a + c.col * 40 + 20), float(0x3e + c.row * 40 + 20)}, false, false, true);
    }
    const int heal = w.data().items[std::size_t(w.data().item_index("fd_bread_s"))].damage;
    std::fprintf(stderr, "hp after bread %d (heal %d)\n", joan.hp, heal);
    CHECK(heal > 0 && joan.hp == std::min(w.max_hp(joan), 10 + heal));

    // CB button (0xf6, 0x13) -> attack-combo page and back.
    click({0xf6 + 22, 0x13 + 22});
    CHECK(win.combo_page);
    frame({600, 600});
    click({0xf6 + 22, 0x13 + 22});
    CHECK(!win.combo_page);

    // F1 closes; the flow runs again.
    flow.action(35);
    frame({600, 600});
    CHECK(!hud.show_character && !m.ui().inventory_open && !flow.paused());

    // ARMORY: a player pickup of a ShowArmoryInterface item opens it once (item hint list), pauses, Esc closes.
    const int longbow = w.data().item_index("LONGBOW");
    CHECK(longbow && w.data().items[std::size_t(longbow)].show_armory);
    const std::vector<game::GameEvent> pick{{0, game::Ev::Effect, joan.id, 1, longbow, "PICKUP"}};
    win.on_world_events(pick, m, hud);
    CHECK(hud.armory_item == longbow && flow.paused());
    frame({10, 10}, false, false, false, SDL_SCANCODE_ESCAPE);
    CHECK(hud.armory_item == 0 && !flow.paused());
    win.on_world_events(pick, m, hud);   // already seen: no second window
    CHECK(hud.armory_item == 0);

    // Shop: facility with the S3L1_POTION sell list, opened like start_buy_interface (script 304).
    game::Facility shop;
    shop.recno = int(m.places().facilities().size()) + 1;
    shop.kind = game::Facility::Shop;
    shop.sell_list = m.objects().sell_list("S3L1_POTION");
    CHECK(shop.sell_list > 0);
    m.places().facilities().push_back(shop);
    m.ui().auto_advance = false;
    m.ui().buy_open = true, m.ui().buy_shop = shop.recno, m.ui().buy_seller = joan.id;
    CHECK(flow.paused());
    const int gold = m.state().gold;
    const std::size_t bag = joan.inv.bag.size();
    frame({10, 10});
    click({112 + 0x100 + 20, 84 + 0x38 + 20});   // first stock item: fd_bread_S, price 100
    std::fprintf(stderr, "gold %d -> %d, bag %zu -> %zu\n", gold, m.state().gold, bag, joan.inv.bag.size());
    CHECK(m.state().gold < gold && joan.inv.bag.size() == bag + 1);
    const int after = m.state().gold;
    click({112 + 0x100 + 20, 84 + 0x38 + 20});   // not enough gold -> BUY 8 box
    CHECK(m.state().gold == after && joan.inv.bag.size() == bag + 1);
    frame({10, 10}, false, false, false, SDL_SCANCODE_RETURN);   // OK
    frame({10, 10}, false, false, false, SDL_SCANCODE_ESCAPE);   // close = commit (FUN_006c3540)
    CHECK(!m.ui().buy_open && !flow.paused());

    // ---- PadWindows: pad only ----
    GameInput gin;
    gin.configure(default_bindings(vfs), GameOptions{}, {}, false);
    RawInput raw;
    raw.pad_buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = true;   // J_JOYPAD 7 = View Character Details
    gin.feed(raw, 0.033f);
    CHECK(gin.pressed(35));
    if (gin.pressed(35)) flow.action(35);
    CHECK(hud.show_character && flow.paused());
    auto padf = [&](UiPad p) {
        UiInput in;   // mouse (-1, -1): no pointer
        in.pad = p, in.pad.used = true;
        quads.clear();
        ui.begin({0, 0, 1024, 768}, quads, in, 0.033f);
        win.draw(ui, m, hud);
    };
    auto press = [&](SDL_GamepadButton b) {
        UiPad p;
        p.button = b, p.activate = b == SDL_GAMEPAD_BUTTON_SOUTH, p.back = b == SDL_GAMEPAD_BUTTON_EAST;
        p.page = (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) - (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        padf(p);
    };
    // D-pad toward a design point (viewport = design here) until the focused rect covers it: sideways first while
    // the target lies beside the focus, a blocked axis switches to the other one.
    auto steer = [&](glm::vec2 t) {
        bool flip = false;
        glm::vec4 last{-1.0f};
        for (int i = 0; i < 120; ++i) {
            const glm::vec4 f = ui.nav_focus();
            if (f.z > 0 && t.x >= f.x && t.y >= f.y && t.x < f.x + f.z && t.y < f.y + f.w) return true;
            UiPad p;
            if (f.z > 0) {
                if (f == last) flip = !flip;
                last = f;
                const glm::vec2 d = t - glm::vec2(f.x + f.z / 2, f.y + f.w / 2);
                const bool vert = (std::abs(d.x) <= f.z / 2) != flip;
                p.dir = vert ? glm::ivec2(0, d.y > 0 ? 1 : -1) : glm::ivec2(d.x > 0 ? 1 : -1, 0);
            }
            padf(p);
        }
        return false;
    };
    padf({}), padf({});   // wake: the window is the focus scope
    CHECK(ui.pad_focus());

    // ADD on Strength (row 2 at (16, 300)): focus the row, South.
    if (joan.attr_points == 0) w.add_xp(joan, w.data().level_row(joan.level)->exp - joan.xp, false, true);
    const int str2 = w.stat_str(joan), pts2 = joan.attr_points;
    CHECK(steer({30, 312}));
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    std::fprintf(stderr, "pad: strength %d -> %d, points %d -> %d\n", str2, w.stat_str(joan), pts2, joan.attr_points);
    CHECK(pts2 > 0 && w.stat_str(joan) == str2 + 1 && joan.attr_points == pts2 - 1);

    // Equip by pad: the old sword back in the bag, South on its cell picks it up, South on melee0 drops it there.
    joan.inv.bag.clear();
    CHECK(old_sword && w.give_item(joan, w.data().items[std::size_t(old_sword)].code, 0, 1));
    padf({});
    CHECK(win.cells().size() == 1 && win.cells()[0].grid == 0);
    if (win.cells().size() == 1) {
        const auto& c = win.cells()[0];
        CHECK(steer({float(0x21a + c.col * 40 + 20), float(0x3e + c.row * 40 + 20)}));
    }
    press(SDL_GAMEPAD_BUTTON_SOUTH);   // pick up
    CHECK(steer({0x15f + 40, 0x3c + 80}));
    press(SDL_GAMEPAD_BUTTON_SOUTH);   // drop on the weapon slot (World::equip)
    std::fprintf(stderr, "pad: melee0 %d (old sword %d), bag %zu\n", joan.inv.melee[0].item, old_sword, joan.inv.bag.size());
    CHECK(joan.inv.melee[0].item == old_sword && joan.inv.bag.size() == 1 && joan.inv.bag[0].item == sword);

    // West on a bread uses it.
    w.set_hp(joan, 10);
    CHECK(w.give_item(joan, "fd_bread_s", 0, 1));
    padf({});
    CHECK(win.cells().size() == 2);
    if (win.cells().size() == 2) {
        const auto& c = win.cells()[1];
        CHECK(steer({float(0x21a + c.col * 40 + 20), float(0x3e + c.row * 40 + 20)}));
    }
    press(SDL_GAMEPAD_BUTTON_WEST);
    CHECK(joan.hp == std::min(w.max_hp(joan), 10 + heal) && joan.inv.bag.size() == 1);

    // RB -> combo page, LB back; East closes (Escape), the flow runs again.
    press(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    CHECK(win.combo_page);
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    CHECK(!win.combo_page);
    press(SDL_GAMEPAD_BUTTON_EAST);
    CHECK(!hud.show_character && !m.ui().inventory_open && !flow.paused());

    // Shop by pad: the focus starts on the first stock item; South buys it, East closes.
    m.state().gold = 1000;
    m.ui().buy_open = true, m.ui().buy_shop = shop.recno;
    const std::size_t bag2 = joan.inv.bag.size();
    padf({}), padf({});
    CHECK(steer({112 + 0x100 + 20, 84 + 0x38 + 20}));
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    std::fprintf(stderr, "pad shop: gold 1000 -> %d, bag %zu -> %zu\n", m.state().gold, bag2, joan.inv.bag.size());
    CHECK(m.state().gold < 1000 && joan.inv.bag.size() == bag2 + 1);
    press(SDL_GAMEPAD_BUTTON_EAST);
    CHECK(!m.ui().buy_open && !flow.paused());
    return test_result();
}
