#version 330 core
in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_texture;

out vec4 o_color;

// The UI pass blends in gamma space like the D3D8 original: write sRGB-encoded colour with the
// framebuffer's sRGB conversion disabled (renderer_gl.cpp draw_ui).
vec3 encode_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    o_color = vec4(encode_srgb(c.rgb), c.a);
}
