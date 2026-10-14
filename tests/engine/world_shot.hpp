#pragma once
// Shared S1L1 start-frame checks for gl_world_render / vk_world_render (640x360 capture).
#include "check.hpp"
#include "png_read.hpp"

#include <cstdlib>
#include <set>
#include <string>

inline void check_world_shot(const std::string& path) {
    Png img;
    CHECK(read_png(path, img));
    CHECK(img.w == 640 && img.h == 360);
    if (g_failures) return;

    // Many distinct colours overall.
    std::set<std::uint32_t> colours;
    for (int y = 0; y < img.h; y += 4)
        for (int x = 0; x < img.w; x += 4) {
            const unsigned char* p = img.at(x, y);
            colours.insert(std::uint32_t(p[0]) << 16 | p[1] << 8 | p[2]);
        }
    std::fprintf(stderr, "distinct colours (4px grid): %zu\n", colours.size());
    CHECK(colours.size() > 500);

    // Terrain: the bottom quarter is textured ground, i.e. mostly differs from the top-left pixel
    // (sky/clear) and varies from pixel to pixel.
    const unsigned char* sky = img.at(2, 2);
    int differ = 0, total = 0;
    std::set<std::uint32_t> ground;
    for (int y = img.h * 3 / 4; y < img.h; y += 2)
        for (int x = 0; x < img.w; x += 2, ++total) {
            const unsigned char* p = img.at(x, y);
            differ += std::abs(p[0] - sky[0]) + std::abs(p[1] - sky[1]) + std::abs(p[2] - sky[2]) > 12;
            ground.insert(std::uint32_t(p[0]) << 16 | p[1] << 8 | p[2]);
        }
    std::fprintf(stderr, "bottom quarter: %d/%d differ from sky, %zu colours\n", differ, total, ground.size());
    CHECK(differ * 10 > total * 8);
    CHECK(ground.size() > 200);

    // Sky dome: blue pixels (b well above r, bright) in the top quarter, between the trees.
    int sky_px = 0;
    for (int y = 0; y < img.h / 4; y += 2)
        for (int x = 0; x < img.w; x += 2) {
            const unsigned char* p = img.at(x, y);
            sky_px += p[2] > p[0] + 25 && p[2] >= p[1] && p[2] > 150;
        }
    std::fprintf(stderr, "sky pixels (top quarter, 2px grid): %d\n", sky_px);
    CHECK(sky_px > 100);

    // Haze: the RE'd scattering fog is blue (Normal_12 fog colour 106,146,193): the distant valley band
    // above Joan's head is bluish grey, as in s04_04_mission1_hud.png (82,104,117).
    long r = 0, b = 0;
    for (int y = img.h * 17 / 100; y < img.h * 30 / 100; ++y)
        for (int x = img.w * 35 / 100; x < img.w * 55 / 100; ++x) r += img.at(x, y)[0], b += img.at(x, y)[2];
    std::fprintf(stderr, "haze band mean r %ld b %ld\n", r, b);
    CHECK(b > r);
}

// Count of pixels in [x0,x1)x[y0,y1) matching pred(r,g,b).
template <class Pred>
inline int count_px(const Png& img, int x0, int y0, int x1, int y1, Pred pred) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const unsigned char* p = img.at(x, y);
            n += pred(p[0], p[1], p[2]);
        }
    return n;
}

// Torch view (camera 12037,13600,250,90,-8; TorchStand at 12035,13853): TorchFire01 particles burn
// orange-yellow above the torch head near the image centre.
inline void check_torch_shot(const std::string& path) {
    Png img;
    CHECK(read_png(path, img));
    CHECK(img.w == 640 && img.h == 360);
    if (g_failures) return;
    const int fire = count_px(img, 270, 110, 370, 210, [](int r, int g, int b) { return r > 200 && g > 100 && r > b + 90; });
    std::fprintf(stderr, "fire pixels around the torch: %d\n", fire);
    CHECK(fire > 60);
}

// Moat view (camera 11000,3200,1350,240,-20): the BumpEnvWaterRiver surface (vertex colour 65,51,80,
// spheremap reflection, half alpha) tints the moat bed dark blue across the middle of the frame.
inline void check_water_shot(const std::string& path) {
    Png img;
    CHECK(read_png(path, img));
    CHECK(img.w == 640 && img.h == 360);
    if (g_failures) return;
    const int blue = count_px(img, 100, 130, 540, 250, [](int r, int g, int b) { return b > r + 15 && b > g + 5 && b < 160; });
    std::fprintf(stderr, "water pixels in the moat band: %d / %d\n", blue, 440 * 120);
    CHECK(blue * 2 > 440 * 120);
}
