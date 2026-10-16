// HDR scene -> display (after post_fog.glsl): volumetric fog, auto exposure (post_exposure_adapt.glsl), ACES-fitted tone
// mapping, sRGB encoding into an RGBA8 UNORM target.
TEX(4) sampler2D u_hdr;
TEX(20) sampler2D u_exposure;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 c = volumetric_fog(texture(u_hdr, v_uv).rgb, v_uv, gl_FragCoord.xy);
    c *= texelFetch(u_exposure, ivec2(0), 0).r;
    if (R.flags.w != 0.0) c = aces(c);
    o_color = vec4(to_srgb(clamp(c, 0.0, 1.0)), 1.0);
}
