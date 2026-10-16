// Graphics presets: names, settings.ini keys, Custom semantics, CLI flags (preset.hpp, cli.cpp).
#include "check.hpp"
#include "engine/cli.hpp"
#include "engine/post_frame.hpp"
#include "engine/preset.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

using namespace joa::engine;

static std::string parse(std::vector<const char*> args, Options& o) {
    args.insert(args.begin(), "joan-native");
    return parse_options(int(args.size()), args.data(), o);
}

int main() {
    GraphicsPreset p{};
    CHECK(parse_preset("original", p) && p == GraphicsPreset::Original);
    CHECK(parse_preset("Enhanced", p) && p == GraphicsPreset::Enhanced);
    CHECK(parse_preset("ULTRA", p) && p == GraphicsPreset::Ultra);
    CHECK(parse_preset("custom", p) && p == GraphicsPreset::Custom);
    CHECK(!parse_preset("high", p));

    // Original: every remaster feature off (the renderer keeps the faithful path).
    const GraphicsSettings orig = preset_settings(GraphicsPreset::Original);
    CHECK(!orig.remastered());
    CHECK(GraphicsSettings{}.remastered() == false);
    const GraphicsSettings enh = preset_settings(GraphicsPreset::Enhanced), ultra = preset_settings(GraphicsPreset::Ultra);
    CHECK(enh.remastered() && ultra.remastered());
    CHECK(enh.pbr && enh.ibl && enh.hdr && enh.ssao && enh.volumetric_fog && enh.shadow_map_size == 2048);
    // Enhanced renders native with 4x MSAA, no post AA (TAA smeared the animated scene, SMAA softened textures:
    // gl/vk_remaster sharpness check).
    CHECK(enh.aa == AntiAliasing::None && enh.upscale == Upscaler::None && enh.render_scale == 1.0f && enh.sharpness == 0.0f);
    CHECK(enh.msaa == 4 && ultra.msaa == 1 && enh.anisotropy == 8 && orig.msaa == 1 && orig.anisotropy == 8 && !orig.original_post());
    CHECK(ultra.shadow_map_size == 4096 && ultra.shadow_softness > enh.shadow_softness && ultra.shadow_distance > enh.shadow_distance);
    CHECK(ultra.aa == AntiAliasing::Taa && ultra.sharpness > 0.0f);
    // Ultra anti-aliases with FSR 3 native AA (TAA above is its fallback); frame generation stays opt-in.
    CHECK(ultra.upscale == Upscaler::Fsr3 && ultra.fsr_quality == FsrQuality::Native && !ultra.frame_generation);
    CHECK(scene_size(ultra, {1920, 1080}) == glm::ivec2(1920, 1080) && ultra.original_post());
    {  // upscale=fsr3 + quality modes + frame generation (needs fsr3 and the remaster path)
        Options o;
        CHECK(apply_settings_ini("[renderer]\npreset=enhanced\n[graphics]\nupscale=fsr3\nfsr_quality=performance\nframe_generation=1\n", o).empty());
        GraphicsSettings g = o.graphics.resolve();
        CHECK(g.upscale == Upscaler::Fsr3 && g.fsr_quality == FsrQuality::Performance && g.frame_generation);
        CHECK(scene_size(g, {1920, 1080}) == glm::ivec2(960, 540));
        fsr3_fallback(g);  // OpenGL / unsupported device: FSR1 at the same scale, no frame generation
        CHECK(g.upscale == Upscaler::Fsr1 && g.render_scale == 0.5f && !g.frame_generation && scene_size(g, {1920, 1080}) == glm::ivec2(960, 540));
        Options o2;
        CHECK(apply_settings_ini("[renderer]\npreset=enhanced\n[graphics]\nupscale=fsr1\nframe_generation=true\n", o2).empty());
        CHECK(!o2.graphics.resolve().frame_generation);
        Options o3;
        CHECK(apply_settings_ini("[renderer]\npreset=original\n[graphics]\nupscale=fsr3\nframe_generation=true\n", o3).empty());
        CHECK(!o3.graphics.resolve().frame_generation && o3.graphics.resolve().original_post());
        Options o4;
        CHECK(!apply_settings_ini("[graphics]\nfsr_quality=ultra\n", o4).empty());
        CHECK(fsr_render_scale(FsrQuality::Native) == 1.0f && fsr_render_scale(FsrQuality::UltraPerformance) < 0.34f);
    }

    // settings.ini: preset fixes the features unless custom.
    {
        Options o;
        CHECK(apply_settings_ini("[renderer]\nbackend=vulkan\npreset=enhanced\n[graphics]\nssao=false\nhd_packs=hd_update, mine\n", o).empty());
        const GraphicsSettings g = o.graphics.resolve();
        CHECK(g.preset == GraphicsPreset::Enhanced && g.ssao);  // ignored: not custom
        CHECK(g.hd_packs == (std::vector<std::string>{"hd_update", "mine"}));
    }
    {
        Options o;
        CHECK(apply_settings_ini("[graphics]\nssao=false\nshadow_map_size=4096\naa=none\nupscale=fsr1\nrender_scale=0.75\n"
                                 "[renderer]\npreset=custom\n",
                                 o)
                  .empty());
        const GraphicsSettings g = o.graphics.resolve();
        CHECK(g.preset == GraphicsPreset::Custom && !g.ssao && g.shadow_map_size == 4096 && g.aa == AntiAliasing::None);
        CHECK(g.upscale == Upscaler::Fsr1 && g.render_scale == 0.75f && g.pbr);  // unspecified keys = Enhanced
    }
    {
        Options o;
        CHECK(!apply_settings_ini("[renderer]\npreset=extreme\n", o).empty());
        Options o2;
        CHECK(!apply_settings_ini("[graphics]\nshadow_map_size=3000\n", o2).empty());
        Options o3;
        CHECK(!apply_settings_ini("[graphics]\nexposure=abc\n", o3).empty());
        Options o4;
        CHECK(!apply_settings_ini("[graphics]\nwarp_drive=1\n", o4).empty());
        Options o5;  // [raytracing] keys (docs/RAYTRACING.md; the full matrix is in vk_rt_fallback_test)
        CHECK(apply_settings_ini("[raytracing]\nreflections=true\n", o5).empty());
        CHECK(o5.graphics.resolve().rt.reflections && !o5.graphics.resolve().rt.gi);
        Options o6;
        CHECK(!apply_settings_ini("[raytracing]\nrays_per_pixel=0\n", o6).empty());
    }
    // Image-quality keys apply to every preset; Original keeps its look (no remaster path), TAA there means SMAA.
    {
        Options o;
        CHECK(apply_settings_ini("[renderer]\npreset=original\n[graphics]\nmsaa=4\naa=smaa\nanisotropy=16\nrender_scale=2\n"
                                 "sharpness=0.5\nssao=true\n",
                                 o)
                  .empty());
        const GraphicsSettings g = o.graphics.resolve();
        CHECK(g.preset == GraphicsPreset::Original && !g.remastered() && !g.ssao);  // look key ignored: not custom
        CHECK(g.msaa == 4 && g.aa == AntiAliasing::Smaa && g.anisotropy == 16 && g.render_scale == 2.0f && g.sharpness == 0.5f);
        CHECK(g.original_post());
        Options o2;
        CHECK(apply_settings_ini("[renderer]\npreset=original\n[graphics]\naa=taa\n", o2).empty());
        CHECK(o2.graphics.resolve().aa == AntiAliasing::Smaa && !o2.graphics.resolve().remastered());
        Options o3;  // Ultra + quality override: still Ultra's look
        CHECK(apply_settings_ini("[renderer]\npreset=ultra\n[graphics]\naa=fxaa\nmsaa=2\n", o3).empty());
        const GraphicsSettings u = o3.graphics.resolve();
        CHECK(u.preset == GraphicsPreset::Ultra && u.remastered() && u.aa == AntiAliasing::Fxaa && u.msaa == 2 && u.shadow_map_size == 4096);
        Options o4;
        CHECK(!apply_settings_ini("[graphics]\nmsaa=3\n", o4).empty());
        Options o5;
        CHECK(!apply_settings_ini("[graphics]\nanisotropy=17\n", o5).empty());
        Options o6;
        CHECK(!apply_settings_ini("[graphics]\naa=msaa\n", o6).empty());
        Options o7;  // Custom with every look feature off is the Original look + quality options
        CHECK(apply_settings_ini("[renderer]\npreset=custom\n[graphics]\npbr=false\nnormal_maps=false\nibl=false\nhdr=false\n"
                                 "shadow_map_size=0\nssao=false\nvolumetric_fog=false\nparticle_lighting=false\nwater=original\naa=fxaa\n",
                                 o7)
                  .empty());
        CHECK(!o7.graphics.resolve().remastered() && o7.graphics.resolve().aa == AntiAliasing::Fxaa);
        Options o8;  // stylised water: Enhanced/Ultra default, `water=original` (custom) turns it off, other values rejected
        CHECK(apply_settings_ini("[renderer]\npreset=custom\n[graphics]\nwater=original\n", o8).empty());
        CHECK(!o8.graphics.resolve().water && preset_settings(GraphicsPreset::Enhanced).water && preset_settings(GraphicsPreset::Ultra).water &&
              !preset_settings(GraphicsPreset::Original).water);
        Options o9;
        CHECK(!apply_settings_ini("[graphics]\nwater=wow\n", o9).empty());
    }
    // No settings: Original.
    {
        Options o;
        CHECK(parse({}, o).empty());
        CHECK(o.graphics.resolve().preset == GraphicsPreset::Original && !o.graphics.resolve().remastered());
    }
    // CLI: --preset wins over defaults, --graphics look keys imply custom (quality keys do not), --hd-packs.
    {
        Options o;
        CHECK(parse({"--preset", "ultra"}, o).empty());
        CHECK(o.graphics.resolve().shadow_map_size == 4096);
    }
    {
        Options o;
        CHECK(parse({"--graphics", "ssao=false,aa=none", "--hd-packs", "/abs/pack,named"}, o).empty());
        const GraphicsSettings g = o.graphics.resolve();
        CHECK(g.preset == GraphicsPreset::Custom && !g.ssao && g.aa == AntiAliasing::None && g.hdr);
        CHECK(g.hd_packs.size() == 2 && g.hd_packs[0] == "/abs/pack");
    }
    {
        Options o;
        CHECK(!parse({"--preset", "nope"}, o).empty());
        Options o2;
        CHECK(!parse({"--graphics", "ssao"}, o2).empty());
        Options o3;
        CHECK(!parse({"--graphics", "render_scale=3"}, o3).empty());
    }
    {
        Options o;
        CHECK(parse({"--preset", "original", "--graphics", "msaa=4,aa=smaa"}, o).empty());
        const GraphicsSettings g = o.graphics.resolve();
        CHECK(g.preset == GraphicsPreset::Original && !g.remastered() && g.msaa == 4 && g.aa == AntiAliasing::Smaa);
        Options o2;  // order does not matter for quality keys
        CHECK(parse({"--graphics", "render_scale=1.5", "--preset", "enhanced"}, o2).empty());
        CHECK(o2.graphics.resolve().preset == GraphicsPreset::Enhanced && o2.graphics.resolve().render_scale == 1.5f);
    }

    // Frame constants: scene size, TAA jitter within half a pixel, stable shadow frustum.
    {
        GraphicsSettings g = preset_settings(GraphicsPreset::Enhanced);
        CHECK(scene_size(g, {1920, 1080}) == glm::ivec2(1920, 1080));
        g.render_scale = 0.5f;
        CHECK(scene_size(g, {3440, 1440}) == glm::ivec2(1720, 720));
        g.render_scale = 2.0f;  // supersampling
        CHECK(scene_size(g, {1920, 1080}) == glm::ivec2(3840, 2160));
        g.render_scale = 1.0f;
        g.aa = AntiAliasing::Taa;  // Enhanced is native; TAA is opt-in (Ultra)
        FrameData f;
        f.view.view = glm::lookAt(glm::vec3(0, 100, 0), glm::vec3(0, 90, -100), glm::vec3(0, 1, 0));
        f.view.projection = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 4.0f, 4096.0f);
        RemasterHistory h;
        const RemasterFrame a = build_remaster_frame(g, f, {1920, 1080}, {1920, 1080}, 7, h);
        CHECK(a.jitter.z == 0.0f);  // first frame: no history
        const RemasterFrame b = build_remaster_frame(g, f, {1920, 1080}, {1920, 1080}, 7, h);
        CHECK(b.jitter.z == 1.0f);
        CHECK(std::fabs(b.jitter.x) <= 1.0f / 1920.0f && std::fabs(b.jitter.y) <= 1.0f / 1080.0f);
        CHECK(a.jitter.x != b.jitter.x);
        CHECK(b.ssao.z == -1.0f && a.fog_range.w != b.fog_range.w);  // TAA: finer mips, animated noise
        g.aa = AntiAliasing::None;
        const RemasterFrame c = build_remaster_frame(g, f, {1920, 1080}, {1920, 1080}, 7, h);
        CHECK(c.jitter == glm::vec4(0.0f) && c.ssao.z == 0.0f && c.fog_range.w == 0.0f);  // no TAA: no jitter, static noise
        // FSR 3: its own Halton jitter (within half a render pixel), no TAA history flag, no RCAS in the final pass.
        g.upscale = Upscaler::Fsr3;
        g.sharpness = 1.0f;
        const RemasterFrame d = build_remaster_frame(g, f, {1280, 720}, {1920, 1080}, 7, h);
        const RemasterFrame e = build_remaster_frame(g, f, {1280, 720}, {1920, 1080}, 7, h);
        CHECK(std::fabs(e.jitter.x) <= 1.0f / 1280.0f && std::fabs(e.jitter.y) <= 1.0f / 720.0f && d.jitter.x != e.jitter.x);
        CHECK(e.jitter.z == 0.0f && e.jitter.w == 0.0f && e.ssao.z == -1.0f);
        CHECK(a.shadow_matrix == b.shadow_matrix);  // same camera: same snapped frustum
        // Moving the camera by less than a shadow texel keeps the texel grid (no shimmer).
        const glm::mat4 s0 = fit_sun_shadow(f.view, {-0.4f, -1.0f, -0.3f}, 2500.0f, 2048);
        f.view.view = glm::translate(f.view.view, glm::vec3(0.5f, 0.0f, 0.0f));
        const glm::mat4 s1 = fit_sun_shadow(f.view, {-0.4f, -1.0f, -0.3f}, 2500.0f, 2048);
        const glm::vec4 p0 = s0 * glm::vec4(10, 0, -50, 1), p1 = s1 * glm::vec4(10, 0, -50, 1);
        const float texels = std::fabs(p0.x - p1.x) * 1024.0f;
        CHECK(texels < 1e-3f || std::fabs(texels - std::round(texels)) < 1e-2f);
    }
    return test_result();
}
