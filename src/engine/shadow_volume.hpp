#pragma once
// Stencil shadow volumes as the original builds them ("create shadow volume", FUN_005e3040, called from
// Scene::draw_shadow 0x5ee1d0 via the per-sector actor loop 0x5f1cc0; RENDERING_PIPELINE.md section 5):
// the caster's triangles facing the light are kept as the near cap, the silhouette edges between lit
// and unlit faces become side quads extruded along the light travel direction; no far cap (z-pass).
#include "engine/renderer.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace joa::engine {

// Welded triangle list of one caster mesh (an <am1>#S.AM1 or item #s.SMO): vertices with equal
// bind-pose positions share one index, so silhouette edges match across UV seams. The original builds
// this remap once per shadow model and keeps it (+0x108 of the mesh, Mem_add).
struct ShadowCaster {
    ShadowCaster() = default;
    ShadowCaster(std::span<const glm::vec3> bind_positions, std::span<const std::uint32_t> triangles);
    std::vector<std::uint32_t> triangles;  // welded, degenerate triangles dropped
    // Directed edge 3t+k (triangle t, edge k = (k, k+1)): the triangles holding its reverse are
    // twins[twin_first[3t+k] .. twin_first[3t+k+1]) -- the silhouette test without a per-frame edge sort.
    std::vector<std::uint32_t> twin_first, twins;
};

// Appends the volume of `caster` posed at `positions` (same order as the bind positions, any space;
// `light` = light travel direction in that space) to `out`, extruding `length` units. Front faces
// (counter-clockwise, outward) increment the stencil, back faces decrement (BlendMode::ShadowVolume).
void append_shadow_volume(const ShadowCaster& caster, std::span<const glm::vec3> positions, glm::vec3 light, float length,
                          MeshData& out);

// Lighting::shadow_color (gamma space, blended in gamma space by D3D8) for the linear-blending sRGB scene
// target: alpha' = 1 - (1 - a)^2.2 darkens like dst * (1 - a) does on gamma bytes (exact for black).
glm::vec4 shadow_fill_color(glm::vec4 gamma_rgba);

}  // namespace joa::engine