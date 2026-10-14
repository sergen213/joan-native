// HDR scene -> display: volumetric height fog with shadowed sun in-scattering (ray-marched against the
// sun shadow map), exposure, ACES-fitted tone mapping, sRGB encoding into an RGBA8 UNORM target.
TEX(1) sampler2DShadow u_shadow;
TEX(4) sampler2D u_hdr;
TEX(6) sampler2D u_depth;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float sun_visibility(vec3 p) {
    if (R.sun_dir.w == 0.0) return 1.0;
    vec4 c = R.shadow_matrix * vec4(p, 1.0);
    vec3 ndc = c.xyz / c.w;
    if (any(greaterThan(abs(ndc), vec3(1.0)))) return 1.0;
    return texture(u_shadow, vec3(ndc_to_uv(ndc.xy), ndc.z * 0.5 + 0.5));
}

// Extinction per original unit: denser in the valleys below the camera, thinning out above it.
float density(vec3 p) { return R.fog_color.w * 0.7e-4 * exp(-max(p.y - R.camera_pos.y + 150.0, 0.0) / 450.0); }

void main() {
    vec3 c = texture(u_hdr, v_uv).rgb;
    if (R.fog_color.w > 0.0) {
        float d = texture(u_depth, v_uv).r;
        vec3 ro = R.camera_pos.xyz, target = world_from_depth(v_uv, d);
        vec3 rd = target - ro;
        float len = length(rd);
        rd /= max(len, 1e-4);
        len = d >= 1.0 ? 2500.0 : min(len, max(R.fog_range.y, 1.0));
        int steps = int(R.fog_range.z);
        float dt = len / float(steps), t = dt * ign(gl_FragCoord.xy);
        float g = 0.55, cosv = dot(rd, -R.sun_dir.xyz);
        float phase = (1.0 - g * g) / (4.0 * kPi * pow(1.0 + g * g - 2.0 * g * cosv, 1.5));
        vec3 sun = to_linear(min(R.sun_color.rgb, vec3(1.0))) * phase * 6.0;
        vec3 amb = to_linear(min(R.fog_color.rgb, vec3(1.0))) * 0.35;
        float trans = 1.0;
        vec3 scatter = vec3(0.0);
        for (int i = 0; i < steps; ++i, t += dt) {
            vec3 p = ro + rd * t;
            float sigma = density(p);
            float step_t = exp(-sigma * dt);
            scatter += trans * (1.0 - step_t) * (sun * sun_visibility(p) + amb);
            trans *= step_t;
        }
        c = c * trans + scatter;
    }
    c *= R.sun_color.w;
    if (R.flags.w != 0.0) c = aces(c);
    o_color = vec4(to_srgb(clamp(c, 0.0, 1.0)), 1.0);
}
