#version 450
#include "remaster_vk.glsl"
#include "../common/remaster_common.glsl"
// AMD FSR 3 inputs at render size (renderer_vk.cpp fsr_upscale, Vulkan only):
// - the device depth as R32F (FSR samples a colour image, not the combined depth-stencil);
// - complete unjittered motion, previous minus current uv: the mesh passes' per-object motion where geometry was drawn,
//   the far plane reprojected through the previous view-projection elsewhere (the background rule of post_taa.glsl);
// - the reactive mask: FSR's autogen formula (ffx_fsr3upscaler_autogen.h, Kabus 22 parameters: tonemap, max component,
//   threshold 0.2 -> 0.9) between the opaque-only and the final HDR scene, so what the translucent pass drew (particles,
//   smoke, fire, water) leans on the current frame. Done here instead of ffxFsr3UpscalerContextGenerateReactiveMask: a
//   second upscaler call per frame advances the SDK's 4-entry descriptor-view ring twice and frees views in flight.
TEX(4) sampler2D u_hdr;
TEX(6) sampler2D u_depth;
TEX(13) sampler2D u_velocity;
TEX(22) sampler2D u_opaque;
VARY(0) in vec2 v_uv;
layout(location = 0) out float o_depth;
layout(location = 1) out vec2 o_motion;
layout(location = 2) out float o_reactive;

vec3 tonemap(vec3 c) { return c / (max(c.r, max(c.g, c.b)) + 1.0); }

void main() {
    float d = texture(u_depth, v_uv).r;
    o_depth = d;
    if (d < 1.0) {
        o_motion = texture(u_velocity, v_uv).xy;
    } else {
        vec4 prev = R.prev_view_proj * vec4(world_from_depth(v_uv, 1.0), 1.0);
        o_motion = ndc_to_uv(prev.xy / prev.w + R.jitter.xy) - v_uv;
    }
    vec3 delta = abs(tonemap(texture(u_hdr, v_uv).rgb) - tonemap(texture(u_opaque, v_uv).rgb));
    o_reactive = max(delta.r, max(delta.g, delta.b)) < 0.2 ? 0.0 : 0.9;
}
