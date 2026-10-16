// FXAA (Lottes 3.11 "quality" algorithm: local-contrast early out, horizontal/vertical edge choice, 12-step
// end-of-edge search with growing strides, sub-pixel blend) on the sRGB-encoded LDR image at scene resolution.
TEX(8) sampler2D u_ldr;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float lum_at(vec2 uv) { return luma(textureLod(u_ldr, uv, 0.0).rgb); }

void main() {
    vec2 t = R.screen.zw;
    vec3 rgb_m = textureLod(u_ldr, v_uv, 0.0).rgb;
    float l_m = luma(rgb_m);
    float l_n = lum_at(v_uv + vec2(0.0, -t.y)), l_s = lum_at(v_uv + vec2(0.0, t.y));
    float l_e = lum_at(v_uv + vec2(t.x, 0.0)), l_w = lum_at(v_uv + vec2(-t.x, 0.0));
    float l_max = max(l_m, max(max(l_n, l_s), max(l_e, l_w))), l_min = min(l_m, min(min(l_n, l_s), min(l_e, l_w)));
    float range = l_max - l_min;
    if (range < max(0.0312, l_max * 0.125)) {  // FXAA_QUALITY edge thresholds (min, relative)
        o_color = vec4(rgb_m, 1.0);
        return;
    }
    float l_nw = lum_at(v_uv - t), l_se = lum_at(v_uv + t);
    float l_ne = lum_at(v_uv + vec2(t.x, -t.y)), l_sw = lum_at(v_uv + vec2(-t.x, t.y));
    float horizontal = abs(l_nw + l_ne - 2.0 * l_n) + 2.0 * abs(l_w + l_e - 2.0 * l_m) + abs(l_sw + l_se - 2.0 * l_s);
    float vertical = abs(l_nw + l_sw - 2.0 * l_w) + 2.0 * abs(l_n + l_s - 2.0 * l_m) + abs(l_ne + l_se - 2.0 * l_e);
    bool is_h = horizontal >= vertical;
    float l1 = is_h ? l_n : l_w, l2 = is_h ? l_s : l_e, g1 = abs(l1 - l_m), g2 = abs(l2 - l_m);
    float step_len = is_h ? -t.y : -t.x, l_local, g_scaled;
    if (g1 >= g2) {
        l_local = 0.5 * (l1 + l_m);
        g_scaled = 0.25 * g1;
    } else {
        step_len = -step_len;
        l_local = 0.5 * (l2 + l_m);
        g_scaled = 0.25 * g2;
    }
    vec2 edge = v_uv + (is_h ? vec2(0.0, 0.5 * step_len) : vec2(0.5 * step_len, 0.0));
    vec2 dir = is_h ? vec2(t.x, 0.0) : vec2(0.0, t.y);
    vec2 p1 = edge - dir, p2 = edge + dir;
    float e1 = lum_at(p1) - l_local, e2 = lum_at(p2) - l_local;
    bool r1 = abs(e1) >= g_scaled, r2 = abs(e2) >= g_scaled;
    for (int i = 0; i < 12 && !(r1 && r2); ++i) {
        float q = i < 4 ? 1.0 : i < 8 ? 1.5 : 2.0;
        if (!r1) {
            p1 -= dir * q;
            e1 = lum_at(p1) - l_local;
            r1 = abs(e1) >= g_scaled;
        }
        if (!r2) {
            p2 += dir * q;
            e2 = lum_at(p2) - l_local;
            r2 = abs(e2) >= g_scaled;
        }
    }
    float d1 = is_h ? v_uv.x - p1.x : v_uv.y - p1.y, d2 = is_h ? p2.x - v_uv.x : p2.y - v_uv.y;
    bool nearer1 = d1 < d2;
    float span = d1 + d2;
    bool correct = ((nearer1 ? e1 : e2) < 0.0) != (l_m - l_local < 0.0);
    float offset = correct ? 0.5 - min(d1, d2) / span : 0.0;
    float average = (2.0 * (l_n + l_s + l_e + l_w) + l_nw + l_ne + l_sw + l_se) / 12.0;
    float sub = clamp(abs(average - l_m) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    offset = max(offset, sub * sub * 0.75);  // FXAA_QUALITY subpix 0.75
    vec2 uv = v_uv + (is_h ? vec2(0.0, offset * step_len) : vec2(offset * step_len, 0.0));
    o_color = vec4(textureLod(u_ldr, uv, 0.0).rgb, 1.0);
}
