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
    CHECK(enh.pbr && enh.ibl && enh.hdr && enh.ssao && enh.volumetric_fog && enh.aa == AntiAliasing::Taa && enh.shadow_map_size == 2048);
    CHECK(ultra.shadow_map_size == 4096 && ultra.shadow_softness > enh.shadow_softness && ultra.shadow_distance > enh.shadow_distance);

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
    // No settings: Original.
    {
        Options o;
        CHECK(parse({}, o).empty());
        CHECK(o.graphics.resolve().preset == GraphicsPreset::Original && !o.graphics.resolve().remastered());
    }
    // CLI: --preset wins over defaults, --graphics implies custom, --hd-packs.
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
        CHECK(!parse({"--graphics", "render_scale=2"}, o3).empty());
    }

    // Frame constants: scene size, TAA jitter within half a pixel, stable shadow frustum.
    {
        GraphicsSettings g = preset_settings(GraphicsPreset::Enhanced);
        CHECK(scene_size(g, {1920, 1080}) == glm::ivec2(1920, 1080));
        g.render_scale = 0.5f;
        CHECK(scene_size(g, {3440, 1440}) == glm::ivec2(1720, 720));
        g.render_scale = 1.0f;
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
