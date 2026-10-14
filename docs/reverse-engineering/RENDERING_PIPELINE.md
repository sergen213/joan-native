# Rendering pipeline (joan.exe, Direct3D 8)

Labels: CONFIRMED = visible in disassembly/decompile; HIGH = strong context; HYPOTHESIS = role guessed. Device-method names are resolved from `IDirect3DDevice8` vtable offsets (`offset/4` = method index in `d3d8.h`).

## 1. Device initialisation — `D3DDevice_create` `0x5f6c00` (CONFIRMED)

Owner: global D3D object at `0xa52420` (layout in `DATA_STRUCTURES.md`). Call chain: `WinMain` → `Sys_init 0x4d6c50` → `Vga_init 0x603710` → `Vga_init_d3d 0x6037f0` → `D3DDevice_init 0x5f6620` → `D3DDevice_create_d3d 0x5f6b60` (`Direct3DCreate8(120)`) + `D3DDevice_create`.

1. Vertex processing mode `obj+0x18`: default 2 (hardware); `MIXEDVP.SYS` → 1, `SOFTVP.SYS` → 0. Downgraded to software when `D3DCAPS8.DevCaps` lacks `D3DDEVCAPS_HWTRANSFORMANDLIGHT` (0x10000). Behaviour flags: 0→`0x20` SOFTWARE, 1→`0x80` MIXED, 2→`0x40` HARDWARE.
2. Mode enumeration: for every adapter (`GetAdapterCount`, vtbl+0x10) and mode (`GetAdapterModeCount` +0x18, `EnumAdapterModes` +0x1c) keep only **1024×768** (immediates `0x400`/`0x300`), with a format accepted by `0x5ff920`. Prefer the adapter in `config+0x7e` with a 32-bit-class format (`0x5ffe60 == 4`) and refresh = `config+0x8e` (default 60). Up to 256 candidates.
3. `GetDeviceCaps` (+0x34) into `obj+0x50`.
4. `D3DPRESENT_PARAMETERS` at `obj+0x1c` (0x34 bytes): `BackBufferWidth/Height` = mode, `BackBufferFormat` = `0x5ff9d0(...)`, `BackBufferCount` = 1, `MultiSampleType` = 0, `SwapEffect` = DISCARD (FLIP if byte `0xa500a0`), `hDeviceWindow` = `0xa50090`, **`Windowed` = 0 (always fullscreen)**, `EnableAutoDepthStencil` = 1, `AutoDepthStencilFormat` = `0x5ffc90(...)`, `FullScreen_RefreshRateInHz` = mode refresh, `FullScreen_PresentationInterval` = `0x80000000` (IMMEDIATE, no vsync).
5. Window restyled to `WS_POPUP|WS_VISIBLE|WS_CLIPSIBLINGS` (`0x92000000`), `WS_EX_TOPMOST|…` and sized by `AdjustWindowRectEx`.
6. `CreateDevice` (IDirect3D8 vtbl+0x3c) → device at `obj+0x08` (= global `0xa52428`). Failure → `Error_run("CreateDevice fails (%s)", D3D_error_string(hr))`.
7. Post-create feature flags (derived from caps): `obj+0x203` vertex shaders usable (`VertexShaderVersion` major ≥ 1 and HW/mixed VP); `obj+0x207` dot3 bump path (needs `MaxSimultaneousTextures ≥ 3`, `D3DTEXOPCAPS_DOTPRODUCT3`, `D3DPMISCCAPS_TSSARGTEMP`); `obj+0x20b` hardware skinning (VS ≥ 1.x, disabled by `SOFTBONE.SYS`); `obj+0x313` usable user clip planes (`MaxUserClipPlanes`, or probed 1..6 via `SetClipPlane` + `D3DRS_CLIPPLANEENABLE`); `obj+0x307` depth format → `obj+0x30b` stencil bits (D15S1→1, D24S8→8, D24X4S4→4).
8. Back/depth surfaces (`GetBackBuffer` → `obj+0x0c`, `GetDepthStencilSurface` → `obj+0x10`), clear all buffers, set default render state cache, create dynamic VB/IB (`0x5f7ca0`, "Cannot create vertex buffer").

After device creation (`WinMain`): `Config_validate_caps 0x45b240` (probes a 64×64 render-target texture + matching depth surface for `config+0x9d`, a 256×256 `D3DFMT_V8U8` texture + `D3DTEXOPCAPS_BUMPENVMAP` for `config+0x9e`; adapter description containing `"geforce3"` disables `+0x9e` when `+0x9d == 2`), then `D3D_create_rt_depth(1024,1024)` when `config+0x9d == 2` and `D3D_create_render_texture(512,256)`.

Resolution: the original **cannot** select another resolution — 1024×768 is hard-coded in the enumeration loop even though `CONFIG.DAT` stores width/height (`config+0x82/+0x86`). Screen size globals used by UI/camera code: `0xa4885c` (W), `0xa48860` (H). CONFIRMED.

## 2. Frame loop (CONFIRMED)

`Sys_main_loop 0x499960`:
- `FrameTimer_init(30, 30, …)` (`0x58a830`, periods `1000/30` ms); paused/background variant uses 500.
- Pump messages (`PeekMessage`/`GetMessage`); otherwise `MsgWaitForMultipleObjectsEx(timeout = FrameTimer_wait_ms)`.
- On timeout: measure elapsed (`sys+0x2e`, clamped to 1000 ms), speed factor `sys+0x40 = elapsed/1000*30` (clamped 5.0).
- If draw due: `Sys_disp_frame 0x4d48f0`, then later `Vga_flip 0x603af0` → `D3D_present 0x5ff0e0` (`Present(0,0,0,0)`).
- If logic due: `Game_process_tick 0x4e5fc0` (unit/item/trigger/particle/physics processes).

`Sys_disp_frame` switches on screen mode `sys+0x3a`: menu/screen draw functions `0x47b7a0..0x47bfd0` for modes 1,3–8,11–15; mode 9 = in-game `World_draw 0x44c800`; mode 2 renders `View3D_render` directly; modes 2/9 also composite WMV playback (`0x685a80`) letterboxed to the screen.

## 3. World / scene rendering

`World_draw 0x44c800` (HIGH):
1. Pick current camera (`CameraArray_current_index`, `CameraArray_get`), run `CamMan_update 0x4237a0`; if changed copy its 10-dword state to the main view (`View3D_set_camera_params 0x614040`, view object pointer `0xa48ccc`).
2. Sector activation in an 8000/16000-unit box around the camera (`0x5479a0`/`0x547630`), optional VRAM-pressure unloading (`free_world_video_memory.sys`).
3. Per-unit pre-render (`0x43b890` / `0x522850`), then `View3D_render 0x614760`.

`View3D_render 0x614760` (CONFIRMED order):
1. `View3D_update_matrices 0x6141a0` (view = inverse camera matrix, projection, frustum planes) — see `CAMERA_SYSTEM.md`.
2. `SetTransform(D3DTS_VIEW)`, `SetTransform(D3DTS_PROJECTION)`, `SetViewport`.
3. Atmospheric scattering constants from the sky/sun object (Rayleigh/Mie phase factors `100/(4π) = 7.957747`, `100·3/(16π) = 5.9683104`), lightning effect (`"Lightning"` lookup), fog/sky clear colour.
4. `Clear(target|zbuffer[, stencil])`.
5. `View3D_render_prepass 0x617370` (HYPOTHESIS: off-screen passes — shadow map / reflection; it reaches `View3D_render_to_texture 0x5d8650`, which does `GetRenderTarget`/`SetRenderTarget`/`SetViewport`/`BeginScene`/`EndScene`).
6. `BeginScene` → `Scene_render 0x5ebac0` → `SetMaterial` → `EndScene`.

Stage names inside `Scene::render` come from the profiler table at `0x9fb2b8` (CONFIRMED strings, ordering HIGH): render sky box → turn on light → sort sector → render all sector front to back (grouped primitives; planes: wall / road / water / 2D / actor / billboard / particles / others; actors in sector: update bones, update skins, disp/flush groupable actor) → render all sector back to front (translucent) → draw shadow (create / render shadow volume) → turn off light → view3d render primitives / actor → view3d EndScene → view2d render → minimap display → `flip (Awaiting GPU to render)`.

World representation: sector tree (`SceneSector`, `SceneSectorTreeNode`), occluders/anti-portals (`AntiPortalPercentage`), "planes" (`en3d\plane.cpp`) for walls/roads/water/terrain pieces. Terrain is rendered as plane geometry within sectors (HYPOTHESIS: no separate heightmap renderer was found; `physics get slope height` queries the same geometry).

## 4. Shaders, skinning, fog, lighting

- Vertex shaders: `Shader_create_nvlinker 0x618990` loads `SHADER\nvlinker.dll`, `CreateLinker(100,1)`, adds 7 `.nvo` fragment files; `Shader_build_vertex_shader 0x618e40` links fragment lists such as `calculate_objectspace_position`, `calculate_skin_vertex` (hardware_skin), `calculate_objectspace_normal`, `calculate_dot3_light`, `c_spot_light_falloff`, `calculate_height_fog_factor`, `calculate_linear_normal_fog_factor`, `write_d3d_fog_factor`, `create_normalized_eye_to_vertex` (strings `0x9d9ea0..0x9da578`). Constants uploaded with `SetVertexShaderConstant` (81 sites in `0x61a8c0..0x61d240`), which read back fixed-function state (`GetTransform`, `GetLight`, `GetLightEnable`, `GetMaterial`, `GetRenderState`, `GetClipPlane`) — i.e. the shader path emulates the fixed-function state the rest of the engine sets. CONFIRMED.
- Skinning: hardware (`hardware_skin.nvo`) when `obj+0x20b`, otherwise CPU `skin_deform` in `v3d_*.dll`. CONFIRMED.
- Fog: vertex fog factor written by shader (`write_d3d_fog_factor`), height fog and linear normal fog fragments. HIGH.
- Lighting: fixed-function lights (`SetLight`/`LightEnable` in `0x5c6110`, `0x5c6250`) + dot3 bump mapping (`item\dot3_directional.vsc`, `Scene::set_dot3_light`). HIGH.
- Pixel shaders: `clip_am.pso`, `clip_at.pso` (`PixelShader_load 0x5e5a50`, ps.1.0) — HYPOTHESIS: alpha-masked / alpha-tested clip-plane variants for water reflection.
- Texture stage blending is data-driven: blend-mode names in `.data` (`alphareplicate`, `modulateinvcolor_addalpha`, `blendtexturealpha`, …, `0x9d6384..0x9d6540`).
- Texture quality picks `TEXTUREH\`, `TEXTURE\` or `TEXTUREL\` (`Texture_select_dir 0x608e10`); only `texture\` ships.

## 5. Shadows, particles, effects

- Shadows: `Scene::draw_shadow`, `render_sector_plane_with_shadow`, profiler "create shadow volume"/"render shadow volume" → stencil shadow volumes (stencil bits from the depth format). HIGH.
- Water: reflection via `Scene::render_reflection` + render-to-texture (HYPOTHESIS: `config+0x9d`, 1024² depth surface, clip planes or `clip_am/clip_at.pso` alpha clipping when user clip planes are missing) and EMBM ripples (`config+0x9e`, V8U8 bump texture).
- Particles: `particle_emitter_res`, particle scripts `SCRIPT\PSCRIPT\*.txt` (`pemires.cpp`), profiler stages emitter/event-sequence/update.
- Dynamic grass (`dyngrass.cpp`, "generate grass", "change grass alpha"), water and water waves (`Object\Models_Items\WaterWave\…`), lens flares (`uti3d\lflare.cpp`), glowing lights, sky mesh (`test\sky_mesh.SMO`).

## 6. UI / 2D

`en2d` (`font2d`, `image2d`, `widget2d`, `txtbox2d`, `popupwin`) draws after the 3D scene ("view2d render"); bitmap fonts are registered by name in `Sys_init_subsystems` (`font_verdana_10`, `Goudy_bold_24`, …). Interface resources: `RESOURCE\*.SET`, `INTERFAC` sets, `int8x6\*.dds`.

## 7. Present

`Vga_flip 0x603af0` → `D3D_present 0x5ff0e0`: `Present(NULL,NULL,NULL,NULL)` and a back-buffer index counter at `obj+0x128`. Error message boxes temporarily present until buffer 0 is visible (`ErrorBox_pre/post`).

## D3D call-site census (from `work/ghidra/export/joan_decomp.c`, global device `0xa52428` only)

BeginScene/EndScene: `0x4f5210`, `0x553aa0` (world_s; off-screen renders), `0x5d6050`, `0x5d8650`, `0x614760`. SetRenderTarget: `0x4f5210`, `0x553aa0`, `0x5d8650`. SetTransform: 39 sites in 7 functions. SetRenderState: 103 sites in 18 functions (most state goes through cached wrappers such as `0x5f8250`, `0x5fa250`). DrawPrimitive/DrawIndexedPrimitive/SetStreamSource: `0x5e67b0` (single submission point). CreatePixelShader: `0x5e5a50`.
