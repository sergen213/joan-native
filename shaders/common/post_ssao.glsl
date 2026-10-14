// SSAO at scene resolution: 12 world-space hemisphere samples around the depth-reconstructed normal,
// rotated per pixel (interleaved gradient noise; TAA and the 3x3 blur in post_ao_apply resolve it).
TEX(6) sampler2D u_depth;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_ao;

const vec3 kKernel[12] = vec3[](vec3(0.53, 0.18, 0.31), vec3(-0.42, 0.37, 0.12), vec3(0.06, -0.55, 0.42), vec3(-0.21, -0.12, 0.66),
                                vec3(0.71, -0.33, 0.24), vec3(-0.66, -0.47, 0.35), vec3(0.24, 0.81, 0.28), vec3(-0.12, 0.22, 0.21),
                                vec3(0.32, -0.08, 0.86), vec3(-0.85, 0.21, 0.40), vec3(0.11, 0.33, 0.52), vec3(-0.34, -0.79, 0.43));

void main() {
    float d = texture(u_depth, v_uv).r;
    if (d >= 1.0) {
        o_ao = vec4(1.0);
        return;
    }
    vec3 p = world_from_depth(v_uv, d);
    vec2 px = R.screen.zw;
    vec3 px1 = world_from_depth(v_uv + vec2(px.x, 0.0), texture(u_depth, v_uv + vec2(px.x, 0.0)).r);
    vec3 px0 = world_from_depth(v_uv - vec2(px.x, 0.0), texture(u_depth, v_uv - vec2(px.x, 0.0)).r);
    vec3 py1 = world_from_depth(v_uv + vec2(0.0, px.y), texture(u_depth, v_uv + vec2(0.0, px.y)).r);
    vec3 py0 = world_from_depth(v_uv - vec2(0.0, px.y), texture(u_depth, v_uv - vec2(0.0, px.y)).r);
    // Smaller of the one-sided differences: no normal smearing across depth edges.
    vec3 dx = abs(px1.z - p.z) + abs(px1.x - p.x) < abs(p.z - px0.z) + abs(p.x - px0.x) ? px1 - p : p - px0;
    vec3 dy = abs(py1.z - p.z) + abs(py1.x - p.x) < abs(p.z - py0.z) + abs(p.x - py0.x) ? py1 - p : p - py0;
    vec3 n = normalize(cross(dx, dy));
    vec3 to_cam = R.camera_pos.xyz - p;
    if (dot(n, to_cam) < 0.0) n = -n;
    float a = ign(gl_FragCoord.xy) * 6.2831853;
    vec3 t = normalize(abs(n.y) < 0.99 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, t);
    vec3 tr = t * cos(a) + b * sin(a), br = cross(n, tr);
    float radius = R.ssao.x, occluded = 0.0;
    float cam_dist = length(to_cam);
    for (int i = 0; i < 12; ++i) {
        vec3 k = kKernel[i];
        vec3 s = p + (tr * k.x + br * k.y + n * k.z) * radius;
        vec4 c = R.view_proj * vec4(s, 1.0);
        vec2 suv = ndc_to_uv(c.xy / c.w);
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) continue;
        vec3 q = world_from_depth(suv, texture(u_depth, suv).r);
        float sd = distance(R.camera_pos.xyz, s), qd = distance(R.camera_pos.xyz, q);
        float range = smoothstep(0.0, 1.0, radius / max(abs(cam_dist - qd), 1e-3));
        occluded += (qd < sd - radius * 0.05 ? 1.0 : 0.0) * range;
    }
    // Fade out with distance (screen-space radius below a pixel), keep the far landscape untouched.
    float fade = 1.0 - smoothstep(R.ssao.w * 0.5, R.ssao.w, cam_dist);
    o_ao = vec4(vec3(1.0 - clamp(occluded / 12.0 * R.ssao.y * fade, 0.0, 1.0)), 1.0);
}
