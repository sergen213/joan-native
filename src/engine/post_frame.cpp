#include "engine/post_frame.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace joa::engine {
namespace {

float halton(std::uint32_t i, std::uint32_t base) {
    float f = 1.0f, r = 0.0f;
    for (; i; i /= base) f /= float(base), r += f * float(i % base);
    return r;
}

}  // namespace

glm::ivec2 scene_size(const GraphicsSettings& g, glm::ivec2 output) {
    const float s = std::clamp(g.render_scale, 0.5f, 2.0f);
    return glm::max(glm::ivec2(glm::round(glm::vec2(output) * s)), glm::ivec2(1));
}

glm::mat4 fit_sun_shadow(const FrameView& view, glm::vec3 sun_dir, float distance, int size) {
    const glm::mat4 inv_view = glm::inverse(view.view);
    const float tx = 1.0f / view.projection[0][0], ty = 1.0f / view.projection[1][1];
    // Bounding sphere of the view frustum slice up to `distance` (8 corners, near taken as ~0).
    glm::vec3 corners[8];
    int n = 0;
    for (float z : {1.0f, distance})
        for (float sx : {-1.0f, 1.0f})
            for (float sy : {-1.0f, 1.0f}) corners[n++] = glm::vec3(inv_view * glm::vec4(sx * z * tx, sy * z * ty, -z, 1.0f));
    glm::vec3 center(0.0f);
    for (const auto& c : corners) center += c / 8.0f;
    float radius = 0.0f;
    for (const auto& c : corners) radius = std::max(radius, glm::distance(c, center));
    radius = std::ceil(radius / 64.0f) * 64.0f;  // stable extent: only grows/shrinks in 64-unit steps

    const glm::vec3 l = glm::normalize(sun_dir);
    const glm::vec3 up = std::abs(l.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    glm::mat4 light = glm::lookAt(glm::vec3(0.0f), l, up);
    // Snap the centre to whole shadow texels in light space.
    const float texel = 2.0f * radius / float(size);
    glm::vec3 lc = glm::vec3(light * glm::vec4(center, 1.0f));
    lc.x = std::floor(lc.x / texel) * texel;
    lc.y = std::floor(lc.y / texel) * texel;
    // Casters up to 3000 units beyond the sphere towards the sun (tall trees, towers, low sun).
    const glm::mat4 proj = glm::ortho(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius, -lc.z - radius - 3000.0f, -lc.z + radius);
    return proj * light;
}

RemasterFrame build_remaster_frame(const GraphicsSettings& g, const FrameData& f, glm::ivec2 scene, glm::ivec2 output, int env_mips,
                                   RemasterHistory& h) {
    RemasterFrame r{};
    const Lighting& l = f.lighting;
    const glm::mat4 unjittered = f.view.projection * f.view.view;
    glm::vec2 jitter(0.0f);
    const bool taa = g.aa == AntiAliasing::Taa;
    if (taa) {
        const std::uint32_t i = h.frame % 8 + 1;
        jitter = (glm::vec2(halton(i, 2), halton(i, 3)) - 0.5f) * 2.0f / glm::vec2(scene);
    }
    if (scene != h.size) h.valid = false;
    r.view_proj = glm::translate(glm::mat4(1.0f), glm::vec3(jitter, 0.0f)) * unjittered;
    r.inv_view_proj = glm::inverse(r.view_proj);
    r.prev_view_proj = h.valid ? h.prev_view_proj : unjittered;
    r.view = f.view.view;
    const bool shadows = g.shadow_map_size > 0;
    if (shadows) r.shadow_matrix = fit_sun_shadow(f.view, l.sun_direction, g.shadow_distance, g.shadow_map_size);
    r.sun_dir = glm::vec4(glm::normalize(l.sun_direction), shadows ? 1.0f : 0.0f);
    r.sun_color = glm::vec4(l.sun_color, g.hdr ? g.exposure : 1.0f);
    r.ambient = glm::vec4(l.ambient, g.ibl ? 1.0f : 0.0f);
    r.fog_color = glm::vec4(l.fog_color, g.volumetric_fog ? g.fog_density : 0.0f);
    // Per-frame noise rotation only when something accumulates it (TAA, RT denoiser); else it would flicker.
    const float noise_frame = taa || g.rt.any() ? float(h.frame % 64) : 0.0f;
    r.fog_range = glm::vec4(l.fog_start, l.fog_end, g.preset == GraphicsPreset::Ultra ? 24.0f : 12.0f, noise_frame);
    r.camera_pos = glm::vec4(f.view.camera_position, g.ssao ? 1.0f : 0.0f);
    // World size of a shadow texel from the length of the matrix's x row: [0][0] alone shrank towards 0 for suns whose
    // light-space x axis lies off world x (night stages: normal offsets of 100+ units, Vulkan shadowed and GL lit all terrain).
    const float row0 = glm::length(glm::vec3(r.shadow_matrix[0][0], r.shadow_matrix[1][0], r.shadow_matrix[2][0]));
    const float texel_world = shadows ? 2.0f / (row0 * float(g.shadow_map_size)) : 0.0f;
    r.shadow_params = glm::vec4(shadows ? 1.0f / float(g.shadow_map_size) : 0.0f, g.shadow_softness, g.shadow_distance, texel_world * 1.5f);
    r.screen = glm::vec4(glm::vec2(scene), 1.0f / glm::vec2(scene));
    r.jitter = glm::vec4(jitter, taa && h.valid ? 1.0f : 0.0f, g.sharpness);
    r.flags = glm::vec4(g.pbr ? 1.0f : 0.0f, g.normal_maps ? 1.0f : 0.0f, g.particle_lighting ? 1.0f : 0.0f, g.hdr ? 1.0f : 0.0f);
    r.ssao = glm::vec4(g.ssao_radius, g.ssao_intensity, taa ? -1.0f : 0.0f, 3000.0f);
    r.env = glm::vec4(float(env_mips), f.view.projection[1][1], f.view.projection[0][0], float(output.x) / float(scene.x));
    // Auto exposure (post_exposure_adapt.glsl): time constant 0.5 s; snaps to its target without history (first frame,
    // resize, settings change) or on a camera cut, so short captures are deterministic; clamped to 0.5..4x the preset exposure
    // (`auto_exposure=false`: clamped to exactly the preset exposure).
    const float dt = f.dt > 0.0f ? f.dt : 1.0f / 30.0f, base = r.sun_color.w;
    const bool snap = !h.valid || glm::distance(f.view.camera_position, h.camera) >= kMotionTeleport;
    const float lo = g.auto_exposure ? 0.5f : 1.0f, hi = g.auto_exposure ? 4.0f : 1.0f;
    r.exposure = glm::vec4(1.0f - std::exp(-dt / 0.5f), snap ? 1.0f : 0.0f, base * lo, base * hi);

    h.prev_view_proj = unjittered;
    h.size = scene;
    h.camera = f.view.camera_position;
    h.valid = true;
    ++h.frame;
    return r;
}

std::size_t MotionHistory::Hash::operator()(const Key& k) const noexcept {
    const std::uint64_t a = (std::uint64_t(k.mesh) << 32 | k.first) * 0x9E3779B97F4A7C15ull;
    const std::uint64_t b = (std::uint64_t(k.object) << 32 | k.n) * 0xC2B2AE3D27D4EB4Full;
    return std::size_t(a ^ (b + (a << 6) + (a >> 2)));
}

void previous_models(std::span<const DrawItem> draws, MotionHistory& h, std::vector<glm::mat4>& prev) {
    prev.resize(draws.size());
    h.next.clear();
    h.counts.clear();
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const DrawItem& d = draws[i];
        MotionHistory::Key k{d.mesh.id, d.first_index, d.object, 0};
        k.n = h.counts[k]++;
        const auto it = h.models.find(k);
        const bool known = it != h.models.end() && glm::distance(glm::vec3(it->second[3]), glm::vec3(d.model[3])) < kMotionTeleport;
        prev[i] = known ? it->second : d.model;
        h.next.emplace(k, d.model);
    }
    std::swap(h.models, h.next);
}

}  // namespace joa::engine
