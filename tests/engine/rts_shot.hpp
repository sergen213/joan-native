#pragma once
// RTS overlay through the real client (--rts-unlocked, mission 1): Joan walks out of the trees, F2, a drag box over the
// view, release (box selection), click on empty ground (deselect). No original capture shows RTS mode (the s04_06_rts_*
// shots were taken in mission 1, where F2 is gated to scenario >= 5), so the checks are pixel facts:
// - drag box (Rts_draw 0x4450f0, 1 px white alpha 0xcc): its top edge row is near-white while dragging, not after;
// - selection ring (select.SMO / Slash_01, cyan): cyan pixels in the 3D view while Joan's team is selected, none after
//   the deselect click.
#include "engine/app.hpp"
#include "png_read.hpp"
#include "ui_shot.hpp"

#include <fstream>

namespace rtsshot {
using namespace joa::engine;

// Fraction of near-white pixels (every channel >= 180) on row y between x0 and x1 (either of rows y-1, y, y+1).
inline double white_row(const Png& img, int y, int x0, int x1) {
    int n = 0;
    for (int x = x0; x < x1; ++x) {
        bool hit = false;
        for (int dy = -1; dy <= 1 && !hit; ++dy) {
            const unsigned char* p = img.at(x, y + dy);
            hit = p[0] >= 180 && p[1] >= 180 && p[2] >= 180;
        }
        n += hit;
    }
    return double(n) / double(x1 - x0);
}

// Cyan ring pixels in the 3D view above the attention message (y < 505), left of the minimap (x < 1070).
inline int cyan(const Png& img) {
    int n = 0;
    for (int y = 0; y < std::min(img.h, 505); ++y)
        for (int x = 0; x < std::min(img.w, 1070); ++x) {
            const unsigned char* p = img.at(x, y);
            n += p[2] > 120 && p[2] > p[0] + 40 && p[1] > p[0] + 20;
        }
    return n;
}

inline int run(RendererKind kind, const char* tag) {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/rts.cm")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    const std::string out = uishot::out_path("");
    setenv("XDG_DATA_HOME", (out + "rts-xdg").c_str(), 1);
    const std::string drag = out + tag + "_rts_drag.png", sel = out + tag + "_rts_selected.png", desel = out + tag + "_rts_deselected.png";
    const std::string script = out + tag + "_rts.txt";
    // RtsFinish: no walk to the cliff first. The hero is AI-driven in RTS now (the player controller used to freeze
    // her there) and from the cliff her mission task sends her toward the English camp, out of view.
    std::ofstream(script) << "40 tap Escape\n340 tap F2\n341 pointer 512 300\n"
                             "530 pointer 60 60\n531 button left down\n540 pointer 900 640\n545 shot " << drag << "\n"
                             "550 pointer 980 740\n552 button left up\n560 shot " << sel << "\n"
                             "580 pointer 120 140\n581 clickbtn left 2\n590 shot " << desel << "\n";
    Options o;
    o.renderer = kind;
    o.width = 1280, o.height = 720;
    o.game_dir = game;
    o.new_game = true;
    o.no_movies = true;
    o.rts_unlocked = true;
    o.seed = 1;
    o.autoplay = script;
    try {
        CHECK(joa::engine::run(o) == 0);
    } catch (const std::exception& e) {
        if (kind == RendererKind::Vulkan && uishot::vulkan_missing(e)) return 77;
        throw;
    }
    Png a, b, c;
    CHECK(read_png(drag, a) && read_png(sel, b) && read_png(desel, c));
    if (g_failures) return test_result();
    // Canvas (60, 60)-(900, 640) at 1280x720: scale 0.9375, x origin 160 -> top edge y 56, x 216..1004.
    const double edge_drag = white_row(a, 56, 260, 960), edge_after = white_row(c, 56, 260, 960);
    const int ring_sel = cyan(b), ring_desel = cyan(c);
    std::fprintf(stderr, "%s drag box top edge white %.2f (after %.2f); ring cyan px selected %d, deselected %d\n", tag, edge_drag,
                 edge_after, ring_sel, ring_desel);
    CHECK(edge_drag >= 0.85 && edge_after <= 0.3);
    // Start position (no cliff walk): measured GL 242 / 104, VK 224 / 104 (name label + flower speckle in both).
    CHECK(ring_sel >= ring_desel + 80);
    return test_result();
}

}  // namespace rtsshot
