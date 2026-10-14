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
};
```

- Asset seam: format loaders (from `joa_formats`) produce `MeshData` / `TextureData`; the app calls `create_mesh` / `create_texture` and references the handles from `DrawItem { mesh, Material{albedo, tint, blend, alpha_cutoff, two_sided, unlit}, model, first_index, index_count }`. Submeshes with different materials are separate `DrawItem`s over index ranges of one mesh.
- Matrices use the OpenGL clip convention (RH, z ∈ [-1,1]). The Vulkan backend premultiplies a fixed y-flip / z-remap matrix; camera code stays shared.
- Blend modes: Opaque, AlphaTest (discard below cutoff), AlphaBlend (sorted back-to-front, no depth write), Additive.
- Colour: textures marked sRGB are decoded on sampling; lighting/fog/blending are linear; the scene renders into an `GL_SRGB8_ALPHA8` offscreen target, so `read_back()` returns correctly encoded sRGB bytes and the window receives a raw blit. Note: the D3D8 original blended in gamma space; translucent effects may need a gamma-space path for faithful look (check against reference screenshots).

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
loads an original map through `src/engine/{assets_cache,world_scene,character_actor,world_view}.*`.
Tests: `engine.world_load` (headless counts), `engine.gl_world_render` (captured image). Format index: `docs/ASSET_FORMATS.md`.
Reference used: `work/reference/shots/s04_04_mission1_hud.png` (original S1L1 start); our match: `work/scene/s1l1.png`.

| Topic | Implementation | Confidence |
|---|---|---|
| Axes | original Z-up → engine `(x, z, -y)`, no mirror | HIGH: the in-game minimap `texture/s1.dds` places the S1L1 lake (stg water at 2827,12401) and the moat/walls (y≈1250) only under an unmirrored mapping |
| Scenario lookup | `map/scenarioN/<name>.{gri,stg}`; day cycle from `<name>w.day`, else `<name>.day` | HIGH (scenario_01.scr WorldFileName = S1L1w) |
| Terrain mesh | GRI vertices 64 units apart, int16 height, quad diagonal alternates with (x+y) parity | CONFIRMED (world_s.cpp builder, decompile ~line 443900) |
| Terrain texturing | `mix(tex1, tex2, (255-byte4)/255) * lightmap * rgb(bytes 1,2,3)`, ground UV repeats every header_c (4) cells (u from y, v from x), unlit; sector lightmaps packed in one atlas, sector (ix,iy) at row rows-1-iy | CONFIRMED stage setup (texture-stage switch cases 0x11/0x12/0x13, decompile ~line 523200); atlas orientation HIGH (lightmap v = 1-(y-y0)/size) |
| Objects | `item/<Path><ObjectFile>.pro` → `Model:`; else `.SMO`/`.AM1`; AM1 props posed at their first AM2 clip; transform `T(X,Y,Z)·Rz(HD)·S(Scale)`; VD, WIDTH/LENGTH/HEIGHT, Kingdom, Treasure, InitState/FinalState unused | HIGH (S1L1: 762/762 placed; 686 within 40 units of the terrain; walls/gate/drawbridge join up) |
| Materials | textures with any alpha < 255 → alpha test 0.5, two-sided; trailer tag-2 flag → two-sided; tag-1 blend values not mapped; tag 3 normals → up | HYPOTHESIS |
| Lighting | stage running at a clock (default 12:00) from `<name>w.day`; lights from the map `.lgt`, `map/common.lgt`, `map/special.lgt` (Type 1 directional with VDir elevation / HDir azimuth, Type 0 ambient); sun × 150/255 material diffuse | HYPOTHESIS; noon matches the original capture (white light, silver armour) |
| Fog / sky | linear fog 300..6000 units towards the `.sca` stage colour, same colour as clear/sky. The `.day` sky dome (`model/sky01.smo`) has sunset textures that the engine recolours by scattering (UNKNOWN), so it is not drawn | HYPOTHESIS, tuned by eye to the capture |
| Water | `CreateWater` = translucent flat rectangle, X,Y as centre | HYPOTHESIS (fits the moat and lake beds) |
| Units | `script/scenario/<map>.sce` `load_unit/add_unit` + first `set_unit_pos/set_unit_dir` (degrees CCW from +X); `unit/<code>.AM1` + `anim/<code>.am2`; AM1 faces +X | HIGH: Joan (115°) and Jean (295°) face each other, as in the capture |
| Animation | CPU skinning (`skin_vertices`) per rendered frame through `IRenderer::update_mesh`; clip time advances per 30 Hz tick and is interpolated by `FixedStep::alpha()`; idle/walk/run chosen by sim speed | — |
| Camera | follow camera 190 units behind, target 55 units up, pitch 8° (framing of the capture); sim metres × 45 units/m (Joan 78.7 units = 1.75 m, HYPOTHESIS) | HYPOTHESIS |

Renderer additions for this (mirrored by the Vulkan backend): `Vertex::uv2/color`, `Material::layer2/lightmap`,
`IRenderer::update_mesh`; GL sets 8x anisotropy on mipmapped textures. Mips are GPU-generated, not the DDS chain.

Perf (S1L1 start view, 1920x1080, RX 7900 XTX radeonsi): 1.3 ms mean CPU frame, ~84 draws; under Xvfb/llvmpipe
13–46 ms, 60–360 draws (frustum-culled objects; terrain = 34 draws, 130 050 triangles).

Gaps: dynamic grass (`dyngrass`, visible in the capture), weapons attached to bones (Joan's sword), blob/stencil
shadows, scattering sky + clouds, water bump/env/reflection, particles (torches, waterfall), point lights (135 `CreateLight`
in S1L1), LOD (`#2/#3`) files, unit `.uni` armies, original mip chains, tag-1 blend modes.
