#pragma once
// Screenshot harness for the original-interface tests (gl_ui_*_test.cpp / vk_ui_*_test.cpp).
// Renders a --ui-screen sample through a real window + backend and compares it with the original game's
// reference screenshots (work/reference/shots, 1024x768, never in the repo) with a masked SSIM:
//   - 4:3 run at 1024x768: SSIM (luma, 8x8 windows, stride 4) over each case's include rects;
//   - 21:9 run at 2520x1080: the HUD/menu must land where widescreen rules put it: menus = the 4:3 canvas
//     centred (pillar bars black), HUD = corners at the real edges. The canvas is resampled back to
//     1024x768 (box filter over the 1.40625 scale) and scored with the same rects, threshold - 0.10.
// Thresholds per case are in the case tables; rationale in docs/reverse-engineering/UI_SYSTEM.md.
// Exit 77 without original data, reference shots, a video driver or (Vulkan) a device.
#include "check.hpp"
#include "engine/platform.hpp"
#include "engine/png.hpp"
#include "engine/renderer_gl.hpp"
#include "engine/renderer_vk.hpp"
#include "engine/ui_runner.hpp"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_surface.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace uishot {
using namespace joa::engine;

struct Rect { int x, y, w, h; };  // 1024x768 design pixels

struct ShotCase {
    const char* screen;            // --ui-screen name
    const char* reference;         // file in work/reference/shots ("" = no reference, only widescreen checks)
    glm::vec2 mouse{-1.0f};        // design-space mouse position reproducing the reference hover (-1 = none)
    std::vector<Rect> include;     // rects scored; empty = whole frame
    double min_ssim = 0.80;        // 4:3 threshold (21:9 uses min_ssim - 0.10)
    bool hud = false;              // HUD layout: corners anchored to the real screen edges
    std::vector<Rect> hud_corner_rects = {};  // for HUD at 21:9: rects measured from their nearest corner
};

struct Gray {
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[std::size_t(y) * w + x]; }
};

inline Gray to_gray(const std::uint8_t* rgba, int w, int h, int stride_px) {
    Gray g{w, h, std::vector<float>(std::size_t(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = rgba + (std::size_t(y) * stride_px + x) * 4;
            g.v[std::size_t(y) * w + x] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
        }
    return g;
}

inline bool load_reference(const std::string& path, Gray& out) {
    SDL_Surface* s = SDL_LoadPNG(path.c_str());
    if (!s) return false;
    SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c) return false;
    out = to_gray(static_cast<const std::uint8_t*>(c->pixels), c->w, c->h, c->pitch / 4);
    SDL_DestroySurface(c);
    return true;
}

// Mean SSIM over 8x8 windows (stride 4) fully inside `r`.
inline double ssim(const Gray& a, const Gray& b, Rect r) {
    const double c1 = 6.5025, c2 = 58.5225;  // (0.01*255)^2, (0.03*255)^2
    double sum = 0;
    int n = 0;
    for (int y = r.y; y + 8 <= r.y + r.h; y += 4)
        for (int x = r.x; x + 8 <= r.x + r.w; x += 4) {
            double ma = 0, mb = 0;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) ma += a.at(x + i, y + j), mb += b.at(x + i, y + j);
            ma /= 64, mb /= 64;
            double va = 0, vb = 0, cov = 0;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    const double da = a.at(x + i, y + j) - ma, db = b.at(x + i, y + j) - mb;
                    va += da * da, vb += db * db, cov += da * db;
                }
            va /= 63, vb /= 63, cov /= 63;
            sum += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++n;
        }
    return n ? sum / n : 1.0;
}

inline double mean_ssim(const Gray& a, const Gray& b, const std::vector<Rect>& rects) {
    std::vector<Rect> rs = rects.empty() ? std::vector<Rect>{{0, 0, 1024, 768}} : rects;
    double s = 0, area = 0;
    for (const Rect& r : rs) {
        s += ssim(a, b, r) * double(r.w) * r.h;
        area += double(r.w) * r.h;
    }
    return s / area;
}

// Box-resample the w x h region at (x0, y0) of `img` to 1024x768.
inline Gray resample(const Image& img, double x0, double y0, double w, double h) {
    Gray full = to_gray(img.rgba.data(), img.width, img.height, img.width);
    Gray g{1024, 768, std::vector<float>(1024 * 768)};
    const double sx = w / 1024.0, sy = h / 768.0;
    for (int y = 0; y < 768; ++y)
        for (int x = 0; x < 1024; ++x) {
            const int ax = int(x0 + x * sx), ay = int(y0 + y * sy);
            const int bx = std::max(ax + 1, int(x0 + (x + 1) * sx)), by = std::max(ay + 1, int(y0 + (y + 1) * sy));
            double s = 0;
            int n = 0;
            for (int j = ay; j < by && j < img.height; ++j)
                for (int i = ax; i < bx && i < img.width; ++i) s += full.at(i, j), ++n;
            g.v[std::size_t(y) * 1024 + x] = float(n ? s / n : 0);
        }
    return g;
}

inline std::string out_path(const std::string& name) {
    const char* dir = std::getenv("JOA_TEST_OUT");
    return std::string(dir ? dir : ".") + "/" + name;
}

// Renders `screen` (frames frames so opening animations settle) and returns the read-back image.
inline Image render(RendererKind kind, int w, int h, const ShotCase& c, const char* game, int frames = 4) {
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
        UiRunner ui(vfs, *r, c.screen);
        std::vector<UiQuad> quads;
        for (int i = 0; i < frames; ++i) {
            const glm::ivec2 size = platform.pixel_size();
            const glm::ivec4 vp{0, 0, size.x, size.y};
            UiInput in;
            if (c.mouse.x >= 0.0f) {
                ui.ui.begin(vp, quads, {}, 0.0f);
                ui.ui.frame(c.hud ? Anchor::TopLeft : Anchor::Center);
                in.mouse = ui.ui.to_pixels(c.mouse);
            }
            quads.clear();
            ui.frame(vp, in, 1.0f / 30.0f, quads);
            FrameData f;
            f.view.viewport = vp;
            f.ui = quads;
            r->begin_frame(size);
            r->submit(f);
            if (i + 1 == frames) img = r->read_back();
            r->end_frame();
        }
    }
    return img;
}

inline bool vulkan_missing(const std::exception& e) {
    const std::string m = e.what();
    for (const char* s : {"SDL_Vulkan_LoadLibrary", "Vulkan loader unavailable", "Vulkan 1.3 loader required", "no Vulkan device",
                          "no usable Vulkan 1.3 device"})
        if (m.find(s) != std::string::npos) return true;
    return false;
}

// Runs every case at 4:3 and 21:9; returns the test exit code.
inline int run_cases(RendererKind kind, std::span<const ShotCase> cases) {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    const std::string shots = std::string(game) + "/work/reference/shots/";
    const char* tag = kind == RendererKind::Vulkan ? "vk" : "gl";
    for (const ShotCase& c : cases) {
        Image a, wide;
        try {
            a = render(kind, 1024, 768, c, game);
            wide = render(kind, 2520, 1080, c, game);
        } catch (const std::exception& e) {
            if (kind == RendererKind::Vulkan && vulkan_missing(e)) {
                std::fprintf(stderr, "SKIP: %s\n", e.what());
                return 77;
            }
            std::fprintf(stderr, "FAIL %s: %s\n", c.screen, e.what());
            ++g_failures;
            continue;
        }
        write_png(out_path(std::string("ui_") + tag + "_" + c.screen + "_4x3.png"), a.width, a.height, a.rgba);
        write_png(out_path(std::string("ui_") + tag + "_" + c.screen + "_21x9.png"), wide.width, wide.height, wide.rgba);
        CHECK(a.width == 1024 && a.height == 768 && wide.width == 2520 && wide.height == 1080);
        if (a.width != 1024 || wide.width != 2520) continue;

        // Something was drawn.
        Gray ga = to_gray(a.rgba.data(), a.width, a.height, a.width);
        double mean = 0;
        for (float v : ga.v) mean += v;
        mean /= double(ga.v.size());
        CHECK(mean > 2.0);

        // Widescreen: 1440x1080 canvas centred at x = 540 (menus); HUD corners stay at the screen edges.
        Gray gw;
        if (!c.hud) {
            gw = resample(wide, 540, 0, 1440, 1080);
            double bar = 0;
            for (int y = 0; y < 1080; y += 7)
                for (int x = 0; x < 540; x += 7) bar += wide.rgba[(std::size_t(y) * 2520 + x) * 4] + wide.rgba[(std::size_t(y) * 2520 + 2519 - x) * 4];
            CHECK(bar == 0.0);  // pillar bars are black
        }

        if (*c.reference) {
            Gray ref;
            if (!load_reference(shots + c.reference, ref) || ref.w != 1024 || ref.h != 768) {
                std::fprintf(stderr, "SKIP compare %s: reference %s unavailable\n", c.screen, c.reference);
                continue;
            }
            const double s43 = mean_ssim(ga, ref, c.include);
            std::fprintf(stderr, "%s %-16s ssim 4:3 = %.3f (min %.2f)", tag, c.screen, s43, c.min_ssim);
            CHECK(s43 >= c.min_ssim);
            if (!c.hud) {
                const double sw = mean_ssim(gw, ref, c.include);
                std::fprintf(stderr, ", 21:9 canvas = %.3f (min %.2f)", sw, c.min_ssim - 0.10);
                CHECK(sw >= c.min_ssim - 0.10);
            } else {
                // Each HUD rect is scored after moving it to its nearest real corner: shift by the 21:9 extra width.
                double s = 0, area = 0;
                for (const Rect& r : c.hud_corner_rects) {
                    const bool right = r.x + r.w / 2 > 512, bottom = r.y + r.h / 2 > 384;
                    const double scale = 1080.0 / 768.0;
                    const double x0 = right ? 2520 - (1024 - r.x) * scale : r.x * scale;
                    const double y0 = bottom ? 1080 - (768 - r.y) * scale : r.y * scale;
                    Gray patch = resample(wide, x0 - r.x * scale, y0 - r.y * scale, 1440, 1080);
                    s += ssim(patch, ref, r) * r.w * r.h;
                    area += double(r.w) * r.h;
                }
                if (area > 0) {
                    std::fprintf(stderr, ", 21:9 corners = %.3f (min %.2f)", s / area, c.min_ssim - 0.10);
                    CHECK(s / area >= c.min_ssim - 0.10);
                }
            }
            std::fprintf(stderr, "\n");
        }
    }
    SDL_Quit();
    return test_result();
}

}  // namespace uishot
