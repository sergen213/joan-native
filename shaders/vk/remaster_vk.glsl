// Vulkan side of the remaster shader macros (shaders/common/remaster_common.glsl): set 2 holds the
// remaster frame block and textures (renderer_vk.cpp create_remaster_layouts).
#define VK_BACKEND 1
#define UBO(b) layout(set = 2, binding = b, std140) uniform
#define TEX(b) layout(set = 2, binding = b) uniform
#define VARY(n) layout(location = n)
#define VERTEX_ID gl_VertexIndex

// Per-draw remaster data (renderer_vk.cpp MaterialExtraGpu, std430), indexed like the draw table.
struct MaterialExtra {
    mat4 prev_model;                // previous frame's model matrix (TAA motion vectors)
    uint normal, rough, metal, ao;  // bindless slots, kNoTexture = none
    int cls;                        // MaterialClass
    int blend;                      // BlendMode
    float water;                    // Material::water when `water=enhanced`, else -1
    int pad1;
};
layout(set = 2, binding = 11, std430) readonly buffer Materials { MaterialExtra u_materials[]; };
