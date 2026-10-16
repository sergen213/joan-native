// Auto exposure, pass 1 into a 64x32 target (after post_fog.glsl), after the translucent pass: per cell, the mean displayed
// luma of the remaster with the volumetric fog at exposures e/2, e, 2e (rgb; e = last frame's exposure) and of the Original
// look (a: from the linear reference luma the mesh passes blend into the reference MRT like their colour). 4x4 samples per cell.
TEX(4) sampler2D u_hdr;
TEX(18) sampler2D u_exposure_prev;
TEX(21) sampler2D u_reference;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    float e = R.exposure.y != 0.0 ? R.sun_color.w : clamp(texelFetch(u_exposure_prev, ivec2(0), 0).r, R.exposure.z, R.exposure.w);
    vec2 cell = R.screen.xy / vec2(64.0, 32.0);
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            ivec2 p = ivec2((floor(gl_FragCoord.xy) + (vec2(x, y) + 0.5) * 0.25) * cell);
            vec3 c = volumetric_fog(texelFetch(u_hdr, p, 0).rgb, (vec2(p) + 0.5) * R.screen.zw, vec2(p));
            float original = texelFetch(u_reference, p, 0).r;
            sum += vec4(display_luma(c * (0.5 * e)), display_luma(c * e), display_luma(c * (2.0 * e)), to_srgb(vec3(min(original, 1.0))).x);
        }
    o_color = sum * (1.0 / 16.0);
}
