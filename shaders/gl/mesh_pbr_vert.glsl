// Remaster mesh vertex shader (prepended: remaster_prelude + remaster_common). Same inputs as mesh_vert.glsl;
// lighting moves to the fragment shader (pbr_shade.glsl), the original linear vertex fog factor is kept.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 5) in vec2 a_uv2;
layout(location = 6) in vec4 a_color;
layout(location = 7) in vec3 a_prev_position;  // previous frame's CPU-skinned pose (renderer_gl.cpp update_mesh)

uniform mat4 u_model;
uniform mat3 u_normal_matrix;
uniform bool u_no_fog;
uniform int u_pass;  // 0 scene, 1 shadow caster
uniform mat4 u_prev_model;
uniform bool u_prev_pose;  // a_prev_position is valid (else the pose did not change)

out vec3 v_world;
out vec3 v_normal;
out vec2 v_uv;
out vec2 v_uv2;
out vec4 v_color;
out float v_fog;
out vec4 v_clip;       // unjittered, this frame
out vec4 v_prev_clip;  // previous frame

invariant gl_Position;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world = world.xyz;
    v_normal = u_normal_matrix * a_normal;
    v_uv = a_uv;
    v_uv2 = a_uv2;
    v_color = a_color;
    float span = R.fog_range.y - R.fog_range.x;
    v_fog = span > 0.0 && !u_no_fog ? clamp((distance(world.xyz, R.camera_pos.xyz) - R.fog_range.x) / span, 0.0, 1.0) : 0.0;
    v_clip = unjittered_clip(world);
    v_prev_clip = R.prev_view_proj * (u_prev_model * vec4(u_prev_pose ? a_prev_position : a_position, 1.0));
    gl_Position = (u_pass == 1 ? R.shadow_matrix : R.view_proj) * world;
}
