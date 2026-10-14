// Vulkan side of the remaster shader macros (shaders/common/remaster_common.glsl): set 2 holds the
// remaster frame block and textures (renderer_vk.cpp create_remaster_layouts).
#define VK_BACKEND 1
#define UBO(b) layout(set = 2, binding = b, std140) uniform
#define TEX(b) layout(set = 2, binding = b) uniform
#define VARY(n) layout(location = n)
#define VERTEX_ID gl_VertexIndex
