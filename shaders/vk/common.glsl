// Shared by the Vulkan shaders: set 0 = bindless textures, set 1 = per-frame data.
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

layout(set = 1, binding = 0, std140) uniform Frame {
    mat4 view_proj;   // Vulkan clip correction already applied
    vec4 sun_dir;     // xyz normalised, direction light travels
    vec4 sun_color;
    vec4 ambient;
    vec4 fog_color;
    vec4 fog_range;   // x start, y end; end <= start disables fog
    vec4 camera_pos;
    vec4 framebuffer; // xy pixels
} u_frame;

struct Draw {
    mat4 model;
    mat4 normal_matrix;  // upper 3x3 used
    vec4 tint;
    float alpha_cutoff;  // < 0 disables alpha test
    uint unlit;
    uint texture;        // bindless slots; kNoTexture = unused
    uint layer2;
    uint lightmap;
    uint pad0, pad1, pad2;
};
const uint kNoTexture = 0xffffffffu;
layout(set = 1, binding = 1, std430) readonly buffer Draws { Draw u_draws[]; };
