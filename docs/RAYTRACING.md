# Hardware ray tracing (Vulkan)

Optional ray-traced **reflections**, **sun shadows** and **GI** on top of the remaster pipeline
([GRAPHICS_ROADMAP.md](GRAPHICS_ROADMAP.md) "Remaster"). Never required for gameplay: OpenGL, GPUs without ray
queries and the Original preset always run the raster path.

## Settings and fallback

settings.ini `[raytracing]` ([LAUNCHER.md](LAUNCHER.md)), resolved by `preset.cpp` into `GraphicsSettings::rt`
and handed to the renderer by `IRenderer::set_graphics`:

| Key | Values | Default |
|---|---|---|
| `reflections`, `shadows`, `gi` | bool | Ultra: on; Enhanced/Custom: off |
| `rays_per_pixel` | 1–4 (reflection and shadow rays per pixel per frame) | 1 |
| `max_roughness` | 0–1: traced up to this roughness, prefiltered IBL beyond (blend over the last 25 %) | 0.6 |
| `denoise` | bool (temporal + à-trous) | true |
| `probe_density` | low / medium / high = probe spacing 192 / 128 / 96 units (vertical half) | medium |

`[raytracing]` keys override the preset for any preset (the launcher writes explicit `false` values, so Ultra
from the launcher has RT only where the user ticked it). Behaviour when RT is requested but cannot run — one log
line, then the raster path of the *same* settings (shadow map or stencil volumes, IBL ambient + reflections):

| Situation | Log | Result |
|---|---|---|
| Original preset | `vulkan ray tracing (...) ignored: the Original preset never changes` | faithful path, bit-identical |
| OpenGL | `opengl: ray tracing needs the Vulkan renderer, using the raster path` | GL remaster |
| Vulkan device without `VK_KHR_acceleration_structure` + `VK_KHR_deferred_host_operations` + `VK_KHR_ray_query` + `bufferDeviceAddress` + `shaderStorageImageArrayDynamicIndexing` | `vulkan ray tracing (...) unavailable (<missing list>), using the raster path` | Vulkan remaster |
| `JOA_VK_DISABLE_RT=1` (tests, troubleshooting) | `... unavailable (disabled by JOA_VK_DISABLE_RT) ...` | same |

Capability detection runs at device selection (`renderer_vk.cpp probe_rt`, `rt_scene.hpp RtCaps`); the
extensions/features are enabled only when all are present. `VK_KHR_ray_tracing_pipeline` is detected and
logged but unused: every effect uses **ray queries** from compute or fragment shaders (no SBT, no recursion,
one code path for alpha tests and shading; RADV/RDNA3 runs ray queries at full speed).

## Architecture

Files: `src/engine/rt_scene.*` (API-free: capabilities, TLAS instance classes, culling, probe grid, `RtFrame`
block), `src/engine/vk_rt.*` (acceleration structures), RT section of `renderer_vk.cpp` (set 3, targets,
passes), `shaders/vk/rt_common.glsl` (shared ray/hit/probe code), `rt_mesh_pbr.frag` (= `mesh_pbr.glsl` with
`RAY_TRACED`), `rt_shadow.comp`, `rt_reflect.comp`, `rt_denoise.comp`, `rt_probe_trace.comp`, `rt_probe_blend.comp`.
`shaders/common/pbr_shade.glsl` has three `#ifdef RT_COMMON` hooks (GL never defines it).

Frame (Vulkan remaster path, RT on), in one command buffer:

1. **Acceleration structures** (`RtAccel::build`): one BLAS per drawn (mesh, index range). Static BLASes are
   built in the frame (`PREFER_FAST_TRACE | ALLOW_COMPACTION`), their compacted size is queried, and once that
   frame has retired they are copied compacted (S1L1: 328 BLAS, 38.2 → 25.3 MiB). Meshes replaced through
   `update_mesh` (CPU-skinned units) get `ALLOW_UPDATE | PREFER_FAST_BUILD` BLASes, refit in place in one batched
   `vkCmdBuildAccelerationStructuresKHR` per frame, rebuilt when the vertex count changes and every 64th update.
   The TLAS is rebuilt every frame from the draw list with **instance culling**: lit opaque/alpha-tested draws
   and distance-faded vegetation (alpha test 0.5, as the raster shadow casters); sky, particles, water, additive
   and stencil-volume draws are skipped; instances whose bounding sphere starts beyond 6000 units or subtends
   < 0.25 % of its distance are dropped. Masks: world / terrain / character / foliage; shadow rays skip the
   terrain (its lightmaps already hold its shading, like the shadow map), probe rays skip characters (no GI flicker).
   Alpha-tested geometry: instances are `FORCE_NO_OPAQUE`; the ray-query candidate loop fetches the triangle's uv
   through buffer references (mesh device addresses) and tests albedo alpha × tint.a × vertex alpha.
2. **G-buffer prepass**: opaque/alpha-tested draws with the RT fragment shader in pass 2 write depth and
   RGBA16F (octahedral shading normal incl. HD normal maps, roughness from the legacy/HD material, view distance).
3. **Compute** (sets 0–3 bound for compute, bindless textures for alpha tests and hit albedo):
   GI probes → shadow trace + denoise → reflection trace + denoise.
4. **Forward opaque pass** reloads the prepass depth (stencil cleared) and reads the RT results by pixel;
   translucent draws use the shadow map, the probes and (bump-mapped water) an inline mirror ray.

All RT images stay in `VK_IMAGE_LAYOUT_GENERAL`; passes are separated by global memory barriers. Every slot has
its own instance table, `RtFrame` UBO and descriptor set; the TLAS/scratch are shared (a barrier at the start of
the AS work orders them after the previous frame's ray queries). Resources replaced while a frame is recorded are
retired one timeline value later.

### Shadows

Directional sun visibility per pixel: `rays_per_pixel` rays in a cone of 1° half angle (uniform disk; the real
sun is 0.27°, widened for visible contact hardening), offset along the normal by 0.3 + 0.0015 × distance.
Characters, buildings and vegetation cast; **contact shadows** come for free (per-pixel rays, no texel size or
bias gap), e.g. self-shadowing of arms and armour plates and grass on the ground. The raster shadow map is kept
for translucent draws, lit smoke and volumetric fog; with RT shadows the stencil volumes are never drawn.
Temporal filter + à-trous (below). Point-light shadows: not done (optional in the brief).

### Reflections

GGX visible-normal sampling (Heitz 2018) of the G-buffer roughness up to `max_roughness`; hits shaded like the
raster path's split D3D8 light sum (diffuse albedo from the same texture/lightmap/vertex-colour/tint rules,
sun with one shadow ray, ambient from the probes or the IBL), the original linear fog over the reflected
segment, sky radiance on misses, 8× HDR firefly clamp, texture LOD from ray cones (triangle uv density × cone
width). The result replaces the prefiltered environment in `shade()` (`env × env_brdf`), fading back to the
IBL between 0.75 and 1.0 × `max_roughness`. Metal comes from the conservative legacy conversion (steel/gilding
texels on characters and carried items) or HD `_m` maps — so mostly armour and weapons reflect. Water
(bump-mapped, blended, not in the G-buffer): one inline ray query per fragment along the bump-perturbed mirror
direction, Schlick Fresnel (F0 0.02) mixed into colour and alpha.

### Denoiser ("SVGF lite", `rt_denoise.comp`)

Temporal: reprojection through `R.prev_view_proj`, 4-tap bilinear history with per-tap validation (distance
to the previous camera within 2 % + 1 unit, normal dot > 0.85, roughness within 0.1), exponential average
1/len (floor 0.1, len ≤ 64); reflection history clamped to the 3×3 raw neighbourhood (mean ± 2σ).
Spatial: 3 à-trous iterations (steps 1, 2, 4) of a 3×3 B-spline kernel with depth, normal (power 32),
roughness and luminance stopping; the luminance tolerance shrinks with history length (no variance estimate).

### GI: choice and implementation

| Option | For | Against | Verdict |
|---|---|---|---|
| DDGI probe grid (ray-queried irradiance + depth moments) | world space: works for off-screen and translucent receivers, characters, reflection and probe hits (multi-bounce); cost independent of resolution; stable | leaks/dark probes without relocation; low frequency | **chosen** |
| Plain irradiance probes (no visibility) | simplest | leaks through every wall | no |
| Ray-query-assisted screen-space GI (SSGI + RT fallback) | high frequency detail | per-pixel cost × resolution, noisy, needs heavy denoising, nothing for translucent/hit points | no |
| Hybrid (DDGI + screen-space detail) | best quality | two systems | later, if wanted |

Implementation: camera-centred volume of 32 × 8 × 32 probes (spacing per `probe_density`, most of it below the
eye) addressed **modulo the grid** so it scrolls without copies; a slot whose world cell changes restarts.
Every frame all 8192 probes trace 64 rays (spherical Fibonacci, random rotation per frame). Hits:
`rt_shade_hit` with last frame's probes as ambient (one bounce traced per frame, further bounces accumulate over
frames); misses: sky radiance × (Original ambient / sky irradiance), so open-air GI equals the raster ambient and
only occlusion and bounce change the image. Back-face hits (facing from the **vertex normals** — the original's
assets are drawn with culling off and their winding is unreliable; two-sided foliage counts as front) store a
negative distance; > 25 % back faces = probe inside geometry → inactive (relocation is off). Update: 8×8 texels
per probe (6×6 octahedral + border evaluated directly), cosine-weighted irradiance and depth moments (power 50),
hysteresis = running mean for the first updates, then 0.97. Sampling: 8 probes, trilinear × wrap shading ×
Chebyshev visibility with weight crushing; falls back to the IBL ambient towards the volume edge and where the
total weight is tiny; floored at 20 % of the Original ambient. Sun bounce from point lights/torches is not traced.

## Performance (RX 7900 XTX, RADV, S1L1 start view, Ultra, 300 frames, `--gpu-timing`, mean ms)

Scene = whole GPU scene command buffer. RT columns are the passes' own timestamps.

| Resolution | Config | Scene | AS build | Prepass | GI probes | Shadows (+denoise) | Reflections (+denoise) |
|---|---|---|---|---|---|---|---|
| 1920×1080 | Ultra raster | 1.47 | – | – | – | – | – |
| 1920×1080 | + RT shadows | 2.58 | 0.32 | 0.18 | – | 0.62 | – |
| 1920×1080 | + RT reflections | 2.58 | 0.31 | 0.18 | – | – | 0.59 |
| 1920×1080 | + RT GI | 3.53 | 0.31 | 0.18 | 0.74 | – | – |
| 1920×1080 | all three | 4.72 | 0.31 | 0.18 | 0.73 | 0.61 | 0.61 |
| 3440×1440 | Ultra raster | 2.91 | – | – | – | – | – |
| 3440×1440 | + RT shadows | 5.23 | 0.33 | 0.29 | – | 1.57 | – |
| 3440×1440 | + RT reflections | 4.91 | 0.32 | 0.29 | – | – | 1.14 |
| 3440×1440 | + RT GI | 6.22 | 0.33 | 0.29 | 0.78 | – | – |
| 3440×1440 | all three | 9.20 | 0.35 | 0.31 | 0.84 | 1.63 | 1.18 |

The rest of each delta is the heavier forward shader (probe sampling: 8 probes × 2 atlas fetches per pixel
costs ~1 ms at 1080p for GI) and the extra barriers. Reproduce: `work/RayTracing/perf.sh`.

## Verification

- `engine.vk_rt_fallback`: capability decision, `[raytracing]` keys (Ultra default, overrides, bad values),
  instance classes, culling, probe addressing; Ultra with `JOA_VK_DISABLE_RT=1` = Ultra with RT keys off
  bit-identically (no crash), OpenGL ignores RT bit-identically, RT-on demo frame differs (RT hardware only);
  0 validation errors.
- `engine.vk_rt_smoke` (S1L1 via `run()`, 640×360, 24 frames, `auto_exposure=false` so every effect set runs at the
  same fixed exposure and only local changes are compared): all effects with validation (incl. synchronization) → 0
  errors, mean luma within 15 % of raster Ultra (measured 69.0 → 64.8); reflections change Joan's armour (MAD 1.27;
  legacy textures reflect by their metal fraction only) but not the tree line/sky (0.000); RT shadows change the
  grass/units (MAD 1.13; units receive but cast through their stencil volumes) and agree with the shadow map under
  the courtyard houses (MAD 0.29); GI changes the covered alley under the castle walkway (MAD 3.4) but not the sky
  (0.000).
- `engine.vk_remaster` (Ultra GL/Vulkan parity now runs Vulkan with RT keys off; Original stays bit-identical
  after an Ultra+RT frame), `engine.preset`.
- Screenshots (viewed side by side): `work/RayTracing/shots/final_s1l1_1080p.png` (raster, shadows,
  reflections, GI, all, GI in the alley), `final_crop_joan.png`, `b_*` (start / alley / courtyard / gate views,
  none vs GI), `v_moat_*` (water). Scripts: `work/RayTracing/{shot.sh,perf.sh,sbs.py,diffimg.py}`.

## Artifacts (honest list)

- **Reflections** at 1 ray/px show low-frequency blotches on rough-ish armour (roughness 0.4–0.6) after the
  à-trous filter; 2–4 rays/px or a lower `max_roughness` reduce them. Surface-motion reprojection smears
  reflections on moving characters for a few frames (no per-object motion vectors; CPU skinning).
- **GI** darkens enclosed places (covered alley, narrow courtyards) relative to the Original's flat ambient —
  physically motivated but a look change; the 20 % floor prevents black. Without relocation, probes stuck in
  thick walls/under the terrain lower the confidence near walls (falls back to the raster ambient there).
  Bounce is subtle (sunlit ground → walls); torches/point lights do not bounce. Characters do not occlude GI.
- **Water** reflections at grazing angles are strong and show the far bank sharply (single ray, no roughness);
  the original's sphere-map shimmer is underneath.
- **Shadows** look close to the 4096 shadow map in open areas; differences are contact shadows, thin casters
  (fences, spikes, grass) and softer far penumbrae. Terrain does not cast RT shadows (baked in its lightmaps).
- First RT frame of a scene builds ~330 BLASes in the frame (one-off hitch); grass patches add one small BLAS per frame while walking.
- Memory: 12 scene-size RGBA16F targets (~475 MB at 3440×1440) + 25 MB BLAS + 8 MB probe rays.

## Gaps

Point-light shadows; ray-traced translucent shadows (translucents still use the shadow map); probe relocation;
per-object motion vectors for the temporal filters; half-resolution reflections; RT in the OpenGL backend (not planned).
