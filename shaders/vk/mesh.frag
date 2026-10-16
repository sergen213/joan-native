#version 450
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_uv2;
layout(location = 2) in vec4 v_color;
layout(location = 3) in vec3 v_light;
layout(location = 4) in float v_fog;
layout(location = 5) flat in uint v_draw;

layout(location = 0) out vec4 o_color;

vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

// Same maths as shaders/gl/mesh_frag.glsl: texture products in linear space, light multiply and fog
// blend in gamma space; the sRGB attachment encodes on write.
void main() {
    Draw d = u_draws[v_draw];
    vec2 uv = d.bump != kNoTexture ? v_uv + d.bump_scale * texture(u_textures[nonuniformEXT(d.bump)], v_uv2).rg : v_uv;
    vec4 base = texture(u_textures[nonuniformEXT(d.texture)], uv);
    if (d.layer2 != kNoTexture) base.rgb = mix(base.rgb, texture(u_textures[nonuniformEXT(d.layer2)], v_uv).rgb, v_color.a);
    else base.a *= v_color.a;
    base.rgb *= v_color.rgb;
    base *= d.tint;
    if (d.lightmap != kNoTexture) base.rgb *= texture(u_textures[nonuniformEXT(d.lightmap)], v_uv2).rgb;
    if ((d.unlit & 4u) != 0u) {  // MSAA alpha-to-coverage: the alpha test becomes a sharpened coverage alpha
        base.a = clamp((base.a - d.alpha_cutoff) / max(fwidth(base.a), 1e-4) + 0.5, 0.0, 1.0);
        if (base.a <= 0.0) discard;
    } else if (base.a < d.alpha_cutoff) discard;
    vec3 c = base.rgb * to_linear(v_light);
    if (v_fog > 0.0) c = to_linear(mix(to_srgb(c), u_frame.fog_color.rgb, v_fog));
    o_color = vec4(c, base.a);
}
