// Auto exposure, pass 2 into the 1x1 exposure state (R32F, ping-pong): the exposure at which the remaster's mean displayed
// luma equals the Original look's, interpolated log-log between the three measured exposures (post_exposure_lum.glsl),
// clamped (R.exposure.zw), then approached exponentially (weight R.exposure.x) or taken directly without history.
TEX(18) sampler2D u_exposure_prev;
TEX(19) sampler2D u_lum;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 m = vec4(0.0);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) m += texelFetch(u_lum, ivec2(x, y), 0);
    vec4 l = log(max(m * (1.0 / 2048.0), vec4(1e-4)));
    bool snap = R.exposure.y != 0.0;
    float e = snap ? R.sun_color.w : clamp(texelFetch(u_exposure_prev, ivec2(0), 0).r, R.exposure.z, R.exposure.w);
    // Steps of log2 exposure from e: segment [e/2, e] below the current luma, [e, 2e] above (extrapolated beyond).
    float t = l.a < l.g ? (l.a - l.g) / max(l.g - l.r, 1e-3) : (l.a - l.g) / max(l.b - l.g, 1e-3);
    float target = clamp(e * exp2(clamp(t, -2.0, 2.0)), R.exposure.z, R.exposure.w);
    o_color = vec4(snap ? target : e * pow(target / e, R.exposure.x), 0.0, 0.0, 1.0);
}
