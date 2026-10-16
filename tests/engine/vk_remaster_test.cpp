// Vulkan remaster presets: Original bit-identical after remastered frames; Enhanced / Ultra (TAA, upscale=none) / FSR1 parity
// with OpenGL (same shaders in shaders/common, same CPU constants in post_frame.cpp / pbr_env.cpp); image-quality
// keys on the Original look; validation clean. Thresholds: PSNR >= 40 dB, mean abs diff <= 1.0 (8-bit RGB).
// RX 7900 XTX radeonsi vs RADV measured: Enhanced (4x MSAA) 44.8 dB / 0.34, Ultra 47.1 / 0.54, FSR1 67% 44.2 / 0.41,
// Original msaa=4 aa=smaa 49.5 / 0.06, Original fxaa + 2x supersampling 59.0 / 0.01, S1L1 Enhanced 42.0 / 0.42
// (MSAA resolves differ slightly between the drivers: a few hundred edge pixels off by > 32). The noise of PCF/SSAO/volumetric dither is identical in both (same IGN seed).
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
        // Parity at Ultra's former RCAS 0.3: full strength (Ultra's 1.0) amplifies sub-LSB GL/Vulkan differences
        // (measured 38.6 dB / 1.50 at 1.0, 47.7 dB / 0.44 at 0.3 with motion-vector TAA).
        Options go = demo_options(RendererKind::OpenGL, p), vo = demo_options(RendererKind::Vulkan, p);
        if (p == GraphicsPreset::Ultra) go.graphics.apply("graphics.sharpness", "0.3"), vo.graphics.apply("graphics.sharpness", "0.3");
        // Ultra's FSR 3 is Vulkan-only (OpenGL runs it as FSR1): parity of the TAA path it falls back to, at native size.
        if (p == GraphicsPreset::Ultra) go.graphics.apply("graphics.upscale", "none"), vo.graphics.apply("graphics.upscale", "none");
        try {
            gl = render(go, 6);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "SKIP: OpenGL unavailable (%s)\n", e.what());
            return 77;
        }
        // raster parity: Ultra's ray tracing (vk_rt_*) off
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
    // AMD FSR 3.1.4 (Ultra's upscaler) and frame generation: validation-clean full-size frames close to native TAA.
    {
        Options o = demo_options(RendererKind::Vulkan, GraphicsPreset::Ultra);
        for (const char* k : {"raytracing.reflections", "raytracing.shadows", "raytracing.gi"}) o.graphics.apply(k, "false");
        o.vk_validation = true;
        Options taa = o;
        taa.graphics.apply("graphics.upscale", "none");
        const Image native = render(taa, 8);
        for (const char* fg : {"false", "true"}) {
            Options f = o;
            CHECK(f.graphics.apply("graphics.frame_generation", fg).empty());
            const Image vk = render(f, 8);
            save(vk, *fg == 't' ? "vk_remaster_fsr3_fg_vk.png" : "vk_remaster_fsr3_vk.png");
            const Diff d = diff(native.rgba, vk.rgba);
            std::fprintf(stderr, "Ultra FSR 3 quality%s vs native TAA: mean abs diff %.3f, PSNR %.2f dB\n", *fg == 't' ? " + frame generation" : "",
                         d.mad, d.psnr);
            CHECK(vk.width == native.width && vk.height == native.height && d.psnr >= 25.0);
        }
    }
    // Image-quality keys on the Original look (no remaster path): GL/Vulkan parity, and the image actually changes.
    const std::vector<std::vector<std::pair<const char*, const char*>>> quality = {
        {{"msaa", "4"}, {"aa", "smaa"}}, {{"aa", "fxaa"}, {"render_scale", "2"}, {"sharpness", "0.5"}, {"anisotropy", "16"}}};
    for (const auto& keys : quality) {
        Options o = demo_options(RendererKind::OpenGL, GraphicsPreset::Original);
        std::string name = "original";
        for (const auto& [k, v] : keys) {
            CHECK(o.graphics.apply(std::string("graphics.") + k, v).empty());
            name += std::string(" ") + k + "=" + v;
        }
        const GraphicsSettings g = o.graphics.resolve();
        CHECK(g.preset == GraphicsPreset::Original && !g.remastered() && g.original_post());
        const auto set = [&](int frame, Platform&, IRenderer& r) {
            if (frame == 0) r.set_graphics(g);
        };
        const Image gl = render(o, 4, set);
        o.renderer = RendererKind::Vulkan;
        o.vk_validation = true;
        const Image vk = render(o, 4, set);
        save(vk, keys.size() == 2 ? "vk_remaster_original_msaa_smaa_vk.png" : "vk_remaster_original_fxaa_ss2_vk.png");
        parity(name.c_str(), gl, vk, 40.0, 1.0);
        CHECK(diff(vk.rgba, vk_plain.rgba).mad > 0.05);  // smoothed edges differ from the plain Original frame
    }
    // Blend state 1 (hair): near layer not overdrawn by later far layers, sub-reference alpha discarded.
    for (GraphicsPreset p : {GraphicsPreset::Original, GraphicsPreset::Enhanced, GraphicsPreset::Ultra}) {
        const LayerShot s = render_layers(RendererKind::Vulkan, p);
        save(s.img, p == GraphicsPreset::Original ? "vk_remaster_layers_original.png" : p == GraphicsPreset::Enhanced ? "vk_remaster_layers_enhanced.png" : "vk_remaster_layers_ultra.png");
        check_layers(s, preset_name(p));
    }
    if (game_dir()) {
        const auto gl = s1l1(RendererKind::OpenGL, "enhanced", "vk_remaster_s1l1_gl.png");
        const auto vk = s1l1(RendererKind::Vulkan, "enhanced", "vk_remaster_s1l1_vk.png");
        const Diff d = diff(gl, vk);
        std::fprintf(stderr, "S1L1 enhanced GL vs Vulkan: mean abs diff %.3f, PSNR %.2f dB, pixels off by >32: %zu\n", d.mad, d.psnr, d.outliers);
        CHECK(d.psnr >= 40.0);
        CHECK(d.mad <= 1.0);
        // Enhanced as sharp as Original (Laplacian variance: 1.00x GL, 1.01x Vulkan; the old TAA default 0.09x).
        const auto orig = s1l1(RendererKind::Vulkan, "original", "vk_remaster_s1l1_original_vk.png");
        const double s0 = laplacian_variance(orig, 640), s1 = laplacian_variance(vk, 640);
        std::fprintf(stderr, "S1L1 Vulkan sharpness (Laplacian variance) original %.1f enhanced %.1f\n", s0, s1);
        CHECK(s1 >= 0.9 * s0);
    }
    CHECK(vk_validation_errors() == 0);
    if (std::getenv("JOA_REQUIRE_VALIDATION")) CHECK(vk_validation_active());
    return test_result();
}
