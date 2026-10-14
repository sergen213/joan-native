#pragma once
// Save description + thumbnail through the real client (run(Options), 1024x768): mission 1, F7 Save screen, SAVE NEW,
// typed "Before Orleans" (shot: description entry, FUN_0074f230 state 2 — no reference exists, decomp layout only),
// Return, BACK, F8 Load screen, the new save selected (shot): its thumbnail (the world frame captured when F7 opened
// the screen, Session::save <stem>.bmp) is drawn 1:1 in the PIC frame at (601, 205) and matches the BMP's 320x240,
// and the screen around it matches s04_07_load_menu.png (masked SSIM: title bar, heading, page bar, PIC frame top,
// backdrop sides; slots / info / buttons differ because a slot is selected).
#include "engine/app.hpp"
#include "engine/game_ui_bridge.hpp"
#include "png_read.hpp"
#include "ui_shot.hpp"

#include <fstream>

namespace savethumbshot {
using namespace joa::engine;
inline constexpr double kMinSsim = 0.98;  // measured 1.000 (GL and VK): the masked backdrop is pixel-identical

inline int run(RendererKind kind, const char* tag) {
    const char* game = std::getenv("JOA_GAME_DIR");
    const std::string ref = game ? std::string(game) + "/work/reference/shots/s04_07_load_menu.png" : "";
    if (!game || !joa::formats::Vfs(game).exists("scenario/scenario_01.scr") || !std::filesystem::exists(ref)) {
        std::fprintf(stderr, "SKIP: no original data / reference shot\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    const std::string out = uishot::out_path("");
    const std::filesystem::path xdg = out + tag + "-save-thumb-xdg";
    std::filesystem::remove_all(xdg);
    setenv("XDG_DATA_HOME", (xdg / "data").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (xdg / "config").c_str(), 1);
    std::filesystem::create_directories(xdg);
    const std::string typing = out + tag + "_save_description.png", load = out + tag + "_load_thumbnail.png";
    const std::string script = (xdg / "play.txt").string();
    // In game lc = -48: SAVE NEW / CONFIRM at x 262, BACK at x 612 (y 664..694); slot 1 at y 252..288.
    // Shot 4 frames after typing: 133 ms + the 200 ms restart = inside the caret's visible half of its 400 ms cycle.
    std::ofstream(script) << "40 tap Escape\n100 tap F7\n170 click 337 679\n180 type Before Orleans\n184 shot " << typing
                          << "\n210 tap Return\n230 click 687 679\n290 tap F8\n360 click 348 270\n380 shot " << load << "\n";
    Options o;
    o.renderer = kind;
    o.width = 1024, o.height = 768;
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
    Png a, b;
    CHECK(read_png(typing, a) && a.w == 1024 && a.h == 768);
    CHECK(read_png(load, b) && b.w == 1024 && b.h == 768);
    if (g_failures) return test_result();

    // Description entry: white glyphs in the text box (286..738, 623..644).
    int bright = 0;
    for (int y = 623; y < 645; ++y)
        for (int x = 286; x < 500; ++x) bright += a.at(x, y)[0] > 200 && a.at(x, y)[1] > 200 && a.at(x, y)[2] > 200;
    std::fprintf(stderr, "%s description: %d white pixels\n", tag, bright);
    CHECK(bright > 150);

    // Thumbnail: the selected save's BMP drawn 1:1 at (601, 205).
    const auto saves = list_saves(joa::formats::Vfs(game));
    CHECK(!saves.empty() && saves[0].description == "Before Orleans");
    if (saves.empty()) return test_result();
    std::filesystem::path bmp = saves[0].file;
    bmp.replace_extension(".bmp");
    std::ifstream f(bmp, std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    CHECK(d.size() == 393270);
    if (d.size() != 393270) return test_result();
    double diff = 0, level = 0;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) {
            const unsigned char* s = &d[54 + std::size_t(255 - y) * 1536 + std::size_t(x) * 3];  // BGR, bottom-up
            const unsigned char* p = b.at(601 + x, 205 + y);
            for (int c = 0; c < 3; ++c) diff += std::abs(int(p[c]) - int(s[2 - c])), level += s[2 - c];
        }
    diff /= 320.0 * 240 * 3, level /= 320.0 * 240 * 3;
    std::fprintf(stderr, "%s thumbnail: mean |shot - bmp| %.2f, bmp mean level %.1f\n", tag, diff, level);
    CHECK(level > 20.0);  // a real world frame, not black
    CHECK(diff < 8.0);

    uishot::Gray reference;
    CHECK(uishot::load_reference(ref, reference) && reference.w == 1024 && reference.h == 768);
    if (reference.w != 1024) return test_result();
    const uishot::Gray shot = uishot::to_gray(b.rgba.data(), b.w, b.h, b.w);
    const double s = uishot::mean_ssim(shot, reference, {{0, 0, 1024, 200}, {0, 200, 580, 40}, {0, 240, 112, 420}, {932, 200, 92, 250}});
    std::fprintf(stderr, "%s load screen vs s04_07_load_menu: SSIM %.3f (min %.2f)\n", tag, s, kMinSsim);
    CHECK(s >= kMinSsim);
    return test_result();
}

}  // namespace savethumbshot
