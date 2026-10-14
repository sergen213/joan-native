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

- `Camera_mouse_aim 0x42bcd0` (CONFIRMED arithmetic): range-mode camera 1. In mode `this+0x10 == 3`, yaw (`state+0x10`) −= `dx`·k and pitch (`+0x14`) += `dy`·k·(invert ? −1 : 1) with k = `((s+1)·0.7+0.2)·0.08` degrees per mouse count (s = sensitivity `ctrl+0xe1`, invert `0xa5802b`); other modes use keys × 1°/2°/3° per frame. The update is accepted only if |pitch − base pitch `ctrl+0xb5`| < AIM_HA (`0xa33754`, 60°) and, outside mode 3, |yaw − base yaw `ctrl+0xb1`| < AIM_HA; then the unit is turned to the camera yaw.
- `Ctrl_range_enter 0x42b3c0` / `FUN_0042cb20` (HIGH): base yaw = unit facing, base pitch = 0; camera 1 = unit position + (0, 0, 70) minus 10 × an actor-matrix axis (HYPOTHESIS: the unit's right → left shoulder), distance 0, pitch 0.
- `Ctrl_mouse_yaw 0x431790` (CONFIRMED): camera 0 yaw −= the integer turn d the unit made; pitch += `ftol(dy·k)`° (k = t·0.2, t·0.4 riding), clamped to [−20°, +35°] (`−0.34906584`, `0.61086524`); a lowering step that would put the eye below the terrain (`Plasma_sample_tri`) is undone.
- Zoom `0x438720` (CONFIRMED): current camera distance (`+0x1c`) ∓ frame_ms·0.6 for actions 48/49, clamped to [80, 350], stored in `0xa0bbf0` (foot) and `0xa0bbf4` (mounted). Initial values (.data, CONFIRMED): distance 187.5 / 187.5, pitch `0xa0bbe8` = 0.19198622 rad (11°) foot / `0xa0bbec` = 15° mounted.
- `FUN_00563a20` (camera on unit switch, CONFIRMED): camera 0 pivot = unit position + 50 z, distance = `0xa0bbf0`/`0xa0bbf4`; the pitch is reset to 11°/15° only when the input scheme is not 3 (keyboard+mouse keeps its pitch).
- `Camera_follow_update 0x43f000` is the **horse** camera (called from the riding branch of the dispatcher): with horse mode B (`0xa5802c == 1`) yaw = horse facing, otherwise yaw follows the mouse limited to ±`0xa5802e` (default 90°) around the horse facing; same pitch rule as `0x431790`.
- `Camera_rts_edge_scroll 0x445990` (CONFIRMED structure): RTS camera — pivot moved by normalized view axes × 5.0 × frame_ms / 10 per key bit (`DAT_00a51804`), mouse in the 10 px screen border scrolls toward the cursor (angle offsets 55°/90°/125° per edge, speed from mouse motion × 0.3); pivot z never below the ground and eased toward it at frame_ms/1000; eye kept ≥ ground + 40 (ground = max(terrain, building, ladder-object) under a ±1000 z query box). The new pose is applied only if `Rts_camera_view_valid 0x453930` (four corner rays hit terrain inside the map).
- `World_draw` uses camera index from `CameraArray_current_index`. Indices (HIGH): 0 follow, 1 range aim, 2 camera tracks, 4 RTS, 5 cut scene.

## Camera tracks (CONFIRMED)

`Resource/cutscene.cm` (`cut_scene_move`, `cut_scene_play`, 15 keys), `Resource/rts.cm` (`rts_init`, `rts_deinit`, `rts_aim`, `rts_play`, 5 keys) and `map/scenarioN/<stem>.cm` (only `S2L1.cm` has a track: `joan_cut_scene_1`, 11 keys) hold `CreateCameraAnimatedTrack` blocks: per key `frame_time` (ms), `x y z` (pivot), `h_dir` (yaw °), `v_dir` (pitch °), `rot_a`, `cam_dist`. `.trk` files are AI path tracks (`CreateTrack`), not camera data.
- Play `FUN_00428850(name, start)`: end = start + last_frame_time / speed (`+0x46`, set by `start_cut_scene_camera`). Update `FUN_00427d90` on camera 2: t = (now − start)·speed; key interval search; pivot = cubic Hermite (`0x4279b0`) with Catmull-Rom tangents (`FUN_00429a60`, tension 0, end keys clamped; built by `FUN_00429d00`), angles and distance linear (`0x427bb0`). Pivot z: raised to the ground, else eased toward it by frame_ms/1000.
- Cut scenes (`FUN_006cb9e0` / `FUN_006cad50` start, `FUN_006cc6a0` end): MissionUi `cs.cam` = pivot x, y, z, distance, yaw, pitch (deg). Without `move` camera 5 is set instantly. With `move` "cut_scene_play" is rebuilt from the "cut_scene_move" template between the current camera A and target B: keys 0–4 at A's x/y (template z added, pitch/dist ≥ template), 5–9 lerp x/y A→B with template pitch/dist, 10.. at B turning yaw A→B; first key = A's pitch/dist, last = B's. The end with `move_back` builds the same from camera 5 back to the saved camera.
- `Rts_init`: "rts_play" = "rts_init" keys around the current camera (yaw kept), distances ≤ the RTS distance `ctrl+0xed` (700 at init), which is first lowered in 200 steps until the view is valid; key 0 keeps the current pitch/distance. Rotate `0x447740`: yaw ± 0.15°/ms (action 50/MMB-right → −, 51/MMB-left → +), pitch π/4. Zoom `0x447960`: ∓ 2/ms, clamped to [`0xa0bbf0` (the follow distance), 2500].

## Native GameCamera (src/engine/game_camera*.cpp)

- One `CamState` per 33 ms tick (pivot, yaw, pitch-below-horizon, distance in original units); `pose(alpha)` lerps eye/target between the last two ticks, mode switches outside tracks cut instantly. Priority: camera track (MissionUi camera script, cut_scene_play, RTS enter/exit) > cut-scene camera > RTS > range aim > follow.
- Follow: pivot = hero + 50, distance 187.5 (zoom 80–350), yaw = `PlayerController::camera_yaw_out` (the controller turns camera 0 with the hero; the camera writes the yaw back after each tick), pitch from mouse dy in float (no `ftol` truncation, `CAMERA_AND_WIDESCREEN.md` §4 rule 10). Start pitch 0°: HIGH — fitting `work/reference/shots/s04_04_mission1_hud.png` (feet at y≈630, horizon at the screen centre) gives distance ≈188 at pitch 0, while 11° would put the feet at y≈615 and the horizon at y≈204. `InputFrame::camera_pitch_deg` carries the view elevation (positive up, the controller's convention) = −pitch.
- Terrain clearance: HYPOTHESIS stand-in for `CamMan_collision_adjust` — the pivot→eye ray (+5) is tested against the terrain only, the eye pulled in (≥ half the distance), then lifted to ground + 4.
- Cut-scene camera is used while `cs.active` and the script wrote `cs.cam` (a no_camera start leaves the follow camera). Limitation: a camera cut scene with exactly the previous `cs.cam` values reads as no_camera. `camera_script_done()` is called when a script track ends or names an unknown track.
- RTS: enter/exit tracks as above (exit: HYPOTHESIS lerp to the follow pose with the "rts_deinit" pitch/distance keys); keys 4/5/6/7 scroll (HYPOTHESIS for the `DAT_00a51804` bits), edge scroll at the key speed (HYPOTHESIS), validity with a 4:3 corner frustum. Mouse: click / box select (allied, `rts_selectable`, Shift adds, cap 100), RMB hostile unit = `rts_attack_unit`, else terrain = `rts_move` (Ctrl = attack-move), commander keys 25–32 (second press centres the camera). Not ported: item / siege / join / building right-click types, portrait clicks, Ctrl/Alt groups, `ViewCtrl_switch_character` on the second commander press, object collision of camera rays.
- Free camera (`--free-camera`, `--camera X,Y,Z,H,P`): original units, heading = unit yaw convention, pitch positive up; actions 4–7 move, 8/9 and 50/51 turn, 48/49 pitch, Shift fast.
- Reset / hero switch / RTS exit / script `turn_camera` put camera 0 behind the hero's heading goal (`Unit::yaw_goal`): a freshly spawned unit's body turns from 0° to its `.uni` heading over the first ticks, so `yaw_deg` at mission start would leave the camera side-on (mission 1: 0° vs 115°).

## Screen ↔ world helpers (HIGH)

`View3D_project 0x6153c0` (world → pixel, reject outside viewport or z ≥ 1), `View3D_unproject 0x6154d0` (pixel → ray, uses `proj._11/_22`, ortho branch when `proj._34 == 0`), `View3D_sphere_visible 0x6159f0`, `View3D_point_visible 0x615840`.

## Community FOV fix mapping (CONFIRMED)

Both AlphaYellow hooks (`work/third_party_ref/AlphaYellowWidescreenFixes/source/fixes/WarsAndWarriorsJoanOfArcFOVFix/dllmain.cpp`, MIT) land inside `D3DXMatrixPerspectiveFovLH 0x7674a4`: pattern `D9 45 0C 8B 45 08 D8 75 FC D9 45 10 D8 F9 D9 18` at `0x7674d1` (replaces `cos(fovy/2)` when equal to `0.9238795`, divided by `FOVFactor`) and `FC D9 45 10 D8 F9`+1 at `0x7674da` (replaces aspect `1.3333334` with `W/H`). Only gameplay caller is `View3D_update_matrices 0x6141a0`. Because aspect is already `W/H`, a native renderer gets Hor+ widescreen by keeping vertical FOV 45° and using the real viewport size.
