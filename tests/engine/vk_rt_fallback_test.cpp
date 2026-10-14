// Ray tracing is optional (docs/RAYTRACING.md): capability decision, `[raytracing]` keys, TLAS instance classes,
// probe-grid addressing; a device forced unsupported (JOA_VK_DISABLE_RT=1) renders Ultra on the raster path
// bit-identically to Ultra with the RT keys off, and so does OpenGL; on RT hardware the RT frame differs.
// Validation clean. Exit 77 without a video driver / Vulkan 1.3.
#include "remaster_util.hpp"

#include "engine/rt_scene.hpp"

#include <SDL3/SDL_init.h>

#include <cstdlib>

using namespace rmtest;

namespace {

GraphicsSettings ini(const char* text) {
    Options o;
    CHECK(apply_settings_ini(text, o).empty());
    return o.graphics.resolve();
}

Options rt_off(Options o) {
    for (const char* k : {"raytracing.reflections", "raytracing.shadows", "raytracing.gi"}) CHECK(o.graphics.apply(k, "false").empty());
    return o;
}

}  // namespace

int main() {
    {  // capabilities
        RtCaps c;
        CHECK(!c.usable());
        c.acceleration_structure = c.deferred_host_operations = c.ray_query = c.buffer_device_address = c.storage_image_indexing = true;
        CHECK(c.usable() && c.missing().empty());
        c.ray_query = false;
        CHECK(!c.usable() && c.missing() == "VK_KHR_ray_query");
        c.ray_query = true;
        c.forced_off = true;
        CHECK(!c.usable() && c.missing().find("JOA_VK_DISABLE_RT") != std::string::npos);
    }
    {  // settings: Ultra turns RT on, [raytracing] keys override any preset, bad values are errors
        const GraphicsSettings u = ini("[renderer]\npreset=ultra\n");
        CHECK(u.rt.reflections && u.rt.shadows && u.rt.gi && u.rt.rays_per_pixel == 1 && u.rt.denoise);
        const GraphicsSettings e = ini("[renderer]\npreset=enhanced\n");
        CHECK(!e.rt.any());
        const GraphicsSettings u2 = ini("[renderer]\npreset=ultra\n[raytracing]\nreflections=false\nrays_per_pixel=2\nmax_roughness=0.4\n"
                                        "denoise=0\nprobe_density=high\n");
        CHECK(!u2.rt.reflections && u2.rt.shadows && u2.rt.gi && u2.rt.rays_per_pixel == 2 && !u2.rt.denoise);
        CHECK_NEAR(u2.rt.max_roughness, 0.4f, 1e-6f);
        CHECK(u2.rt.probe_density == ProbeDensity::High);
        const GraphicsSettings e2 = ini("[renderer]\npreset=enhanced\n[raytracing]\ngi=true\n");
        CHECK(e2.rt.gi && !e2.rt.shadows && !e2.rt.reflections);
        CHECK(!ini("[raytracing]\nshadows=true\n").remastered());  // Original: RT keys never touch the faithful path
        for (const char* bad : {"[raytracing]\nrays_per_pixel=9\n", "[raytracing]\nmax_roughness=2\n", "[raytracing]\nprobe_density=max\n",
                                "[raytracing]\nshadows=maybe\n", "[raytracing]\nwarp=1\n"}) {
            Options o;
            CHECK(!apply_settings_ini(bad, o).empty());
        }
    }
    {  // TLAS instance classes and culling
        Material m;
        CHECK(rt_instance_class(m, MaterialClass::Building, false).mask == kRtMaskWorld);
        CHECK(rt_instance_class(m, MaterialClass::Character, false).mask == kRtMaskCharacter);
        CHECK(rt_instance_class(m, MaterialClass::Generic, true).mask == kRtMaskTerrain);
        m.blend = BlendMode::AlphaTest;
        m.alpha_cutoff = 0.3f;
        const RtInstanceClass t = rt_instance_class(m, MaterialClass::Vegetation, false);
        CHECK(t.mask == kRtMaskFoliage && t.alpha_test && t.cutoff == 0.3f);
        m.blend = BlendMode::AlphaBlend;
        CHECK(rt_instance_class(m, MaterialClass::Building, false).mask == 0);  // water, glass: not traced
        CHECK(rt_instance_class(m, MaterialClass::Vegetation, false).cutoff == 0.5f);  // distance-faded grass
        m.blend = BlendMode::Opaque;
        m.unlit = true;
        CHECK(rt_instance_class(m, MaterialClass::Building, false).mask == 0);
        CHECK((kRtMaskShadow & kRtMaskTerrain) == 0 && (kRtMaskProbe & kRtMaskCharacter) == 0);
        CHECK(rt_keep_instance({0, 0, 0}, 40.0f, {1000, 0, 0}));
        CHECK(!rt_keep_instance({0, 0, 0}, 2.0f, {3000, 0, 0}));     // sub-pixel far away
        CHECK(!rt_keep_instance({0, 0, 0}, 300.0f, {7000, 0, 0}));   // beyond kRtRange
    }
    {  // probe volume: scrolling modulo addressing
        CHECK(probe_slot({-1, -1, -1}, {32, 8, 32}) == glm::ivec3(31, 7, 31));
        CHECK(probe_slot({33, 9, 64}, {32, 8, 32}) == glm::ivec3(1, 1, 0));
        const glm::vec3 cam(5000.0f, 300.0f, -7000.0f);
        const ProbeGrid g = probe_grid(cam, ProbeDensity::Medium);
        const glm::ivec3 c(glm::floor(cam / g.spacing));
        CHECK(glm::all(glm::greaterThanEqual(c, g.base)) && glm::all(glm::lessThan(c, g.base + g.dims)));
        CHECK(probe_grid(cam, ProbeDensity::High).spacing.x < g.spacing.x && probe_grid(cam, ProbeDensity::Low).spacing.x > g.spacing.x);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return test_result() ? 1 : 77;
    }
    SDL_Quit();
    const Options ultra = demo_options(RendererKind::Vulkan, GraphicsPreset::Ultra);  // RT requested by the preset
    Image forced;
    setenv("JOA_VK_DISABLE_RT", "1", 1);
    try {
        forced = render_demo(ultra, 6);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", vulkan_missing(e) ? "SKIP" : "FAIL", e.what());
        return vulkan_missing(e) ? 77 : 1;
    }
    unsetenv("JOA_VK_DISABLE_RT");
    CHECK(!vk_ray_tracing_available());
    const Image raster = render_demo(rt_off(ultra), 6);
    save(forced, "vk_rt_fallback_forced.png");
    CHECK(!forced.rgba.empty() && forced.rgba == raster.rgba);  // explicit request on unsupported HW = raster Ultra

    const Image rt = render_demo(ultra, 6);
    if (vk_ray_tracing_available()) {
        save(rt, "vk_rt_fallback_rt.png");
        const Diff d = diff(raster.rgba, rt.rgba);
        std::fprintf(stderr, "demo Ultra raster vs ray traced: mean abs diff %.3f, PSNR %.2f dB\n", d.mad, d.psnr);
        CHECK(d.mad > 0.05);  // RT changed the frame...
        CHECK(std::fabs(mean_luma(rt.rgba) - mean_luma(raster.rgba)) < 0.25 * mean_luma(raster.rgba));  // ...without wrecking it
    } else {
        std::fputs("note: no ray-query device, RT-on frame not compared\n", stderr);
    }

    try {  // OpenGL ignores RT (logged once): Ultra with RT keys = Ultra without
        Options gl = ultra;
        gl.renderer = RendererKind::OpenGL;
        CHECK(render_demo(gl, 4).rgba == render_demo(rt_off(gl), 4).rgba);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "note: OpenGL unavailable (%s)\n", e.what());
    }
    CHECK(vk_validation_errors() == 0);
    if (std::getenv("JOA_REQUIRE_VALIDATION")) CHECK(vk_validation_active());
    return test_result();
}
