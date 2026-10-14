#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec4 a_bones;   // skinning inputs: bound, unused (characters are CPU-skinned)
layout(location = 4) in vec4 a_weights;
layout(location = 5) in vec2 a_uv2;
layout(location = 6) in vec4 a_color;

uniform mat4 u_model;
uniform mat4 u_view_proj;
uniform mat3 u_normal_matrix;

out vec3 v_world;
out vec3 v_normal;
out vec2 v_uv;
out vec2 v_uv2;
out vec4 v_color;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world = world.xyz;
    v_normal = u_normal_matrix * a_normal;
    v_uv = a_uv;
    v_uv2 = a_uv2;
    v_color = a_color;
    gl_Position = u_view_proj * world;
}
