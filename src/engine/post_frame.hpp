#pragma once
// Backend-independent per-frame constants of the remaster path (Enhanced/Ultra): the std140 mirror of
// shaders/common/remaster_common.glsl `RemasterFrame`, TAA jitter, the stabilised sun shadow frustum.
// Shared by renderer_gl.cpp and renderer_vk.cpp so both backends feed identical numbers to identical shaders.
#include "engine/preset.hpp"
#include "engine/renderer.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace joa::engine {

struct RemasterFrame {  // std140; field meanings in remaster_common.glsl
    glm::mat4 view_proj, inv_view_proj, prev_view_proj, view, shadow_matrix;
    glm::vec4 sun_dir, sun_color, ambient, fog_color, fog_range, camera_pos, shadow_params, screen, jitter, flags, ssao, env;
};
static_assert(sizeof(RemasterFrame) == 512);

// Temporal state kept by a backend between frames.
struct RemasterHistory {
    glm::mat4 prev_view_proj{1.0f};
    glm::ivec2 size{0, 0};
    std::uint32_t frame = 0;
    bool valid = false;  // false: the next frame's TAA starts from the current sample
};

// Scene (render-scale) size for an output viewport.
glm::ivec2 scene_size(const GraphicsSettings& g, glm::ivec2 output);

// Orthographic sun view-projection (GL clip convention) around the camera frustum slice [near, distance],
// texel-snapped so the static world does not shimmer as the camera moves.
glm::mat4 fit_sun_shadow(const FrameView& view, glm::vec3 sun_dir, float distance, int size);

// Fills the frame block (view_proj = the TAA-jittered matrix the scene passes rasterise with); advances `history`.
RemasterFrame build_remaster_frame(const GraphicsSettings& g, const FrameData& f, glm::ivec2 scene, glm::ivec2 output, int env_mips,
                                   RemasterHistory& history);

}  // namespace joa::engine
