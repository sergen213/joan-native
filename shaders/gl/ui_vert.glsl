#version 330 core
layout(location = 0) in vec2 a_position;  // framebuffer pixels, top-left origin
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_uv_rect;

uniform vec2 u_framebuffer;

out vec2 v_uv;
out vec4 v_color;
flat out vec4 v_uv_rect;

void main() {
    vec2 ndc = a_position / u_framebuffer * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
    v_uv_rect = a_uv_rect;
}
