#version 330 core
// OpenGL side of the remaster shader macros (shaders/common/remaster_common.glsl). Sampler units are
// assigned by name in renderer_gl.cpp (Vulkan set 2 binding b = GL unit b + 7); the block binds to point 0.
#define VK_BACKEND 0
#define UBO(b) layout(std140) uniform
#define TEX(b) uniform
#define VARY(n)
#define VERTEX_ID gl_VertexID
