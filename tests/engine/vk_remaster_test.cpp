// Vulkan remaster presets: Original bit-identical after remastered frames; Enhanced / Ultra / FSR1 parity
// with OpenGL (same shaders in shaders/common, same CPU constants in post_frame.cpp / pbr_env.cpp);
// validation clean. Thresholds: PSNR >= 40 dB, mean abs diff <= 1.0 (8-bit RGB). RX 7900 XTX radeonsi vs
// RADV measured: Enhanced 53.1 dB / 0.26, Ultra 52.4 / 0.30, FSR1 67% 51.6 / 0.32, S1L1 Enhanced 50.8 / 0.38
// (no pixel off by > 32). The noise of PCF/SSAO/volumetric dither is identical in both (same IGN seed).
// Exit 77 without Vulkan 1.3 or GL.
#include "remaster_util.hpp"

#include <SDL3/SDL_init.h>

using namespace rmtest;

namespace {

Image render(Options o, int frames, const FrameHook& hook = {}) { return render_demo(o, frames, hook); }

void parity(const char* what, const Image& gl, const Image& vk, double min_psnr, double max_mad) {
    const Diff d = diff(gl.rgba, vk.rgba);
    std::fprintf(stderr, "%s GL vs Vulkan: mean abs diff %.3f, PSNR %.2f dB, pixels off by >32: %zu\n", what, d.mad, d.psnr, d.outliers);
    CHECK(gl.width == vk.width && gl.height == vk.height);
    CHECK(d.psnr >= min_psnr);
    CHECK(d.mad <= max_mad);
}

}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    Image vk_plain;
    try {
        vk_plain = render(demo_options(RendererKind::Vulkan, GraphicsPreset::Original), 3);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", vulkan_missing(e) ? "SKIP" : "FAIL", e.what());
        return vulkan_missing(e) ? 77 : 1;
    }
    // Original after Enhanced/Ultra frames == plain Original.
    const Image vk_toggled = render(demo_options(RendererKind::Vulkan, GraphicsPreset::Original), 4, [](int frame, Platform&, IRenderer& r) {
        r.set_graphics(preset_settings(frame == 0 ? GraphicsPreset::Enhanced : frame == 1 ? GraphicsPreset::Ultra : GraphicsPreset::Original));
    });
    CHECK(!vk_plain.rgba.empty() && vk_plain.rgba == vk_toggled.rgba);

    for (GraphicsPreset p : {GraphicsPreset::Enhanced, GraphicsPreset::Ultra}) {
        Image gl;
        try {
            gl = render(demo_options(RendererKind::OpenGL, p), 6);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "SKIP: OpenGL unavailable (%s)\n", e.what());
            return 77;
        }
        Options vo = demo_options(RendererKind::Vulkan, p);  // raster parity: Ultra's ray tracing (vk_rt_*) off
        for (const char* k : {"raytracing.reflections", "raytracing.shadows", "raytracing.gi"}) vo.graphics.apply(k, "false");
        const Image vk = render(vo, 6);
        const bool enh = p == GraphicsPreset::Enhanced;
        save(gl, enh ? "vk_remaster_enhanced_gl.png" : "vk_remaster_ultra_gl.png");
        save(vk, enh ? "vk_remaster_enhanced_vk.png" : "vk_remaster_ultra_vk.png");
        parity(preset_name(p), gl, vk, 40.0, 1.0);
    }
    {
        Options o = demo_options(RendererKind::OpenGL, GraphicsPreset::Custom);
        CHECK(o.graphics.apply("graphics.upscale", "fsr1").empty() && o.graphics.apply("graphics.render_scale", "0.67").empty() &&
              o.graphics.apply("graphics.sharpness", "0.5").empty() && o.graphics.apply("graphics.ssao", "false").empty());
        const Image gl = render(o, 4);
        o.renderer = RendererKind::Vulkan;
        o.vk_validation = true;
        const Image vk = render(o, 4);
        save(vk, "vk_remaster_fsr1_vk.png");
        parity("custom fsr1 67%", gl, vk, 40.0, 1.0);
    }
    if (game_dir()) {
        const auto gl = s1l1(RendererKind::OpenGL, "enhanced", "vk_remaster_s1l1_gl.png");
        const auto vk = s1l1(RendererKind::Vulkan, "enhanced", "vk_remaster_s1l1_vk.png");
        const Diff d = diff(gl, vk);
        std::fprintf(stderr, "S1L1 enhanced GL vs Vulkan: mean abs diff %.3f, PSNR %.2f dB, pixels off by >32: %zu\n", d.mad, d.psnr, d.outliers);
        CHECK(d.psnr >= 40.0);
        CHECK(d.mad <= 1.0);
    }
    CHECK(vk_validation_errors() == 0);
    if (std::getenv("JOA_REQUIRE_VALIDATION")) CHECK(vk_validation_active());
    return test_result();
}
