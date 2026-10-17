#version 450
#include "common.glsl"

layout(location = 0) in vec2 a_position;  // framebuffer pixels, top-left origin
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_uv_rect;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out vec4 v_uv_rect;

void main() {
    // Vulkan NDC is already y-down, so pixels map without the flip the GL shader needs.
    gl_Position = vec4(a_position / u_frame.framebuffer.xy * 2.0 - 1.0, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
    v_uv_rect = a_uv_rect;
}
