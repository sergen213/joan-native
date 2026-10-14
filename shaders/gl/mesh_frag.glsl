#version 330 core
in vec2 v_uv;
in vec2 v_uv2;
in vec4 v_color;
in vec3 v_light;
in float v_fog;

uniform sampler2D u_albedo;
uniform sampler2D u_layer2;    // optional second albedo, weight = vertex colour alpha
uniform sampler2D u_lightmap;  // optional rgb multiplier on uv2
uniform sampler2D u_bump;      // optional uv offset (D3DTOP_BUMPENVMAP), sampled on uv2
uniform float u_bump_scale;    // 0 = no bump
uniform bool u_has_layer2;
uniform bool u_has_lightmap;
uniform vec4 u_tint;
uniform float u_alpha_cutoff;  // < 0 disables alpha test
uniform vec3 u_fog_color;      // gamma space

out vec4 o_color;

vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

// Texture products in linear space (the sRGB framebuffer encodes on write); the light multiply and
// the fog blend happen in gamma space as on the original D3D8 device.
void main() {
    vec2 uv = u_bump_scale != 0.0 ? v_uv + u_bump_scale * texture(u_bump, v_uv2).rg : v_uv;
    vec4 base = texture(u_albedo, uv);
    if (u_has_layer2) base.rgb = mix(base.rgb, texture(u_layer2, v_uv).rgb, v_color.a);
    else base.a *= v_color.a;
    base.rgb *= v_color.rgb;
    base *= u_tint;
    if (u_has_lightmap) base.rgb *= texture(u_lightmap, v_uv2).rgb;
    if (base.a < u_alpha_cutoff) discard;
    vec3 c = base.rgb * to_linear(v_light);
    if (v_fog > 0.0) c = to_linear(mix(to_srgb(c), u_fog_color, v_fog));
    o_color = vec4(c, base.a);
}
