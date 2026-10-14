// OpenGL remaster presets: Original stays bit-identical to the faithful path (also after the renderer
// ran Enhanced frames), Enhanced/Ultra produce a lit, tone-mapped image close in brightness to Original.
// S1L1 checks run when the original data is present. Exit 77 without a video driver.
#include "remaster_util.hpp"
#include "world_shot.hpp"

#include <SDL3/SDL_init.h>

using namespace rmtest;

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();

    // 1. Original: default renderer == explicit Original == Original after Enhanced/Ultra frames (no state leaks).
    const Image plain = render_demo(demo_options(RendererKind::OpenGL, GraphicsPreset::Original), 3);
    const Image toggled = render_demo(demo_options(RendererKind::OpenGL, GraphicsPreset::Original), 4, [](int frame, Platform&, IRenderer& r) {
        r.set_graphics(preset_settings(frame == 0 ? GraphicsPreset::Enhanced : frame == 1 ? GraphicsPreset::Ultra : GraphicsPreset::Original));
    });
    save(plain, "gl_remaster_original.png");
    CHECK(!plain.rgba.empty() && plain.rgba == toggled.rgba);

    // 2. Enhanced / Ultra demo frames.
    for (GraphicsPreset p : {GraphicsPreset::Enhanced, GraphicsPreset::Ultra}) {
        const Image img = render_demo(demo_options(RendererKind::OpenGL, p), 6);
        save(img, p == GraphicsPreset::Enhanced ? "gl_remaster_enhanced.png" : "gl_remaster_ultra.png");
        const Diff d = diff(plain.rgba, img.rgba);
        const double l0 = mean_luma(plain.rgba), l1 = mean_luma(img.rgba);
        std::fprintf(stderr, "%s vs original: mean abs diff %.2f, luma %.1f -> %.1f, %zu colours\n", preset_name(p), d.mad, l0, l1,
                     distinct_colours(img.rgba));
        CHECK(img.width == plain.width && img.height == plain.height);
        CHECK(d.mad > 0.5);                         // something changed
        CHECK(std::fabs(l1 - l0) < 0.25 * l0);      // art direction: overall brightness kept
        CHECK(distinct_colours(img.rgba) > 300);
    }

    // 3. FSR1 upscale from 67% + RCAS: still a full-size, sane frame.
    {
        Options o = demo_options(RendererKind::OpenGL, GraphicsPreset::Custom);
        CHECK(o.graphics.apply("graphics.upscale", "fsr1").empty() && o.graphics.apply("graphics.render_scale", "0.67").empty() &&
              o.graphics.apply("graphics.sharpness", "0.5").empty());
        const Image img = render_demo(o, 4);
        save(img, "gl_remaster_fsr1.png");
        CHECK(img.width == 640 && img.height == 360 && distinct_colours(img.rgba) > 200);
    }

    // 3b. TAA on a static view keeps detail: history read at the un-jittered position (Catmull-Rom), not a
    // sub-texel bilinear re-sample every frame (Laplacian variance vs no AA: 0.15 before the fix, 0.53 after).
    {
        Options o = demo_options(RendererKind::OpenGL, GraphicsPreset::Custom);
        CHECK(o.graphics.apply("graphics.aa", "none").empty() && o.graphics.apply("graphics.ssao", "false").empty());
        const Image none = render_demo(o, 24);
        CHECK(o.graphics.apply("graphics.aa", "taa").empty());
        const Image taa = render_demo(o, 24);
        save(taa, "gl_remaster_taa.png");
        const double s0 = laplacian_variance(none.rgba, none.width), s1 = laplacian_variance(taa.rgba, taa.width);
        std::fprintf(stderr, "TAA static view sharpness: none %.1f taa %.1f (%.2f)\n", s0, s1, s1 / s0);
        CHECK(s1 >= 0.35 * s0);
    }

    // 4. Blend state 1 (hair): near layer not overdrawn by later far layers, sub-reference alpha discarded.
    for (GraphicsPreset p : {GraphicsPreset::Original, GraphicsPreset::Enhanced, GraphicsPreset::Ultra}) {
        const LayerShot s = render_layers(RendererKind::OpenGL, p);
        save(s.img, p == GraphicsPreset::Original ? "gl_remaster_layers_original.png" : p == GraphicsPreset::Enhanced ? "gl_remaster_layers_enhanced.png" : "gl_remaster_layers_ultra.png");
        check_layers(s, preset_name(p));
    }

    // 5. S1L1 start view.
    if (game_dir()) {
        const auto orig = s1l1(RendererKind::OpenGL, nullptr, "gl_remaster_s1l1_default.png");
        const auto orig2 = s1l1(RendererKind::OpenGL, "original", "gl_remaster_s1l1_original.png");
        CHECK(!orig.empty() && orig == orig2);
        const auto enh = s1l1(RendererKind::OpenGL, "enhanced", "gl_remaster_s1l1_enhanced.png");
        check_world_shot(out_path("gl_remaster_s1l1_enhanced.png"));
        const double l0 = mean_luma(orig), l1 = mean_luma(enh);
        std::fprintf(stderr, "S1L1 enhanced vs original: luma %.1f -> %.1f, mean abs diff %.2f\n", l0, l1, diff(orig, enh).mad);
        CHECK(std::fabs(l1 - l0) < 0.25 * l0);
        // Enhanced as sharp as Original (Laplacian variance: 1.00x GL, 1.01x Vulkan; the old TAA default 0.09x).
        const double s0 = laplacian_variance(orig, 640), s1 = laplacian_variance(enh, 640);
        std::fprintf(stderr, "S1L1 sharpness (Laplacian variance) original %.1f enhanced %.1f\n", s0, s1);
        CHECK(s1 >= 0.9 * s0);
    } else {
        std::fprintf(stderr, "no original data: S1L1 part skipped\n");
    }
    return test_result();
}
