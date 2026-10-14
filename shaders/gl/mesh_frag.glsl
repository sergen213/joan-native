#version 330 core
in vec3 v_world;
in vec3 v_normal;
in vec2 v_uv;
in vec2 v_uv2;
in vec4 v_color;

uniform sampler2D u_albedo;
uniform sampler2D u_layer2;    // optional second albedo, weight = vertex colour alpha
uniform sampler2D u_lightmap;  // optional rgb multiplier on uv2
uniform bool u_has_layer2;
uniform bool u_has_lightmap;
uniform vec4 u_tint;
uniform float u_alpha_cutoff;  // < 0 disables alpha test
uniform bool u_unlit;
uniform vec3 u_sun_dir;        // normalised, direction light travels
uniform vec3 u_sun_color;
uniform vec3 u_ambient;
uniform vec3 u_fog_color;
uniform vec2 u_fog_range;      // start, end; end <= start disables fog
uniform vec3 u_camera_pos;

out vec4 o_color;

// All maths in linear space; the sRGB framebuffer encodes on write.
void main() {
    vec4 base = texture(u_albedo, v_uv);
    if (u_has_layer2) base.rgb = mix(base.rgb, texture(u_layer2, v_uv).rgb, v_color.a);
    else base.a *= v_color.a;
    base.rgb *= v_color.rgb;
    base *= u_tint;
    if (u_has_lightmap) base.rgb *= texture(u_lightmap, v_uv2).rgb;
    if (base.a < u_alpha_cutoff) discard;
    vec3 c = base.rgb;
    if (!u_unlit) {
        vec3 n = normalize(v_normal);
        if (!gl_FrontFacing) n = -n;
        c *= u_ambient + u_sun_color * max(dot(n, -u_sun_dir), 0.0);
    }
    if (u_fog_range.y > u_fog_range.x) {
        float f = clamp((distance(v_world, u_camera_pos) - u_fog_range.x) / (u_fog_range.y - u_fog_range.x), 0.0, 1.0);
        c = mix(c, u_fog_color, f);
    }
    o_color = vec4(c, base.a);
}
