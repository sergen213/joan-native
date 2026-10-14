// Temporal anti-aliasing on the tone-mapped (sRGB-encoded) image: depth reprojection with the previous
// unjittered view-projection, 3x3 YCoCg neighbourhood clamp, 10% new sample (100% when history is invalid).
TEX(6) sampler2D u_depth;
TEX(8) sampler2D u_ldr;
TEX(9) sampler2D u_history;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 ycocg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 rgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

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
    vec3 world = world_from_depth(nearest_uv, nearest);
    vec4 prev = R.prev_view_proj * vec4(world, 1.0);
    vec2 puv = ndc_to_uv(prev.xy / prev.w) + (v_uv - nearest_uv);
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) {
        o_color = vec4(cur, 1.0);
        return;
    }
    vec3 hist = rgb(clamp(ycocg(texture(u_history, puv).rgb), lo, hi));
    o_color = vec4(mix(hist, cur, 0.1), 1.0);
}
