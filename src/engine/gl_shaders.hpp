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
extern const char* const post_composite;
extern const char* const post_taa;
extern const char* const post_easu;
extern const char* const post_rcas;
}  // namespace joa::engine::gl_shaders
