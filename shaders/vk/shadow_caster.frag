#version 450
// Remaster sun shadow map, Vulkan: depth only + the alpha test of mesh_pbr_frag.glsl's shadow pass (blended
// foliage at 0.5). A separate shader because mesh_pbr.frag statically uses the shadow map it would render to.
#include "common.glsl"
#include "remaster_vk.glsl"

layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec2 v_uv2;
layout(location = 4) in vec4 v_color;
layout(location = 6) flat in uint v_draw;

void main() {
    Draw d = u_draws[v_draw];
    vec2 uv = d.bump != kNoTexture ? v_uv + d.bump_scale * texture(u_textures[nonuniformEXT(d.bump)], v_uv2).rg : v_uv;
    float a = texture(u_textures[nonuniformEXT(d.texture)], uv).a;
    if (d.layer2 == kNoTexture) a *= v_color.a;
    float cutoff = u_materials[v_draw].blend == 2 ? 0.5 : d.alpha_cutoff;  // BlendMode::AlphaBlend
    if (a * d.tint.a < cutoff) discard;
}
