#pragma once
// GLSL sources embedded at configure time from shaders/gl/<name>.glsl and shaders/common/<name>.glsl
// (see src/engine/CMakeLists.txt).
namespace joa::engine::gl_shaders {
extern const char* const mesh_vert;
extern const char* const mesh_frag;
extern const char* const ui_vert;
extern const char* const ui_frag;
extern const char* const shadow_fill_vert;
extern const char* const shadow_fill_frag;
// Remaster (Enhanced/Ultra): remaster_prelude + remaster_common [+ pbr_shade] + body.
extern const char* const remaster_prelude;
extern const char* const remaster_common;
extern const char* const pbr_shade;
extern const char* const mesh_pbr_vert;
extern const char* const mesh_pbr_frag;
extern const char* const post_vert;
extern const char* const post_ssao;
extern const char* const post_ao_apply;
extern const char* const post_fog;  // volumetric fog, prepended to post_composite and post_exposure_lum
extern const char* const post_composite;
extern const char* const post_exposure_lum;    // auto exposure: measure (64x32) ...
extern const char* const post_exposure_adapt;  // ... and adapt (1x1 state)
extern const char* const post_taa;
extern const char* const post_easu;
extern const char* const post_rcas;
// Image-quality post (any preset): FXAA, SMAA 1x (smaa_config + third_party/smaa/SMAA.hlsl + pass body).
extern const char* const post_fxaa;
extern const char* const smaa_config;
extern const char* const SMAA;
extern const char* const post_smaa_edges;
extern const char* const post_smaa_weights;
extern const char* const post_smaa_blend;
}  // namespace joa::engine::gl_shaders
