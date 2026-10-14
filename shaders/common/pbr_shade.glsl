// Remaster surface shading shared by shaders/gl/mesh_pbr_frag.glsl and shaders/vk/mesh_pbr.frag.
// Needs remaster_common.glsl. Conservative conversion: the per-pixel D3D8 light sum of the Original path,
//   total = min(ambient + diffuse * (sun * N.L + point lights), 1)   (gamma space)
// is kept as the reference brightness of an unshadowed, unoccluded surface; it is split into an ambient
// part (shaped by the sky irradiance, occluded by AO/SSAO) and a direct part (sun share shadowed by the
// shadow map). Metallic-roughness specular (GGX sun + prefiltered sky environment) is added on top.

TEX(1) sampler2DShadow u_shadow;  // sun shadow map, compare LEQUAL
TEX(2) sampler2D u_env;           // equirect sky radiance, mip = roughness (pbr_env.cpp)
TEX(3) sampler2D u_irradiance;    // equirect cosine-convolved sky, rgb / average in a

// MaterialClass (renderer.hpp)
const int kClassGeneric = 0, kClassTerrain = 1, kClassCharacter = 2, kClassItem = 3, kClassBuilding = 4, kClassVegetation = 5,
          kClassParticle = 6;

struct Surface {
    vec3 albedo;      // linear, after vertex colour / layer2 / lightmap / tint
    vec3 n;           // shading normal (normal-mapped), world
    vec3 ng;          // geometric normal, world
    vec3 p;           // world position
    vec3 diffuse;     // D3D diffuse material, gamma (material or vertex colour)
    vec3 points;      // sum over point lights of colour * N.L / attenuation, gamma
    float roughness;
    float metal;
    float ao;         // material AO (HD pack map), 1 = none
    float shadow_strength;  // 1 = full sun shadow; vegetation 0.5 (dense tufts would otherwise shade themselves brown)
};

// Conservative legacy -> PBR conversion from the albedo alone (no HD maps): only unsaturated bright texels
// (steel) and saturated yellow ones (gilding) on characters and carried items become partly metallic.
void legacy_material(int cls, vec3 albedo, out float roughness, out float metal) {
    vec3 g = to_srgb(albedo);
    float hi = max(g.r, max(g.g, g.b)), lo = min(g.r, min(g.g, g.b));
    float sat = (hi - lo) / max(hi, 1e-4), lum = luma(g);
    float steel = (1.0 - smoothstep(0.10, 0.24, sat)) * smoothstep(0.38, 0.62, lum);
    float gold = smoothstep(0.35, 0.55, sat) * smoothstep(0.40, 0.60, lum) * step(g.b, g.g) * step(g.g * 0.55, g.b + 0.25) *
                 smoothstep(0.55, 0.75, g.g / max(g.r, 1e-4));
    float m = max(steel, gold);
    metal = 0.0;
    roughness = 0.8;
    if (cls == kClassCharacter) metal = 0.55 * m, roughness = mix(0.72, 0.38, m);
    else if (cls == kClassItem) metal = 0.75 * m, roughness = mix(0.65, 0.30, m);
    else if (cls == kClassTerrain) roughness = 0.95;
    else if (cls == kClassBuilding) roughness = 0.88;
    else if (cls == kClassVegetation) roughness = 0.75;
}

// Screen-space cotangent frame (Schueler 2006): tangent-space normal maps without vertex tangents.
// Map convention (docs/HD_ASSETS.md): rgb = n * 0.5 + 0.5, green = +V up in the image (OpenGL style).
vec3 perturb_normal(vec3 n, vec3 p, vec2 uv, vec3 tn) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float inv = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    // UV v grows downwards in the image (D3D), so image-up = -b.
    return normalize(mat3(t * inv, -b * inv, n) * tn);
}

const vec2 kPoisson[12] = vec2[](vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
                                 vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893),
                                 vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));

// Sun visibility from the shadow map (1 = lit): normal offset + 12-tap rotated Poisson PCF over the
// hardware 2x2 compare, faded out towards the edge of the shadow distance.
float sun_shadow(vec3 p, vec3 ng, vec2 pixel) {
    if (R.sun_dir.w == 0.0) return 1.0;
    vec3 l = -R.sun_dir.xyz;
    float cosl = clamp(dot(ng, l), 0.0, 1.0);
    p += ng * R.shadow_params.w * (1.0 - cosl * 0.5);
    vec4 c = R.shadow_matrix * vec4(p, 1.0);
    vec3 ndc = c.xyz / c.w;
    if (any(greaterThan(abs(ndc), vec3(1.0)))) return 1.0;
    vec2 uv = ndc_to_uv(ndc.xy);
    float z = ndc.z * 0.5 + 0.5;
    float a = ign(pixel) * 6.2831853;
    mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));
    float radius = max(R.shadow_params.y, 0.5) * R.shadow_params.x;
    float lit = 0.0;
    for (int i = 0; i < 12; ++i) lit += texture(u_shadow, vec3(uv + rot * kPoisson[i] * radius, z));
    lit /= 12.0;
    float fade = smoothstep(0.85, 1.0, distance(p, R.camera_pos.xyz) / R.shadow_params.z);
    return mix(lit, 1.0, fade);
}

vec2 equirect_uv(vec3 d) { return vec2(atan(d.z, d.x) * (0.5 / kPi) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / kPi); }

// Karis' analytic environment BRDF approximation (UE4 mobile).
vec3 env_brdf(vec3 f0, float roughness, float nv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

float d_ggx(float nh, float a) {
    float a2 = a * a, d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}
float v_smith(float nv, float nl, float a) {
    float k = a * 0.5;
    return 0.25 / max((nv * (1.0 - k) + k) * (nl * (1.0 - k) + k), 1e-4);
}

// Returns the lit colour; `ambient_out` receives the occludable (ambient + environment) part for SSAO.
vec3 shade(Surface s, vec2 pixel, out vec3 ambient_out) {
    vec3 l = -R.sun_dir.xyz, v = normalize(R.camera_pos.xyz - s.p);
    float nl = max(dot(s.n, l), 0.0);
    vec3 sun_g = s.diffuse * R.sun_color.rgb * nl, point_g = s.diffuse * s.points;
    vec3 amb_lin = to_linear(min(R.ambient.rgb, vec3(1.0)));
    vec3 total_lin = to_linear(min(R.ambient.rgb + sun_g + point_g, vec3(1.0)));
    vec3 direct_lin = max(total_lin - amb_lin, vec3(0.0));
    float sun_share = luma(sun_g) / max(luma(sun_g) + luma(point_g), 1e-4);
    float shadow = mix(1.0, sun_shadow(s.p, s.ng, pixel), s.shadow_strength);
    vec3 direct = direct_lin * mix(1.0, shadow, sun_share);

    vec3 ambient = amb_lin;
    if (R.ambient.w != 0.0) {  // directional sky light with the same average
        vec4 irr = textureLod(u_irradiance, equirect_uv(s.n), 0.0);
        ambient *= irr.rgb / max(irr.a, 1e-3);
    }
    ambient *= s.ao;
    vec3 diffuse_albedo = s.albedo * (1.0 - s.metal);
    vec3 color = diffuse_albedo * direct;
    ambient_out = diffuse_albedo * ambient;

    if (R.flags.x != 0.0) {
        float a = max(s.roughness * s.roughness, 0.002);
        vec3 f0 = mix(vec3(0.04), s.albedo, s.metal);
        float nv = clamp(dot(s.n, v), 1e-3, 1.0);
        vec3 h = normalize(l + v);
        float vh = clamp(dot(v, h), 0.0, 1.0);
        vec3 f = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
        vec3 sun_lin = to_linear(min(R.sun_color.rgb, vec3(1.0)));
        color += d_ggx(max(dot(s.n, h), 0.0), a) * v_smith(nv, nl, a) * f * sun_lin * nl * shadow;
        if (R.ambient.w != 0.0) {
            vec3 r = reflect(-v, s.n);
            vec3 env = textureLod(u_env, equirect_uv(r), s.roughness * (R.env.x - 1.0)).rgb;
            // Sky radiance scaled to the scene's ambient level; occluded like the ambient light.
            ambient_out += env * env_brdf(f0, s.roughness, nv) * s.ao;
        }
    }
    return color + ambient_out;
}

// Original linear vertex fog, mixed in gamma space like D3D8 (v_fog from the vertex shader).
vec3 apply_fog(vec3 c, float fog) { return fog > 0.0 ? to_linear(mix(to_srgb(c), R.fog_color.rgb, fog)) : c; }
