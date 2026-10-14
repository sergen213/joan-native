#version 450
#include "common.glsl"

// Locations match shaders/gl/mesh_vert.glsl; 3/4 (bones, weights) are not bound: characters are CPU-skinned.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 5) in vec2 a_uv2;
layout(location = 6) in vec4 a_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_uv2;
layout(location = 2) out vec4 v_color;
layout(location = 3) out vec3 v_light;  // gamma-space D3D vertex diffuse, clamped to [0, 1]
layout(location = 4) out float v_fog;   // 0 = no fog, 1 = fog colour
layout(location = 5) flat out uint v_draw;

// One draw item per vkCmdDrawIndexed; firstInstance carries its index into u_draws.
// Same D3D8 vertex lighting and linear vertex fog as shaders/gl/mesh_vert.glsl.
void main() {
    Draw d = u_draws[gl_InstanceIndex];
    vec4 world = d.model * vec4(a_position, 1.0);
    v_uv = a_uv;
    v_uv2 = a_uv2;
    v_color = a_color;
    v_draw = gl_InstanceIndex;
    v_light = vec3(1.0);
    if ((d.unlit & 1u) == 0u) {
        vec3 diffuse = d.diffuse.rgb;
        if (d.diffuse.w != 0.0) diffuse = a_color.rgb, v_color.rgb = vec3(1.0);
        vec3 n = normalize(mat3(d.normal_matrix) * a_normal);
        vec3 l = u_frame.sun_color.rgb * max(dot(n, -u_frame.sun_dir.xyz), 0.0);
        for (uint i = 0u; i < d.light_count; ++i) {
            Light p = u_lights[(d.lights[i >> 1] >> ((i & 1u) * 16u)) & 0xffffu];
            vec3 to = p.pos_range.xyz - world.xyz;
            float dist = length(to);
            if (dist < p.pos_range.w)
                l += p.color.rgb * max(dot(n, to / max(dist, 1e-4)), 0.0) / max(dot(p.att.xyz, vec3(1.0, dist, dist * dist)), 1e-4);
        }
        v_light = min(u_frame.ambient.rgb + diffuse * l, vec3(1.0));
    }
    float span = u_frame.fog_range.y - u_frame.fog_range.x;
    v_fog = span > 0.0 && (d.unlit & 2u) == 0u ? clamp((distance(world.xyz, u_frame.camera_pos.xyz) - u_frame.fog_range.x) / span, 0.0, 1.0) : 0.0;
    gl_Position = u_frame.view_proj * world;
}
