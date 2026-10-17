#version 450
#include "common.glsl"

layout(push_constant) uniform Push { uint texture; } u_push;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in vec4 v_uv_rect;  // sampled sub-rectangle (UiVertex::uv_rect)

layout(location = 0) out vec4 o_color;

// The UI pass blends in gamma space like the D3D8 original: it renders into a UNORM view of the sRGB
// colour image, so the shader writes sRGB-encoded colour and blending operates on the stored bytes.
vec3 encode_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    // Clamp to the rect's edge texel centres: magnified (UI scale > 1) atlas sprites and glyphs would otherwise
    // filter in their neighbours' edge rows (pale lines along button art, specks around glyphs).
    const vec2 half_texel = 0.5 / vec2(textureSize(u_textures[nonuniformEXT(u_push.texture)], 0));
    const vec2 uv = clamp(v_uv, v_uv_rect.xy + half_texel, max(v_uv_rect.zw - half_texel, v_uv_rect.xy + half_texel));
    vec4 c = texture(u_textures[nonuniformEXT(u_push.texture)], uv) * v_color;
    o_color = vec4(encode_srgb(c.rgb), c.a);
}
