#version 450
#include "common.glsl"

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec2 v_uv2;
layout(location = 4) in vec4 v_color;
layout(location = 5) flat in uint v_draw;

layout(location = 0) out vec4 o_color;

// Same maths as shaders/gl/mesh_frag.glsl: linear space, the sRGB attachment encodes on write.
void main() {
    Draw d = u_draws[v_draw];
    vec4 base = texture(u_textures[nonuniformEXT(d.texture)], v_uv);
    if (d.layer2 != kNoTexture) base.rgb = mix(base.rgb, texture(u_textures[nonuniformEXT(d.layer2)], v_uv).rgb, v_color.a);
    else base.a *= v_color.a;
    base.rgb *= v_color.rgb;
    base *= d.tint;
    if (d.lightmap != kNoTexture) base.rgb *= texture(u_textures[nonuniformEXT(d.lightmap)], v_uv2).rgb;
    if (base.a < d.alpha_cutoff) discard;
    vec3 c = base.rgb;
    if (d.unlit == 0u) {
        vec3 n = normalize(v_normal);
        if (!gl_FrontFacing) n = -n;
        c *= u_frame.ambient.rgb + u_frame.sun_color.rgb * max(dot(n, -u_frame.sun_dir.xyz), 0.0);
    }
    vec2 fog = u_frame.fog_range.xy;
    if (fog.y > fog.x) {
        float f = clamp((distance(v_world, u_frame.camera_pos.xyz) - fog.x) / (fog.y - fog.x), 0.0, 1.0);
        c = mix(c, u_frame.fog_color.rgb, f);
    }
    o_color = vec4(c, base.a);
}
