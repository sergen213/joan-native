#version 450
// Remaster mesh vertex shader: shaders/gl/mesh_pbr_vert.glsl with the Vulkan draw table (common.glsl).
#include "common.glsl"
#include "remaster_vk.glsl"
#include "../common/remaster_common.glsl"

layout(push_constant) uniform Push { uint pass; } u_push;  // 0 scene, 1 shadow caster

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 5) in vec2 a_uv2;
layout(location = 6) in vec4 a_color;
layout(location = 7) in vec3 a_prev_position;  // vertex binding 1: the previous frame's pose (CPU skinning), else this frame's

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec2 v_uv2;
layout(location = 4) out vec4 v_color;
layout(location = 5) out float v_fog;
layout(location = 6) flat out uint v_draw;
layout(location = 7) out vec4 v_clip;       // unjittered, this frame
layout(location = 8) out vec4 v_prev_clip;  // previous frame

invariant gl_Position;

void main() {
    Draw d = u_draws[gl_InstanceIndex];
    vec4 world = d.model * vec4(a_position, 1.0);
    v_world = world.xyz;
    v_normal = mat3(d.normal_matrix) * a_normal;
    v_uv = a_uv;
    v_uv2 = a_uv2;
    v_color = a_color;
    v_draw = gl_InstanceIndex;
    float span = R.fog_range.y - R.fog_range.x;
    v_fog = span > 0.0 && (d.unlit & 2u) == 0u ? clamp((distance(world.xyz, R.camera_pos.xyz) - R.fog_range.x) / span, 0.0, 1.0) : 0.0;
    MaterialExtra x = u_materials[gl_InstanceIndex];
    v_clip = unjittered_clip(world);
    v_prev_clip = R.prev_view_proj * (x.prev_model * vec4(a_prev_position, 1.0));
    vec4 p = (u_push.pass == 1u ? R.shadow_matrix : R.view_proj) * world;
    gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);  // GL clip -> Vulkan (renderer_vk.cpp kClipCorrection)
}
