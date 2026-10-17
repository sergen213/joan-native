// Shared by the Vulkan shaders: set 0 = bindless textures, set 1 = per-frame data.
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

layout(set = 1, binding = 0, std140) uniform Frame {
    mat4 view_proj;   // Vulkan clip correction already applied
    vec4 sun_dir;     // xyz normalised, direction light travels
    vec4 sun_color;   // gamma space
    vec4 ambient;     // gamma space
    vec4 fog_color;   // gamma space
    vec4 fog_range;   // x start, y end; end <= start disables fog
    vec4 camera_pos;
    vec4 framebuffer; // xy pixels
} u_frame;

struct Draw {
    mat4 model;
    mat4 normal_matrix;  // upper 3x3 used
    vec4 tint;
    vec4 diffuse;        // rgb material diffuse (gamma); w != 0: vertex colour rgb is the diffuse material
    float alpha_cutoff;  // < 0 disables alpha test
    uint unlit;  // bit 0 unlit, bit 1 no fog, bit 2 alpha-to-coverage (MSAA), bit 3 unit (Material::volume_caster)
    uint texture;        // bindless slots; kNoTexture = unused
    uint layer2;
    uint lightmap;
    uint light_count;
    uint bump;           // bindless slot, kNoTexture = none: albedo uv += bump_scale * bump(uv2).rg
    float bump_scale;
    uvec4 lights;        // u_lights indices, two 16-bit values per component (low half first)
};
const uint kNoTexture = 0xffffffffu;
layout(set = 1, binding = 1, std430) readonly buffer Draws { Draw u_draws[]; };

struct Light {
    vec4 pos_range;  // xyz position, w range
    vec4 color;      // rgb gamma space
    vec4 att;        // a0, a1, a2
};
layout(set = 1, binding = 2, std430) readonly buffer Lights { Light u_lights[]; };
