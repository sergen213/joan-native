#pragma once
// HudGaps mapobj through a real backend (gl_/vk_hud_mapobj_test.cpp), on the "objective" HUD sample (UiRunner):
// settled objective panel vs s04_06_objective (masked SSIM, as ui_cases_hud), panel mid fade-in (alpha ~127),
// a COMPLETED! highlight mid-reveal, and the minimap fog (vision circle + a script rectangle): fogged texels are
// half as bright as without the mask, texels inside the vision are unchanged.
#include "ui_shot.hpp"

#include "engine/hud_mapobj.hpp"

#include <functional>

namespace hudmapobj {
using namespace joa::engine;

inline int run(RendererKind kind) {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
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
        // `setup` edits the sample HUD; `frames` frames of `dt` s are drawn, the last one is read back.
        auto shot = [&](const char* name, int frames, float dt, const std::function<void(HudModel&)>& setup) {
            UiRunner ui(vfs, *r, "objective");
            setup(ui.hud);
            const glm::ivec4 vp{0, 0, 1024, 768};
            Image img;
            for (int i = 0; i < frames; ++i) {
                std::vector<UiQuad> quads;
                ui.frame(vp, {}, dt, quads);
                FrameData f;
                f.view.viewport = vp;
                f.ui = quads;
                r->begin_frame({1024, 768});
                r->submit(f);
                if (i + 1 == frames) img = r->read_back();
                r->end_frame();
            }
            write_png(uishot::out_path(std::string("hud_mapobj_") + tag + "_" + name + ".png"), img.width, img.height, img.rgba);
            CHECK(img.width == 1024 && img.height == 768);
            return uishot::to_gray(img.rgba.data(), img.width, img.height, img.width);
        };
        auto mean = [](const uishot::Gray& g, uishot::Rect rc) {
            double s = 0;
            for (int y = rc.y; y < rc.y + rc.h; ++y)
                for (int x = rc.x; x < rc.x + rc.w; ++x) s += g.at(x, y);
            return s / (rc.w * rc.h);
        };

        // Settled panel (the sample shows it without the state machine) vs the original.
        const uishot::Gray settled = shot("objective_settled", 4, 1.0f / 30.0f, [](HudModel&) {});
        uishot::Gray ref;
        const std::string shots = std::string(game) + "/work/reference/shots/";
        if (uishot::load_reference(shots + "s04_06_objective.png", ref)) {
            const double v = uishot::mean_ssim(settled, ref, {{0, 148, 384, 40}});
            std::fprintf(stderr, "%s objective settled ssim = %.3f (min 0.53)\n", tag, v);
            CHECK(v >= 0.53);
        }
        // Mid fade-in: 1 s of the 2 s fade (alpha 127): bars + text at about half their settled contrast over black.
        const uishot::Gray mid = shot("objective_midfade", 30, 1.0f / 30.0f, [](HudModel& h) {
            h.show_objective = false;
            hud_objective_show(h);
        });
        const uishot::Rect panel{0, 148, 384, 40};
        const double m_set = mean(settled, panel), m_mid = mean(mid, panel);
        std::fprintf(stderr, "%s panel mean settled %.1f, mid-fade %.1f\n", tag, m_set, m_mid);
        CHECK(m_mid > 0.35 * m_set && m_mid < 0.65 * m_set);
        // COMPLETED! typing in (gold, 6 prefix characters left hidden) on the first line.
        shot("objective_completed", 2, 1.0f / 30.0f, [](HudModel& h) {
            h.objective_fade = 3, h.objective_pinned = true;
            h.objective_list[0].status = 0, h.objective_list[0].reveal = 5;
        });

        // Minimap fog: vision circle (VISION_R 2000) at the player + a script rectangle north-west of it.
        auto fog = [](HudModel& h) {
            h.minimap_fog = true;
            h.minimap_vision = {{1, {h.player_world}, 2000.0f, true},
                                {0, {h.player_world + glm::vec2(-3000, 1200), h.player_world + glm::vec2(-1500, 2200)}, 0.0f, false}};
        };
        const uishot::Gray off = shot("minimap_nofog", 2, 1.0f / 30.0f, [](HudModel&) {});
        const uishot::Gray on = shot("minimap_fog", 2, 1.0f / 30.0f, fog);
        // View centre (912.5, 110) = player; 1 px = 40.96 world units at zoom 1 (S1L1 16384 / 400 px).
        const uishot::Rect inside{905, 132, 10, 10}, rect{850, 62, 10, 10}, outside{825, 170, 10, 10};
        const double r_in = mean(on, inside) / mean(off, inside), r_rect = mean(on, rect) / mean(off, rect),
                     r_out = mean(on, outside) / mean(off, outside);
        std::fprintf(stderr, "%s minimap fog/nofog: vision %.2f, script rect %.2f, outside %.2f\n", tag, r_in, r_rect, r_out);
        CHECK(r_in > 0.95 && r_in < 1.05);
        CHECK(r_rect > 0.95 && r_rect < 1.05);
        CHECK(r_out > 0.4 && r_out < 0.6);
    }
    r.reset();
    platform.reset();
    SDL_Quit();
    return test_result();
}

}  // namespace hudmapobj
