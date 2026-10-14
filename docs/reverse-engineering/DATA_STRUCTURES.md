# Data structures and globals (joan.exe)

Structures are byte-packed (`#pragma pack(1)`), so offsets are often unaligned. Offsets are bytes. Evidence = function that reads/writes the field. Ghidra shows `this` as `in_ECX`; the global object address comes from the `mov ecx, imm32` before the call (disassembly).

## Global singletons (CONFIRMED from `WinMain` 0x420ea0 disassembly unless noted)

| address | object | evidence |
|---|---|---|
| `0xa50088` | `Sys` | `Sys_init`, `Sys_deinit`, main loop fields |
| `0xa52420` | D3D device wrapper (`Vga`/`sysen`) | `D3D_create_shadow_depth`, `D3D_create_render_texture`; fields used in `D3DDevice_create` |
| `0xa52424` / `0xa52428` / `0xa5242c` / `0xa52430` | `IDirect3D8*`, `IDirect3DDevice8*`, back-buffer surface, depth surface (= wrapper `+4/+8/+0xc/+0x10`) | vtable call patterns |
| `0xa57fb8` | `Config` (0xB05 bytes, file image of `CONFIG.DAT`) | `Config_load`, `Config_read` |
| `0xa58f90` | `Game` | `Game_main` |
| `0xa485a8` | CPU-speed / usage object (`cpuusage.cpp`); `+0x44` 64-bit cycles/s | `CpuSpeed_measure`, `Config_default_display` divides by 1e6 |
| `0xa47d78` | error handler (`Error_run` arg; `+4`/`+8` pre/post callbacks) | `Error_run 0x586bc0`, `0x4171b0` |
| `0xa45fa8`, `0xa50490` | objects initialised by `0x5904e0` (seeded with CRT `time(0)`; HYPOTHESIS: random generators) | WinMain |
| `0xa48ccc` | `View3D*` main 3D view | `World_draw` → `View3D_update_matrices` |
| `0xa45f3e` | `World*` (`+0x6e` scene, `+0x72` count, `+0x7a` array) | `World_draw` (HIGH) |
| `0xa50090` | `HWND` main window (`Sys+0x08`) | `CreateDevice`, `MessageBoxA` |
| `0xa4885c` / `0xa48860` | screen width / height (1024/768) | letterbox + edge scroll math |
| `0xa48880..0xa4888c` | 3D viewport rect x1,y1,x2,y2 | `View3D_set_viewport` call in `Vga_init_d3d` |
| `0xa516c4` / `0xa516c8` | mouse x / y | edge scroll, cursor init |
| `0xa516dc/e0/e4` → `0xa516e8/ec/f0` | mouse dx/dy/wheel accumulators, latched and cleared per drawn frame | `Sys_main_loop` |
| `0xa516f4` / `0xa516f8` | mouse look deltas | `Camera_mouse_aim` |
| `0xa69b28` | available texture memory at device creation | `D3DDevice_create` |
| `0x9fb2b8` | profiler section names, 99 × 0x32 bytes | strings |

## Sys (`0xa50088`) — offsets CONFIRMED (`Sys_main_loop 0x499960`, `Sys_init`, `Sys_disp_frame`); meanings of the flag bytes +0x0c/+0x0d/+0x11/+0x151 are HYPOTHESIS, the timing fields HIGH

| off | type | meaning |
|---|---|---|
| +0x08 | HWND | main window (`0xa50090`) |
| +0x0c | u8 | active (draw/process only when set) |
| +0x0d | u8 | paused/minimised (skip when set) |
| +0x10 | u8 | quit flag (WM_QUIT) |
| +0x11 | u8 | force redraw |
| +0x18 | u8 | slow-timer mode (500 ms periods); also selects `D3DSWAPEFFECT_FLIP` (`0xa500a0`) — meaning UNKNOWN |
| +0x22 | u32 | logic tick counter |
| +0x26 | u32 | drawn frame counter |
| +0x2a, +0x32 | u32 | time accumulators (real ms, scaled) |
| +0x2e | u32 | elapsed ms this frame, clamped to 1000 (`0xa500b6`) |
| +0x36 | u32 | `ftol(speed)` |
| +0x3a | u8 | current screen mode (9 = in-game world, 2 = 3D view only, others = menus) |
| +0x3c | u8 | requested screen mode |
| +0x40 | float | speed factor = elapsed/1000·30, max 5.0 (init 1.0) |
| +0x44 | float | game speed (process skipped when 0; init 1.0) |
| +0x151 | u8 | suspend display/process |

## D3D wrapper (`0xa52420`) — CONFIRMED from `D3DDevice_create 0x5f6c00`

| off | meaning |
|---|---|
| +0x00 | init stage (0 none, 1 D3D created, 2 ready, 3 device created) |
| +0x04 | `IDirect3D8*` |
| +0x08 | `IDirect3DDevice8*` |
| +0x0c | back buffer `IDirect3DSurface8*` |
| +0x10 | depth-stencil surface |
| +0x14 | `D3DDEVTYPE` (1 = HAL) |
| +0x18 | vertex processing: 0 software, 1 mixed, 2 hardware |
| +0x1c..+0x4f | `D3DPRESENT_PARAMETERS` |
| +0x50.. | `D3DCAPS8` |
| +0x128 | present counter mod back-buffer count |
| +0x12c, +0x2db | ready flags set by `D3DDevice_init` (`0xa526fb` gates `Vga_flip`) |
| +0x1fb, +0x1ff | dynamic VB/IB sizes (0x218 / 0x208 by VP mode; HYPOTHESIS) |
| +0x203 | vertex shaders usable |
| +0x207 | dot3 bump path usable |
| +0x20b | hardware skinning |
| +0x307 | depth format; +0x30b stencil bits |
| +0x313 | usable user clip planes |

## Config (`0xa57fb8`, 0xB05 bytes = `CONFIG.DAT`) — defaults from `Config_set_defaults` (HIGH)

| off | default | meaning |
|---|---|---|
| +0x05..+0x10 | 1, 0x2d, 1, 2, …, 0x14 | game options group 1 (UNKNOWN) |
| +0x17 | "Player" | player name |
| +0x33..+0x4e | bytes | gameplay/audio options (UNKNOWN individually) |
| +0x6b | 3 | (UNKNOWN) |
| +0x6f | float 0.0 | mouse sensitivity (`(slider-10)/10`, options menu `0x745b10`) |
| +0x73 | 0 | invert mouse |
| +0x74 | 0 | second toggle in options (UNKNOWN) |
| +0x76 | float 90.0 | camera auto-turn angle limit, degrees (used by `Camera_follow_update`) |
| +0x7a | 0 | (UNKNOWN) |
| +0x7e | 0 | adapter ordinal |
| +0x82 / +0x86 | 1024 / 768 | width / height (stored; device ignores them) |
| +0x8a | 32 | colour depth |
| +0x8e | 60 | refresh rate (preferred in mode search) |
| +0x92..+0x97 | 1,2,3,5,1,1 | (UNKNOWN) |
| +0x98 | 0x2001 | (UNKNOWN) |
| +0x9c | 1..3 by CPU MHz (<1001, <1501, else) | detail level |
| +0x9d | 0/1 by CPU MHz | render-to-texture effect level, HYPOTHESIS water reflection: mode 2 needs user clip planes (`d3d+0x313`) or a pixel shader (`caps.PixelShaderVersion`, `0xa5253c`) plus a 64×64 render-target texture and depth surface; WinMain then creates a 1024×1024 depth surface; mode 1 needs `d3d+0x213` |
| +0x9e | 1 | environment-mapped bump (EMBM): needs `D3DTEXOPCAPS_BUMPENVMAP` (`caps.TextureOpCaps & 0x200000`, `0xa52500`), `MaxTextureBlendStages ≥ 2` and a 256×256 `D3DFMT_V8U8` texture; forced off on adapters named "geforce3" when `+0x9d == 2` (HIGH) |
| +0x9f | 0/1 (<700 MHz) | (UNKNOWN detail toggle) |
| +0xa0 / +0xa4 | 500..1600 / 30..80 by MHz | view distance / object density (HYPOTHESIS) |
| +0xa8 / +0xac / +0xb0 | −0.2, 1.1, 1.0 | brightness/contrast/gamma (HYPOTHESIS) |
| +0xb4 | 0 | (UNKNOWN) |
| +0xb5..+0xb04 | 0xA50 bytes | key bindings / profile block (preserved across load; HYPOTHESIS) |

## CameraState (0x28 bytes) — CONFIRMED layout from `View3D_update_matrices`, `CameraState_init`

| off | type | meaning |
|---|---|---|
| +0x00 | u32 | projection: 0 perspective, 1 orthographic |
| +0x04 | vec3 | position |
| +0x10 | float | yaw (rad, wrapped ±π) |
| +0x14 | float | pitch (rad) |
| +0x18 | float | roll (rad) |
| +0x1c | float | 100.0 default; adjusted by collision (HYPOTHESIS: follow distance) |
| +0x20 | float | FOV parameter k, vertical FOV = atan(k); default 1.0 |
| +0x24 | float | ortho scale; default 1.0 |

## CamMan (camera manager, 200 bytes; array at `world_s+0x04`) — CONFIRMED from `CamMan_update`

| off | meaning |
|---|---|
| +0x00 | start `CameraState` |
| +0x28 | target `CameraState` |
| +0x50 | current (output) `CameraState` |
| +0x78 | easing: 1 instant, 2 linear, 3 ease in/out, 4 callback |
| +0x7c | duration ms (or easing callback) |
| +0x80 / +0x84 | accel / decel fractions |
| +0x88 | interpolation: 1 built-in, 2 callback `+0x8c` |
| +0x98 | progress t (1.0 = done) |
| +0x9c | start time (ms) |
| +0xa4 | skip collision adjust |
| +0xc0 | linked list head (`Mem_add(0x11)`), +0xc4 count |

## View3D — CONFIRMED from `View3D_init 0x613730`, `View3D_update_matrices 0x6141a0`

| off | meaning |
|---|---|
| +0x00 | pointer to world-matrix holder (`0x60eae0` object) |
| +0x08 / +0x0c | render target / depth pointers passed to `View3D_set_viewport` |
| +0x10..+0x1c | viewport x1,y1,x2,y2; +0x20/+0x24 width/height |
| +0x44 | zn = 1.0 |
| +0x48 | active zf |
| +0x4c / +0x50 | zf perspective 8193.0 / orthographic 16385.0 |
| +0x54 | sky/atmosphere object (`0x5bfd80`, 0x50 bytes) |
| +0x58..+0x74 | fog/scatter params (`+0x64 = 10000.0`, `+0x68 = 0.005`; HYPOTHESIS) |
| +0x78 | previous `CameraState` |
| +0xa0 | current `CameraState` |
| +0xea / +0xeb | lock / dirty flags |
| +0xee | view matrix; +0x12e projection; +0x16e view·proj; +0x1ae, +0x1ee, +0x22e, +0x26e copies used for screen mapping |
| +0x2ae | `D3DVIEWPORT8` |
| +0x326 | 6 frustum planes; +0x39e user planes, count +0x3fe |

## DynArray / DynArrayB (ilib) — HIGH

`+0x0c` size (record count), `+0x10` element size, `+0x19` body pointer; `DynArrayB_get_ptr(recno)` is 1-based and returns `body[recno-1]` (pointer array). Used for `unit_array`, `item_array`, etc.

## Unit — partial (HYPOTHESIS)

Obtained via `UnitArray_get 0x41b3c0(recno)`. Observed fields: `+0x00` short recno, `+0x22` int (`< 3` selects `0x43b890` vs `0x522850` pre-render path), `+0x2db` short, `+0x2f5` / `+0x31d` model/actor pointers (non-zero = has 3D actor). Full layout is UNKNOWN; gameplay agents should extend this table.
