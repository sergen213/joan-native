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
    if (k == "pbr") return boolean(v, s.pbr);
    if (k == "normal_maps") return boolean(v, s.normal_maps);
    if (k == "ibl") return boolean(v, s.ibl);
    if (k == "hdr") return boolean(v, s.hdr);
    if (k == "exposure") return real(v, s.exposure, 0.05f, 16.0f);
    if (k == "shadow_map_size") {
        const auto r = std::from_chars(v.data(), v.data() + v.size(), i);
        if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || (i != 0 && i != 1024 && i != 2048 && i != 4096 && i != 8192)) return false;
        return s.shadow_map_size = i, true;
    }
    if (k == "shadow_softness") return real(v, s.shadow_softness, 0.0f, 8.0f);
    if (k == "shadow_distance") return real(v, s.shadow_distance, 200.0f, 8192.0f);
    if (k == "ssao") return boolean(v, s.ssao);
    if (k == "ssao_radius") return real(v, s.ssao_radius, 1.0f, 500.0f);
    if (k == "ssao_intensity") return real(v, s.ssao_intensity, 0.0f, 4.0f);
    if (k == "aa") {
        if (l == "none" || l == "off") return s.aa = AntiAliasing::None, true;
        if (l == "taa") return s.aa = AntiAliasing::Taa, true;
        return false;
    }
    if (k == "volumetric_fog") return boolean(v, s.volumetric_fog);
    if (k == "fog_density") return real(v, s.fog_density, 0.0f, 10.0f);
    if (k == "particle_lighting") return boolean(v, s.particle_lighting);
    if (k == "upscale") {
        if (l == "none" || l == "off") return s.upscale = Upscaler::None, true;
        if (l == "fsr1" || l == "fsr") return s.upscale = Upscaler::Fsr1, true;
        return false;
    }
    if (k == "render_scale") return real(v, s.render_scale, 0.5f, 1.0f);
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
    return pbr || normal_maps || ibl || hdr || shadow_map_size > 0 || ssao || aa != AntiAliasing::None || volumetric_fog ||
           particle_lighting || upscale != Upscaler::None || render_scale < 1.0f || sharpness > 0.0f;
}

GraphicsSettings preset_settings(GraphicsPreset preset) {
    GraphicsSettings s;
    s.preset = preset;
    if (preset == GraphicsPreset::Original) return s;
    s.pbr = s.normal_maps = s.ibl = s.hdr = true;
    // ACES lifts mid-tones (0.2 -> 0.30); 0.85 keeps S1L1's mean luma near Original (measured +3%, docs).
    s.exposure = 0.85f;
    s.shadow_map_size = 2048;
    s.shadow_softness = 1.5f;
    s.shadow_distance = 2500.0f;
    s.ssao = true;
    s.aa = AntiAliasing::Taa;
    s.volumetric_fog = true;
    s.fog_density = 1.0f;
    s.particle_lighting = true;
    s.sharpness = 0.2f;  // RCAS after TAA
    if (preset == GraphicsPreset::Ultra) {
        s.shadow_map_size = 4096;
        s.shadow_softness = 2.5f;
        s.shadow_distance = 4000.0f;
        s.ssao_radius = 64.0f;
        s.sharpness = 0.3f;
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
    if (preset == GraphicsPreset::Custom)
        for (const auto& [k, v] : keys)
            if (!feature(s, k, v) && error && error->empty()) *error = "settings: bad graphics." + k;
    for (const auto& [k, v] : rt_keys) rt_feature(s.rt, k, v);  // validated by apply()
    s.hd_packs = hd_packs;
    return s;
}

}  // namespace joa::engine
