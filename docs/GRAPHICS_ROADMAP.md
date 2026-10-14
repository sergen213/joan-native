# Graphics roadmap and native client foundation

Status of `src/engine/` (`joa_engine` static lib + `joan-native` executable) and the plan for the OpenGL and Vulkan renderers.
Camera/projection reverse engineering lives in [CAMERA_AND_WIDESCREEN.md](CAMERA_AND_WIDESCREEN.md); this file only records what the engine implements from it.

## Layout

| File | Role | Depends on |
|---|---|---|
| `sim.hpp/.cpp` | Fixed-step accumulator, `World`, `step()`, state hash, render interpolation | glm only |
| `input.hpp/.cpp` | Action map (keys, mouse, SDL_Gamepad), radial dead zone, sensitivity, text remap format | SDL enums/name tables only, no init |
| `camera.hpp/.cpp` | Hor+ projection, viewport fitting, follow camera, UI anchoring | glm only |
| `renderer.hpp` | API-neutral `IRenderer` + mesh/texture/material/frame structs | glm only |
| `renderer_gl.*`, `gl_loader.*`, `gl_shaders.hpp` | OpenGL 3.3 core backend, own loader via `SDL_GL_GetProcAddress` | SDL3, `<GL/glcorearb.h>` |
| `renderer_vk.*`, `vk_api.hpp`, `vk_memory.*` | Vulkan 1.3 backend (see [Vulkan](#vulkan)), runtime dispatch via SDL's `vkGetInstanceProcAddr`, sub-allocator | SDL3, Vulkan headers |
| `platform.*` | SDL3 window (Wayland/X11 chosen by SDL), modes, high-DPI pixel size, event pump, gamepad hotplug | SDL3 |
| `cli.*` | Flags + launcher `settings.ini` subset ([LAUNCHER.md](LAUNCHER.md)) | — |
| `demo_scene.*` | Procedural ground/boxes/UI markers until original assets load | renderer.hpp |
| `png.*` | RGBA8 PNG writer (zlib) | zlib |
| `app.*`, `main.cpp` | Loop: pump → actions → fixed ticks → interpolate → camera → draw list → render | all |
| `shaders/gl/*.glsl` | Embedded into the binary at configure time | — |
| `shaders/vk/*.{vert,frag}` | Compiled by `glslc` at build time (`-mfmt=num`, depfile), `#include`d into `renderer_vk.cpp` | glslc (shaderc) |
| `shaders/common/*.glsl` | Remaster shader bodies shared by both backends (embedded for GL, `#include`d by `shaders/vk`) | — |
| `preset.*` | Graphics presets + `[renderer] preset` / `[graphics]` keys ([Remaster](#remaster-enhanced--ultra-presets)) | — |
| `post_frame.*`, `pbr_env.*` | Remaster per-frame constants (jitter, sun shadow frustum) and sky IBL maps, shared by both backends | glm |
| `hd_pack.*` | HD texture override packs ([HD_ASSETS.md](HD_ASSETS.md)) | joa_formats |

Simulation never includes renderer or SDL headers; the app turns `World` into a `FrameData` draw list each frame.

## Render interface

```cpp
class IRenderer {
    virtual const char* name() const;
    virtual MeshHandle create_mesh(const MeshData&);        // Vertex: pos, normal, uv, u8 bones[4], weights
    virtual void destroy_mesh(MeshHandle);
    virtual TextureHandle create_texture(const TextureData&); // RGBA8 sRGB or UNORM, top row first, UV (0,0)=top-left
    virtual void destroy_texture(TextureHandle);
    virtual void begin_frame(glm::ivec2 framebuffer_pixels);
    virtual void submit(const FrameData&);                    // view+projection+viewport, Lighting, DrawItems, UiQuads
    virtual void end_frame();                                 // present
    virtual Image read_back();                                // sRGB RGBA8, top row first
    virtual void set_graphics(const GraphicsSettings&);       // preset/features (default Original = faithful path)
    virtual void set_material_maps(TextureHandle albedo, const MaterialMaps&); // HD normal/rough/metal/AO + class
};
```

- Asset seam: format loaders (from `joa_formats`) produce `MeshData` / `TextureData`; the app calls `create_mesh` / `create_texture` and references the handles from `DrawItem { mesh, Material{albedo, tint, blend, alpha_cutoff, two_sided, unlit}, model, first_index, index_count }`. Submeshes with different materials are separate `DrawItem`s over index ranges of one mesh.
- Matrices use the OpenGL clip convention (RH, z ∈ [-1,1]). The Vulkan backend premultiplies a fixed y-flip / z-remap matrix; camera code stays shared.
- Blend modes: Opaque, AlphaTest (discard below cutoff), AlphaBlend (sorted back-to-front, no depth write), Additive.
- Colour: textures marked sRGB are decoded on sampling; lighting/fog/blending are linear; the scene renders into an `GL_SRGB8_ALPHA8` offscreen target, so `read_back()` returns correctly encoded sRGB bytes and the window receives a raw blit. Note: the D3D8 original blended in gamma space; translucent effects may need a gamma-space path for faithful look (check against reference screenshots). The 2D UI pass already blends in gamma space (GL: sRGB conversion off + encoding `ui_frag.glsl`; Vulkan: second pass into a UNORM view) — docs/reverse-engineering/UI_SYSTEM.md §3.

## Camera and widescreen (implemented)

- Vertical FOV 45° (`kOriginalVerticalFovDeg`) — HIGH CONFIDENCE: projection builder `0x7674A4` computes `_22 = cot(fovy/2)`, `_11 = _22/aspect`; the FOV fix keys on `cos(fovy/2) == 0.9238795` (= 45°). Details and callers: CAMERA_AND_WIDESCREEN.md §1–2.
- Hor+ at every aspect: `_22` constant, `hfov = 2·atan(tan(22.5°)·aspect)`. Unit-tested for 4:3 (57.82°), 5:4 (54.75°), 16:9 (72.73°), 16:10 (67.07°), 21:9 (88.05°), 32:9 (111.65°), custom 1.9 (76.41°).
- `--aspect W:H` renders a letter/pillarboxed viewport of that aspect (no stretching).
- UI: `ui_layout()` gives a centred 4:3 safe area scaled from the 1024×768 design canvas (HIGH CONFIDENCE canvas size, see CAMERA_AND_WIDESCREEN.md); `ui_place()` anchors elements to the safe area (menus) or the real screen edges (HUD).
- Near/far 1.0/8193.0 — HIGH CONFIDENCE (EngineRE): `View3D_init` `0x613730` defaults (view `+0x44`, perspective far `+0x4c`; ortho far 16385.0 at `+0x50`), consumed by `View3D_update_matrices` `0x6141a0`. Device mode is hard-coded 1024×768 (`0x5f6c00`). The demo scene's metre-ish units do not match the original world scale yet. See reverse-engineering/CAMERA_SYSTEM.md.

## Simulation and input (implemented)

- Fixed tick 30 Hz — HIGH CONFIDENCE (EngineRE): the original runs logic/draw on a 30 Hz timer (`0x58a830(30,30)`) and scales movement by `elapsed_ms/1000*30` clamped to 5. The port uses exact fixed ticks instead (deterministic; equivalent when frames keep up). Accumulator caps catch-up at 8 ticks/frame; rendering interpolates `previous → current`.
- Determinism: same command sequence ⇒ identical `state_hash` regardless of render frame partitioning (test `engine.sim`); `joan-native --headless --frames N` prints the hash for regression use.
- Input actions: MoveX/Y, LookX/Y, Zoom, Attack, Block, Jump, Use, Run, MouseLook, Pause, ToggleFullscreen. Default bindings are provisional WASD + gamepad; the original DirectInput key table is not recovered yet. Remaps load from `--input-config FILE` or `${XDG_CONFIG_HOME:-~/.config}/joan-native/input.cfg` (format in `input.hpp`). Launcher `[controller] deadzone/sensitivity` apply on top.

## Verification

- `ctest -R engine`: `engine.camera`, `engine.input`, `engine.sim`, `engine.cli`, `engine.gl_smoke`, `engine.vk_memory`, `engine.vk_smoke`, `engine.vk_parity`.
- `engine.gl_smoke` runs the real client path under a private `xvfb-run` server (llvmpipe; falls back to `SDL_VIDEO_DRIVER=offscreen` + EGL if xvfb-run is missing), captures 640×360 at native and forced-4:3 aspect, decodes the PNG and checks colour variety, sky/ground/UI marker pixels and black pillarbox bars.
- Manual capture: `SDL_VIDEO_DRIVER=x11 xvfb-run -a joan-native --screenshot work/engine/first.png --frames 5`. `SDL_VIDEO_DRIVER=offscreen` also works and uses the real GPU through EGL without a window.

## Not done yet (in order)

1. Original asset loading through the `MeshData`/`TextureData` seam (DDS decode, SMO/AM1/AM3 meshes, terrain) — depends on `joa_formats`.
2. GPU skinning: vertex layout and attributes are in place; bone palette uniform + shader path once the animation format is known (bone count limit unknown).
3. Vulkan: done for raster parity (section below); next RT (reflections/shadows/GI per launcher `[raytracing]`).
4. Shadows, particles, MSAA, anisotropic filtering, gamma-space blending option, terrain renderer.
5. Original key bindings, mouse-camera behaviour (CAMERA_AND_WIDESCREEN.md §3 mouse delta clamp) and menus/UI art.
6. Tracy: `-DJOA_TRACY=ON` fetches Tracy v0.11.1 via FetchContent (network at configure time; verified to build and run). Dear ImGui is not installed system-wide and cannot be vendored offline, so no dev overlay yet.

## Vulkan

`--renderer vulkan` (or launcher `[renderer] backend=vulkan`) selects `renderer_vk.cpp`, a Vulkan 1.3 implementation of the same `IRenderer`; camera, draw lists and the demo scene are shared with GL.

- **Loader / device**: entry points come from SDL's `vkGetInstanceProcAddr` (no link-time libvulkan; `vk_api.hpp` X-macro table). Every physical device is logged; the best usable one wins (discrete > integrated > virtual > cpu), `--gpu N` forces enumeration index N. Required: Vulkan 1.3 with dynamic rendering, synchronization2, shaderDemoteToHelperInvocation (glslc lowers `discard` to demote), timeline semaphores and descriptor indexing (partially bound, update-after-bind, non-uniform sampled-image indexing). Sampler anisotropy is used when present.
- **Frame structure**: the scene renders into an offscreen `R8G8B8A8_SRGB` colour + `D32_SFLOAT` depth target (same as GL's FBO). Two frames in flight, each with its own command pool, scene + present command buffers, acquire semaphore, UBO, draw-table SSBO, UI vertex buffer and descriptor set; one timeline semaphore retires frames (no fences). Swapchain present = blit offscreen → swapchain image (sRGB on both ends, so bytes pass through unchanged; RGBA → BGRA handled by the blit), black where sizes differ. One "render done" semaphore per swapchain image.
- **Swapchain**: recreated when the window pixel size changes or acquire/present report out-of-date/suboptimal; minimised (0×0) frames render without presenting. `--present-mode fifo|relaxed|mailbox|immediate`, default FIFO (vsync); unsupported modes fall back to FIFO (logged). MAILBOX/IMMEDIATE suit VRR displays.
- **Headless**: under `SDL_VIDEO_DRIVER=offscreen`/`dummy` (or when no device can present, e.g. RADV on Xvfb without DRI3) no surface is created; frames render to the offscreen target and `read_back()` works. Tests and `--screenshot` use this.
- **Clip space**: `view_proj = C · projection · view` with C = y-flip + z∈[−1,1]→[0,1]; front face stays counter-clockwise; viewport keeps the top-left origin. Read-back rows are already top-first.
- **Materials**: three mesh pipelines (opaque/alpha-test share one: discard by per-draw cutoff; alpha blend and additive without depth write) + one UI pipeline; cull mode is dynamic (`two_sided`). Blended draws are sorted back to front exactly like GL. Mesh shaders port `shaders/gl/mesh_*.glsl` 1:1 (layer2/vertex colour/lightmap, alpha test, sun + ambient, unlit, linear fog). Per-draw data (model, normal matrix, tint, cutoff, texture slots) lives in a per-frame SSBO indexed by `firstInstance`; frame data (view-proj, light, fog, camera) in a UBO.
- **Textures**: one bindless `sampler2D[4096]` set (update-after-bind); slot 0 = white. Uploads go through a staging buffer, mips are generated with a blit chain (sRGB filtered in linear space, like `glGenerateMipmap`). Samplers: repeat/clamp × 8× anisotropic (mipmapped, as GL) / plain. Destroyed meshes/textures (and `update_mesh` replacements) are released, and their bindless slots reused, only after the timeline passes every frame that could reference them.
- **Memory**: `vk_memory.*` — 64 MiB `VkDeviceMemory` blocks per memory type, buffers and optimal images in separate blocks (bufferImageGranularity), first-fit free list with coalescing (`engine.vk_memory`), persistent mapping for host-visible blocks, dedicated blocks above 32 MiB. VMA was not vendored (no network); the allocator is ~150 lines.
- **Pipeline cache**: `$XDG_CACHE_HOME/joan-native/vk_pipeline.cache` (default `~/.cache/…`), loaded only if the header matches vendor/device/pipelineCacheUUID, written atomically (tmp + rename) at shutdown.
- **Validation**: `--vk-validation` enables `VK_LAYER_KHRONOS_validation` with synchronization validation and a debug-utils messenger; errors are counted (`vk_validation_errors()`) and every warning/error is logged. The layer is not installed system-wide on the dev box: extract the `vulkan-validation-layers` package to `work/vvl` (manifest `library_path` pointed at `../../../lib/…`) — CMake picks it up as `JOA_VK_LAYER_PATH` for tests; for manual runs set `VK_LAYER_PATH=$PWD/work/vvl/usr/share/vulkan/explicit_layer.d`.
- **GPU timing**: `--gpu-timing` writes timestamps around the scene pass, UI, whole scene command buffer and present blit; means are logged at exit (demo scene, RX 7900 XTX, 1280×720: scene ≈0.02 ms, present blit ≈0.01 ms).

### Vulkan tests

- `engine.vk_smoke`: SDL offscreen driver, validation on (required when the layer is found), 800×450 → resized to 640×360 mid-run, creates/updates/destroys a mesh and texture every frame; checks size, >100 distinct colours, upright sky/ground/UI marker pixels, opaque alpha, 0 validation errors.
- `engine.vk_parity`: renders the demo frame (+ an additive box; every material mode and the UI pass) with GL and Vulkan at 640×360 and compares RGB. Thresholds PSNR ≥ 35 dB and mean absolute difference ≤ 1.0. Measured on RX 7900 XTX (radeonsi GL vs RADV): **60.6 dB, MAD 0.005**, 5 pixels (0.002 %) off by > 32 at polygon edges. For scale, a y-flipped frame scores 15.9 dB and a gamma-2.2 error 13.6 dB. Full app captures at 1280×720 (near/far 1/8193): 46.0 dB.
- Both exit 77 without a Vulkan 1.3 device. Test environment: `SDL_VIDEO_DRIVER=offscreen`, `VK_LOADER_LAYERS_DISABLE=~implicit~` (user overlay/frame-gen layers off), `XDG_CACHE_HOME=<build>/src/engine/test-cache`.
- Real swapchain (FIFO/MAILBOX/IMMEDIATE, resize recreation, validation clean) was checked inside a private `kwin_wayland --virtual` session; Xvfb cannot present Vulkan on hardware drivers (falls back to offscreen).

### Vulkan gaps

- Uploads and `read_back()` are synchronous (`vkQueueWaitIdle`); `update_mesh` re-uploads through staging each call — fine for loading, slow for many per-frame CPU-skinned meshes (switch to a per-frame host-visible ring).
- No MSAA, no RT, single graphics queue (no async transfer/compute), no HDR swapchain formats.
- Swapchain needs an `*_SRGB` surface format for byte-exact presentation; a UNORM-only surface would present darker (all common Linux drivers expose B8G8R8A8_SRGB).

## First original scene

`joan-native --game-dir <dir> --scenario S1L1 [--screenshot work/scene/s1l1.png --frames 10] [--free-camera | --camera X,Y,Z,H,P] [--anim f_run]`
loads an original map through `src/engine/{assets_cache,world_scene,character_actor,world_view,attach_items,sky_dome,grass_field}.*`.
Tests: `engine.world_load` (headless counts: 72 start units placed, 73 items attached, 0 missing; unit/item look-ups;
sword on the hand bone; shadow vertices on the ground), `engine.gl_world_render` / `engine.vk_world_render` (captured
image: colour variety, terrain, sky pixels; Vulkan with validation). Format index: `docs/ASSET_FORMATS.md`.
Reference used: `work/reference/shots/s04_04_mission1_hud.png` (original S1L1 start); our match: `work/scene/s1l1.png`.

| Topic | Implementation | Confidence |
|---|---|---|
| Axes | original Z-up → engine `(x, z, -y)`, no mirror | HIGH: the in-game minimap `texture/s1.dds` places the S1L1 lake (stg water at 2827,12401) and the moat/walls (y≈1250) only under an unmirrored mapping |
| Scenario lookup | `map/scenarioN/<name>.{gri,stg}`; day cycle from `<name>w.day`, else `<name>.day` | HIGH (scenario_01.scr WorldFileName = S1L1w) |
| Terrain mesh | GRI vertices 64 units apart, int16 height, quad diagonal alternates with (x+y) parity | CONFIRMED (world_s.cpp builder, decompile ~line 443900) |
| Terrain texturing | `mix(tex1, tex2, (255-byte4)/255) * lightmap * DIFFUSE`, ground UV repeats every header_c (4) cells (u from y, v from x); DIFFUSE = the lit vertex colour (bytes 1,2,3 as D3D diffuse material, `Material::vertex_diffuse`): at noon ambient 0.72 + sun·0.78 clamps to 1; sector lightmaps packed in one atlas, sector (ix,iy) at row rows-1-iy | CONFIRMED stage setup (case 0x13 ~line 523175: tex, BLENDDIFFUSEALPHA, MODULATE lightmap, MODULATE DIFFUSE); lit vertex colour HYPOTHESIS (unlit was 0.78× darker than the capture; lit matches within 5%) |
| Objects | `item/<Path><ObjectFile>.pro` → `Model:`; else `.SMO`/`.AM1`; AM1 props posed at their first AM2 clip; transform `T(X,Y,Z)·Rz(HD)·S(Scale)`; VD, WIDTH/LENGTH/HEIGHT, Kingdom, Treasure, InitState/FinalState unused | HIGH (S1L1: 762/762 placed; 686 within 40 units of the terrain; walls/gate/drawbridge join up) |
| Materials | textures with any alpha < 255 → alpha test 0.5, two-sided; trailer tag-2 flag → two-sided; tag 3 normals → up | HYPOTHESIS |
| Tag-1 blend | any value ≠ -1 → material +0x38 = 1 (+0x34 = 0x20000, +0x40 = 1/2) in the SMO loader 0x5e0530 (decompile ~512576); render-state set 1 of `FUN_005f82e0`: SRCALPHA/INVSRCALPHA, alpha test, lighting, z-write → `BlendMode::AlphaBlend` (sorted, no z-write) | CONFIRMED mapping; z-write difference deliberate |
| Mip maps | the DDS file chain (face 0) is uploaded as-is (`TextureData::mip_pixels`, GL `glTexImage2D` per level + `GL_TEXTURE_MAX_LEVEL`, Vulkan one staging copy per level); single-level files get GPU mips | — |
| Lighting | stage at a clock (default 12:00) from `<name>w.day`; `.lgt` Type 1 directional (VDir elevation / HDir azimuth, HYPOTHESIS), Type 0 ambient. D3D8 fixed function emulated per vertex in gamma space: `min(ambient·240/255 + diffuse·(sun·N.L + Σ point), 1)`, texture × light, diffuse = SMO/AM1 colour1 (0.588); material ambient is the engine constant 0xF0F0F0 (DAT_00a69cd0 ← DAT_00a18a6c, FUN_0060baf0) | CONFIRMED constants/formula (lighting.nvo, D3D semantics); gamma-space sum HIGH |
| Fog | linear vertex fog on the eye distance (fog.nvo `calculate_linear_normal_fog_factor`, D3DFOG_LINEAR), 900 → view distance 3996 (FUN_005634e0: min(far−200, 4096 [×0.7 night/rain]) reached within 100), mixed in gamma space; colour = clear colour = Hoffman-Preetham inscattering 2000 units along the view direction from the `.sca` stage (View3D_render 0x614760; betas from 0x617ee0; Normal_12 level view = 106,146,193), recomputed per frame | CONFIRMED (HG denominator taken literally as `2g·cos + g + 1`) |
| Depth range | in-game near 4 / far 4096 (FUN_005634e0 → 0x613ca0); objects culled at 4096; sky dome radius 4000 | HIGH |
| Point lights | `.stg CreateLight` light_type 3 = D3DLIGHT_POINT (FUN_005c5c30): RGB/255, ATT0..2, Radius = Range, registered per `scene_name` sector (lights reaching several sectors are repeated, shared Group); item `.pro HasLight` lights join their object's sector (HYPOTHESIS); a draw's light set = its sector's lights (FUN_005e67b0 switches light sets per primitive group), ≤ 7 + sun; skinned units 1 point + 1 directional (Shader_build_vertex_shader); RANDOM_ATTENUATION + ModifyRange flickers ATT1 per Group (FUN_004afd60, timing approximated) | CONFIRMED parse/selection; S1L1: 144 lights |
| Sky | `sky_dome`: camera-centred hemisphere (radius 4000), unfogged, not a shadow receiver: fog colour at the horizon → (150,186,220) band → (118,160,206) zenith; `.day` CloudTexture1/2 as two alpha-blended layers. The original draws `model/sky01.smo` through `scatter.nvo` (oD0 = extinction·terrain reflectance·sun, oD1 = inscattering) — not reproduced | HYPOTHESIS, colours sampled from s04_04/s04_06 |
| Shadows | stencil shadow volumes (`shadow_volume.cpp`, port of FUN_005e3040): welded `<am1>#S.AM1` (+ item `#s.SMO`), light-facing faces = near cap, silhouette edges extruded height+300 along the sun, no far cap; opaque pass writes stencil 128 on receivers (terrain/objects; units, sky 0); volumes front INCR / back DECR clamped (z-pass, 0x5f1cc0), then a full-screen black alpha-128 fill where 128 < stencil (0x5ee1d0) before the translucent pass; CONFIG.DAT 0x9C modes (volume within 1000 units, `ShadowModel` quad within 2000) | CONFIRMED order, ops, colour, cap/edge rules; HYPOTHESIS: extrusion length, units not receivers |
| Dynamic grass | `grass_field`: 3×3-cell (192-unit) patches, ground texture covering ≥ half the cells → `DynGrass.res` `<tex>_M<scenario>` then `<tex>`: `model : count : percent : yaw : tilt`, `rand(100) < percent` → `max(1, count·density/100)` tufts, random yaw + tilt−(6..10)°, slope skip/sink, radius 2000 (CONFIG.DAT), one new patch per frame, alpha 255−128·(d−100)/(R−100) beyond 100 (FUN_00464740/00465a20/00466560) | CONFIRMED; `_M1` lookup HIGH; wind not done |
| Water | `water.cpp`: `CreateWater` (FUN_004f5210) 800-unit grid around X,Y, vertex colour 65,51,80 a 0x80; texteff `BumpEnvWaterRiver`: BUMPENVMAP over `Lake1..65.bmp` (bump matrix 0.5, 30 ms/frame) then `spheremap` at uv = world reflection.xy + 0.5 (`Material::bump`) | CONFIRMED default (Reflection = Modulate, Bump = Yes); Reflection = High (planar RTT) not done |
| Particles | `particles.cpp`: `.stg CreateParticle` + item `.pro` HasEmitter slots 0/1 → pscript event sequences (fade steps in seconds of age, random gravity, EmitRate carry, NumParticles cap, Loop≠0 burst), camera-facing quads, SRCALPHA + ONE/DESTALPHA → Additive, INVSRCALPHA → AlphaBlend (pemires.cpp 0x67f380/0x67faf0) | CONFIRMED sim; S1L1 11 emitters (9 torches, 2 waterfall); weather not active at mission start, not done |
| Units | `script/scenario/<map>.sce` `load_unit/add_unit` + first `set_unit_pos/set_unit_dir` (degrees CCW from +X), then `<map>.uni` `CreateUnit` (UnitFile upper-cased = `unit/unit.res` record, X/Y on the terrain, HD same convention); AM1 faces +X | HIGH: Joan (115°) and Jean (295°) face each other, as in the capture; S1L1: 71 .uni units, all placed |
| Unit look | `attach_items` `UnitDb`: unit.res `Model` (`unit/` + name), `Animation` → STD.SET `ANIMATE` {CODE, DBFNAME} → per-unit table, first row POSITION=STAND, MOVEMENT=IDLE, COMBO empty → `anim/<UPPERFILE>.am2` clip `UPPERNAME` (archers share `english_archer_share.am2`); bone names from `unit/<model>.AM3` | CONFIRMED tables; idle-row rule HIGH |
| Armour mesh | `Model` is the mesh worn with the start armour (`*_ARMOR_LV1`); the `ModelLevel`/`ModelName` list holds the other levels (Joan: joan_armorC = Model; Joan_of_Arc, joan_armorB listed) | HIGH: only joan_armorC has the gold-trimmed skirt of s04_04 |
| Equipment | `PWeaponId`, `SWeaponId` (else `PRangeWeaponId`: archers hold the bow) → item.res record: first `ModelName` (carried mesh; `Model` is the pickup mesh, e.g. itm_Warbrand.am1) on `AttachBoneName` (`bip01 R Hand`, bows `bip01 L Hand`); item vertices are in bone space: `T_unit · C · key[bone] · Cᵀ` (C = Z-up → Y-up); AM1 items (bows) posed at the first key of `item/anim/<name>.am2` | HIGH: sword angle/hand matches s04_04 |
| Animation | CPU skinning (`skin_vertices`) per rendered frame through `IRenderer::update_mesh`; clip time advances per 30 Hz tick and is interpolated by `FixedStep::alpha()`; idle/walk/run chosen by sim speed | — |
| Camera | follow camera 190 units behind, target 55 units up, pitch 8° (framing of the capture); sim metres × 45 units/m (Joan 78.7 units = 1.75 m, HYPOTHESIS) | HYPOTHESIS |

Renderer additions for this (mirrored by the Vulkan backend): `Vertex::uv2/color`, `Material::layer2/lightmap/fog/diffuse/
vertex_diffuse/bump/bump_scale/receive_shadow`, `BlendMode::ShadowVolume`, `Lighting::point_lights/shadow_color`,
`DrawItem::lights` (Vulkan: light SSBO at set 1 binding 2), `TextureData::mip_pixels`, `IRenderer::update_mesh`; depth-
stencil targets (GL DEPTH24_STENCIL8, Vulkan D32S8/D24S8). Lighting and fog run per vertex like D3D8; `Lighting`
colours are gamma-space D3D values.
Parse caches in `AssetCache` (`skinned_mesh`, `static_mesh`, `animation`, `skeleton`) share AM1/AM2/AM3 between units.

Perf (S1L1 start view, 1920x1080, RX 7900 XTX, 300 frames, mean CPU frame build+submit+present): before this pass
GL 3.59 ms / Vulkan 4.70 ms (389 draws, Vulkan GPU scene 0.68 ms); after (point lights, volumes, grass patches, water,
particles, objects culled at the 4096 far plane) GL 2.92 ms / Vulkan 2.33 ms (555 draws, Vulkan GPU scene 0.72 ms).

Tests: `engine.world_load` (fog range/colour, 144 point lights, sector light sets, water, 11 emitters, shadow volume),
`engine.gl_world_render` / `engine.vk_world_render` (start view incl. blue haze band, torch view `--camera
12037,13600,250,90,-8` fire pixels, moat view `--camera 11000,3200,1350,240,-20` water pixels).

Gaps: scatter.nvo sky, point lights on terrain/grass draws and point-light shadow volumes (FUN_005f0f80), grass wind,
water Reflection = High, weather, combat particle features (orbits, SMO particles), LOD (`#2/#3`), unit distance culling.

## Remaster (Enhanced / Ultra presets)

`[renderer] preset=original|enhanced|ultra|custom` + `[graphics]` keys (docs/LAUNCHER.md), CLI `--preset`,
`--graphics k=v,...` (implies custom), `--hd-packs` ([HD_ASSETS.md](HD_ASSETS.md)). `src/engine/preset.*` resolves
them into `GraphicsSettings`; the app calls `IRenderer::set_graphics()` after creating the backend (may be called
again between frames). Without a settings file the engine runs **Original**.

- **Original** = the faithful D3D8-emulating path, untouched: verified byte-identical to the pre-remaster build
  (S1L1 1920×1080 and the demo frame, GL and Vulkan: `work/remaster/baseline` vs current), and
  `engine.gl_remaster` / `engine.vk_remaster` check Original stays bit-identical after Enhanced/Ultra frames.
- **Enhanced / Ultra** (Custom = Enhanced + keys). Every feature has its own key:

| Feature | Key | Enhanced | Ultra | Implementation |
|---|---|---|---|---|
| PBR metallic-roughness | `pbr` | on | on | `shaders/common/pbr_shade.glsl`: per-pixel D3D8 light sum kept as the reference brightness (ambient part × sky irradiance shape × AO, direct part × shadow), GGX sun specular + prefiltered sky reflection on top. Legacy conversion by `MaterialClass` + albedo: only unsaturated bright (steel) / saturated yellow (gilding) texels of characters (≤0.55) and carried items (≤0.75) become metallic; terrain 0.95, buildings 0.88 roughness |
| Normal maps | `normal_maps` | on | on | HD pack `_n` maps, screen-space cotangent frame (no vertex tangents) |
| IBL | `ibl` | on | on | `pbr_env.cpp`: 64×32 equirect sky (sky_dome gradient + lit ground) with 7 GGX-prefiltered mips + 32×16 irradiance normalised to the up normal (terrain keeps its Original ambient); rebuilt when the lighting changes |
| HDR + tone map | `hdr`, `exposure` | 0.85 | 0.85 | RGBA16F scene, Narkowicz ACES fit, sRGB encode |
| Sun shadows | `shadow_map_size`, `shadow_softness`, `shadow_distance` | 2048, 1.5 texel, 2500 | 4096, 2.5, 4000 | replaces the stencil volumes (0 = volumes, also in the HDR path); casters = lit opaque/alpha-test/blended (alpha 0.5) draws except terrain; texel-snapped bounding-sphere ortho, slope bias + normal offset, 12-tap rotated Poisson PCF on hardware compare; vegetation receives at 50 % |
| SSAO | `ssao`, `ssao_radius`, `ssao_intensity` | 48 | 64 | 12 world-space samples from depth, applied only to the ambient/environment MRT (reverse-subtract pass, 3×3 depth-aware blur); not on foliage |
| AA | `aa=taa` | TAA | TAA | Halton(2,3) jitter, depth reprojection, YCoCg variance clip |
| Volumetric fog | `volumetric_fog`, `fog_density` | 12 steps | 24 steps | height fog below the camera with shadow-mapped HG(0.55) sun in-scattering, on top of the original linear fog |
| Particle lighting | `particle_lighting` | on | on | alpha-blended smoke lit by ambient + shadowed sun; additive fire ×1.6 HDR |
| Upscaling | `upscale=fsr1`, `render_scale`, `sharpness` | off, 1.0, 0.2 | off, 1.0, 0.3 | FSR1 EASU (12-tap, texelFetch port) + RCAS (MIT, AMD FidelityFX) |

Passes (both backends, same shader bodies in `shaders/common/`, same CPU constants from `post_frame.cpp`): shadow map →
opaque (HDR colour + ambient MRT, depth-stencil) [→ stencil volumes + fill when shadow maps are off] → SSAO → AO apply →
translucent → composite (volumetric fog, exposure, ACES) → TAA → EASU → RCAS/copy into the display target → UI.
GL: units = Vulkan set-2 binding + 7, block binding 0 (`shaders/gl/remaster_prelude.glsl`). Vulkan: set 2 (frame
block, textures, per-draw `MaterialExtra` SSBO), separate depth-only shadow caster shader, `independentBlend` required.

Tests: `engine.preset` (names, ini/CLI keys, custom semantics, jitter, shadow texel snapping), `engine.gl_remaster`
(Original bit-identical incl. after toggling; Enhanced/Ultra/FSR1 sane and within ±25 % mean luma of Original;
S1L1 when data present), `engine.vk_remaster` (GL vs Vulkan: Enhanced 53.1 dB / MAD 0.26, Ultra 52.4 / 0.30, FSR1
67 % 51.6 / 0.32, S1L1 Enhanced 50.8 / 0.38; thresholds 40 dB / 1.0; 0 validation errors), `engine.hd_pack`,
`tools.hd_upscale`.

Remaster gaps: SMAA not implemented (TAA only); no soft particles (GL 3.3 cannot sample the attached depth);
characters ghost slightly under TAA (no per-object motion vectors, CPU skinning); GL GPU timing (`JOA_GPU_TIMING=1`)
uses timestamps that include driver submission bubbles; no bloom; the volumetric density is relative to the camera
height (no absolute ground reference); the in-game options menu does not expose presets yet.
