#version 330 core
in vec2 v_uv;
in vec4 v_color;
flat in vec4 v_uv_rect;  // sampled sub-rectangle (UiVertex::uv_rect)

uniform sampler2D u_texture;

out vec4 o_color;

// The UI pass blends in gamma space like the D3D8 original: write sRGB-encoded colour with the
// framebuffer's sRGB conversion disabled (renderer_gl.cpp draw_ui).
vec3 encode_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    // Clamp to the rect's edge texel centres: magnified (UI scale > 1) atlas sprites and glyphs would otherwise
    // filter in their neighbours' edge rows (pale lines along button art, specks around glyphs).
    vec2 half_texel = 0.5 / vec2(textureSize(u_texture, 0));
    vec2 uv = clamp(v_uv, v_uv_rect.xy + half_texel, max(v_uv_rect.zw - half_texel, v_uv_rect.xy + half_texel));
    // -0.5 LOD bias: mipmapped HD pack images (ui_core.cpp ui_image) stay on the sharp level while only mildly
    // minified (a 4x pack at 4K); no effect on the original, unmipmapped UI textures.
    vec4 c = texture(u_texture, uv, -0.5) * v_color;
    o_color = vec4(encode_srgb(c.rgb), c.a);
}
