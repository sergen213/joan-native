// options.ini round trip (menu_options_store.cpp) and, with original data, the STD.SET key tables.
#include "check.hpp"
#include "engine/menu_options_store.hpp"

#include <SDL3/SDL_scancode.h>

#include <cstdlib>
#include <filesystem>

using namespace joa::engine;

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "joa_options_store_test";
    std::filesystem::remove_all(dir);
    const std::filesystem::path ini = dir / "joan-native" / "options.ini";
    const joa::formats::Vfs empty(dir);  // no STD.SET / CONFIG.DAT: GameOptions{} defaults, no key rows

    GameOptions o;
    o.shadow = 1, o.brightness = 0.36f, o.contrast = 2.2f, o.gamma = 4.2f, o.draw_distance = 7, o.dynamic_grass = false;
    o.grass_distance = 460, o.grass_density = 33, o.music_volume = 5, o.sound_volume = 66, o.environment_volume = 12;
    o.sound_3d = false, o.input_method = 0, o.mouse_sensitivity = -0.3f, o.invert_mouse = true, o.riding_camera_angle = 37;
    o.difficulty = 3, o.riding_mode = 1, o.melee_auto_target = false;
    KeyBindingRow row;
    row.action = 4;
    row.def = {0xc8, 0xffff, 0xff, 0x11, 0};
    row.code = {0x1e, 0xffff, 0xff, 0xffff, 2};
    CHECK(save_options(ini, o, {row}));

    GameOptions r;
    std::vector<KeyBindingRow> rows;
    load_options(empty, ini, r, rows);
    CHECK(rows.empty());  // keys only apply to rows that exist (STD.SET)
    CHECK(r.shadow == 1 && r.draw_distance == 7 && !r.dynamic_grass && r.grass_distance == 460 && r.grass_density == 33);
    CHECK_NEAR(r.brightness, 0.36f, 1e-5);
    CHECK_NEAR(r.contrast, 2.2f, 1e-5);
    CHECK_NEAR(r.gamma, 4.2f, 1e-5);
    CHECK(r.music_volume == 5 && r.sound_volume == 66 && r.environment_volume == 12 && !r.sound_3d);
    CHECK(r.input_method == 0 && r.invert_mouse && r.difficulty == 3 && r.riding_mode == 1 && !r.melee_auto_target);
    CHECK_NEAR(r.mouse_sensitivity, -0.3f, 1e-5);
    CHECK_NEAR(r.riding_camera_angle, 37.0f, 1e-5);
    // Point & Click (input_method 3, mode 2) persists.
    GameOptions pc;
    pc.input_method = 3;
    CHECK(save_options(ini, pc, {}));
    load_options(empty, ini, r, rows);
    CHECK(r.input_method == 3 && key_scheme(r.input_method) == 2);

    // Missing file = defaults.
    load_options(empty, dir / "absent.ini", r, rows);
    CHECK(r.shadow == GameOptions{}.shadow && r.music_volume == GameOptions{}.music_volume);

    CHECK(sdl_to_dik(SDL_SCANCODE_W) == 0x11 && sdl_to_dik(SDL_SCANCODE_UP) == 0xc8 && sdl_to_dik(SDL_SCANCODE_F12) == 0x58);
    CHECK(sdl_to_dik(SDL_SCANCODE_0) == 0x0b && sdl_to_dik(SDL_SCANCODE_LSHIFT) == 0x2a);

    // With the original data: DEFAULTK rows, KEYDESC names per scheme, and the key line round trip.
    const char* game = std::getenv("JOA_GAME_DIR");
    if (game && joa::formats::Vfs(game).exists("Resource/STD.SET")) {
        const joa::formats::Vfs vfs(game);
        const std::vector<KeyBindingRow>& def = default_bindings(vfs);
        CHECK(def.size() == 55);
        if (def.size() == 55) {
            CHECK(def[3].label == "Move forward" && def[3].def[0] == 0xc8 && def[3].def[3] == 0x11);
            CHECK(def[0].header && def[0].def[0] == 0xffff && def[0].def[4] == 0xff);
            const std::string* n = binding_name(vfs, def[3], 1);
            CHECK(n && *n == "W");
            n = binding_name(vfs, def[3], 0);
            CHECK(n && *n == "Up");
            n = binding_name(vfs, def[12], 1);  // Light attack: FPS_KEY 0 -> mouse button 1
            CHECK(n && *n == "Left mouse button");
            n = binding_name(vfs, def[0], 1);
            CHECK(n && *n == "Not Available");
            std::vector<KeyBindingRow> keys = def;
            keys[3].code[3] = 0x1e;  // forward = A
            CHECK(save_options(ini, o, keys));
            const joa::formats::Vfs data_only(game);
            std::vector<KeyBindingRow> back;
            load_options(data_only, ini, r, back);
            CHECK(back.size() == 55 && back[3].code[3] == 0x1e && back[4].code == def[4].code);
            MenuModel m;
            m.bindings = back;
            CHECK(action_key_name(vfs, m, 4) == "A");

            // RemapKey_assign conflicts (keyboard & mouse scheme 3, FPS_KEY = code[3]).
            std::vector<KeyBindingRow> k = def;
            CHECK(remap_assign(vfs, k, 3, 5, 0, 0x11));       // Move back := W, held by Move forward
            CHECK(k[4].code[3] == 0x11 && k[3].code[3] == 0x1f);  // forward's default W is taken -> swap (gets S)
            k = def;
            CHECK(remap_assign(vfs, k, 3, 5, 0, 0x2d));       // Move back := X (free)
            CHECK(remap_assign(vfs, k, 3, 7, 0, 0x2d));       // Strafe Right := X, held by Move back
            CHECK(k[6].code[3] == 0x2d && k[4].code[3] == 0x1f);  // back's default S is free -> restored
            CHECK(remap_assign(vfs, k, 3, 13, 2, 1));         // Light attack := right mouse button (held by Heavy)
            CHECK(k[12].code[4] == 2 && k[12].code[3] == 0 && k[13].code[4] == 1);  // heavy gets light's old LMB
            CHECK(!remap_assign(vfs, k, 3, 0x22, 0, 0x10));   // Main Game Menu is fixed
            CHECK(!remap_assign(vfs, k, 0, 4, 0, 0x99));      // not in KEYDESC
        }
    }
    std::filesystem::remove_all(dir);
    return test_result();
}
