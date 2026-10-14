#pragma once
// Image-based lighting inputs for the remaster path, built on the CPU from the frame's Lighting (the
// same sky model as sky_dome.cpp: fog colour at the horizon -> light band -> zenith blue, lit ground
// below) so both backends sample identical data:
//  - radiance: 64x32 equirect, mip i = GGX-like prefilter for roughness i / (levels - 1), sRGB;
//  - irradiance: 32x16 equirect cosine convolution / the upward irradiance, x0.5 (UNORM; shader x2), a = 0.5.
// Equirect: u = atan(z, x) / 2pi + 0.5, v = acos(y) / pi (row 0 = zenith); see pbr_shade.glsl equirect_uv.
#include "engine/renderer.hpp"

#include <cstdint>

namespace joa::engine {

struct EnvMaps {
    TextureData radiance, irradiance;
    int radiance_levels = 0;
    glm::vec3 sky_up{1.0f};  // linear cosine-weighted radiance over the upper hemisphere (RT GI calibration)
};

// Quantised hash of the inputs: rebuild only when it changes (day cycle drifts slowly).
std::uint64_t env_key(const Lighting& l);
EnvMaps build_env_maps(const Lighting& l);

}  // namespace joa::engine
