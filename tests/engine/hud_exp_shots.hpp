#pragma once
// MGEXP HUD through a real backend (gl_/vk_hud_exp_test.cpp): EXP counter mid-roll + LEVELUP3 hold + "2 COMBO"
// in one frame, the "LEVEL n" stage, and the combo HUD alone vs the original (s08_combat.mp4 @ 39 s, extracted to
// work/PlayableGame2/HudGaps/ExpCombo/ref_combo_39s.png; masked SSIM over the "2 COMBO" rect). Each scene also
// renders at 21:9, where the bottom-right anchored group must match the 4:3 render moved to the right edge.
#include "ui_shot.hpp"

#include "engine/hud_exp.hpp"

#include <functional>

namespace hudexp {
using namespace joa::engine;
using uishot::Rect;

constexpr float kDt = 1.0f / 30.0f;

// Draws `frames` HUD frames of `setup`-prepared state at w x h and reads the last one back.
inline Image render(RendererKind kind, const char* game, int w, int h, const std::function<void(HudModel&)>& setup, int frames) {
    Options o;
    o.renderer = kind;
    o.width = w;
    o.height = h;
    o.game_dir = game;
    o.vk_validation = kind == RendererKind::Vulkan;
    Platform platform(o, true);
    std::unique_ptr<IRenderer> r =
        kind == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
    Image img;
    {
        joa::formats::Vfs vfs(game);
        Ui ui(vfs, *r);
        HudModel hud;
        setup(hud);
        for (int i = 0; i < frames; ++i) {
            const glm::ivec2 size = platform.pixel_size();
            std::vector<UiQuad> quads;
            ui.begin({0, 0, size.x, size.y}, quads, {}, kDt);
            hud_exp(ui, hud);
            FrameData f;
            f.view.viewport = {0, 0, size.x, size.y};
            f.ui = quads;
            r->begin_frame(size);
            r->submit(f);
            if (i + 1 == frames) img = r->read_back();
            r->end_frame();
        }
    }
    return img;
}

struct Scene {
    const char* name;
    std::function<void(HudModel&)> setup;
    int frames;
    Rect group;  // bottom-right anchored area checked at 21:9
};

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
    // 6 frames: EXPTEXT2 settled (200 ms), 247 rolled to "47" (hundreds not shown yet); LEVELUP3 hold; "2 COMBO".
    const Scene scenes[] = {
        {"all",
         [](HudModel& h) {
             hud_exp_add(h.exp, 247);
             hud_exp_level_up(h.exp, 2);
             h.exp.level_state = 4;
             hud_exp_combo(h.exp, 2);
             h.exp.combo_hits = 2;
             h.exp.combo_state = 3;
         },
         6, {600, 250, 424, 300}},
        {"level", [](HudModel& h) { hud_exp_level_up(h.exp, 12), h.exp.level_state = 6; }, 2, {700, 340, 324, 90}},
        {"combo", [](HudModel& h) { hud_exp_combo(h.exp, 2), h.exp.combo_hits = 2, h.exp.combo_state = 3; }, 2,
         {760, 270, 264, 90}},
    };
    for (const Scene& s : scenes) {
        Image a, wide;
        try {
            a = render(kind, game, 1024, 768, s.setup, s.frames);
            wide = render(kind, game, 2520, 1080, s.setup, s.frames);
        } catch (const std::exception& e) {
            if (kind == RendererKind::Vulkan && uishot::vulkan_missing(e)) {
                std::fprintf(stderr, "SKIP: %s\n", e.what());
                return 77;
            }
            std::fprintf(stderr, "FAIL %s: %s\n", s.name, e.what());
            ++g_failures;
            continue;
        }
        write_png(uishot::out_path(std::string("hud_exp_") + tag + "_" + s.name + "_4x3.png"), a.width, a.height, a.rgba);
        write_png(uishot::out_path(std::string("hud_exp_") + tag + "_" + s.name + "_21x9.png"), wide.width, wide.height, wide.rgba);
        CHECK(a.width == 1024 && a.height == 768 && wide.width == 2520 && wide.height == 1080);
        if (a.width != 1024 || wide.width != 2520) continue;
        const uishot::Gray ga = uishot::to_gray(a.rgba.data(), 1024, 768, 1024);
        double lit = 0;
        for (int y = s.group.y; y < s.group.y + s.group.h; ++y)
            for (int x = s.group.x; x < s.group.x + s.group.w; ++x) lit += ga.at(x, y) > 40.0f;
        CHECK(lit > 500);  // the group was drawn where the exe puts it
        // 21:9: the canvas is 1440x1080 with its right edge at the screen's right edge.
        const uishot::Gray gw = uishot::resample(wide, 2520 - 1440, 0, 1440, 1080);
        const double sw = uishot::mean_ssim(gw, ga, {s.group});
        std::fprintf(stderr, "%s %-6s 21:9 vs 4:3 (bottom-right) ssim = %.3f (min 0.85)\n", tag, s.name, sw);
        CHECK(sw >= 0.85);
        if (std::string(s.name) == "combo") {
            uishot::Gray ref;
            if (!uishot::load_reference(std::string(game) + "/work/PlayableGame2/HudGaps/ExpCombo/ref_combo_39s.png", ref) ||
                ref.w != 1024) {
                std::fprintf(stderr, "SKIP compare combo: reference frame unavailable\n");
                continue;
            }
            // The world behind the HUD differs (black here), so SSIM is low in absolute terms; the position check is
            // the shift of our render (±8 px) that best matches the reference, which must be ~0.
            const Rect box{784, 282, 240, 62};
            auto shifted = [&](int dx, int dy) {
                uishot::Gray g = ga;
                for (int y = box.y; y < box.y + box.h; ++y)
                    for (int x = box.x; x < box.x + box.w; ++x) g.v[std::size_t(y) * 1024 + x] = ga.at(x - dx, y - dy);
                return g;
            };
            const double v = uishot::mean_ssim(ga, ref, {box});
            double best = -1;
            int bx = 0, by = 0;
            for (int dy = -8; dy <= 8; ++dy)
                for (int dx = -8; dx <= 8; ++dx)
                    if (const double t = uishot::mean_ssim(shifted(dx, dy), ref, {box}); t > best) best = t, bx = dx, by = dy;
            std::fprintf(stderr, "%s combo vs s08_combat@39s ssim = %.3f (min 0.18), best shift (%d,%d) ssim %.3f\n", tag, v, bx,
                         by, best);
            CHECK(v >= 0.18);
            CHECK(std::abs(bx) <= 2 && std::abs(by) <= 2);
        }
    }
    SDL_Quit();
    return test_result();
}

}  // namespace hudexp
