#pragma once
// Backend-independent per-frame constants of the remaster path (Enhanced/Ultra): the std140 mirror of
// shaders/common/remaster_common.glsl `RemasterFrame`, TAA jitter, the stabilised sun shadow frustum.
// Shared by renderer_gl.cpp and renderer_vk.cpp so both backends feed identical numbers to identical shaders.
#include "engine/preset.hpp"
#include "engine/renderer.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace joa::engine {

struct RemasterFrame {  // std140; field meanings in remaster_common.glsl
    glm::mat4 view_proj, inv_view_proj, prev_view_proj, view, shadow_matrix;
    glm::vec4 sun_dir, sun_color, ambient, fog_color, fog_range, camera_pos, shadow_params, screen, jitter, flags, ssao, env, exposure, waves;
};
static_assert(sizeof(RemasterFrame) == 544);

// Temporal state kept by a backend between frames.
struct RemasterHistory {
    glm::mat4 prev_view_proj{1.0f};
    glm::ivec2 size{0, 0};
    std::uint32_t frame = 0;
    glm::vec3 camera{0.0f};  // previous camera position: a jump of kMotionTeleport units or more snaps the auto exposure
    bool valid = false;  // false: the next frame's TAA starts from the current sample
};

// Previous-frame model matrices for the motion vectors of the remaster mesh passes (TAA). A draw matches last frame's
// draw with the same mesh, first index, DrawItem::object and occurrence among those; unmatched draws (new objects) and
// jumps of kMotionTeleport units or more keep their current matrix, i.e. camera-only motion.
struct MotionHistory {
    struct Key {
        std::uint32_t mesh, first, object, n;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key& k) const noexcept;
    };
    std::unordered_map<Key, glm::mat4, Hash> models, next;
    std::unordered_map<Key, std::uint32_t, Hash> counts;  // per-frame scratch
};
inline constexpr float kMotionTeleport = 256.0f;  // world units per frame, far above any unit's per-frame travel

// prev[i] = the model matrix draws[i] had in the previous call (the previous rendered frame); advances `h`.
void previous_models(std::span<const DrawItem> draws, MotionHistory& h, std::vector<glm::mat4>& prev);

// Scene (render-scale) size for an output viewport; render_scale > 1 supersamples.
glm::ivec2 scene_size(const GraphicsSettings& g, glm::ivec2 output);

// SMAA area (160x560, rg) and search (64x16, r) lookup textures, RGBA8 Unorm, clamped, no mips (smaa_luts.cpp).
TextureData smaa_area_texture();
TextureData smaa_search_texture();

// Orthographic sun view-projection (GL clip convention) around the camera frustum slice [near, distance],
// texel-snapped so the static world does not shimmer as the camera moves.
glm::mat4 fit_sun_shadow(const FrameView& view, glm::vec3 sun_dir, float distance, int size);

// Fills the frame block (view_proj = the TAA-jittered matrix the scene passes rasterise with); advances `history`.
RemasterFrame build_remaster_frame(const GraphicsSettings& g, const FrameData& f, glm::ivec2 scene, glm::ivec2 output, int env_mips,
                                   RemasterHistory& history);

}  // namespace joa::engine
