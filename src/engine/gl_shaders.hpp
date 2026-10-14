#pragma once
// GLSL sources embedded at configure time from shaders/gl/<name>.glsl (see src/engine/CMakeLists.txt).
namespace joa::engine::gl_shaders {
extern const char* const mesh_vert;
extern const char* const mesh_frag;
extern const char* const ui_vert;
extern const char* const ui_frag;
}  // namespace joa::engine::gl_shaders
