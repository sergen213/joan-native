// Remaster mesh fragment shader body: shaders/gl/mesh_pbr_frag.glsl with the Vulkan draw table + bindless textures.
// Included by mesh_pbr.frag and, with RAY_TRACED defined, by rt_mesh_pbr.frag (ray-traced effects, docs/RAYTRACING.md).
#include "common.glsl"
#include "remaster_vk.glsl"
#include "../common/remaster_common.glsl"
#ifdef RAY_TRACED
#include "rt_common.glsl"
#endif
#include "../common/pbr_shade.glsl"

layout(push_constant) uniform Push { uint pass; } u_push;  // 0 scene, 1 shadow caster, 2 RT G-buffer prepass

struct MaterialExtra {
    uint normal, rough, metal, ao;  // bindless slots, kNoTexture = none
    int cls;                        // MaterialClass
    int blend;                      // BlendMode
    int pad0, pad1;
};
layout(set = 2, binding = 11, std430) readonly buffer Materials { MaterialExtra u_materials[]; };

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec2 v_uv2;
layout(location = 4) in vec4 v_color;
layout(location = 5) in float v_fog;
layout(location = 6) flat in uint v_draw;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_ambient;

vec4 tex(uint slot, vec2 uv) { return texture(u_textures[nonuniformEXT(slot)], uv); }

void main() {
    Draw d = u_draws[v_draw];
    MaterialExtra x = u_materials[v_draw];
    bool unlit = (d.unlit & 1u) != 0u, no_fog = (d.unlit & 2u) != 0u, vertex_diffuse = d.diffuse.w != 0.0;
    vec2 uv = d.bump != kNoTexture ? v_uv + d.bump_scale * tex(d.bump, v_uv2).rg : v_uv;
    vec4 base = tex(d.texture, uv);
    vec4 vcol = v_color;
    if (!unlit && vertex_diffuse) vcol.rgb = vec3(1.0);
    if (d.layer2 != kNoTexture) base.rgb = mix(base.rgb, tex(d.layer2, v_uv).rgb, vcol.a);
    else base.a *= vcol.a;
    base.rgb *= vcol.rgb;
    base *= d.tint;
    if (d.lightmap != kNoTexture) base.rgb *= tex(d.lightmap, v_uv2).rgb;
    if (base.a < d.alpha_cutoff) discard;
    if (u_push.pass == 1u) {
        o_color = vec4(0.0);
        o_ambient = vec4(0.0);
        return;
    }
    vec3 ambient = vec3(0.0);
    vec3 c;
    if (unlit) {
#ifdef RAY_TRACED
        if (u_push.pass == 2u) {  // G-buffer: never reflective
            o_color = vec4(oct_encode(vec3(0.0, 1.0, 0.0)), 1.0, distance(v_world, R.camera_pos.xyz));
            o_ambient = vec4(0.0);
            return;
        }
#endif
        c = base.rgb;
        if (R.flags.z != 0.0 && !no_fog) {  // particles: lit smoke, HDR emissive fire
            if (x.blend == 3) c *= 1.6;
            else {
                float sun = sun_shadow(v_world, vec3(0.0, 1.0, 0.0), gl_FragCoord.xy);
                c *= to_linear(min(R.ambient.rgb + R.sun_color.rgb * (0.55 * sun), vec3(1.0)));
            }
        }
    } else {
        Surface s;
        s.albedo = base.rgb;
        vec3 ng = normalize(v_normal);
        // No back-face flip (D3D8 vertex lighting, see mesh_pbr_frag.glsl).
        s.ng = ng;
        s.n = ng;
        if (x.normal != kNoTexture && R.flags.y != 0.0) s.n = perturb_normal(ng, v_world, uv, tex(x.normal, uv).xyz * 2.0 - 1.0);
        s.p = v_world;
        s.diffuse = vertex_diffuse ? v_color.rgb : d.diffuse.rgb;
        legacy_material(x.cls, s.albedo, s.roughness, s.metal);
        if (x.rough != kNoTexture) s.roughness = tex(x.rough, uv).r;
        if (x.metal != kNoTexture) s.metal = tex(x.metal, uv).r;
#ifdef RAY_TRACED
        if (u_push.pass == 2u) {  // G-buffer prepass: what the ray-traced passes need per pixel
            o_color = vec4(oct_encode(s.n), s.roughness, distance(v_world, R.camera_pos.xyz));
            o_ambient = vec4(0.0);
            return;
        }
        rt_screen = x.blend <= 1;  // opaque / alpha-tested: this pixel is the prepass surface
#endif
        s.points = vec3(0.0);
        for (uint i = 0u; i < d.light_count; ++i) {
            Light p = u_lights[(d.lights[i >> 1] >> ((i & 1u) * 16u)) & 0xffffu];
            vec3 to = p.pos_range.xyz - v_world;
            float dist = length(to);
            if (dist < p.pos_range.w)
                s.points += p.color.rgb * max(dot(s.n, to / max(dist, 1e-4)), 0.0) / max(dot(p.att.xyz, vec3(1.0, dist, dist * dist)), 1e-4);
        }
        s.ao = x.ao != kNoTexture ? tex(x.ao, uv).r : 1.0;
        s.shadow_strength = x.cls == kClassVegetation ? 0.5 : 1.0;
        c = shade(s, gl_FragCoord.xy, ambient);
        if (x.cls == kClassVegetation) ambient = vec3(0.0);  // no SSAO on thin foliage
#ifdef RAY_TRACED
        if (x.blend == 2 && d.bump != kNoTexture && RT.effects.x != 0.0) {  // bump-mapped water: inline mirror ray
            vec4 w = rt_water(v_world, tex(d.bump, v_uv2).rg * d.bump_scale);
            c = mix(c, w.rgb, w.a);
            base.a = mix(base.a, 1.0, w.a);
        }
#endif
    }
    float fog = no_fog ? 0.0 : v_fog;
    o_color = vec4(apply_fog(c, fog), base.a);
    o_ambient = vec4(ambient * (1.0 - fog), 1.0);
}
