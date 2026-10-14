#pragma once
// HudGaps crit through a real backend (gl_/vk_hud_crit_test.cpp): the mission-1 sample HUD plus a crit sequence
// "AAAB" with two keys entered, the CA banner 500 ms after CRITATT (Glow01 half in) and both kill interface panels.
// No original frame shows the crit HUD (mission 1 enables critical attacks after 600 s of scenario time; the
// reference captures are shorter), so the checks are placement (art where Crit_drawHUD / FUN_006df180 draw it, at
// 4:3 and at the 21:9 anchors) and the untouched HUD panels vs s04_04_mission1_hud.png (masked SSIM).
#include "ui_cases_hud.hpp"

namespace critshot {
using namespace joa::engine;

inline Image render(RendererKind kind, int w, int h, const char* game) {
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
        UiRunner ui(vfs, *r, "hud");
        HudModel::Crit& c = ui.hud.crit;
        c.enabled = true, c.keys = "AAAB", c.progress = 2, c.banner = 1, c.banner_ms = 500.0f;
        ui.hud.kill_panel[0] = {true, 0, "3/10"};
        ui.hud.kill_panel[1] = {true, 3, "2:0"};
        std::vector<UiQuad> quads;
        for (int i = 0; i < 3; ++i) {
            const glm::ivec2 size = platform.pixel_size();
            const glm::ivec4 vp{0, 0, size.x, size.y};
            quads.clear();
            ui.frame(vp, {}, 0.0f, quads);   // dt 0: the banner clock stays at 500 ms
            FrameData f;
            f.view.viewport = vp;
            f.ui = quads;
            r->begin_frame(size);
            r->submit(f);
            if (i == 2) img = r->read_back();
            r->end_frame();
        }
    }
    return img;
}

// Mean luma of the pixel rect (x, y, w, h) scaled by s from design units (origin ox, oy).
inline double luma(const Image& img, double ox, double oy, double s, uishot::Rect d) {
    double sum = 0;
    int n = 0;
    for (int y = int(oy + d.y * s); y < int(oy + (d.y + d.h) * s); ++y)
        for (int x = int(ox + d.x * s); x < int(ox + (d.x + d.w) * s); ++x) {
            const std::uint8_t* p = &img.rgba[(std::size_t(y) * std::size_t(img.width) + std::size_t(x)) * 4];
            sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2], ++n;
        }
    return n ? sum / n : 0.0;
}

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
    Image a, wide;
    try {
        a = render(kind, 1024, 768, game);
        wide = render(kind, 2520, 1080, game);
    } catch (const std::exception& e) {
        if (kind == RendererKind::Vulkan && uishot::vulkan_missing(e)) {
            std::fprintf(stderr, "SKIP: %s\n", e.what());
            return 77;
        }
        throw;
    }
    write_png(uishot::out_path(std::string("hud_crit_") + tag + "_4x3.png"), a.width, a.height, a.rgba);
    write_png(uishot::out_path(std::string("hud_crit_") + tag + "_21x9.png"), wide.width, wide.height, wide.rgba);
    CHECK(a.width == 1024 && a.height == 768 && wide.width == 2520 && wide.height == 1080);
    if (a.width != 1024 || wide.width != 2520) return test_result();

    // Design rects: key icons (0x12 + 0x2d i, H-0xa1), CA banner (W/2-256, H/2-100-32), kill panels (0x1ac, 8 / 0x33),
    // and an empty gap between icon 3 and the banner row.
    const uishot::Rect icons[4] = {{18, 607, 40, 40}, {63, 607, 40, 40}, {108, 607, 40, 40}, {153, 607, 40, 40}};
    const uishot::Rect ca{256, 252, 512, 64}, kill0{428, 8, 176, 40}, kill1{428, 51, 176, 40}, gap{200, 560, 40, 40};
    const double s = 1080.0 / 768.0;
    struct Frame { const Image& img; double left, bottom, centre; };   // x origin of the left / centre frames, y of bottom
    for (const Frame& f : {Frame{a, 0, 0, 0}, Frame{wide, 0, 1080 - 768 * s, (2520 - 1024 * s) / 2}}) {
        const double sc = f.img.width == 1024 ? 1.0 : s;
        double lit = 0, off = 0;
        for (int i = 0; i < 4; ++i) {
            const double l = luma(f.img, f.left, f.bottom, sc, icons[i]);
            std::fprintf(stderr, "%s %dx%d icon %d luma %.1f\n", tag, f.img.width, f.img.height, i, l);
            CHECK(l > 8.0);
            (i < 2 ? lit : off) += l;
        }
        CHECK(lit > off);   // Light (lit) brighter than LightOFF / HeavyOFF
        const double b = luma(f.img, f.centre, 0, sc, ca), g = luma(f.img, f.left, f.bottom, sc, gap);
        const double k0 = luma(f.img, 0, 0, sc, kill0), k1 = luma(f.img, 0, 0, sc, kill1);
        std::fprintf(stderr, "%s %dx%d banner %.1f gap %.1f kill %.1f %.1f\n", tag, f.img.width, f.img.height, b, g, k0, k1);
        CHECK(b > 20.0 && k0 > 8.0 && k1 > 8.0 && g < 1.0);
    }

    // The rest of the HUD is unchanged: panel strip / party / minimap edges vs the original (as engine.gl_ui_hud).
    uishot::Gray ref;
    const std::string shots = std::string(game) + "/work/reference/shots/";
    if (uishot::load_reference(shots + "s04_04_mission1_hud.png", ref) && ref.w == 1024 && ref.h == 768) {
        const double ss = uishot::mean_ssim(uishot::to_gray(a.rgba.data(), a.width, a.height, a.width), ref, hudcase::kPanels);
        std::fprintf(stderr, "%s hud panels ssim %.3f (min 0.70)\n", tag, ss);
        CHECK(ss >= 0.70);
    }
    SDL_Quit();
    return test_result();
}

}  // namespace critshot
