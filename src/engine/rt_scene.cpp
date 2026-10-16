#include "engine/rt_scene.hpp"

#include <glm/gtc/color_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace joa::engine {

std::string RtCaps::missing() const {
    std::string m;
    const auto add = [&](bool ok, const char* what) {
        if (!ok) m += (m.empty() ? "" : ", ") + std::string(what);
    };
    if (forced_off) return "disabled by JOA_VK_DISABLE_RT";
    add(acceleration_structure, "VK_KHR_acceleration_structure");
    add(deferred_host_operations, "VK_KHR_deferred_host_operations");
    add(ray_query, "VK_KHR_ray_query");
    add(buffer_device_address, "bufferDeviceAddress");
    add(storage_image_indexing, "shaderStorageImageArrayDynamicIndexing");
    return m;
}

RtInstanceClass rt_instance_class(const Material& m, MaterialClass cls, bool terrain) {
    if (m.unlit) return {};
    const bool foliage = cls == MaterialClass::Vegetation;
    const std::uint8_t mask = terrain ? kRtMaskTerrain : foliage ? kRtMaskFoliage : m.volume_caster ? kRtMaskCharacter : kRtMaskWorld;
    switch (m.blend) {
    case BlendMode::Opaque: return {mask, false, 0.0f};
    case BlendMode::AlphaTest: return {mask, true, m.alpha_cutoff};
    case BlendMode::AlphaBlend:
        if (m.depth_write) return {mask, true, m.alpha_cutoff};
        return foliage ? RtInstanceClass{mask, true, 0.5f} : RtInstanceClass{};
    default: return {};
    }
}

bool rt_keep_instance(glm::vec3 centre, float radius, glm::vec3 camera) {
    const float d = glm::distance(centre, camera);
    return d - radius < kRtRange && radius > d * 0.0025f;
}

ProbeGrid probe_grid(glm::vec3 camera, ProbeDensity density) {
    ProbeGrid g;
    const float s = density == ProbeDensity::Low ? 192.0f : density == ProbeDensity::High ? 96.0f : 128.0f;
    g.spacing = {s, s * 0.5f, s};
    const glm::ivec3 cell(glm::floor(camera / g.spacing));
    // Most of the volume below the eye: the camera looks down on the terrain and the buildings.
    g.base = cell - glm::ivec3(g.dims.x / 2, g.dims.y - 2, g.dims.z / 2);
    return g;
}

glm::ivec3 probe_slot(glm::ivec3 cell, glm::ivec3 dims) { return ((cell % dims) + dims) % dims; }

RtFrame build_rt_frame(const RayTracingSettings& rt, const Lighting& l, glm::vec3 camera, glm::vec3 prev_camera, bool history,
                       std::uint32_t frame, glm::vec3 sky_up) {
    RtFrame f{};
    // Random rotation per frame (hash of the frame index) so successive probe updates cover the sphere.
    const auto rnd = [&](std::uint32_t k) {
        std::uint32_t h = frame * 747796405u + k * 2891336453u;
        h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
        return float((h >> 22u) ^ h) / 4294967296.0f;
    };
    const float z = rnd(1) * 2.0f - 1.0f, phi = rnd(2) * 6.2831853f;
    const glm::vec3 axis(std::sqrt(1.0f - z * z) * std::cos(phi), z, std::sqrt(1.0f - z * z) * std::sin(phi));
    f.probe_rotation = glm::rotate(glm::mat4(1.0f), rnd(3) * 6.2831853f, axis);
    f.prev_camera = glm::vec4(prev_camera, history ? 1.0f : 0.0f);
    f.params = {float(rt.rays_per_pixel), rt.max_roughness, rt.denoise ? 1.0f : 0.0f, float(frame)};
    // Sun cone: 1 degree half angle (the real sun is 0.27); wide enough for visible contact-hardening penumbrae.
    f.effects = {rt.reflections ? 1.0f : 0.0f, rt.shadows ? 1.0f : 0.0f, rt.gi ? 1.0f : 0.0f, std::tan(glm::radians(1.0f))};
    const glm::vec3 ambient = glm::convertSRGBToLinear(glm::clamp(l.ambient, 0.0f, 1.0f));
    f.gi_scale = glm::vec4(ambient / glm::max(sky_up, glm::vec3(1e-3f)), 0.97f);
    const ProbeGrid g = probe_grid(camera, rt.probe_density);
    f.grid_base = glm::ivec4(g.base, kProbeRays);
    f.grid_dims = glm::ivec4(g.dims, g.dims.x * g.dims.y * g.dims.z);
    f.grid_spacing = glm::vec4(g.spacing, 1.5f * glm::length(g.spacing));
    return f;
}

}  // namespace joa::engine
