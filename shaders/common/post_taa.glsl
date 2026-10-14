// Temporal anti-aliasing on the tone-mapped (sRGB-encoded) image: depth reprojection with the previous
// unjittered view-projection, 3x3 YCoCg neighbourhood clamp, 10% new sample (100% when history is invalid).
// The reprojection removes this frame's jitter (a static pixel reads its history texel centre) and the history
// is read with a 5-tap Catmull-Rom filter: a bilinear read at a sub-texel offset every frame compounds into blur.
TEX(6) sampler2D u_depth;
TEX(8) sampler2D u_ldr;
TEX(9) sampler2D u_history;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 ycocg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 rgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Catmull-Rom (Jimenez 2016, 5 bilinear taps; the 4 corner taps' weights are negligible).
vec3 history_catmull_rom(vec2 uv) {
    vec2 size = vec2(textureSize(u_history, 0)), inv = 1.0 / size;
    vec2 pos = uv * size, c = floor(pos - 0.5) + 0.5, f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2, t0 = (c - 1.0) * inv, t3 = (c + 2.0) * inv, t12 = (c + w2 / w12) * inv;
    vec3 s = texture(u_history, vec2(t12.x, t0.y)).rgb * (w12.x * w0.y) + texture(u_history, vec2(t0.x, t12.y)).rgb * (w0.x * w12.y) +
             texture(u_history, t12).rgb * (w12.x * w12.y) + texture(u_history, vec2(t3.x, t12.y)).rgb * (w3.x * w12.y) +
             texture(u_history, vec2(t12.x, t3.y)).rgb * (w12.x * w3.y);
    return max(s / (w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y), 0.0);
}

void main() {
    vec3 cur = texture(u_ldr, v_uv).rgb;
    if (R.jitter.z == 0.0) {
        o_color = vec4(cur, 1.0);
        return;
    }
    vec3 lo = vec3(1e9), hi = vec3(-1e9), m1 = vec3(0.0), m2 = vec3(0.0);
    float nearest = 1.0;
    vec2 nearest_uv = v_uv;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = v_uv + vec2(x, y) * R.screen.zw;
            vec3 s = ycocg(texture(u_ldr, uv).rgb);
            lo = min(lo, s);
            hi = max(hi, s);
            m1 += s;
            m2 += s * s;
            float d = texture(u_depth, uv).r;
            if (d < nearest) nearest = d, nearest_uv = uv;
        }
    // Variance clip box (Salvi), intersected with the min/max box.
    vec3 mu = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mu * mu, 0.0));
    lo = max(lo, mu - sigma * 1.25);
    hi = min(hi, mu + sigma * 1.25);
    // world_from_depth uses the jittered inverse: the point lies at ndc - jitter in the unjittered projection.
    vec3 world = world_from_depth(nearest_uv, nearest);
    vec4 prev = R.prev_view_proj * vec4(world, 1.0);
    vec2 puv = ndc_to_uv(prev.xy / prev.w + R.jitter.xy) + (v_uv - nearest_uv);
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) {
        o_color = vec4(cur, 1.0);
        return;
    }
    vec3 hist = rgb(clamp(ycocg(history_catmull_rom(puv)), lo, hi));
    o_color = vec4(mix(hist, cur, 0.1), 1.0);
}
