#version 450
#include "common.glsl"

layout(push_constant) uniform Push { uint texture; } u_push;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 o_color;

void main() { o_color = texture(u_textures[nonuniformEXT(u_push.texture)], v_uv) * v_color; }
