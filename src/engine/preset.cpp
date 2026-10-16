#include "engine/preset.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace joa::engine {
namespace {

std::string lowered(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool boolean(std::string_view v, bool& out) {
    const std::string l = lowered(v);
    if (l == "true" || l == "1" || l == "on" || l == "yes") return out = true, true;
    if (l == "false" || l == "0" || l == "off" || l == "no") return out = false, true;
    return false;
}

bool real(std::string_view v, float& out, float lo, float hi) {
    float f = 0;
    const auto r = std::from_chars(v.data(), v.data() + v.size(), f);
    if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || !(f >= lo && f <= hi)) return false;
    return out = f, true;
}

std::vector<std::string> split_list(std::string_view v) {
    std::vector<std::string> out;
    for (std::size_t p = 0; p <= v.size();) {
        const std::size_t c = std::min(v.find(',', p), v.size());
        std::string_view item = v.substr(p, c - p);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.remove_prefix(1);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.remove_suffix(1);
        if (!item.empty()) out.emplace_back(item);
        p = c + 1;
    }
    return out;
}

// One `[graphics]` feature key onto `s`; false = unknown key or bad value.
bool feature(GraphicsSettings& s, std::string_view k, std::string_view v) {
    int i = 0;
    const std::string l = lowered(v);
    const auto integer = [&](int lo, int hi) {
        const auto r = std::from_chars(v.data(), v.data() + v.size(), i);
        return r.ec == std::errc{} && r.ptr == v.data() + v.size() && i >= lo && i <= hi;
    };
    if (k == "pbr") return boolean(v, s.pbr);
    if (k == "normal_maps") return boolean(v, s.normal_maps);
    if (k == "ibl") return boolean(v, s.ibl);
    if (k == "hdr") return boolean(v, s.hdr);
    if (k == "exposure") return real(v, s.exposure, 0.05f, 16.0f);
    if (k == "auto_exposure") return boolean(v, s.auto_exposure);
    if (k == "shadow_map_size") {
        if (!integer(0, 8192) || (i != 0 && i != 1024 && i != 2048 && i != 4096 && i != 8192)) return false;
        return s.shadow_map_size = i, true;
    }
    if (k == "shadow_softness") return real(v, s.shadow_softness, 0.0f, 8.0f);
    if (k == "shadow_distance") return real(v, s.shadow_distance, 200.0f, 8192.0f);
    if (k == "ssao") return boolean(v, s.ssao);
    if (k == "ssao_radius") return real(v, s.ssao_radius, 1.0f, 500.0f);
    if (k == "ssao_intensity") return real(v, s.ssao_intensity, 0.0f, 4.0f);
    if (k == "aa") {
        if (l == "none" || l == "off") return s.aa = AntiAliasing::None, true;
        if (l == "fxaa") return s.aa = AntiAliasing::Fxaa, true;
        if (l == "smaa") return s.aa = AntiAliasing::Smaa, true;
        if (l == "taa") return s.aa = AntiAliasing::Taa, true;
        return false;
    }
    if (k == "msaa") {
        if (!integer(1, 8) || (i & (i - 1))) return false;  // 1, 2, 4, 8
        return s.msaa = i, true;
    }
    if (k == "anisotropy") return integer(1, 16) && (s.anisotropy = i, true);
    if (k == "volumetric_fog") return boolean(v, s.volumetric_fog);
    if (k == "fog_density") return real(v, s.fog_density, 0.0f, 10.0f);
    if (k == "water") {
        if (l == "original") return s.water = false, true;
        if (l == "enhanced") return s.water = true, true;
        return false;
    }
    if (k == "particle_lighting") return boolean(v, s.particle_lighting);
    if (k == "upscale") {
        if (l == "none" || l == "off") return s.upscale = Upscaler::None, true;
        if (l == "fsr1" || l == "fsr") return s.upscale = Upscaler::Fsr1, true;
        if (l == "fsr3") return s.upscale = Upscaler::Fsr3, true;
        return false;
    }
    if (k == "fsr_quality") {
        if (l == "native") return s.fsr_quality = FsrQuality::Native, true;
        if (l == "quality") return s.fsr_quality = FsrQuality::Quality, true;
        if (l == "balanced") return s.fsr_quality = FsrQuality::Balanced, true;
        if (l == "performance") return s.fsr_quality = FsrQuality::Performance, true;
        if (l == "ultra_performance") return s.fsr_quality = FsrQuality::UltraPerformance, true;
        return false;
    }
    if (k == "frame_generation") return boolean(v, s.frame_generation);
    if (k == "render_scale") return real(v, s.render_scale, 0.5f, 2.0f);
    if (k == "sharpness") return real(v, s.sharpness, 0.0f, 1.0f);
    return false;
}

// One `[raytracing]` key onto `rt`; false = unknown key or bad value.
bool rt_feature(RayTracingSettings& rt, std::string_view k, std::string_view v) {
    const std::string l = lowered(v);
    if (k == "reflections") return boolean(v, rt.reflections);
    if (k == "shadows") return boolean(v, rt.shadows);
    if (k == "gi") return boolean(v, rt.gi);
    if (k == "denoise") return boolean(v, rt.denoise);
    if (k == "max_roughness") return real(v, rt.max_roughness, 0.0f, 1.0f);
    if (k == "rays_per_pixel") {
        int i = 0;
        const auto r = std::from_chars(v.data(), v.data() + v.size(), i);
        if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || i < 1 || i > 4) return false;
        return rt.rays_per_pixel = i, true;
    }
    if (k == "probe_density") {
        if (l == "low") return rt.probe_density = ProbeDensity::Low, true;
        if (l == "medium") return rt.probe_density = ProbeDensity::Medium, true;
        if (l == "high") return rt.probe_density = ProbeDensity::High, true;
        return false;
    }
    return false;
}

}  // namespace

bool GraphicsSettings::remastered() const {
    return pbr || normal_maps || ibl || hdr || shadow_map_size > 0 || ssao || volumetric_fog || particle_lighting || water;
}

bool GraphicsSettings::original_post() const {
    return msaa > 1 || aa != AntiAliasing::None || render_scale != 1.0f || sharpness > 0.0f || upscale == Upscaler::Fsr3;
}

bool quality_key(std::string_view k) {
    return k == "aa" || k == "msaa" || k == "anisotropy" || k == "render_scale" || k == "upscale" || k == "sharpness" ||
           k == "fsr_quality" || k == "frame_generation" ||
           k == "auto_exposure";  // remaster only; a viewing preference like sharpness, so it overrides Enhanced/Ultra too
}

float fsr_render_scale(FsrQuality q) {
    switch (q) {
    case FsrQuality::Native: return 1.0f;
    case FsrQuality::Quality: return 1.0f / 1.5f;
    case FsrQuality::Balanced: return 1.0f / 1.7f;
    case FsrQuality::Performance: return 0.5f;
    case FsrQuality::UltraPerformance: return 1.0f / 3.0f;
    }
    return 1.0f;
}

void fsr3_fallback(GraphicsSettings& g) {
    if (g.upscale != Upscaler::Fsr3) return;
    g.upscale = Upscaler::Fsr1;
    g.render_scale = fsr_render_scale(g.fsr_quality);
    g.frame_generation = false;
}

GraphicsSettings preset_settings(GraphicsPreset preset) {
    GraphicsSettings s;
    s.preset = preset;
    if (preset == GraphicsPreset::Original) return s;
    s.pbr = s.normal_maps = s.ibl = s.hdr = true;
    // Auto exposure start; it tracks the Original's displayed mean luma within 0.5..4x of this (post_frame.cpp).
    s.exposure = 0.85f;
    s.shadow_map_size = 2048;
    s.shadow_softness = 1.5f;
    s.shadow_distance = 2500.0f;
    s.ssao = true;
    // No TAA (it smeared the always-animating characters, grass and foliage before per-object motion vectors) and no
    // post AA (SMAA/FXAA also soften texture detail: S1L1 sharpness 0.74x at 640x360): 4x MSAA smooths geometry and,
    // through alpha-to-coverage, alpha-tested foliage while textures keep Original's sharpness. Ultra keeps TAA.
    s.aa = AntiAliasing::None;
    s.msaa = 4;
    s.volumetric_fog = true;
    s.fog_density = 1.0f;
    s.water = true;
    s.particle_lighting = true;
    s.sharpness = 0.0f;  // RCAS: for TAA / FSR1 upscaling
    if (preset == GraphicsPreset::Ultra) {
        s.shadow_map_size = 4096;
        s.shadow_softness = 2.5f;
        s.shadow_distance = 4000.0f;
        s.ssao_radius = 64.0f;
        s.aa = AntiAliasing::Taa;
        s.msaa = 1;          // TAA resolves the edges
        // RCAS after TAA: an accumulated, jittered image is ~0.5x as detailed as one sample; full strength brings the
        // running hero back to ~0.9x the no-AA Laplacian variance (docs/GRAPHICS_ROADMAP.md AA row).
        s.sharpness = 1.0f;
        // AMD FSR 3.1.4 replaces TAA on Vulkan; its RCAS takes the sharpness. Native AA (render scale 1), not Kabus 22's
        // quality mode: running Joan (work/gfxbug/run.txt, 1080p crop) keeps the no-AA Laplacian variance (0.97-1.06x)
        // and 2.2-3.8x that of the unsharpened TAA accumulation; quality mode (720p) drops to 1.05-1.9x. TAA above stays
        // the fallback where FSR 3 cannot run. Frame generation stays opt-in (ETI Yami enables it on no preset either):
        // under vsync it halves the rendered rate.
        s.upscale = Upscaler::Fsr3;
        s.fsr_quality = FsrQuality::Native;
        s.rt.reflections = s.rt.shadows = s.rt.gi = true;  // used only on ray-query hardware (Vulkan)
    }
    return s;
}

bool parse_preset(std::string_view text, GraphicsPreset& out) {
    const std::string l = lowered(text);
    if (l == "original") return out = GraphicsPreset::Original, true;
    if (l == "enhanced") return out = GraphicsPreset::Enhanced, true;
    if (l == "ultra") return out = GraphicsPreset::Ultra, true;
    if (l == "custom") return out = GraphicsPreset::Custom, true;
    return false;
}

const char* preset_name(GraphicsPreset p) {
    switch (p) {
    case GraphicsPreset::Original: return "original";
    case GraphicsPreset::Enhanced: return "enhanced";
    case GraphicsPreset::Ultra: return "ultra";
    case GraphicsPreset::Custom: return "custom";
    }
    return "?";
}

std::string GraphicsConfig::apply(std::string_view key, std::string_view value) {
    if (key == "renderer.preset") return parse_preset(value, preset) ? "" : "settings: bad renderer.preset";
    if (key == "graphics.hd_packs") return hd_packs = split_list(value), "";
    if (key.starts_with("graphics.")) {
        const std::string k(key.substr(9));
        GraphicsSettings probe;
        if (!feature(probe, k, value)) return "settings: bad graphics." + k;
        keys.emplace_back(k, std::string(value));
    }
    if (key.starts_with("raytracing.")) {
        const std::string k(key.substr(11));
        RayTracingSettings probe;
        if (!rt_feature(probe, k, value)) return "settings: bad raytracing." + k;
        rt_keys.emplace_back(k, std::string(value));
    }
    return {};
}

GraphicsSettings GraphicsConfig::resolve(std::string* error) const {
    GraphicsSettings s = preset_settings(preset == GraphicsPreset::Custom ? GraphicsPreset::Enhanced : preset);
    s.preset = preset;
    for (const auto& [k, v] : keys)
        if ((preset == GraphicsPreset::Custom || quality_key(k)) && !feature(s, k, v) && error && error->empty())
            *error = "settings: bad graphics." + k;
    // TAA reprojects with the PBR pass's motion vectors; the Original look has none (docs/GRAPHICS_ROADMAP.md).
    if (s.aa == AntiAliasing::Taa && !s.remastered()) s.aa = AntiAliasing::Smaa;
    // Frame generation interpolates FSR 3's output with its motion vectors: remaster path + upscale=fsr3 only.
    if (s.upscale != Upscaler::Fsr3 || !s.remastered()) s.frame_generation = false;
    for (const auto& [k, v] : rt_keys) rt_feature(s.rt, k, v);  // validated by apply()
    s.hd_packs = hd_packs;
    return s;
}

}  // namespace joa::engine
