#version 450
#include "common.glsl"

// Locations match shaders/gl/mesh_vert.glsl; 3/4 (bones, weights) are not bound: characters are CPU-skinned.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 5) in vec2 a_uv2;
layout(location = 6) in vec4 a_color;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec2 v_uv2;
layout(location = 4) out vec4 v_color;
layout(location = 5) flat out uint v_draw;

// One draw item per vkCmdDrawIndexed; firstInstance carries its index into u_draws.
void main() {
    mat4 model = u_draws[gl_InstanceIndex].model;
    vec4 world = model * vec4(a_position, 1.0);
    v_world = world.xyz;
    v_normal = mat3(u_draws[gl_InstanceIndex].normal_matrix) * a_normal;
    v_uv = a_uv;
    v_uv2 = a_uv2;
    v_color = a_color;
    v_draw = gl_InstanceIndex;
    gl_Position = u_frame.view_proj * world;
}
