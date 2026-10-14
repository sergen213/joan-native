#pragma once
// Pad-focus screenshots (gl_gamepad_test.cpp / vk_gamepad_test.cpp): menus driven only by UiPad input, focus moved
// onto the widget the original's reference shot hovers. Pad focus draws the hover state, so the frame must match
// the reference and the port's own mouse-hover render (cursor area masked; the pad hides the cursor).
// PadWindows: plus the F1 character window (mission 1) with the pad focus on the bag sword's grid cell.
// Writes gamepad_<gl|vk>_<case>.png to $JOA_TEST_OUT. Exit 77 without data / video driver / Vulkan.
#include "ui_shot.hpp"

#include "engine/hud_character.hpp"
#include "engine/session.hpp"

namespace padshot {
using namespace joa::engine;
using uishot::Rect;

struct PadCase {
    const char* name;
    const char* screen;       // --ui-screen sample
    int input_method;         // GameOptions::input_method (2 = Gamepad/Joystick: remap shows the joypad column)
    glm::vec2 target;         // design point the focus must cover
    const char* reference;    // work/reference/shots file, "" = none
    double min_ssim;          // vs the reference
    double min_parity;        // vs the port's mouse hover at `target` (cursor area masked), 0 = skip
    int remap_category = 0;   // remap page category (1 = Control Keys: most J_JOYPAD bindings)
};

// Renders the sample with the pad: wake, then steer the focus toward `target` (one D-pad press every other frame),
// let it settle. mouse >= 0 renders the mouse-hover variant instead.
inline Image render(RendererKind kind, const PadCase& c, const char* game, glm::vec2 mouse = {-1, -1}) {
    Options o;
    o.renderer = kind;
    o.width = 1024, o.height = 768;
    o.game_dir = game;
    o.vk_validation = kind == RendererKind::Vulkan;
    Platform platform(o, true);
    std::unique_ptr<IRenderer> r =
        kind == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
    Image img;
    {
        joa::formats::Vfs vfs(game);
        UiRunner ui(vfs, *r, c.screen);
        ui.menu.options.input_method = c.input_method;
        ui.menu.remap_category = c.remap_category;
        std::vector<UiQuad> quads;
        const glm::ivec4 vp{0, 0, 1024, 768};
        for (int i = 0; i < 90; ++i) {
            UiInput in;
            in.mouse = mouse;
            if (mouse.x < 0 && (i == 4 || (i > 4 && i % 2 == 0 && i < 70))) {
                in.pad.used = true;
                const glm::vec4 f = ui.ui.nav_focus();
                const glm::vec2 fc{f.x + f.z / 2, f.y + f.w / 2};
                const bool inside = c.target.x >= f.x && c.target.y >= f.y && c.target.x < f.x + f.z && c.target.y < f.y + f.w;
                if (i > 4 && !inside) {
                    const glm::vec2 d = c.target - fc;
                    in.pad.dir = std::abs(d.y) > f.w / 2 ? glm::ivec2(0, d.y > 0 ? 1 : -1) : glm::ivec2(d.x > 0 ? 1 : -1, 0);
                }
            }
            quads.clear();
            ui.frame(vp, in, 1.0f / 30.0f, quads);
            FrameData fd;
            fd.view.viewport = vp;
            fd.ui = quads;
            r->begin_frame({1024, 768});
            r->submit(fd);
            if (i == 89) img = r->read_back();
            r->end_frame();
        }
    }
    return img;
}

// PadWindows: character window, pad focus steered onto the bag sword at personal cell (0, 0): the cell shows the
// window's own hover visuals (the #M icon) and the item tooltip hangs below it.
inline Image charwin(RendererKind kind, const char* game) {
    Options o;
    o.renderer = kind;
    o.width = 1024, o.height = 768;
    o.game_dir = game;
    o.vk_validation = kind == RendererKind::Vulkan;
    Platform platform(o, true);
    std::unique_ptr<IRenderer> r =
        kind == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
    Image img;
    {
        joa::formats::Vfs vfs(game);
        Ui ui(vfs, *r);
        Session s(vfs);
        s.seed = 1;
        std::string err;
        if (!s.new_game(1, 1, &err)) throw std::runtime_error("new_game: " + err);
        joa::game::Mission& m = *s.mission();
        m.autosave = false;
        m.ui().auto_advance = false;
        joa::game::World& w = m.world();
        joa::game::Unit& joan = *w.unit(w.player_id());
        joan.inv.bag.clear();
        w.give_item(joan, "JOAN_SWORD_LV2", 0, 1);
        HudModel hud;
        hud.show_character = true;
        CharacterWindow win;
        const glm::vec2 target{0x21a + 20, 0x3e + 20};
        const glm::ivec4 vp{0, 0, 1024, 768};
        glm::vec4 last{-1.0f};
        bool flip = false;
        for (int i = 0; i < 90; ++i) {
            UiInput in;
            in.pad.used = i >= 2 && i < 70;
            const glm::vec4 f = ui.nav_focus();
            const bool inside = target.x >= f.x && target.y >= f.y && target.x < f.x + f.z && target.y < f.y + f.w;
            if (in.pad.used && i >= 4 && f.z > 0 && !inside && i % 2 == 0) {  // a blocked axis switches to the other one
                if (f == last) flip = !flip;
                last = f;
                const glm::vec2 d = target - glm::vec2(f.x + f.z / 2, f.y + f.w / 2);
                const bool vert = (std::abs(d.x) <= f.z / 2) != flip;   // sideways first while the target lies beside
                in.pad.dir = vert ? glm::ivec2(0, d.y > 0 ? 1 : -1) : glm::ivec2(d.x > 0 ? 1 : -1, 0);
            }
            std::vector<UiQuad> quads;
            ui.begin(vp, quads, in, 1.0f / 30.0f);
            win.draw(ui, m, hud);
            FrameData fd;
            fd.view.viewport = vp;
            fd.ui = quads;
            r->begin_frame({1024, 768});
            r->submit(fd);
            if (i == 89) img = r->read_back();
            r->end_frame();
        }
        const glm::vec4 f = ui.nav_focus();
        std::fprintf(stderr, "charwin focus (%.0f, %.0f, %.0f, %.0f)\n", f.x, f.y, f.z, f.w);
        CHECK(f == glm::vec4(0x21a, 0x3e, 40, 40));
    }
    return img;
}

inline int run(RendererKind kind, std::span<const PadCase> cases) {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) return std::fprintf(stderr, "SKIP: no original data\n"), 77;
    if (!SDL_Init(SDL_INIT_VIDEO)) return std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError()), 77;
    const std::string shots = std::string(game) + "/work/reference/shots/";
    const char* tag = kind == RendererKind::Vulkan ? "vk" : "gl";
    for (const PadCase& c : cases) {
        Image pad, mouse;
        try {
            pad = render(kind, c, game);
            if (c.min_parity > 0) mouse = render(kind, c, game, c.target);
        } catch (const std::exception& e) {
            if (kind == RendererKind::Vulkan && uishot::vulkan_missing(e)) return std::fprintf(stderr, "SKIP: %s\n", e.what()), 77;
            std::fprintf(stderr, "FAIL %s: %s\n", c.name, e.what());
            ++g_failures;
            continue;
        }
        write_png(uishot::out_path(std::string("gamepad_") + tag + "_" + c.name + ".png"), pad.width, pad.height, pad.rgba);
        CHECK(pad.width == 1024 && pad.height == 768);
        if (pad.width != 1024) continue;
        const uishot::Gray g = uishot::to_gray(pad.rgba.data(), 1024, 768, 1024);
        if (c.min_parity > 0 && mouse.width == 1024) {
            // Everything but the 40x40 cursor area at the mouse.
            const int cx = int(c.target.x) - 4, cy = int(c.target.y) - 4, cw = 40;
            const std::vector<Rect> rects = {{0, 0, 1024, cy}, {0, cy + cw, 1024, 768 - cy - cw}, {0, cy, cx, cw}, {cx + cw, cy, 1024 - cx - cw, cw}};
            const double s = uishot::mean_ssim(g, uishot::to_gray(mouse.rgba.data(), 1024, 768, 1024), rects);
            std::fprintf(stderr, "%s %-14s pad focus vs mouse hover ssim = %.4f (min %.3f)\n", tag, c.name, s, c.min_parity);
            CHECK(s >= c.min_parity);
        }
        if (*c.reference) {
            uishot::Gray ref;
            if (!uishot::load_reference(shots + c.reference, ref) || ref.w != 1024 || ref.h != 768) {
                std::fprintf(stderr, "SKIP compare %s: reference %s unavailable\n", c.name, c.reference);
                continue;
            }
            const double s = uishot::mean_ssim(g, ref, {});
            std::fprintf(stderr, "%s %-14s vs %s ssim = %.3f (min %.2f)\n", tag, c.name, c.reference, s, c.min_ssim);
            CHECK(s >= c.min_ssim);
        }
    }
    try {
        const Image cw = charwin(kind, game);
        write_png(uishot::out_path(std::string("gamepad_") + tag + "_charwin.png"), cw.width, cw.height, cw.rgba);
        CHECK(cw.width == 1024 && cw.height == 768);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL charwin: %s\n", e.what());
        ++g_failures;
    }
    SDL_Quit();
    return test_result();
}

// Main menu focus on Load a Campaign (no hovered original shot: parity with the mouse hover); options root on Audio
// (ui_options_root's gauntlet hotspot sits 3 px left of the button, so the original draws it unhovered and the pad
// shot differs from it by the hover sprite only); in-game menu on Options (F9) and remap on CONFIRM as the original's
// captures hover them; remap in the Gamepad/Joystick scheme (joypad column, JOYPADn names), Control Keys, Defend.
inline const PadCase kCases[] = {
    {"main", "main", 1, {258, 332}, "", 0, 0.99},
    {"options", "options", 1, {300, 420}, "ui_options_root.png", 0.97, 0.99},
    {"ingame", "ingame", 1, {510, 486}, "s04_07_ingame_menu.png", 0.98, 0.99},
    {"remap", "remap", 1, {285, 722}, "ui_options_remap_0.png", 0.97, 0.99},
    {"remap_pad", "remap", 2, {300, 312}, "", 0, 0.99, 1},
};

}  // namespace padshot
