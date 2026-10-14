# Camera system (joan.exe)

Projection math and the community FOV-fix analysis are in `docs/CAMERA_AND_WIDESCREEN.md` (ModsResearch); this file covers the camera objects, update flow and constants. Addresses are VAs; struct layouts in `DATA_STRUCTURES.md`.

## Objects

| object | where | role |
|---|---|---|
| `CameraState` (0x28 bytes, 10 dwords) | embedded | one camera pose: projection type, position, 3 angles, distance(?), FOV parameter, ortho scale |
| `CamMan` (camera manager, 200 bytes) | array at `world_s+0x04`, index `world_s+0x2f` (`CameraArray_get 0x563c80`, `CameraArray_current_index 0x563dc0`) | eases from a start state to a target state; output at `+0x50` |
| `View3D` | pointer `0xa48ccc` (main view) | owns matrices, viewport, frustum; consumes a `CameraState` at `+0xa0` |

## Per-frame flow (HIGH CONFIDENCE, CONFIRMED where stated)

1. Gameplay code picks/feeds a camera: `CamMan_start_transition 0x423ec0(from, to)` or `CamMan_set_target 0x423f40(to)` (current pose becomes the start; start time = `Misc_get_time()`). Callers (21 + 14) include `world.cpp` / `world_s.cpp` control code, interface screens (`interfac\i_buy.cpp` 3D previews) and cut-scene/camera-track code.
2. `World_draw 0x44c800` calls `CamMan_update 0x4237a0` on the current camera; when it reports a change, `View3D_set_camera_params 0x614040` copies `CamMan+0x50` (10 dwords) to `View3D+0xa0` and marks the view dirty.
3. `View3D_update_matrices 0x6141a0` (CONFIRMED):
   - world matrix from angles `+0xb0/+0xb4/+0xb8` (`0x60bd00`) then translation `+0xa4..+0xac` (`matrix_multiply_translation`, v3d); view = `matrix_inverse(world)` at `+0xee`.
   - projection at `+0x12e`: type `+0xa0 == 0` → `D3DXMatrixPerspectiveFovLH(fovy = atan(k = +0xc0), aspect = W/H (+0x20/+0x24), zn = +0x44, zf = +0x4c)`; type 1 → `D3DXMatrixOrthoLH(W·s, H·s, zn, zf = +0x50)` with `s = +0xc4`.
   - view·proj `+0x16e`, viewport matrix (W/2, H/2, MinZ/MaxZ) folded in for screen projection, 6 frustum planes `+0x326` (16 bytes each) plus `+0x3fe` extra planes at `+0x39e`.
4. `View3D_render 0x614760` sets `D3DTS_VIEW`/`D3DTS_PROJECTION` and the viewport.

## CamMan_update 0x4237a0 (CONFIRMED)

- Progress `t = +0x98` (1.0 = done, early-out). Easing mode `+0x78`: 1 instant; 2 linear over `+0x7c` ms; 3 accelerate/decelerate with fractions `+0x80`/`+0x84` over `+0x7c` ms; 4 callback `+0x7c(elapsed, &params)`.
- Yaw (`state+0x10`) of start and target wrapped to ±π before blending.
- Interpolation kind `+0x88`: 1 = built-in (position vector lerp `0x425ac0`, wrap-aware yaw `Cam_lerp_angle 0x425b90`, scalar `Cam_lerp_float 0x425c80` for `+0x14..+0x24` incl. FOV parameter); 2 = callback `+0x8c`.
- If start == target, `t = 1`. Unless `+0xa4` is set, `CamMan_collision_adjust 0x424010` runs ray tests (`0x5d4a80`) against scene geometry and pulls the camera in / lifts it (constants 4.0, 5.0, 0.005, 0.95, 0.99; modifies `state+0x1c` and position z). HYPOTHESIS for exact semantics.

## Constants (CONFIRMED)

| value | source |
|---|---|
| FOV parameter `k = 1.0` → **vertical FOV = atan(1) = 45°** | `CameraState_init 0x616d50` (`+0x20 = 1.0`); FOV fix keys on `cos(fovy/2) = 0.9238795` = 45° |
| aspect = viewport W/H = 1024/768 = 1.3333334 | `View3D_set_viewport 0x613bf0(0,0,1023,767)` via globals `0xa48880..0xa4888c`; device fixed at 1024×768 |
| near `zn = 1.0`, far `zf = 8193.0` (perspective), `16385.0` (ortho) | `View3D_init 0x613730` |
| ortho scale 1.0, `state+0x1c = 100.0` (HYPOTHESIS: follow distance) | `CameraState_init` |
| unrelated: `CD3DCamera_SetProjParams 0x5bec90(π/4, 1.0, 1.0, 1000.0)` | DX8 SDK framework camera, not the game view |

Whether any scripted/cut-scene camera sets `k ≠ 1` is UNKNOWN (needs a runtime trace of `View3D+0xc0`; `CamRes_load_tracks 0x428e90` reads camera tracks from text resources with `CreateCameraTrack*` keywords).

## Player camera control (HYPOTHESIS unless noted)

- `Camera_mouse_aim 0x42bcd0`: in mode `this+0x10 == 3`, mouse deltas (`0xa516f4/0xa516f8`) × `((s+1)·0.7+0.2)·0.08` (s from `0x4322d0`, likely the sensitivity option `config+0x6f`), sign from invert flag `config+0x73` (`0xa5802b`); otherwise input from `0x577b10/0x577b50` × 1°/2°/3° (`0.017453292`, `0.034906585`, `0.05235988` rad) selected by `param_2` 1/2/3. Wraps angles, clamps yaw around the unit facing (`this+0xb1`), then `CamMan_set_target` on camera 1 and turns the unit (`0x50abd0`).
- `Camera_follow_update 0x43f000`: third-person follow; auto-turn limited by config `+0x76` (`0xa5802e`, default 90°, slider in options menu `0x745b10`); pitch range constants 20°/35° (`0.34906584`, `0.61086524`).
- `Camera_rts_edge_scroll 0x445990`: strategic/tactical view — screen-edge scrolling (mouse `0xa516c4/c8` vs screen W/H `0xa4885c/60`) scaled by frame ms `sys+0x2e` (`0xa500b6`) / 10, angle limits 55°/90°/125° (`0.9599311`, `1.5707964`, `2.1816616`), ±1000 height clamps, 40-unit ground clearance.
- `World_draw` uses camera index from `CameraArray_current_index`; camera 1 is the mouse-aim camera (CONFIRMED in `0x42bcd0`), roles of other indices UNKNOWN.

## Screen ↔ world helpers (HIGH)

`View3D_project 0x6153c0` (world → pixel, reject outside viewport or z ≥ 1), `View3D_unproject 0x6154d0` (pixel → ray, uses `proj._11/_22`, ortho branch when `proj._34 == 0`), `View3D_sphere_visible 0x6159f0`, `View3D_point_visible 0x615840`.

## Community FOV fix mapping (CONFIRMED)

Both AlphaYellow hooks (`work/third_party_ref/AlphaYellowWidescreenFixes/source/fixes/WarsAndWarriorsJoanOfArcFOVFix/dllmain.cpp`, MIT) land inside `D3DXMatrixPerspectiveFovLH 0x7674a4`: pattern `D9 45 0C 8B 45 08 D8 75 FC D9 45 10 D8 F9 D9 18` at `0x7674d1` (replaces `cos(fovy/2)` when equal to `0.9238795`, divided by `FOVFactor`) and `FC D9 45 10 D8 F9`+1 at `0x7674da` (replaces aspect `1.3333334` with `W/H`). Only gameplay caller is `View3D_update_matrices 0x6141a0`. Because aspect is already `W/H`, a native renderer gets Hor+ widescreen by keeping vertical FOV 45° and using the real viewport size.
