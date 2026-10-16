#pragma once
// Graphics presets (settings.ini `[renderer] preset`, per-feature `[graphics]` keys; docs/LAUNCHER.md,
// docs/GRAPHICS_ROADMAP.md "Remaster"). Original = the faithful D3D8-emulating look, untouched by every
// remaster feature. Enhanced/Ultra fix every look feature; Custom starts from Enhanced and applies the
// `[graphics]` look keys. Image-quality keys (aa, msaa, anisotropy, render_scale, upscale, sharpness) apply to
// every preset, Original included, without switching it to the remaster path. `hd_packs` is independent too.
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace joa::engine {

enum class GraphicsPreset { Original, Enhanced, Ultra, Custom };
// Fxaa/Smaa: post passes on the tone-mapped LDR image (before the UI). Taa needs the remaster path's
// per-object motion vectors: on the Original look resolve() turns it into Smaa.
enum class AntiAliasing { None, Fxaa, Smaa, Taa };
// Fsr3: AMD FSR 3.1.4 temporal upscaling (Vulkan remaster path; replaces TAA/FXAA/SMAA). Where it cannot run (OpenGL,
// the Original look, a device without its shader features) it degrades to Fsr1 at the same render scale.
enum class Upscaler { None, Fsr1, Fsr3 };
// FSR 3 quality modes: render scale 1, 1/1.5, 1/1.7, 1/2, 1/3 per axis (AMD's ratios, as Kabus 22 `fsr=`).
enum class FsrQuality { Native, Quality, Balanced, Performance, UltraPerformance };
float fsr_render_scale(FsrQuality q);
struct GraphicsSettings;
// A renderer that cannot run FSR 3 (OpenGL, the Original look, unsupported device): FSR1 EASU at the FSR 3 quality
// mode's render scale, no frame generation. No-op unless upscale == Fsr3.
void fsr3_fallback(GraphicsSettings& g);
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
    float exposure = 1.0f;     // linear multiplier before tone mapping (auto exposure: its start value)
    bool auto_exposure = true; // remaster exposure tracks the Original's mean luma (false: fixed `exposure`, e.g. A/B tests)
    int shadow_map_size = 0;   // 0 = original stencil shadow volumes; else sun shadow map texels (PCF)
    float shadow_softness = 1.5f;     // PCF filter radius in shadow-map texels
    float shadow_distance = 2500.0f;  // shadow map coverage from the camera, original units
    bool ssao = false;
    float ssao_radius = 48.0f;  // original units
    float ssao_intensity = 1.0f;
    AntiAliasing aa = AntiAliasing::None;  // image-quality key (any preset)
    int msaa = 1;                          // 1/2/4/8 samples per pixel of the scene target (any preset); alpha-to-coverage when > 1
    int anisotropy = 8;                    // 1..16 texture anisotropic filtering (any preset; 8 = the D3D8-era default kept so far)
    bool volumetric_fog = false;  // height fog + shadowed sun in-scattering (ray-marched)
    float fog_density = 1.0f;     // multiplier of the volumetric extinction
    // `water=original|enhanced`: .stg water as the original blended bump/sphere map, or stylised (WoW: Forever-like depth
    // colour, refraction, shoreline foam, scrolling normals, Fresnel sky/RT reflection, sun glints; pbr_shade.glsl).
    bool water = false;
    bool particle_lighting = false;  // lit smoke, HDR emissive fire
    Upscaler upscale = Upscaler::None;
    FsrQuality fsr_quality = FsrQuality::Quality;  // upscale=fsr3: resolve() sets render_scale from it
    // FSR 3.1.4 frame generation: one interpolated frame presented between real frames (needs upscale=fsr3 on Vulkan;
    // resolve() clears it otherwise, the Vulkan renderer when the device lacks the features).
    bool frame_generation = false;
    float render_scale = 1.0f;  // scene resolution / output resolution, 0.5..2 (FSR1 EASU or bilinear when < 1, supersampling when > 1)
    float sharpness = 0.0f;     // RCAS strength 0..1 (0 = off), any preset
    std::vector<std::string> hd_packs;  // pack names under $XDG_DATA_HOME/joan-native/hd/ or absolute dirs, first wins
    RayTracingSettings rt;              // Ultra: all three effects on (used only where supported)

    // True when any remaster look feature is on (else the renderer takes the Original path; RT needs it too).
    bool remastered() const;
    // Original path only: true when an image-quality option needs the offscreen scene target + post chain.
    bool original_post() const;
};

GraphicsSettings preset_settings(GraphicsPreset preset);
bool parse_preset(std::string_view text, GraphicsPreset& out);
const char* preset_name(GraphicsPreset preset);

// Collects settings.ini/CLI keys in order, then resolves them: preset base, image-quality keys (every preset),
// look keys (Custom only), then `[raytracing]` keys (any preset: explicit RT choices override the preset's default).
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

// True for the image-quality `[graphics]` keys that apply to every preset (and do not imply `custom` on the CLI).
bool quality_key(std::string_view key);

}  // namespace joa::engine
