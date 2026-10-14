// FSR 1.0 EASU (edge-adaptive spatial upsampling), after AMD FidelityFX FSR1 ffx_fsr1.h (MIT licence,
// Copyright (c) 2021 Advanced Micro Devices, Inc.): 12-tap directional Lanczos-2 approximation with
// deringing, written with texelFetch for GLSL 330. Input: u_src at scene size; output: R.screen * R.env.w.
TEX(10) sampler2D u_src;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 fetch(ivec2 p, ivec2 size) { return texelFetch(u_src, clamp(p, ivec2(0), size - 1), 0).rgb; }
float lum2(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void easu_set(inout vec2 dir, inout float len, float w, float la, float lb, float lc, float ld, float le) {
    float dc = ld - lc, cb = lc - lb;
    float lenx = max(abs(dc), abs(cb));
    lenx = lenx > 0.0 ? 1.0 / lenx : 0.0;
    float dirx = ld - lb;
    dir.x += dirx * w;
    lenx = clamp(abs(dirx) * lenx, 0.0, 1.0);
    len += lenx * lenx * w;
    float ec = le - lc, ca = lc - la;
    float leny = max(abs(ec), abs(ca));
    leny = leny > 0.0 ? 1.0 / leny : 0.0;
    float diry = le - la;
    dir.y += diry * w;
    leny = clamp(abs(diry) * leny, 0.0, 1.0);
    len += leny * leny * w;
}

void easu_tap(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;
    wb *= wb;
    wa *= wa;
    wb = 1.5625 * wb - 0.5625;
    float w = wb * wa;
    ac += c * w;
    aw += w;
}

void main() {
    ivec2 in_size = textureSize(u_src, 0);
    vec2 out_size = floor(R.screen.xy * R.env.w + 0.5);
    vec2 pp = gl_FragCoord.xy * (vec2(in_size) / out_size) - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 f0 = ivec2(fp);
    vec3 b = fetch(f0 + ivec2(0, -1), in_size), c = fetch(f0 + ivec2(1, -1), in_size);
    vec3 e = fetch(f0 + ivec2(-1, 0), in_size), f = fetch(f0, in_size), g = fetch(f0 + ivec2(1, 0), in_size), h = fetch(f0 + ivec2(2, 0), in_size);
    vec3 i = fetch(f0 + ivec2(-1, 1), in_size), j = fetch(f0 + ivec2(0, 1), in_size), k = fetch(f0 + ivec2(1, 1), in_size), l = fetch(f0 + ivec2(2, 1), in_size);
    vec3 n = fetch(f0 + ivec2(0, 2), in_size), o = fetch(f0 + ivec2(1, 2), in_size);
    float bl = lum2(b), cl = lum2(c), el = lum2(e), fl = lum2(f), gl = lum2(g), hl = lum2(h);
    float il = lum2(i), jl = lum2(j), kl = lum2(k), ll = lum2(l), nl = lum2(n), ol = lum2(o);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bl, el, fl, gl, jl);
    easu_set(dir, len, pp.x * (1.0 - pp.y), cl, fl, gl, hl, kl);
    easu_set(dir, len, (1.0 - pp.x) * pp.y, fl, il, jl, kl, nl);
    easu_set(dir, len, pp.x * pp.y, gl, jl, kl, ll, ol);
    float dirr = dot(dir, dir);
    bool zro = dirr < 1.0 / 32768.0;
    dirr = zro ? 1.0 : inversesqrt(dirr);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirr;
    len *= 0.5;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + (0.25 - 0.04 - 0.5) * len;
    float clp = 1.0 / lob;
    vec3 ac = vec3(0.0);
    float aw = 0.0;
    easu_tap(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easu_tap(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easu_tap(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easu_tap(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easu_tap(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easu_tap(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easu_tap(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easu_tap(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easu_tap(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easu_tap(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easu_tap(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easu_tap(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    vec3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));
    o_color = vec4(min(mx, max(mn, ac / aw)), 1.0);
}
