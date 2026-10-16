#pragma once
// Ray tracing, API-independent part (docs/RAYTRACING.md): the device capability decision, which draws become
// TLAS instances (masks, alpha test, culling), the camera-centred DDGI probe grid and the std140 `RtFrame`
// block of shaders/vk/rt_common.glsl. The Vulkan side lives in vk_rt.* and renderer_vk.cpp.
#include "engine/preset.hpp"
#include "engine/renderer.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace joa::engine {

// What the device offers. The effects use ray queries from compute/fragment shaders; the RT pipeline
// extension is only reported (no shader binding tables are used).
struct RtCaps {
    bool acceleration_structure = false;    // VK_KHR_acceleration_structure + accelerationStructure feature
    bool deferred_host_operations = false;  // VK_KHR_deferred_host_operations (required by the above)
    bool ray_query = false;                 // VK_KHR_ray_query + rayQuery feature
    bool buffer_device_address = false;     // Vulkan 1.2 bufferDeviceAddress
    bool storage_image_indexing = false;    // shaderStorageImageArrayDynamicIndexing (denoiser image table)
    bool ray_tracing_pipeline = false;      // VK_KHR_ray_tracing_pipeline (informational)
    bool forced_off = false;                // JOA_VK_DISABLE_RT=1 (tests, troubleshooting)

    bool usable() const { return missing().empty(); }
    std::string missing() const;  // "" when usable, else a comma list for the log
};

// TLAS instance masks. Shadow rays skip the terrain (its lightmaps already hold the terrain's own shading,
// as the raster shadow map) and the units (Material::volume_caster: their shadow stays the original stencil
// volume), GI probe rays skip the units too (moving units would make the probes flicker).
inline constexpr std::uint8_t kRtMaskWorld = 0x01, kRtMaskTerrain = 0x02, kRtMaskCharacter = 0x04, kRtMaskFoliage = 0x08;
inline constexpr std::uint8_t kRtMaskShadow = kRtMaskWorld | kRtMaskFoliage;
inline constexpr std::uint8_t kRtMaskProbe = kRtMaskWorld | kRtMaskTerrain | kRtMaskFoliage;
inline constexpr std::uint8_t kRtMaskAll = 0xff;
inline constexpr float kRtRange = 6000.0f;  // instances whose bounds start beyond this are not in the TLAS

struct RtInstanceClass {
    std::uint8_t mask = 0;  // 0 = not ray traced (sky, particles, water, shadow volumes, additive, unlit)
    bool alpha_test = false;
    float cutoff = 0.0f;  // alpha test threshold (alpha x tint.a x vertex alpha, as the raster pass)
};
// Mirrors the raster shadow casters: lit opaque/alpha-tested draws, blended ones only for vegetation
// (distance-faded grass) at cutoff 0.5. `terrain` = lightmapped vertex-diffuse terrain.
RtInstanceClass rt_instance_class(const Material& m, MaterialClass cls, bool terrain);

// Instance culling: inside kRtRange and not sub-pixel tiny (bounding radius below 0.25 % of the distance).
bool rt_keep_instance(glm::vec3 centre, float radius, glm::vec3 camera);

// Camera-centred DDGI volume, addressed modulo `dims` so it scrolls without copying probes: world cell c
// (probe at c * spacing) lives in slot c mod dims; the grid covers cells [base, base + dims).
struct ProbeGrid {
    glm::ivec3 base{0}, dims{32, 8, 32};
    glm::vec3 spacing{128.0f, 64.0f, 128.0f};
};
ProbeGrid probe_grid(glm::vec3 camera, ProbeDensity density);
glm::ivec3 probe_slot(glm::ivec3 cell, glm::ivec3 dims);  // positive modulo, as the shaders

inline constexpr int kProbeRays = 64;  // rays per probe update (one workgroup)

struct RtFrame {  // std140 mirror of shaders/vk/rt_common.glsl `RtFrame`
    glm::mat4 probe_rotation;  // random rotation of this frame's probe ray set
    glm::vec4 prev_camera;     // xyz previous camera, w 1 = temporal history valid
    glm::vec4 params;          // x rays per pixel, y max roughness, z denoise, w frame index
    glm::vec4 effects;         // x reflections, y shadows, z gi, w tan(sun cone half angle)
    glm::vec4 gi_scale;        // rgb sky radiance -> Original ambient scale (ambient / sky irradiance), w probe hysteresis
    glm::ivec4 grid_base;      // xyz first world cell, w rays per probe
    glm::ivec4 grid_dims;      // xyz dims, w probe count
    glm::vec4 grid_spacing;    // xyz spacing, w max probe ray length
};
static_assert(sizeof(RtFrame) == 176);

// `sky_up` = cosine-weighted sky radiance seen by an up normal (EnvMaps::sky_up).
RtFrame build_rt_frame(const RayTracingSettings& rt, const Lighting& l, glm::vec3 camera, glm::vec3 prev_camera, bool history,
                       std::uint32_t frame, glm::vec3 sky_up);

}  // namespace joa::engine
