#pragma once
// Character details window through a real backend (gl_/vk_character_details_test.cpp): mission 1 Joan with the
// original's starting kit, scored against s04_06_character_details.png (masked SSIM on the two panels above the
// attention message; the 3D world behind the translucent parchment is not rendered here). Also writes the combo
// page and the shop (no reference shots exist for those).
#include "ui_shot.hpp"

#include "engine/hud_character.hpp"
#include "engine/session.hpp"

namespace chardetails {
using namespace joa::engine;

inline int run(RendererKind kind) {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: SDL video unavailable: %s\n", SDL_GetError());
        return 77;
    }
    const char* tag = kind == RendererKind::Vulkan ? "vk" : "gl";
    Options o;
    o.renderer = kind;
    o.width = 1024;
    o.height = 768;
    o.game_dir = game;
    o.vk_validation = kind == RendererKind::Vulkan;
    std::unique_ptr<Platform> platform;
    std::unique_ptr<IRenderer> r;
    try {
        platform = std::make_unique<Platform>(o, true);
        r = kind == RendererKind::Vulkan ? create_vk_renderer(platform->window(), o) : create_gl_renderer(platform->window());
    } catch (const std::exception& e) {
        if (kind == RendererKind::Vulkan && uishot::vulkan_missing(e)) {
            std::fprintf(stderr, "SKIP: %s\n", e.what());
            return 77;
        }
        throw;
    }
    {
        joa::formats::Vfs vfs(game);
        Ui ui(vfs, *r);
        Session s(vfs);
        s.seed = 1;
        std::string err;
        if (!s.new_game(1, 1, &err)) {
            std::fprintf(stderr, "FAIL new_game: %s\n", err.c_str());
            return 1;
        }
        joa::game::Mission& m = *s.mission();
        m.autosave = false;
        m.ui().auto_advance = false;
        HudModel hud;
        hud.show_character = true;
        CharacterWindow win;
        auto shot = [&](const char* name, const char* reference, const std::vector<uishot::Rect>& include, double min_ssim) {
            Image img;
            for (int i = 0; i < 3; ++i) {
                std::vector<UiQuad> quads;
                UiInput in;
                in.mouse = {1000, 760};   // parked: no hover, no tooltip
                ui.begin({0, 0, 1024, 768}, quads, in, 0.05f);
                win.draw(ui, m, hud);
                FrameData f;
                f.view.viewport = {0, 0, 1024, 768};
                f.ui = quads;
                r->begin_frame({1024, 768});
                r->submit(f);
                if (i == 2) img = r->read_back();
                r->end_frame();
            }
            write_png(uishot::out_path(std::string("chardetails_") + tag + "_" + name + ".png"), img.width, img.height, img.rgba);
            CHECK(img.width == 1024 && img.height == 768);
            if (!*reference || img.width != 1024) return;
            uishot::Gray ref;
            if (!uishot::load_reference(std::string(game) + "/work/reference/shots/" + reference, ref)) {
                std::fprintf(stderr, "SKIP compare %s: reference unavailable\n", name);
                return;
            }
            const double v = uishot::mean_ssim(uishot::to_gray(img.rgba.data(), 1024, 768, 1024), ref, include);
            std::fprintf(stderr, "%s %-10s ssim = %.3f (min %.2f)\n", tag, name, v, min_ssim);
            CHECK(v >= min_ssim);
        };
        // Panels down to the attention message (y 540), which the original draws over the window. Measured 0.952 GL = VK.
        shot("window", "s04_06_character_details.png", {{8, 8, 292, 530}, {324, 8, 484, 530}}, 0.93);
        win.combo_page = true;
        shot("combos", "", {}, 0);
        hud.armory_item = m.data().item_index("LONGBOW"), hud.armory_level = 1;
        shot("armory", "", {}, 0);
        hud.armory_item = 0;
        hud.show_character = false;
        joa::game::Facility f;
        f.recno = int(m.places().facilities().size()) + 1;
        f.kind = joa::game::Facility::Shop;
        m.places().facilities().push_back(f);
        m.places().set_sell_list(f.recno, m.objects().sell_list("S3L1_WEAPON"));
        m.ui().buy_open = true, m.ui().buy_shop = f.recno;
        shot("shop", "", {}, 0);
    }
    r.reset();
    platform.reset();
    SDL_Quit();
    return test_result();
}

}  // namespace chardetails
