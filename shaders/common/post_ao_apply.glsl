// Removes the occluded share of the ambient + environment light (o_ambient of the opaque pass): the
// pipeline blends dst - src (reverse subtract). 3x3 depth-aware blur of the raw SSAO term.
TEX(5) sampler2D u_ambient;
TEX(6) sampler2D u_depth;
TEX(7) sampler2D u_ao;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    float d0 = texture(u_depth, v_uv).r;
    vec3 p0 = world_from_depth(v_uv, d0);
    float sum = 0.0, wsum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = v_uv + vec2(x, y) * R.screen.zw;
            vec3 p = world_from_depth(uv, texture(u_depth, uv).r);
            float w = 1.0 / (1.0 + distance(p, p0) / max(R.ssao.x * 0.25, 1e-3));
            sum += texture(u_ao, uv).r * w;
            wsum += w;
        }
    float ao = sum / wsum;
    o_color = vec4(texture(u_ambient, v_uv).rgb * (1.0 - ao), 0.0);
}
