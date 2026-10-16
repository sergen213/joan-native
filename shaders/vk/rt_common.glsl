// Ray-traced effects (docs/RAYTRACING.md), shared by rt_mesh_pbr.frag and the rt_*.comp passes.
// Needs common.glsl (set 0 bindless textures), remaster_vk.glsl + remaster_common.glsl (set 2 `R` block),
// and the including file's #extension lines (GL_EXT_ray_query, GL_EXT_buffer_reference[_uvec2]).
// Set 3 (renderer_vk.cpp create_rt): 0 RtFrame, 1 TLAS, 2 instance table, 3-8 sampled RT targets,
// 9-11 storage images (compute only), 12 probe state, 13 probe rays (compute only).
#define RT_COMMON 1

TEX(2) sampler2D u_env;         // equirect sky radiance, mip = roughness (pbr_env.cpp)
TEX(3) sampler2D u_irradiance;  // equirect cosine-convolved sky, rgb / average in a
vec2 equirect_uv(vec3 d) { return vec2(atan(d.z, d.x) * (0.5 / kPi) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / kPi); }

layout(set = 3, binding = 0, std140) uniform RtFrameBlock {  // mirror: rt_scene.hpp RtFrame
    mat4 probe_rotation;  // random rotation of this frame's probe rays
    vec4 prev_camera;     // xyz previous camera, w 1 = temporal history valid
    vec4 params;          // x rays per pixel, y max roughness, z denoise, w frame index
    vec4 effects;         // x reflections, y shadows, z gi, w tan(sun cone half angle)
    vec4 gi_scale;        // rgb sky radiance -> Original ambient scale, w probe hysteresis
    ivec4 grid_base;      // xyz first world cell of the probe volume, w rays per probe
    ivec4 grid_dims;      // xyz probe counts, w total
    vec4 grid_spacing;    // xyz probe spacing, w max probe depth
} RT;
layout(set = 3, binding = 1) uniform accelerationStructureEXT u_tlas;

struct RtInstance {   // mirror: renderer_vk.cpp RtInstanceGpu
    uvec2 vertices;   // buffer device address of the mesh's Vertex array (renderer.hpp, 19 floats)
    uvec2 indices;    // ... of its uint32 indices
    uint first_index, albedo, layer2, lightmap;  // bindless slots, kNoTexture = none
    vec4 tint;
    vec4 diffuse;     // rgb D3D diffuse material, w != 0: vertex colour rgb is the diffuse material
    float cutoff;     // alpha test (non-opaque instances)
    uint flags, pad0, pad1;  // flags: 1 terrain, 2 two-sided (back faces count as front faces)
};
layout(set = 3, binding = 2, std430) readonly buffer RtInstances { RtInstance u_instances[]; };
layout(set = 3, binding = 3) uniform sampler2D u_gbuffer;       // oct normal xy, roughness, distance to the camera
layout(set = 3, binding = 4) uniform sampler2D u_gbuffer_prev;  // previous frame's
layout(set = 3, binding = 5) uniform sampler2D u_rt_shadow;     // r = sun visibility (denoised)
layout(set = 3, binding = 6) uniform sampler2D u_rt_reflection; // rgb = reflected radiance (denoised), a < 0: not traced
layout(set = 3, binding = 7) uniform sampler2D u_probe_irradiance;  // DDGI atlas, 8x8 texels per probe (6x6 + border)
layout(set = 3, binding = 8) uniform sampler2D u_probe_depth;       // rg = mean distance, mean squared distance
#ifndef RT_STATE_ACCESS
#define RT_STATE_ACCESS readonly
#endif
layout(set = 3, binding = 12, std430) RT_STATE_ACCESS buffer ProbeState { ivec4 u_probe_state[]; };  // xyz world cell, w updates | inactive

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtFloats { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtUints { uint v[]; };

const uint kRtMaskWorld = 0x01u, kRtMaskTerrain = 0x02u, kRtMaskCharacter = 0x04u, kRtMaskFoliage = 0x08u;  // rt_scene.hpp
const uint kRtMaskShadow = kRtMaskWorld | kRtMaskFoliage, kRtMaskProbe = kRtMaskWorld | kRtMaskTerrain | kRtMaskFoliage,
           kRtMaskAll = 0xffu;
const int kProbeInactive = 1 << 30;

// ---- helpers ---------------------------------------------------------------------------------------------

vec2 oct_encode(vec3 n) {  // y-up octahedral map onto [-1, 1]^2
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.xz;
    if (n.y < 0.0) e = (1.0 - abs(e.yx)) * vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    return e;
}
vec3 oct_decode(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

// Per-pixel sample k of the frame: interleaved gradient noise (animated by R) + R2 low-discrepancy offsets.
vec2 rt_xi(vec2 pixel, uint k) { return fract(vec2(ign(pixel), ign(pixel.yx + vec2(17.0, 59.0))) + float(k) * vec2(0.7548777, 0.5698403)); }

vec3 rt_sky(vec3 d) { return textureLod(u_env, equirect_uv(d), 0.0).rgb; }
vec3 rt_ibl_ambient(vec3 n) {  // the raster path's ambient (pbr_shade.glsl shade)
    vec3 a = to_linear(min(R.ambient.rgb, vec3(1.0)));
    if (R.ambient.w == 0.0) return a;
    vec4 irr = textureLod(u_irradiance, equirect_uv(n), 0.0);
    return a * irr.rgb / max(irr.a, 1e-3);
}
vec3 rt_fog(vec3 c, float dist) {  // the original linear fog over a reflected segment
    float span = R.fog_range.y - R.fog_range.x;
    float f = span > 0.0 ? clamp((dist - R.fog_range.x) / span, 0.0, 1.0) : 0.0;
    return f > 0.0 ? to_linear(mix(to_srgb(c), R.fog_color.rgb, f)) : c;
}

// ---- geometry --------------------------------------------------------------------------------------------

const uint kVertexFloats = 19u;  // sizeof(Vertex) / 4: position 0, normal 3, uv 6, bones 8, weights 9, uv2 13, colour 15
vec3 rt_v3(RtFloats f, uint i, uint o) {
    uint b = i * kVertexFloats + o;
    return vec3(f.v[b], f.v[b + 1u], f.v[b + 2u]);
}
vec2 rt_v2(RtFloats f, uint i, uint o) {
    uint b = i * kVertexFloats + o;
    return vec2(f.v[b], f.v[b + 1u]);
}
uvec3 rt_tri(RtInstance x, uint prim) {
    RtUints ind = RtUints(x.indices);
    uint b = x.first_index + prim * 3u;
    return uvec3(ind.v[b], ind.v[b + 1u], ind.v[b + 2u]);
}

// Alpha test of a candidate triangle, as the raster alpha test: albedo alpha x vertex alpha (no layer2) x tint.a.
bool rt_alpha_pass(uint id, uint prim, vec2 bary) {
    RtInstance x = u_instances[id];
    uvec3 t = rt_tri(x, prim);
    RtFloats f = RtFloats(x.vertices);
    vec3 b = vec3(1.0 - bary.x - bary.y, bary);
    vec2 uv = rt_v2(f, t.x, 6u) * b.x + rt_v2(f, t.y, 6u) * b.y + rt_v2(f, t.z, 6u) * b.z;
    float a = textureLod(u_textures[nonuniformEXT(x.albedo)], uv, 0.0).a * x.tint.a;
    if (x.layer2 == kNoTexture) a *= f.v[t.x * kVertexFloats + 18u] * b.x + f.v[t.y * kVertexFloats + 18u] * b.y + f.v[t.z * kVertexFloats + 18u] * b.z;
    return a >= x.cutoff;
}

struct RtHit {
    bool hit;
    float t;
    uint id, prim;
    vec2 bary;
    mat4x3 o2w;
};

RtHit rt_trace(vec3 o, vec3 d, float tmax, uint mask) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, u_tlas, gl_RayFlagsNoneEXT, mask, o, 0.0, d, tmax);
    while (rayQueryProceedEXT(q))
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            rt_alpha_pass(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false), rayQueryGetIntersectionPrimitiveIndexEXT(q, false),
                          rayQueryGetIntersectionBarycentricsEXT(q, false)))
            rayQueryConfirmIntersectionEXT(q);
    RtHit h;
    h.hit = rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
    h.t = tmax;
    h.id = h.prim = 0u;
    h.bary = vec2(0.0);
    h.o2w = mat4x3(1.0);
    if (h.hit) {
        h.t = rayQueryGetIntersectionTEXT(q, true);
        h.id = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
        h.prim = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
        h.bary = rayQueryGetIntersectionBarycentricsEXT(q, true);
        h.o2w = rayQueryGetIntersectionObjectToWorldEXT(q, true);
    }
    return h;
}

// 1 = nothing (after the alpha test) between o and o + d * tmax.
float rt_visibility(vec3 o, vec3 d, float tmax, uint mask) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT, mask, o, 0.0, d, tmax);
    while (rayQueryProceedEXT(q))
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            rt_alpha_pass(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false), rayQueryGetIntersectionPrimitiveIndexEXT(q, false),
                          rayQueryGetIntersectionBarycentricsEXT(q, false)))
            rayQueryConfirmIntersectionEXT(q);
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

struct RtSurface {
    vec3 p, n, ng;  // world; normals turned towards the ray origin
    vec2 uv, uv2;
    vec4 color;
    float uv_per_unit;  // texture-space footprint per world unit (ray-cone mip selection)
    bool front;         // hit the counter-clockwise side
};

RtSurface rt_surface(RtHit h, vec3 dir) {
    RtInstance x = u_instances[h.id];
    uvec3 t = rt_tri(x, h.prim);
    RtFloats f = RtFloats(x.vertices);
    vec3 b = vec3(1.0 - h.bary.x - h.bary.y, h.bary);
    vec3 p0 = h.o2w * vec4(rt_v3(f, t.x, 0u), 1.0), p1 = h.o2w * vec4(rt_v3(f, t.y, 0u), 1.0), p2 = h.o2w * vec4(rt_v3(f, t.z, 0u), 1.0);
    vec3 c = cross(p1 - p0, p2 - p0);
    float area = max(length(c), 1e-8);
    RtSurface s;
    s.p = p0 * b.x + p1 * b.y + p2 * b.z;
    s.ng = c / area;
    vec3 ni = mat3(h.o2w) * (rt_v3(f, t.x, 3u) * b.x + rt_v3(f, t.y, 3u) * b.y + rt_v3(f, t.z, 3u) * b.z);
    bool has_n = dot(ni, ni) > 1e-12;  // some original meshes carry zero normals
    // Facing from the vertex normals (what the original lit with): winding is unreliable on assets drawn with
    // culling off. Thin two-sided foliage cards have no inside at all.
    s.front = dot(has_n ? ni : s.ng, dir) < 0.0 || (x.flags & 2u) != 0u;
    if (dot(s.ng, dir) > 0.0) s.ng = -s.ng;
    s.n = has_n ? normalize(ni) : s.ng;
    if (dot(s.n, s.ng) < 0.0) s.n = -s.n;
    vec2 u0 = rt_v2(f, t.x, 6u), u1 = rt_v2(f, t.y, 6u), u2 = rt_v2(f, t.z, 6u);
    s.uv = u0 * b.x + u1 * b.y + u2 * b.z;
    s.uv2 = rt_v2(f, t.x, 13u) * b.x + rt_v2(f, t.y, 13u) * b.y + rt_v2(f, t.z, 13u) * b.z;
    s.color = vec4(rt_v3(f, t.x, 15u), f.v[t.x * kVertexFloats + 18u]) * b.x + vec4(rt_v3(f, t.y, 15u), f.v[t.y * kVertexFloats + 18u]) * b.y +
              vec4(rt_v3(f, t.z, 15u), f.v[t.z * kVertexFloats + 18u]) * b.z;
    vec2 d1 = u1 - u0, d2 = u2 - u0;
    s.uv_per_unit = sqrt(abs(d1.x * d2.y - d1.y * d2.x) / area);
    return s;
}

vec4 rt_tex(uint slot, vec2 uv, float footprint) {  // footprint: cone width in texture space (uv units)
    float lod = log2(max(footprint * float(textureSize(u_textures[nonuniformEXT(slot)], 0).x), 1.0));
    return textureLod(u_textures[nonuniformEXT(slot)], uv, lod);
}

// ---- DDGI probes (Majercik et al. 2019, relocation off) -----------------------------------------------------

ivec3 rt_probe_slot(ivec3 cell) { return (cell + RT.grid_dims.xyz * 4096) % RT.grid_dims.xyz; }  // operands kept positive
int rt_probe_index(ivec3 slot) { return slot.x + RT.grid_dims.x * (slot.y + RT.grid_dims.y * slot.z); }
ivec2 rt_probe_tile(ivec3 slot) { return ivec2((slot.x + slot.y * RT.grid_dims.x) * 8, slot.z * 8); }
vec2 rt_probe_uv(ivec3 slot, vec3 dir) {
    vec2 px = vec2(rt_probe_tile(slot)) + 1.0 + (oct_encode(dir) * 0.5 + 0.5) * 6.0;
    return px / vec2(RT.grid_dims.x * RT.grid_dims.y * 8, RT.grid_dims.z * 8);
}
bool rt_probe_live(ivec4 st, ivec3 cell) { return st.xyz == cell && (st.w & 0xffff) > 0 && (st.w & kProbeInactive) == 0; }

// Irradiance at p for normal n (v towards the viewer), in Original ambient units; IBL ambient outside the volume.
vec3 rt_gi(vec3 p, vec3 n, vec3 v) {
    vec3 sp = RT.grid_spacing.xyz;
    vec3 pb = p + (n * 0.2 + v * 0.8) * (0.3 * min(sp.x, sp.y));  // surface bias against self-shadowing in the moments
    vec3 g = pb / sp;
    ivec3 c0 = ivec3(floor(g));
    vec3 a = g - vec3(c0);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 cell = c0 + off, rel = cell - RT.grid_base.xyz;
        if (any(lessThan(rel, ivec3(0))) || any(greaterThanEqual(rel, RT.grid_dims.xyz))) continue;
        ivec3 slot = rt_probe_slot(cell);
        if (!rt_probe_live(u_probe_state[rt_probe_index(slot)], cell)) continue;
        vec3 to = vec3(cell) * sp - pb;
        float dist = length(to);
        vec3 dir = to / max(dist, 1e-4);
        vec3 tri = mix(1.0 - a, a, vec3(off));
        float wrap = (dot(dir, n) + 1.0) * 0.5;
        float w = wrap * wrap + 0.2;
        vec2 m = textureLod(u_probe_depth, rt_probe_uv(slot, -dir), 0.0).rg;
        if (dist > m.x) {  // Chebyshev visibility
            float var = abs(m.y - m.x * m.x), d = dist - m.x;
            w *= max(pow(var / (var + d * d), 3.0), 0.0);
        }
        w = max(w, 1e-6);
        if (w < 0.2) w *= w * w / 0.04;  // crush tiny weights (light leaks)
        w *= tri.x * tri.y * tri.z;
        sum += w * textureLod(u_probe_irradiance, rt_probe_uv(slot, n), 0.0).rgb;
        wsum += w;
    }
    vec3 ibl = rt_ibl_ambient(n);
    if (wsum <= 1e-5) return ibl;
    // Confidence: when visibility rejects nearly every probe (point hemmed in by probes stuck in walls or under the
    // terrain -- relocation is off) the normalised sum rests on a few crushed weights; fade to the raster ambient.
    // Volume edges fade the same way. Floor at 20 % of the Original ambient: the game has no fill lights, a fully
    // enclosed corner would otherwise go black (docs/RAYTRACING.md "artifacts").
    vec3 lo = g - vec3(RT.grid_base.xyz), hi = vec3(RT.grid_base.xyz + RT.grid_dims.xyz - 1) - g;
    float edge = clamp(min(min(min(lo.x, lo.z), min(hi.x, hi.z)), min(lo.y, hi.y)) * 0.5, 0.0, 1.0);
    return max(mix(ibl, sum / wsum, edge * smoothstep(0.01, 0.1, wsum)), 0.2 * ibl);
}

// ---- hit shading -----------------------------------------------------------------------------------------

// Radiance leaving the hit towards the ray origin: the raster path's split D3D8 light sum (ambient part from
// the probes or the IBL, sun part with one shadow ray), diffuse only. `spread` = ray cone angle (radians).
vec3 rt_shade_hit(RtHit h, vec3 dir, float spread, out bool front) {
    RtSurface s = rt_surface(h, dir);
    front = s.front;
    RtInstance x = u_instances[h.id];
    float fp = h.t * spread * s.uv_per_unit;
    vec4 base = rt_tex(x.albedo, s.uv, fp);
    vec4 vcol = s.color;
    if (x.diffuse.w != 0.0) vcol.rgb = vec3(1.0);
    if (x.layer2 != kNoTexture) base.rgb = mix(base.rgb, rt_tex(x.layer2, s.uv, fp).rgb, vcol.a);
    base.rgb *= vcol.rgb * x.tint.rgb;
    if (x.lightmap != kNoTexture) base.rgb *= textureLod(u_textures[nonuniformEXT(x.lightmap)], s.uv2, 0.0).rgb;
    vec3 diffuse = x.diffuse.w != 0.0 ? s.color.rgb : x.diffuse.rgb;
    vec3 l = -R.sun_dir.xyz;
    float nl = max(dot(s.n, l), 0.0);
    vec3 amb = to_linear(min(R.ambient.rgb, vec3(1.0)));
    vec3 direct = max(to_linear(min(R.ambient.rgb + diffuse * R.sun_color.rgb * nl, vec3(1.0))) - amb, vec3(0.0));
    if (nl > 0.0) direct *= rt_visibility(s.p + s.ng * (0.5 + h.t * 1e-3), l, 6000.0, kRtMaskShadow);
    vec3 ambient = RT.effects.z != 0.0 ? rt_gi(s.p, s.n, -dir) : rt_ibl_ambient(s.n);
    return base.rgb * (direct + ambient);
}

// ---- screen-space RT results in the forward pass -----------------------------------------------------------

bool rt_screen = false;  // set by mesh_pbr.glsl: this fragment is the G-buffer prepass surface
float rt_screen_shadow(vec2 pixel) { return texelFetch(u_rt_shadow, ivec2(pixel), 0).r; }
vec3 rt_screen_reflection(vec3 env, float roughness, vec2 pixel) {
    if (!rt_screen || RT.effects.x == 0.0) return env;
    vec4 r = texelFetch(u_rt_reflection, ivec2(pixel), 0);
    if (r.a < 0.0) return env;
    return mix(r.rgb, env, smoothstep(RT.params.y * 0.75, RT.params.y, roughness));  // fade into the IBL near max roughness
}

// Water (blended, not in the G-buffer): one mirror ray per fragment about the surface normal n. Returns radiance + Fresnel weight.
vec4 rt_water(vec3 p, vec3 n) {
    vec3 v = normalize(R.camera_pos.xyz - p);
    vec3 r = reflect(-v, n);
    r = normalize(vec3(r.x, max(r.y, 0.02), r.z));
    RtHit h = rt_trace(p + vec3(0.0, 0.5, 0.0), r, 6000.0, kRtMaskAll);
    bool front;
    vec3 c = h.hit ? rt_fog(rt_shade_hit(h, r, 1e-3, front), h.t) : rt_sky(r);
    return vec4(min(c, vec3(8.0)), 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0));
}
