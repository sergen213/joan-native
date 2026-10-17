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
    virtual void submit(const FrameData&);                    // view+projection+viewport, Lighting, DrawItems, UiQuads, clipped UI triangles
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

## Developer tools

All optional; the default/release build has none of them active (RenderDoc support is compiled in but inert without
`--renderdoc`; Tracy and the overlay are CMake options, off by default).

- **RenderDoc** (`src/engine/renderdoc.cpp`, vendored MIT header `third_party/renderdoc/renderdoc_app.h`):
  `--renderdoc` loads `librenderdoc.so` (`$RENDERDOC_LIB`, else the system library; reused if RenderDoc already
  injected it) before any window or GPU object; `--renderdoc-capture N` (implies `--renderdoc`) captures frame-loop
  iteration N with `StartFrameCapture`/`EndFrameCapture` (NULL device/window), so it works without presentation
  (hidden `--screenshot` windows, Vulkan's offscreen fallback, where `TriggerCapture` never fires). The written path
  (`/tmp/RenderDoc/joan-native_<date>_capture.rdc` by default) is logged; a library that cannot load, or a capture
  that fails, is logged too.
  - OpenGL: a late `dlopen` cannot hook SDL's `dlopen`/`dlsym` of libGL/libEGL, so the client restarts itself once
    (`execv /proc/self/exe`) with `LD_PRELOAD=librenderdoc.so` and the library's directory on `LD_LIBRARY_PATH`
    (`LD_PRELOAD` splits paths at spaces).
  - Vulkan: RenderDoc's implicit layer (`ENABLE_VULKAN_RENDERDOC_CAPTURE=1` is set automatically). A system install
    registers it; for the extracted tarball fix the manifest's `library_path` (`../../../lib/librenderdoc.so`) and
    export `VK_ADD_IMPLICIT_LAYER_PATH=<tarball>/etc/vulkan/implicit_layer.d`. Without it the capture logs "failed".
  - Verified 2026-10-15 with the official RenderDoc 1.46 Linux tarball (renderdoc.org/builds), S1L1 frame 30
    (`--scenario S1L1 --screenshot … --frames 31 --renderdoc-capture 30`): GL under `xvfb-run` (llvmpipe) → 222 MB .rdc,
    driver OpenGL, 15126 chunks, 392 draw calls, embedded thumbnail = the S1L1 frame; Vulkan with
    `SDL_VIDEO_DRIVER=offscreen` (RADV, no swapchain) → 183 MB .rdc, driver Vulkan, 3461 chunks, 392 draws (no
    thumbnail: nothing presented). Both opened by `renderdoccmd convert -c xml` / `renderdoccmd thumb`.
- **Tracy** (`-DJOA_TRACY=ON`, Tracy v0.11.1 via FetchContent, needs network at configure time): `src/engine/profile.hpp`
  zones `sim.tick`, `render.submit`, `render.present`, `audio.mix` (mixer thread or the fixed-step capture mix) and a
  frame mark per frame. Verified 2026-10-15: `tracy-capture` built from the fetched sources
  (`cmake -S <build>/_deps/tracy-src/capture`, its CPM step downloads capstone) recorded S1L1 Vulkan offscreen,
  120 frames: 480 zones (4 × 120); `tracy-csvexport` lists `sim.tick`, `render.submit`, `audio.mix` 120 each and
  `render.present` 98 (the offscreen Vulkan `end_frame` is a ~20 ns no-op). Run the client with `TRACY_NO_EXIT=1`
  for short `--frames` runs so it waits for the capture to drain.
- **Dev overlay** (`-DJOA_DEV_OVERLAY=ON`, vendored Dear ImGui 1.93.0 under `third_party/imgui`, MIT,
  `src/engine/dev_overlay.*`): Ctrl+F12 in game toggles a window with frame time + graph, sim tick time/count, submit
  CPU time, draw items / UI quads, unit counts (total/alive/hostile), the backend name, and graphics preset +
  per-feature toggles (applied with `IRenderer::set_graphics`; switching backend needs a restart). It renders through
  `FrameData::ui_vertices`/`ui_triangles`: non-indexed textured triangles with a per-batch scissor rect, drawn after
  the UI quads in the same gamma-space UI pass by both backends (GL `glScissor` with the y flip, Vulkan
  `vkCmdSetScissor`). Tests `engine.gl_dev_overlay` / `engine.vk_dev_overlay` (built only with the option) render
  the demo plain, with the overlay and with a scissored full-screen triangle, and check the window background's
  gamma-space blend, text pixels, the exact scissor rect and untouched pixels elsewhere (Vulkan: zero validation errors).

## Not done yet (in order)

1. Original asset loading through the `MeshData`/`TextureData` seam (DDS decode, SMO/AM1/AM3 meshes, terrain) — depends on `joa_formats`.
2. GPU skinning: vertex layout and attributes are in place; bone palette uniform + shader path once the animation format is known (bone count limit unknown).
3. Vulkan: done for raster parity (section below) and optional hardware ray tracing (reflections/shadows/GI, [RAYTRACING.md](RAYTRACING.md)).
4. Shadows, particles, MSAA, anisotropic filtering, gamma-space blending option, terrain renderer.
5. Original key bindings, mouse-camera behaviour (CAMERA_AND_WIDESCREEN.md §3 mouse delta clamp) and menus/UI art.

## Vulkan

`--renderer vulkan` (or launcher `[renderer] backend=vulkan`) selects `renderer_vk.cpp`, a Vulkan 1.3 implementation of the same `IRenderer`; camera, draw lists and the demo scene are shared with GL.

- **Loader / device**: entry points come from SDL's `vkGetInstanceProcAddr` (no link-time libvulkan; `vk_api.hpp` X-macro table). Every physical device is logged; the best usable one wins (discrete > integrated > virtual > cpu), `--gpu N` forces enumeration index N. Required: Vulkan 1.3 with dynamic rendering, synchronization2, shaderDemoteToHelperInvocation (glslc lowers `discard` to demote), timeline semaphores and descriptor indexing (partially bound, update-after-bind, non-uniform sampled-image indexing). Sampler anisotropy is used when present.
- **Frame structure**: the scene renders into an offscreen `R8G8B8A8_SRGB` colour + `D32_SFLOAT` depth target (same as GL's FBO). Two frames in flight, each with its own command pool, scene + present command buffers, acquire semaphore, UBO, draw-table SSBO, UI vertex buffer and descriptor set; one timeline semaphore retires frames (no fences). Swapchain present = blit offscreen → swapchain image (sRGB on both ends, so bytes pass through unchanged; RGBA → BGRA handled by the blit), black where sizes differ. One "render done" semaphore per swapchain image.
- **Swapchain**: recreated when the window pixel size changes or acquire/present report out-of-date/suboptimal; minimised (0×0) frames render without presenting. `--present-mode fifo|relaxed|mailbox|immediate`, default FIFO (vsync); unsupported modes fall back to FIFO (logged). MAILBOX/IMMEDIATE suit VRR displays.
- **Headless**: under `SDL_VIDEO_DRIVER=offscreen`/`dummy` (or when no device can present, e.g. RADV on Xvfb without DRI3) no surface is created; frames render to the offscreen target and `read_back()` works. Tests and `--screenshot` use this.
- **Clip space**: `view_proj = C · projection · view` with C = y-flip + z∈[−1,1]→[0,1]; front face stays counter-clockwise; viewport keeps the top-left origin. Read-back rows are already top-first.
- **Materials**: three mesh pipelines (opaque/alpha-test share one: discard by per-draw cutoff; alpha blend and additive without depth write) + one UI pipeline; cull mode is dynamic (`two_sided`). Blended draws are sorted back to front exactly like GL. Mesh shaders port `shaders/gl/mesh_*.glsl` 1:1 (layer2/vertex colour/lightmap, alpha test, sun + ambient, unlit, linear fog). Per-draw data (model, normal matrix, tint, cutoff, texture slots) lives in a per-frame SSBO indexed by `firstInstance`; frame data (view-proj, light, fog, camera) in a UBO.
- **Textures**: one bindless `sampler2D[4096]` set (update-after-bind); slot 0 = white. Uploads go through a staging buffer, mips are generated with a blit chain (sRGB filtered in linear space, like `glGenerateMipmap`). Mesh and texture uploads are submitted without a CPU wait (`submit_upload`: the copy's barrier orders it before every later submission; staging retires on the timeline), so runtime creation (grass patches, effects) no longer stalls a frame. Samplers: repeat/clamp × 8× anisotropic (mipmapped, as GL) / plain. Destroyed meshes/textures (and `update_mesh` replacements) are released, and their bindless slots reused, only after the timeline passes every frame that could reference them.
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

- `read_back()` and render-target setup are synchronous (`vkQueueWaitIdle`, rare: screenshots, resizes); `update_mesh` (CPU-skinned units in view) writes fresh host-visible buffers each call — a per-mesh ring would save the buffer creation (~0.2 ms for 45 units).
- No MSAA, single graphics queue (no async transfer/compute), no HDR swapchain formats. RT: [RAYTRACING.md](RAYTRACING.md).
- Swapchain needs an `*_SRGB` surface format for byte-exact presentation; a UNORM-only surface would present darker (all common Linux drivers expose B8G8R8A8_SRGB).

## Performance (measured 2026-10-17, RX 7900 XTX, 3840×2160, FSR 3 quality)

- **Units**: only units whose body + shadow reach (stencil volume height + 300; RT shadow height / sin(sun elevation)) intersects the view are skinned and drawn (`WorldView::frame`), as static objects already were; skinning writes into a kept `MeshData` (no per-frame index rebuild/allocations); stencil volumes use per-caster edge twins (no per-frame edge sort) and are not built at all when the backend drops them (`IRenderer::draws_shadow_volumes`, Vulkan RT shadows). S4L1 siege (135 units in range): world-view CPU 6.4 → 1.0 ms, VK+RT GPU 11.1 → 7.2 ms (BLAS refits/prepass/opaque shrink with the culled units).
- **Environment maps** (`pbr_env.cpp`): the fog colour follows the view direction, so turning the camera rebuilt them; the convolution is now a fixed linear basis combined per frame, and Vulkan rewrites the two images in place (14 ms spikes → < 0.5 ms).
- **Minimap unit dots** are recomputed once per simulation tick (units × vision areas: 0.95 → 0.23 ms per frame in a crowd).
- **OpenGL on Wayland** runs with swap interval 0 and paces itself to the display refresh in `end_frame` (`SDL_DelayPrecise` deadline): SDL's emulated vsync there stalled one frame in ~8 for the VRR panel's minimum refresh (20.9 ms). S1L1: 234 fps, p95 5.3 ms.
- Remaining: simulation ticks cost 3.5–5.5 ms in crowds/towns (AI unit search + nav line of sight, units × units); with RT on the GPU is the limit (crowd ≈ 8.4 ms: RT passes ≈ 3 ms, FSR ≈ 1 ms).

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
| Terrain mesh | GRI vertices 64 units apart, int16 height, quad diagonal alternates with (x+y) parity; a cell is drawn when its own node is present (byte 0, Level_load_terrain 0x4f6eb0) — the last vertex row/column of every node has byte 0 = 0 but holds the neighbour node's first-row heights, so the edge cells close the node seams (requiring all four corners left a 64-unit hole along every seam of multi-node maps, alpha.16) | CONFIRMED (world_s.cpp builder, decompile ~line 443900) |
| Terrain texturing | `mix(tex1, tex2, (255-byte4)/255) * lightmap * DIFFUSE`, ground UV repeats every header_c (4) cells (u from y, v from x); DIFFUSE = the lit vertex colour (bytes 1,2,3 as D3D diffuse material, `Material::vertex_diffuse`): at noon ambient 0.72 + sun·0.78 clamps to 1; sector lightmaps packed in one atlas, sector (ix,iy) at row rows-1-iy | CONFIRMED stage setup (case 0x13 ~line 523175: tex, BLENDDIFFUSEALPHA, MODULATE lightmap, MODULATE DIFFUSE); lit vertex colour HYPOTHESIS (unlit was 0.78× darker than the capture; lit matches within 5%) |
| Objects | `item/<Path><ObjectFile>.pro` → `Model:`; else `.SMO`/`.AM1`; AM1 props posed at their first AM2 clip; transform `T(X,Y,Z)·Rz(HD)·S(Scale)`; VD, WIDTH/LENGTH/HEIGHT, Kingdom, Treasure, InitState/FinalState unused | HIGH (S1L1: 762/762 placed; 686 within 40 units of the terrain; walls/gate/drawbridge join up) |
| Materials | textures with any alpha < 255 → alpha test 0.5, two-sided; trailer tag-2 flag → two-sided; tag 3 normals → up | HYPOTHESIS |
| Tag-1 blend | any value ≠ -1 → material +0x38 = 1 (+0x34 = 0x20000, +0x40 = 1/2) in the SMO loader 0x5e0530 (decompile ~512576); render-state set 1 of `FUN_005f82e0`: SRCALPHA/INVSRCALPHA, alpha test (ALPHAREF 0x80, GREATEREQUAL from `FUN_005f8110`), lighting, z-write → `BlendMode::AlphaBlend` + `Material::depth_write` (sorted back to front, alpha test 0.5, z-write; all presets, both backends). Without the z-write the far-side layers of Joan's head/hair/ponytail submeshes (joanface.tga) drew over the near ones (see-through hair) | CONFIRMED |
| Mip maps | the DDS file chain (face 0) is uploaded as-is (`TextureData::mip_pixels`, GL `glTexImage2D` per level + `GL_TEXTURE_MAX_LEVEL`, Vulkan one staging copy per level); single-level files get GPU mips | — |
| Lighting | stage at a clock (default 12:00) from `<name>w.day`; `.lgt` Type 1 directional (VDir elevation / HDir azimuth, HYPOTHESIS), Type 0 ambient. D3D8 fixed function emulated per vertex in gamma space: `min(ambient·240/255 + diffuse·(sun·N.L + Σ point), 1)`, texture × light, diffuse = SMO/AM1 colour1 (0.588); material ambient is the engine constant 0xF0F0F0 (DAT_00a69cd0 ← DAT_00a18a6c, FUN_0060baf0) | CONFIRMED constants/formula (lighting.nvo, D3D semantics); gamma-space sum HIGH |
| Fog | linear vertex fog on the eye distance (fog.nvo `calculate_linear_normal_fog_factor`, D3DFOG_LINEAR), 900 → view distance 3996 (FUN_005634e0: min(far−200, 4096 [×0.7 night/rain]) reached within 100), mixed in gamma space; colour = clear colour = Hoffman-Preetham inscattering 2000 units along the view direction from the `.sca` stage (View3D_render 0x614760; betas from 0x617ee0; Normal_12 level view = 106,146,193), recomputed per frame | CONFIRMED (HG denominator taken literally as `2g·cos + g + 1`) |
| Depth range | in-game near 4 / far 4096 (FUN_005634e0 → 0x613ca0); objects culled at 4096; sky dome radius 4000 | HIGH |
| Point lights | `.stg CreateLight` light_type 3 = D3DLIGHT_POINT (FUN_005c5c30): RGB/255, ATT0..2, Radius = Range, registered per `scene_name` sector (lights reaching several sectors are repeated, shared Group); item `.pro HasLight` lights join their object's sector (HYPOTHESIS); a draw's light set = its sector's lights (FUN_005e67b0 switches light sets per primitive group), ≤ 7 + sun; skinned units 1 point + 1 directional (Shader_build_vertex_shader); RANDOM_ATTENUATION + ModifyRange flickers ATT1 per Group (FUN_004afd60, timing approximated) | CONFIRMED parse/selection; S1L1: 144 lights |
| Sky | `sky_dome`: camera-centred hemisphere (radius 4000), unfogged, not a shadow receiver: fog colour at the horizon → (150,186,220) band → (118,160,206) zenith; `.day` CloudTexture1/2 as two alpha-blended layers. Built at noon; per frame the whole dome is scaled per channel by this view's fog colour / the noon one (≤ 4×), so its horizon is the colour the scene fogs to (towards a low sun the +X colour capped at 1.5× left a teal sky over beige fog, S5L1 Jargeau). The original draws `model/sky01.smo` through `scatter.nvo` (oD0 = extinction·terrain reflectance·sun, oD1 = inscattering) — not reproduced | HYPOTHESIS, colours sampled from s04_04/s04_06 |
| Shadows | stencil shadow volumes (`shadow_volume.cpp`, port of FUN_005e3040): welded `<am1>#S.AM1` (+ item `#s.SMO`), light-facing faces = near cap, silhouette edges extruded height+300 along the sun, no far cap; opaque pass writes stencil 128 on receivers (terrain/objects; units, sky 0); volumes front INCR / back DECR clamped (z-pass, 0x5f1cc0), then a full-screen black alpha-128 fill where 128 < stencil (0x5ee1d0) before the translucent pass; CONFIG.DAT 0x9C modes (volume within 1000 units, `ShadowModel` quad within 2000) | CONFIRMED order, ops, colour, cap/edge rules; HYPOTHESIS: extrusion length, units not receivers |
| Dynamic grass | `grass_field`: 3×3-cell (192-unit) patches, ground texture covering ≥ half the cells → `DynGrass.res` `<tex>_M<scenario>` then `<tex>`: `model : count : percent : yaw : tilt`, `rand(100) < percent` → `max(1, count·density/100)` tufts, random yaw + tilt−(6..10)°, slope skip/sink, radius 2000 (CONFIG.DAT), one new patch per frame, alpha 255−128·(d−100)/(R−100) beyond 100 (FUN_00464740/00465a20/00466560) | CONFIRMED; `_M1` lookup HIGH; wind not done |
| Water | `water.cpp`: `CreateWater` of each node's `.stg` (FUN_004f5210) and of the world's `.wor` (FUN_0054a6c0 -> FUN_00553aa0, world coordinates: the S2L1 / S4L1 / S5L1 / S6L1 rivers) 800-unit grid around X,Y, vertex colour 65,51,80 a 0x80; texteff `BumpEnvWaterRiver`: BUMPENVMAP over `Lake1..65.bmp` (bump matrix 0.5, 30 ms/frame) then `spheremap` at uv = world reflection.xy + 0.5 (`Material::bump`). Remaster `water=enhanced`: stylised shader (section "Remaster", `Material::water`) | CONFIRMED default (Reflection = Modulate, Bump = Yes); Reflection = High (planar RTT) not done |
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
`--graphics k=v,...` (look keys imply custom; image-quality keys do not), `--hd-packs` ([HD_ASSETS.md](HD_ASSETS.md)). `src/engine/preset.*` resolves
them into `GraphicsSettings`; the app calls `IRenderer::set_graphics()` after creating the backend (may be called
again between frames). Without a settings file the engine runs **Original**.

- **Original** = the faithful D3D8-emulating path, untouched: verified byte-identical to the pre-remaster build
  (S1L1 1920×1080 and the demo frame, GL and Vulkan: `work/remaster/baseline` vs current), and
  `engine.gl_remaster` / `engine.vk_remaster` check Original stays bit-identical after Enhanced/Ultra frames.
- **Image-quality keys work with every preset** (section below): `aa`, `msaa`, `anisotropy`, `render_scale`, `upscale`,
  `sharpness` are applied on top of any preset (Original included) and never switch the renderer to the remaster path;
  the remaster path runs only for the look keys (`pbr`, `normal_maps`, `ibl`, `hdr`, `shadow_map_size`, `ssao`,
  `volumetric_fog`, `particle_lighting`, `water`: `GraphicsSettings::remastered()`), which apply only with `custom`.
- **Enhanced / Ultra** (Custom = Enhanced + keys). Every feature has its own key:

| Feature | Key | Enhanced | Ultra | Implementation |
|---|---|---|---|---|
| PBR metallic-roughness | `pbr` | on | on | `shaders/common/pbr_shade.glsl`: per-pixel D3D8 light sum kept as the reference brightness (ambient part × sky irradiance shape × AO, direct part × shadow), GGX sun specular + prefiltered sky reflection on top. Legacy conversion by `MaterialClass` + albedo: only unsaturated bright (steel) / saturated yellow (gilding) texels of characters (≤0.55) and carried items (≤0.75) become metallic; terrain 0.95, buildings 0.88 roughness |
| Normal maps | `normal_maps` | on | on | HD pack `_n` maps, screen-space cotangent frame (no vertex tangents) |
| IBL | `ibl` | on | on | `pbr_env.cpp`: 64×32 equirect sky (sky_dome gradient + lit ground) with 7 GGX-prefiltered mips + 32×16 irradiance normalised to the up normal (terrain keeps its Original ambient); rebuilt when the lighting changes. Sky/RT reflection weight = metal fraction on legacy textures (1 with an HD roughness map): the 4 % Fresnel sheen on every legacy dielectric lifted dark texels towards the sky and cost characters ~30 % local contrast (Joan crop Laplacian variance 0.69–0.72× Original → 0.96–1.15×, 1080p/4K, 1 tick and 240 fps pacing) |
| HDR + tone map | `hdr`, `exposure` | 0.85 | 0.85 | RGBA16F scene, auto exposure, Narkowicz ACES fit, sRGB encode. Auto exposure tracks the Original look's displayed brightness: every mesh/fill draw also writes the pixel as the Original would light it (D3D8 light sum, Original fog, no shadow map/AO/specular; unlit = its colour) as linear luma into an R16F reference MRT (location 3), blended like the colour (translucent draws and the units' shadow fill included, cleared to the clear colour). After the translucent pass `post_exposure_lum.glsl` takes 4×4 samples per cell of a 64×32 target: mean displayed luma (sRGB of the luma after the volumetric fog, exposure, ACES) of the scene at e/2, e, 2e and of the reference; `post_exposure_adapt.glsl` (1×1 R32F, ping-pong) averages the 2048 cells, interpolates log-log to the exposure where both match, clamps to 0.5–4× `exposure` and approaches it with a 0.5 s time constant (`FrameData::dt`), snapping without history (first frame, resize, settings change, camera jump ≥ 256 units). All on the GPU, no readback; the composite reads the state. Mean luma at 1080p, Original; Enhanced before → after; Ultra before → after (GL; Vulkan within 0.6): night (`JOA_WORLD_CLOCK=1450`) m1 run 33.2; 25.8 → 32.9; 26.1 → 33.2 — m7 run 48.7; 39.3 → 48.4; 39.3 → 48.4 — S4L1 church z=100 45.5; 26.3 → 44.7; 26.1 → 44.7 (Vulkan Ultra RT GI 18.7 → 45.1) — z=300 51.6; 34.9 → 50.9; → 51.2 — S1L1 day 66.1; 64.9 → 66.0; → 66.8. Joan crop sharpness Enhanced 0.97–1.09× Original (1 tick and 240 fps, GL and Vulkan). The 4× upper clamp is for RT GI interiors (2.5× left the church at −11 %) |
| Sun shadows | `shadow_map_size`, `shadow_softness`, `shadow_distance` | 2048, 1.5 texel, 2500 | 4096, 2.5, 4000 | world shadows; the units keep the original stencil volumes / shadow models (`Material::volume_caster`, also with RT shadows: units are not in `kRtMaskShadow`) — at ~3 units/texel the map blurred unit shadows away and self-shadowed hair/armour in blotches. The fill darkens the HDR colour, the ambient MRT and the auto exposure reference alike (`shadow_fill_rm.frag`), so SSAO does not over-subtract inside unit shadows. Casters = lit opaque/alpha-test/blended (alpha 0.5) non-unit draws except terrain; texel-snapped bounding-sphere ortho, slope bias + normal offset (1.5 texels; the texel size comes from the length of the shadow matrix's x row — its `[0][0]` element alone collapsed for night suns, offsetting receivers by 100+ units: Vulkan shadowed, GL lit the whole m7 terrain), 12-tap rotated Poisson PCF on hardware compare; vegetation receives at 50 % |
| SSAO | `ssao`, `ssao_radius`, `ssao_intensity` | 48 | 64 | 12 world-space samples from depth, applied only to the ambient/environment MRT (reverse-subtract pass, 3×3 depth-aware blur); not on foliage |
| AA | `aa`, `msaa` | 4× MSAA | TAA | Halton(2,3) jitter; per-pixel motion vectors (RG16F MRT of the opaque and depth-writing blended mesh draws: previous minus current uv, both unjittered; previous model matrix per draw matched by mesh/index range/`DrawItem::object`/occurrence in `post_frame.cpp previous_models`, previous CPU-skinned pose kept by `update_mesh` as a second vertex stream; new or >256-unit jumps = camera-only), taken from the nearest-depth texel of the 3×3 neighbourhood; cleared background reprojects the far plane; 5-tap Catmull-Rom history, YCoCg variance clip (mean ± 1.25 σ), 10 % new sample rising to 30 % with the sub-pixel part (full at ¼ px) or speed (full at 8 px) of the motion; mesh texture LOD bias −1. Camera-only reprojection doubled the running hero and left her greaves semi-transparent; now opaque and single at 30 and 240 fps pacing (GL and Vulkan) |
| Volumetric fog | `volumetric_fog`, `fog_density` | 12 steps | 24 steps | height fog below the camera with shadow-mapped HG(0.55) sun in-scattering, on top of the original linear fog |
| Particle lighting | `particle_lighting` | on | on | alpha-blended smoke lit by ambient + shadowed sun; additive fire ×1.6 HDR |
| Stylised water | `water=original\|enhanced` | enhanced | enhanced | World of Warcraft: Forever-style `.stg CreateWater` surfaces (not ice), `pbr_shade.glsl shade_water`: before the translucent pass the opaque HDR colour (MSAA: resolved) and depth are copied (GL blit into `water_color`/`water_depth`, units 29/30; Vulkan `RmTargets::opaque` — shared with the FSR 3 reactive-mask input — + `water_depth`, set-2 bindings 22/23); water draws first among the blended draws, writes alpha 1 over the copy plus its own depth and motion vectors (blend_zwrite pipeline / GL depth mask + motion buffer: TAA, FSR 3 and the volumetric fog see the surface, not the bed), so every other blend lands on top and FSR 3's reactive mask (opaque copy vs final HDR) marks it. Colour = refracted bed (uv offset by the ripple slope, scaled up with depth, cancelled where the offset sample lies above the surface) × per-channel absorption exp(−(0.008, 0.0026, 0.0019) × path) + in-scatter teal (0.05, 0.32, 0.30) → deep blue (0.006, 0.05, 0.11) over 20–260 units of bed depth, lit by the Original water's D3D light; Schlick-Fresnel sky (IBL env mip 0; Ultra RT: `rt_water` mirror ray about the rippled normal); GGX (α 0.07) sun glints shadowed by the sun map; shoreline foam over the first 40 units of depth broken into drifting lace by fine value noise (12 and 5 units; the 260/900-unit height maps alone and a sin of the depth drew a solid white line along every bank, alpha.15); reflection, glints and foam fade in over the first units so the waterline has no hard edge. Normals: two world-anchored layers (900 and 260 units per tile, crossing directions, 0.02/0.05 tiles/s) of a normal map made from the original `Lake1.bmp` height map (`water_normal_map`, height in alpha = foam noise), registered as the water albedo's `MaterialMaps::normal`. Water casts no sun shadow. No vertex waves (800-unit grid) |
| Water waves | with `water=enhanced` | on | on | Port extension (the original water is static): a CPU wave simulation on the stylised water, so waves from every source interfere, reflect off the banks and scatter around wading legs. `water_waves.hpp WaveField`: a 256² height field of 4-unit cells (a 1024-unit window following the camera target, scrolled by whole cells; shifted-in cells start at rest; 8-unit cells could not hold the ~30-unit ripples that read at the gameplay camera), damped 2D wave equation (Verlet, fixed 1/60 s substeps, 80 units/s, velocity damping 2.5/s: a splash is ~0.3 units after 1 s and gone by 3 s, so a small pond does not fill with reverberating chop; a viscous velocity-Laplacian term (0.005) eats the cell-sized checkerboard moving legs stamp in; a weak spring back to the rest level), walls = cells without a `WaterField::surface_height` surface or with terrain at/above it (cached per cell, computed as cells scroll in) and, per step, 10-unit discs at wading units' feet; neighbours behind a wall count as the cell itself (reflection without a phase flip). Heights are displacements relative to each pond's own surface. Kicks are zero-volume Mexican hats (crater down, rim up; strength s: 7·s^0.7 units deep, σ = 6 + 4·s; the part walls or legs clip off is levelled back, else the pond level creeps up): wading units (feet under the surface, not deeper than their height) push a bow mound ahead and a stern trough behind while moving (`WaveField::wade`: 0.3 units per unit walked each frame, σ 6 just outside the legs, speed capped at 500), which leaves a V wake (faster than the waves) of ~2-unit waves, and bob every 1.2–1.5 s standing (0.15); weapon tips (`Character::attachment_tip`) crossing the surface or sweeping 40 units under it (0.6); projectiles through the surface or vanishing just above it, map items breaking and bodies dying in it (1.0, plus the shipped but otherwise unused `jumpintowatersplash_s` pscript), knock-downs and items dropped in it (0.8). Nothing kicked for 8 s: the field is at rest and `FrameData::waves` is empty. Cost: 0.2 ms per substep (`engine.water_waves`, RelWithDebInfo). `water=original` ignores it. Tests: `engine.water_waves` (superposition / cancellation, shore reflection, legs and clipped-kick volume, damping, scrolling, wake height, timing), `engine.world_view_session` (S1L1 "Water 4" pond: wading moves the field near Joan, the bank never moves, a dropped item kicks). Shading: `build_remaster_frame` maps the field into `RemasterFrame::waves` (origin xz, 1/(cell·256), cell; all 0 unless the span holds exactly 256² heights); GL respecifies an R16F texture on unit 31 in frames with a field, Vulkan packs half floats into the frame slot's staging buffer and copies them into the slot's R16F image (set-2 binding 24) before the scene passes (frames without a field bind the white dummy). `pbr_shade.glsl water_waves` samples it at the pixel's xz, faded over the window's outer 4 %, with taps 1.5 cells out (central differences / Laplacian; the bilinear fetch cancels the cell checkerboard that otherwise speckled the crests). The S1L1 pond wade measures \|height\| 0.10 / 0.36 / 0.83 (median / p90 / p99), slope 0.012 / 0.065 / 0.17, \|Laplacian\| 0.0026 / 0.018 / 0.054, and the gains follow from that: the slope tilts the normal ~1:1, softly capped towards 0.3 (steeper tilts swing the ray-traced reflection across bank / sky edges into hard-edged blotches), plus a 0.015 uv-per-slope shallow-water refraction offset of the bed; crest light / trough darkening (60 % mix towards half sky, half fog colour / 0.4×) from the curvature, not the height — a high-pass, so sloshing and a wake's raised wedge stay a tilt and fronts read as lines (bend = −100 × Laplacian through smoothstep(0.4, 3): the median chop stays dark, p90 fronts half lit, p99 full); foam where \|height\| exceeds 4–7 units (weapon and splash craters) or the Laplacian falls below −0.12…−0.25, broken up by the normal-map pattern. Against the alpha.8 analytic rings at the gameplay camera (mean / p95 \|luma − no-wave frame\| in the pond crop: 5.0–8.7 / 18–66 vs 5.9–11.1 / 31–41): `work/ripples/eval/b11_z4.png` (Vulkan Ultra, new left / alpha.8 right), `g11_vs_b11.png` (GL Enhanced vs Vulkan), `lakek_vk_gl.png` (S1L1 lake camera, a circling wader + splashes). Khronos validation (+synchronization): no new messages (the 240 pre-existing final-pass binding-12 / present hazards appear identically without waves). Smoke: `work/ripples/` (S1L1 pond wade, `wade_waves.txt` / `wade_waves_gl.txt`) |
| Upscaling | `upscale=fsr1`, `render_scale`, `sharpness` | off, 1.0, 0 | off, 1.0, 1.0 | FSR1 EASU (12-tap, texelFetch port) + RCAS (MIT, AMD FidelityFX). Ultra RCAS 1.0 after TAA: a converged TAA image is ~0.5× the no-AA Laplacian variance (demo static view 0.49–0.59×); at 1.0 the running Joan crop (`work/gfxbug/run.txt` shots 130/160/185, 1080p, torso) is 1.07/0.90/1.11× (GL) and 1.07/0.92/1.13× (Vulkan) the `aa=none` frame at the old 0.3, 1.18/0.94/1.18× at 240 fps pacing; motion-vector TAA at 0.3 gives 0.33/0.32/0.45×, the old camera-only TAA 0.17/0.21/0.19× |

`auto_exposure=false` (any remaster preset, like `sharpness`) pins the exposure to `exposure` — A/B comparisons and
`engine.vk_rt_smoke` use it so a feature's local change is not rescaled by the brightness tracking.

Passes (both backends, same shader bodies in `shaders/common/`, same CPU constants from `post_frame.cpp`): shadow map →
opaque (HDR colour + ambient + motion + auto exposure reference MRTs, depth-stencil; multisampled with `msaa`, then depth +
ambient resolved) [→ stencil volumes + fill when shadow maps are off] → SSAO → AO apply → translucent (resolves HDR, motion,
reference, depth) → auto exposure measure (64×32) + adapt (1×1) → composite (volumetric fog `post_fog.glsl`, exposure,
ACES) → TAA | FXAA | SMAA → EASU → RCAS/copy/downsample into the display target → UI.
GL: units = Vulkan set-2 binding + 7, block binding 0 (`shaders/gl/remaster_prelude.glsl`). Vulkan: set 2 (frame
block, textures, per-draw `MaterialExtra` SSBO), separate depth-only shadow caster shader, `independentBlend` required.

Tests: `engine.preset` (names, ini/CLI keys, custom semantics, image-quality keys on every preset incl. Original + `taa` →
`smaa`, `--graphics` quality keys not implying custom, supersampling scene size, jitter/LOD bias/noise index, shadow texel snapping),
`engine.gl_remaster` (Original bit-identical incl. after toggling; Enhanced/Ultra/FSR1 sane and within ±25 % mean luma
of Original; TAA static-view sharpness; blend-state-1 layer/halo pixels for Original/Enhanced/Ultra; S1L1 when data
present, incl. Enhanced Laplacian variance ≥ 0.95× Original), `engine.vk_remaster` (GL vs Vulkan parity: Enhanced, Ultra
(TAA, RCAS pinned to 0.3: 1.0 amplifies sub-LSB API differences to 38.6 dB), FSR1 67 %, Original look with `msaa=4,aa=smaa` and
with FXAA + `render_scale=2` + RCAS + 16× anisotropy (also: differs from plain Original), S1L1 Enhanced; thresholds 40 dB / 1.0; layer pixels and S1L1 sharpness as GL; 0 validation errors),
`engine.hd_pack`, `tools.hd_upscale`.

Remaster gaps: no soft particles (GL 3.3 cannot sample the attached
depth); TAA motion vectors cover only the mesh passes (particles and non-depth-writing blends reproject with what lies behind them; the RT denoiser history is still camera-only); GL GPU timing (`JOA_GPU_TIMING=1`)
uses timestamps that include driver submission bubbles; no bloom; the volumetric density is relative to the camera
height (no absolute ground reference); the in-game options menu does not expose presets yet. Stylised water: refraction
sees only the opaque scene (blends below the surface are hidden), and the straight edges of the original water
rectangles stay visible where they end above lower terrain.

### FSR 3 (`upscale=fsr3`, `frame_generation`; Vulkan)

AMD FidelityFX SDK v1.1.4 (FSR 3.1.4 upscaler, frame interpolation, optical flow, Vulkan backend) is vendored in
`third_party/fidelityfx-sdk` (MIT; `README.md`: provenance from ETI Yami, glue in `joa/`, the 7 local patches). CMake
compiles only the permutations the clamped capabilities request (glslangValidator; glslc strips the resource names the
SDK binds by) into `joa_fsr3`; `tools/embed_fsr_shaders.py` writes the blob accessor. Wrapper: `src/engine/fsr_vk.cpp`
(ported from IKV `src/fsr_vk.cpp`). Device needs `shaderStorageImageWriteWithoutFormat`, extended storage formats and
compute subgroup quad ops (frame generation also subgroup arithmetic and `shaderStorageImageReadWithoutFormat`); the log
prints `vulkan FSR 3.1.4 available|unavailable: <reason>`.

Pipeline (`render_remaster` → `fsr_upscale`, render size from `fsr_quality`): FSR's Halton(2,3) jitter of
8·(display/render)² phases replaces the TAA jitter (`build_remaster_frame`); the tone-mapped composite is the colour input;
`post_fsr_inputs.frag` writes R32F device depth, full unjittered motion (the per-object TAA motion vectors where geometry
was drawn, far-plane camera reprojection elsewhere) and the reactive mask (FSR's autogen formula between the opaque-only HDR
copy taken before the translucent pass and the final HDR: smoke, fire, particles, water). FSR replaces TAA/FXAA/SMAA and
MSAA (forced to 1); its RCAS takes `sharpness` and the final pass only copies. History resets on resize, settings changes,
camera cuts (the auto-exposure snap) and after any frame without the pass. The HUD/UI is drawn at display resolution
after the upscale. Only one upscaler call per frame: each call advances the SDK's 4-entry descriptor-view ring, so the
SDK's own reactive-mask dispatch (a second call) freed views still in flight.

Frame generation (`frame_generate`, ETI Yami / IKV present path): the HUD-less display image is copied as the
interpolation source, FSR writes the frame halfway between the previous and this one, this frame's UI is drawn onto it,
and `end_frame` presents it before the real frame (own acquire semaphore and command buffer per slot). With FIFO vsync
(the default) each image shows for one refresh, so presentation runs at the refresh rate and rendering at half of it. No
generated frame after a history reset. Logged at exit: `vulkan frame generation: N generated + M real frames presented`.

Ultra uses **native AA** (render scale 1), not Kabus 22's quality mode. Measured on the real display (RX 7900 XTX RADV,
1920×1080, Ultra with RT, `work/gfxbug/run.txt` shots 130/160/185, Joan crop Laplacian variance, `work/gfxbug/Fsr3/`):
unsharpened TAA 702/836/1607; no AA at all 2427/3244/3750; FSR 3 native + RCAS 1.0 2580/3194/3490 (0.97–1.06× no AA,
2.2–3.8× the TAA accumulation); FSR 3 quality (720p) 1320/1617/1687; the old Ultra TAA + RCAS 1.0 5338/4590/5493 reaches
its number by over-sharpening (7.6× its own accumulation, above the aliased frame). Frame generation leaves the real
frames unchanged (identical shots) and presented 189 generated + 190 real frames over the 190-frame script. Khronos
validation (+synchronization) reports 0 errors for Ultra FSR 3 with and without frame generation (raster + RT).

### Image quality on every preset (`aa`, `msaa`, `anisotropy`, `render_scale`, `sharpness`)

The original game offers none of these: `VSetting.exe` exposes only device, 1024×768×32/16, reflection and bump
detail, and `CreateDevice` uses `MultiSampleType = 0` (reverse-engineering/ORIGINAL_BEHAVIOR.md, RENDERING_PIPELINE.md);
D3D8 texture filtering was the driver default, which the native build has always replaced with 8× anisotropic. The keys
below are therefore native additions, named like the remaster keys and resolved by `GraphicsConfig::resolve` for every
preset (`quality_key()`); unset keys keep the preset's value. Original with all of them at their defaults (msaa 1, aa
none, scale 1, sharpness 0) keeps the direct faithful path (bit-identical, `engine.gl_remaster` / `engine.vk_remaster`).

| Key | Values | Original / Enhanced / Ultra | Implementation |
|---|---|---|---|
| `msaa` | 1, 2, 4, 8 | 1 / 4 / 1 | Original look: the unchanged Original passes render into a scene-size multisampled sRGB colour + D24S8 target, resolved (linear-space average) before post and UI. Remaster: HDR, ambient, motion and depth-stencil multisampled; after the opaque pass depth (sample 0) and ambient are resolved for SSAO, after the translucent pass HDR, motion (average) and depth. Alpha-tested draws use alpha-to-coverage (alpha sharpened by `fwidth` around the cutoff, `mesh_frag.glsl`/`mesh.frag`/`mesh_pbr*`), so foliage, grass cards and fences antialias too. Clamped to the device's supported counts; ignored (1×) on the Vulkan ray-traced path. Cost at 3840×2160 4×: ~0.3 GB (Original) / ~0.9 GB (remaster) extra VRAM |
| `aa` | none, fxaa, smaa, taa | none / none / taa | Post passes on the tone-mapped (sRGB-encoded) LDR image at scene resolution, before EASU/RCAS and the UI. `fxaa`: FXAA 3.11 quality algorithm (`post_fxaa.glsl`, own implementation, 12-step search, subpix 0.75). `smaa`: SMAA 1x HIGH (luma edges, 16 + 8 diagonal search steps, corner rounding 25) from the unmodified MIT `third_party/smaa/SMAA.hlsl` + area/search lookup textures (`smaa_luts.cpp`), 3 passes. `taa` needs the PBR pass's motion vectors: on the Original look `resolve()` turns it into `smaa`. FXAA/SMAA soften texture detail too (S1L1 640×360 Laplacian variance 0.74× with SMAA), so Enhanced uses MSAA by default |
| `anisotropy` | 1–16 | 8 / 8 / 8 | GL `GL_TEXTURE_MAX_ANISOTROPY` on mipmapped textures (updated in place when changed); Vulkan anisotropic samplers recreated and every mipmapped bindless slot rewritten |
| `render_scale` | 0.5–2 | 1 / 1 / 1 | scene size = output × scale. < 1: EASU with `upscale=fsr1`, else bilinear. > 1 = supersampling: the final pass averages a 2×2 bilinear box per output pixel (exact 2×2 box at 2.0; RCAS then sharpens at output resolution) |
| `sharpness` | 0–1 | 0 / 0 / 1.0 | RCAS in the final pass, usable without TAA |
| `upscale` | none, fsr1, fsr3 | none / none / fsr3 | `fsr1`: EASU from `render_scale`. `fsr3`: AMD FSR 3.1.4 temporal upscaling (Vulkan remaster path, section "FSR 3"); on OpenGL, the Original look or a device without its features it runs as `fsr1` at the FSR 3 render scale (`fsr3_fallback`) |
| `fsr_quality` | native, quality, balanced, performance, ultra_performance | quality / quality / native | FSR 3 render scale 1, 1/1.5, 1/1.7, 1/2, 1/3 per axis (replaces `render_scale` while `upscale=fsr3`) |
| `frame_generation` | 0, 1 | 0 / 0 / 0 | FSR 3.1.4 frame generation (optical flow + frame interpolation): one generated frame presented before every real one. Needs `upscale=fsr3` and Vulkan (`resolve()` / the renderer clear it otherwise) |

Original-look post path (`draw_scene_original_post` / `render_original_post`): the frame block of the remaster path is
filled by `build_remaster_frame` (no jitter, no TAA), the scene goes through `draw_scene` unchanged into the scene target,
and the shared post tail (`finish_post` / `post_tail`) runs AA → EASU → final into the viewport of the display target.
Vulkan writes set 2 with valid dummies (white texture, 1×1 shadow map) for the remaster-only bindings; bindings 14–17 are
the SMAA edges, weights, area and search textures (GL units 21–24).

Measured (S1L1 cut scene, `--frames 40`, 1920×1080, soldier + sword crop 690,470–870,620; `work/gfxbug/AaOptions/`):

| Image | strong 1-px steps (>48/255) | mean step height | Laplacian variance |
|---|---|---|---|
| Original (GL / Vulkan) | 2.42 % / 2.43 % | 89.2 / 89.1 | 2504 / 2505 |
| Original `msaa=4,aa=smaa` | 1.85 % / 1.89 % | 65.9 / 65.5 | 1222 / 1229 |
| Original `aa=fxaa` | 1.97 % / 1.97 % | 78.1 / 78.1 | 1306 / 1303 |
| Original `render_scale=2` | 2.46 % / 2.47 % | 75.5 / 75.4 | 1999 / 1995 |

Joan running (`work/gfxbug/run.txt` shot 130, crop 840,340–1080,760, `--preset original`, GL / Vulkan): plain 2.98 % /
2.98 % strong steps, mean step 75.3 / 75.2; `msaa=4,aa=smaa` 1.95 % / 1.93 %, 64.9 / 65.7; `render_scale=2,sharpness=0.25`
keeps more detail (Laplacian variance 1.18× plain) at mean step 67.5. `anisotropy=1` vs the default 8: ground strip
(y 800–1000) mean |Δ| 2.1 and 9 % lower horizontal second-derivative variance (both APIs).

GL vs Vulkan (whole frame): Original look `msaa=4,aa=smaa` mean |Δ| 0.17, `aa=fxaa` 0.05, `render_scale=2` 0.03 (8-bit);
Enhanced 4× MSAA 46.8 dB, Enhanced SMAA 44.9 dB; `engine.vk_remaster` demo frames 49.5 dB (Original `msaa=4,aa=smaa`) and
59.0 dB (Original FXAA + 2× supersampling + RCAS 0.5 + 16× anisotropy); 0 Vulkan validation errors.

Dev: `JOA_CAPTURE_FPS=240` makes `--screenshot`/`--autoplay-script` runs advance 1/240 s per frame instead of one
33 ms tick, so render interpolation runs as on a high-refresh display (scale script frame numbers by fps/30).
`JOA_WORLD_CLOCK=<minutes>` picks the world's `.day` lighting stage (the port otherwise starts at 720 = noon), e.g.
1450 = `Normal_Night`.
