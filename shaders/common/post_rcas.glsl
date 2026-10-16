// Final pass into the display target: FSR 1.0 RCAS (robust contrast-adaptive sharpening, after AMD
// FidelityFX ffx_fsr1.h, MIT licence) when R.jitter.w > 0, else a plain copy. u_final is sampled at the
// output pixel (bilinear when its size differs, i.e. render_scale < 1 without EASU). Supersampled input
// (render_scale > 1, R.env.w < 1): every tap is a 2x2 bilinear box over one output pixel (exact at 2x).
TEX(12) sampler2D u_final;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec2 g_px;  // one output pixel (or one source texel when not downsampling) in uv

vec3 tap(vec2 uv) {
    if (R.env.w >= 1.0) return texture(u_final, uv).rgb;
    vec2 q = 0.25 * g_px;
    return 0.25 * (texture(u_final, uv - q).rgb + texture(u_final, uv + q).rgb + texture(u_final, uv + vec2(q.x, -q.y)).rgb +
                   texture(u_final, uv + vec2(-q.x, q.y)).rgb);
}

void main() {
    g_px = 1.0 / (vec2(textureSize(u_final, 0)) * min(R.env.w, 1.0));
    vec3 e = tap(v_uv);
    if (R.jitter.w <= 0.0) {
        o_color = vec4(e, 1.0);
        return;
    }
    vec3 b = tap(v_uv + vec2(0.0, -g_px.y)), d = tap(v_uv + vec2(-g_px.x, 0.0));
    vec3 f = tap(v_uv + vec2(g_px.x, 0.0)), h = tap(v_uv + vec2(0.0, g_px.y));
    vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
    vec3 hit_min = min(mn4, e) / (4.0 * mx4 + 1e-5);
    vec3 hit_max = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0);
    vec3 lobe3 = max(-hit_min, hit_max);
    // sharpness 1 = 0 stops (strongest), 0 = 2 stops.
    float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * exp2(-(1.0 - R.jitter.w) * 2.0);
    o_color = vec4(clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0), 1.0);
}
