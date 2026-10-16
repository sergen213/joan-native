// Remaster mesh fragment shader (prepended: remaster_prelude + remaster_common + pbr_shade).
// Texture combination identical to mesh_frag.glsl; lighting from pbr_shade.glsl.
in vec3 v_world;
in vec3 v_normal;
in vec2 v_uv;
in vec2 v_uv2;
in vec4 v_color;
in float v_fog;
in vec4 v_clip;
in vec4 v_prev_clip;

const int kMaxLights = 7;
uniform sampler2D u_albedo;
uniform sampler2D u_layer2;
uniform sampler2D u_lightmap;
uniform sampler2D u_bump;
uniform sampler2D u_normal_map;
uniform sampler2D u_rough_map;
uniform sampler2D u_metal_map;
uniform sampler2D u_ao_map;
uniform float u_bump_scale;
uniform bool u_has_layer2;
uniform bool u_has_lightmap;
uniform ivec4 u_maps;     // has normal, roughness, metal, ao map
uniform int u_class;      // MaterialClass
uniform int u_blend;      // BlendMode
uniform float u_water;    // Material::water when `water=enhanced`, else -1
uniform vec4 u_tint;
uniform float u_alpha_cutoff;
uniform bool u_a2c;  // MSAA alpha-to-coverage: the alpha test becomes a sharpened coverage alpha
uniform bool u_unlit;
uniform bool u_no_fog;
uniform vec3 u_diffuse;
uniform bool u_vertex_diffuse;
uniform int u_pass;
uniform int u_light_count;
uniform vec4 u_light_pos_range[kMaxLights];
uniform vec3 u_light_color[kMaxLights];
uniform vec3 u_light_att[kMaxLights];

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_ambient;
layout(location = 2) out vec4 o_motion;  // TAA motion (previous minus current uv); alpha 1 so blending passes it through
// Auto exposure reference (post_exposure_lum.glsl): linear luma of this pixel in the Original look, blended like o_color.
layout(location = 3) out vec4 o_reference;

// R.ssao.z: texture LOD bias (negative under TAA jitter: the accumulated image resolves finer mips).
vec4 tex(sampler2D s, vec2 uv) { return texture(s, uv, R.ssao.z); }

void main() {
    vec2 uv = u_bump_scale != 0.0 ? v_uv + u_bump_scale * tex(u_bump, v_uv2).rg : v_uv;
    vec4 base = tex(u_albedo, uv);
    vec4 vcol = v_color;
    if (!u_unlit && u_vertex_diffuse) vcol.rgb = vec3(1.0);
    if (u_has_layer2) base.rgb = mix(base.rgb, tex(u_layer2, v_uv).rgb, vcol.a);
    else base.a *= vcol.a;
    base.rgb *= vcol.rgb;
    base *= u_tint;
    if (u_has_lightmap) base.rgb *= tex(u_lightmap, v_uv2).rgb;
    if (u_a2c) {
        base.a = clamp((base.a - u_alpha_cutoff) / max(fwidth(base.a), 1e-4) + 0.5, 0.0, 1.0);
        if (base.a <= 0.0) discard;
    } else if (base.a < u_alpha_cutoff) discard;
    o_motion = vec4(motion_uv(v_clip, v_prev_clip), 0.0, 1.0);
    if (u_pass == 1) {  // shadow caster: depth only
        o_color = vec4(0.0);
        o_ambient = vec4(0.0);
        o_reference = vec4(0.0);
        return;
    }
    vec3 ambient = vec3(0.0);
    vec3 c, original;  // original: this pixel in the Original look (o_reference)
    if (u_unlit) {
        c = original = base.rgb;
        if (R.flags.z != 0.0 && !u_no_fog) {  // particles: lit smoke, HDR emissive fire
            if (u_blend == 3) c *= 1.6;
            else {
                float sun = sun_shadow(v_world, vec3(0.0, 1.0, 0.0), gl_FragCoord.xy);
                c *= to_linear(min(R.ambient.rgb + R.sun_color.rgb * (0.55 * sun), vec3(1.0)));
            }
        }
    } else {
        Surface s;
        s.albedo = base.rgb;
        vec3 ng = normalize(v_normal);
        // No back-face flip: D3D8 vertex lighting used the vertex normal for both faces (two-sided grass
        // with up normals would otherwise light its back faces from the ground).
        s.ng = ng;
        s.n = ng;
        if (u_maps.x != 0 && R.flags.y != 0.0) s.n = perturb_normal(ng, v_world, uv, tex(u_normal_map, uv).xyz * 2.0 - 1.0);
        s.p = v_world;
        s.diffuse = u_vertex_diffuse ? v_color.rgb : u_diffuse;
        s.points = vec3(0.0);
        for (int i = 0; i < u_light_count; ++i) {
            vec3 to = u_light_pos_range[i].xyz - v_world;
            float d = length(to);
            if (d < u_light_pos_range[i].w)
                s.points += u_light_color[i] * max(dot(s.n, to / max(d, 1e-4)), 0.0) / max(dot(u_light_att[i], vec3(1.0, d, d * d)), 1e-4);
        }
        legacy_material(u_class, s.albedo, s.roughness, s.metal);
        if (u_maps.y != 0) s.roughness = tex(u_rough_map, uv).r;
        if (u_maps.z != 0) s.metal = tex(u_metal_map, uv).r;
        s.ao = u_maps.w != 0 ? tex(u_ao_map, uv).r : 1.0;
        s.shadow_strength = u_class == kClassVegetation ? 0.5 : 1.0;
        s.env_weight = u_maps.y != 0 ? 1.0 : s.metal;
        c = shade(s, gl_FragCoord.xy, ambient);
        original = original_color(s);
        if (u_class == kClassVegetation) ambient = vec3(0.0);  // no SSAO on thin foliage
    }
    float alpha = base.a;
    if (u_water >= 0.0 && !u_unlit) {  // stylised water (renderer_gl.cpp: `water=enhanced` only), opaque over the scene copy
        vec2 wa, wb;
        water_uv(v_world, u_water, wa, wb);
        vec4 flat_n = vec4(0.5, 0.5, 1.0, 0.5);
        c = shade_water(v_world, u_maps.x != 0 ? tex(u_normal_map, wa) : flat_n, u_maps.x != 0 ? tex(u_normal_map, wb) : flat_n, u_water,
                        gl_FragCoord.xy);
        alpha = 1.0;
    }
    float fog = u_no_fog ? 0.0 : v_fog;
    o_color = vec4(apply_fog(c, fog), alpha);
    o_ambient = vec4(ambient * (1.0 - fog), 1.0);
    o_reference = vec4(luma(apply_fog(original, fog)), 0.0, 0.0, base.a);
}
