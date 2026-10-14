#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec4 a_bones;   // skinning inputs: bound, unused (characters are CPU-skinned)
layout(location = 4) in vec4 a_weights;
layout(location = 5) in vec2 a_uv2;
layout(location = 6) in vec4 a_color;

const int kMaxLights = 7;  // kMaxDrawLights

uniform mat4 u_model;
uniform mat4 u_view_proj;
uniform mat3 u_normal_matrix;
uniform bool u_unlit;
uniform bool u_no_fog;
uniform vec3 u_diffuse;        // material diffuse, gamma space
uniform bool u_vertex_diffuse; // vertex colour rgb = diffuse material (D3DMCS_COLOR1)
uniform vec3 u_sun_dir;        // normalised, direction light travels
uniform vec3 u_sun_color;      // gamma space
uniform vec3 u_ambient;        // gamma space
uniform vec2 u_fog_range;      // start, end; end <= start disables fog
uniform vec3 u_camera_pos;
uniform int u_light_count;
uniform vec4 u_light_pos_range[kMaxLights];  // xyz position, w range
uniform vec3 u_light_color[kMaxLights];      // gamma space
uniform vec3 u_light_att[kMaxLights];        // a0, a1, a2

out vec2 v_uv;
out vec2 v_uv2;
out vec4 v_color;
out vec3 v_light;  // gamma-space D3D vertex diffuse, clamped to [0, 1]
out float v_fog;   // 0 = no fog, 1 = fog colour

// D3D8 fixed-function vertex lighting and linear vertex fog (shader/lighting.nvo, fog.nvo).
void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_uv = a_uv;
    v_uv2 = a_uv2;
    v_color = a_color;
    v_light = vec3(1.0);
    if (!u_unlit) {
        vec3 diffuse = u_diffuse;
        if (u_vertex_diffuse) diffuse = a_color.rgb, v_color.rgb = vec3(1.0);
        vec3 n = normalize(u_normal_matrix * a_normal);
        vec3 l = u_sun_color * max(dot(n, -u_sun_dir), 0.0);
        for (int i = 0; i < u_light_count; ++i) {
            vec3 to = u_light_pos_range[i].xyz - world.xyz;
            float d = length(to);
            if (d < u_light_pos_range[i].w)
                l += u_light_color[i] * max(dot(n, to / max(d, 1e-4)), 0.0) / max(dot(u_light_att[i], vec3(1.0, d, d * d)), 1e-4);
        }
        v_light = min(u_ambient + diffuse * l, vec3(1.0));
    }
    float span = u_fog_range.y - u_fog_range.x;
    v_fog = span > 0.0 && !u_no_fog ? clamp((distance(world.xyz, u_camera_pos) - u_fog_range.x) / span, 0.0, 1.0) : 0.0;
    gl_Position = u_view_proj * world;
}
