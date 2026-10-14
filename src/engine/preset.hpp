#pragma once
// Graphics presets (settings.ini `[renderer] preset`, per-feature `[graphics]` keys; docs/LAUNCHER.md,
// docs/GRAPHICS_ROADMAP.md "Remaster"). Original = the faithful D3D8-emulating path, untouched by every
// remaster feature. Enhanced/Ultra fix every feature; Custom starts from Enhanced and applies the
// `[graphics]` keys. `hd_packs` is independent of the preset (HD textures also work with Original).
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace joa::engine {

enum class GraphicsPreset { Original, Enhanced, Ultra, Custom };
enum class AntiAliasing { None, Taa };
enum class Upscaler { None, Fsr1 };
enum class ProbeDensity { Low, Medium, High };  // GI probe spacing 192 / 128 / 96 units (docs/RAYTRACING.md)

// Hardware ray tracing (Vulkan only, docs/RAYTRACING.md): settings.ini `[raytracing]`. Needs a remastered
// preset (the Original path never changes); unsupported devices and OpenGL log once and keep the raster path.
struct RayTracingSettings {
    bool reflections = false;    // GGX-sampled ray-query reflections up to max_roughness, IBL beyond
    bool shadows = false;        // ray-traced sun visibility (cone-sampled penumbra) instead of the shadow map
    bool gi = false;             // DDGI-style probe grid (sky + bounce) replacing the flat/IBL ambient
    int rays_per_pixel = 1;      // 1..4, reflection and shadow rays per pixel per frame
    float max_roughness = 0.6f;  // 0..1
    bool denoise = true;         // temporal accumulation + edge-aware a-trous filter
    ProbeDensity probe_density = ProbeDensity::Medium;
    bool any() const { return reflections || shadows || gi; }
};

struct GraphicsSettings {
    GraphicsPreset preset = GraphicsPreset::Original;
    bool pbr = false;          // metallic-roughness shading (conservative legacy-material conversion)
    bool normal_maps = false;  // HD pack `<name>_n` maps (derivative TBN, no tangents needed)
    bool ibl = false;          // prefiltered sky/scattering environment + irradiance
    bool hdr = false;          // RGBA16F scene target, exposure, ACES-fitted tone mapping
    float exposure = 1.0f;     // linear multiplier before tone mapping
    int shadow_map_size = 0;   // 0 = original stencil shadow volumes; else sun shadow map texels (PCF)
    float shadow_softness = 1.5f;     // PCF filter radius in shadow-map texels
    float shadow_distance = 2500.0f;  // shadow map coverage from the camera, original units
    bool ssao = false;
    float ssao_radius = 48.0f;  // original units
    float ssao_intensity = 1.0f;
    AntiAliasing aa = AntiAliasing::None;
    bool volumetric_fog = false;  // height fog + shadowed sun in-scattering (ray-marched)
    float fog_density = 1.0f;     // multiplier of the volumetric extinction
    bool particle_lighting = false;  // lit smoke, HDR emissive fire
    Upscaler upscale = Upscaler::None;
    float render_scale = 1.0f;  // scene resolution / output resolution (FSR1 EASU when < 1)
    float sharpness = 0.0f;     // RCAS strength 0..1 (0 = off)
    std::vector<std::string> hd_packs;  // pack names under $XDG_DATA_HOME/joan-native/hd/ or absolute dirs, first wins
    RayTracingSettings rt;              // Ultra: all three effects on (used only where supported)

    // True when any remaster feature is on (else the renderer takes the Original path; RT needs it too).
    bool remastered() const;
};

GraphicsSettings preset_settings(GraphicsPreset preset);
bool parse_preset(std::string_view text, GraphicsPreset& out);
const char* preset_name(GraphicsPreset preset);

// Collects settings.ini/CLI keys in order, then resolves them: preset base + (Custom only) feature keys,
// then `[raytracing]` keys (any preset: explicit RT choices override the preset's default).
struct GraphicsConfig {
    GraphicsPreset preset = GraphicsPreset::Original;
    std::vector<std::pair<std::string, std::string>> keys;     // `[graphics]` key/value, in file order
    std::vector<std::pair<std::string, std::string>> rt_keys;  // `[raytracing]` key/value, in file order
    std::vector<std::string> hd_packs;

    // key = "renderer.preset", "graphics.<name>" or "raytracing.<name>". Returns an error message, "" if accepted or not ours.
    std::string apply(std::string_view key, std::string_view value);
    // Returns the resolved settings; `error` receives the first bad feature key/value.
    GraphicsSettings resolve(std::string* error = nullptr) const;
};

}  // namespace joa::engine
