#pragma once
// Mission 1 gameplay + HUD screenshots through the real client (run(Options): --new-game, autoplay Esc skips the
// opening cut scene, capture at frame 140) compared with the original's s04_04_mission1_hud.png (1024x768) by masked
// SSIM (ui_shot.hpp windows). Each scored rect is sampled where the widescreen rules put it: the 3D view as the centred
// 4:3 crop (Hor+ keeps the vertical framing), HUD corners at the real screen edges scaled by height / 768.
// Thresholds (docs/PLAYING.md "Screenshot tests", measured GL/VK 0.52..0.60): world 0.45, minimap 0.45, party 0.48,
// panel 0.45 — lighting, grass and idle poses differ from the
// capture (Wine reference vs our renderer) and the tutorial message covers the lower centre, so the score guards
// framing and HUD placement, not pixels.
#include "engine/app.hpp"
#include "png_read.hpp"
#include "ui_shot.hpp"

#include <fstream>

namespace sessionshot {
using namespace joa::engine;
using uishot::Gray;
using uishot::Rect;

enum class Space { World, Left, Right };
struct Region {
    const char* name;
    Rect r;  // 1024x768 reference pixels
    Space space;
    double min_ssim;
};

// 3D view without HUD (Joan, Jean, valley, trees) and the three HUD corners of s04_04_mission1_hud.png.
inline const Region kRegions[] = {
    {"world", {200, 60, 600, 560}, Space::World, 0.45},
    {"player panel", {0, 656, 272, 112}, Space::Left, 0.45},
    {"party", {848, 652, 176, 116}, Space::Right, 0.48},
    {"minimap frame", {806, 2, 216, 214}, Space::Right, 0.45},
};

// Samples `img` into reference space for one region (box filter over the scale).
inline Gray sample(const Png& img, const Region& g) {
    const double s = img.h / 768.0, x0 = (img.w - 1024.0 * s) / 2.0;
    Gray out{1024, 768, std::vector<float>(1024 * 768, 0.0f)};
    for (int y = g.r.y; y < g.r.y + g.r.h; ++y)
        for (int x = g.r.x; x < g.r.x + g.r.w; ++x) {
            const double fx = g.space == Space::World ? x0 + x * s : g.space == Space::Left ? x * s : img.w - (1024.0 - x) * s;
            const int ax = int(fx), ay = int(y * s), bx = std::max(ax + 1, int(fx + s)), by = std::max(ay + 1, int((y + 1) * s));
            double sum = 0;
            int n = 0;
            for (int j = ay; j < by && j < img.h; ++j)
                for (int i = std::max(ax, 0); i < bx && i < img.w; ++i) {
                    const unsigned char* p = img.at(i, j);
                    sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2], ++n;
                }
            out.v[std::size_t(y) * 1024 + x] = float(n ? sum / n : 0);
        }
    return out;
}

inline int run(RendererKind kind, const char* tag) {
    const char* game = std::getenv("JOA_GAME_DIR");
    const std::string ref = game ? std::string(game) + "/work/reference/shots/s04_04_mission1_hud.png" : "";
    if (!game || !joa::formats::Vfs(game).exists("scenario/scenario_01.scr") || !std::filesystem::exists(ref)) {
        std::fprintf(stderr, "SKIP: no original data / reference shot\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    Gray reference;
    CHECK(uishot::load_reference(ref, reference) && reference.w == 1024 && reference.h == 768);
    const std::string out = uishot::out_path("");
    setenv("XDG_DATA_HOME", (out + "session-xdg").c_str(), 1);
    struct Aspect {
        const char* name;
        int w, h;
    };
    for (const Aspect a : {Aspect{"16x9", 1280, 720}, Aspect{"21x9", 2520, 1080}}) {
        const std::string shot = out + tag + "_session_hud_" + a.name + ".png", script = out + tag + "_session_hud.txt";
        std::ofstream(script) << "40 tap Escape\n140 shot " << shot << "\n";
        Options o;
        o.renderer = kind;
        o.width = a.w, o.height = a.h;
        o.game_dir = game;
        o.new_game = true;
        o.seed = 1;
        o.autoplay = script;
        try {
            CHECK(joa::engine::run(o) == 0);
        } catch (const std::exception& e) {
            if (kind == RendererKind::Vulkan && uishot::vulkan_missing(e)) return 77;
            throw;
        }
        Png img;
        CHECK(read_png(shot, img) && img.w == a.w && img.h == a.h);
        if (g_failures) return test_result();
        for (const Region& g : kRegions) {
            const double v = uishot::ssim(sample(img, g), reference, g.r);
            std::fprintf(stderr, "%s %s %s: SSIM %.3f (min %.2f)\n", tag, a.name, g.name, v, g.min_ssim);
            CHECK(v >= g.min_ssim);
        }
    }
    return test_result();
}

}  // namespace sessionshot
