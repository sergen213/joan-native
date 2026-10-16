// Temporal anti-aliasing on the tone-mapped (sRGB-encoded) image: per-pixel motion reprojection, 3x3 YCoCg variance
// clip, 10% new sample (up to 30% for moving pixels, 100% when history is invalid or off screen).
// Motion (u_velocity, written by the mesh passes: previous minus current uv, both unjittered) is taken from the nearest
// surface of the 3x3 neighbourhood so moving silhouettes reproject with the object, not with what lies behind it.
// Because both ends are unjittered, a static pixel reads its history texel centre. Pixels without geometry (cleared
// background) reproject the far plane through the previous view-projection. The history is read with a 5-tap
// Catmull-Rom filter: a bilinear read at a sub-texel offset every frame compounds into blur.
TEX(6) sampler2D u_depth;
TEX(8) sampler2D u_ldr;
TEX(9) sampler2D u_history;
TEX(13) sampler2D u_velocity;
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
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float nearest = 1.0;
    vec2 nearest_uv = v_uv;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = v_uv + vec2(x, y) * R.screen.zw;
            vec3 s = ycocg(texture(u_ldr, uv).rgb);
            m1 += s;
            m2 += s * s;
            float d = texture(u_depth, uv).r;
            if (d < nearest) nearest = d, nearest_uv = uv;
        }
    vec2 motion;
    if (nearest < 1.0) motion = texture(u_velocity, nearest_uv).xy;
    else {
        vec4 prev = R.prev_view_proj * vec4(world_from_depth(v_uv, 1.0), 1.0);
        motion = ndc_to_uv(prev.xy / prev.w + R.jitter.xy) - v_uv;
    }
    vec2 puv = v_uv + motion;
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) {
        o_color = vec4(cur, 1.0);
        return;
    }
    // Variance clip (Salvi): pull the history towards the neighbourhood mean until it lies inside mean +- 1.25 sigma,
    // so colours a moving object left behind cannot survive in its trail.
    vec3 mu = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mu * mu, 0.0)) * 1.25 + 1e-4;
    vec3 d = ycocg(history_catmull_rom(puv)) - mu;
    vec3 k = abs(d) / sigma;
    vec3 hist = rgb(mu + d / max(1.0, max(k.x, max(k.y, k.z))));
    // A history read at a fractional texel offset loses detail every frame (Catmull-Rom still low-passes), and the loss
    // compounds over the ~10 frames the history remembers. Weight the new sample up with the sub-pixel part of the motion
    // (0 for static pixels, whose history sits on texel centres; full at a quarter pixel, so the small per-frame steps
    // of a 240 Hz display count too) and with its speed: 10% -> 30%.
    vec2 px = motion * R.screen.xy, sub = abs(fract(px + 0.5) - 0.5);
    float alpha = mix(0.1, 0.3, clamp(max(4.0 * (sub.x + sub.y), length(px) / 8.0), 0.0, 1.0));
    o_color = vec4(mix(hist, cur, alpha), 1.0);
}
