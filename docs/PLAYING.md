# Playing joan-native

`joan-native --game-dir <original install>` (the launcher passes `--settings <settings.ini>` too). Without the
original data (`scenario/scenario_01.scr`) the binary runs the procedural demo scene instead.

## Flow

`src/engine/session_flow.{hpp,cpp}` (`GameFlow`, no rendering) drives `src/engine/session.{hpp,cpp}` (`Session`:
GameData + CampaignRunner → Mission = World + AiSystem + script VM + MissionUi); the client loop is
`src/engine/session_client.cpp`.

| State | Screen | Leaves on |
|---|---|---|
| (startup) | `movie/enlight.wmv`, `movie/intro.wmv` — Esc / click skips one movie | end of movies |
| Menu | MENU1024 main menu (MUSIC type M): New Campaign → difficulty (Easy/Normal/Hard/Very Hard), Load a Campaign (native saves + original `Save/*.SAV`), Options, Credits, Quit | NewGame / Load |
| Loading | loading screen of the mission (UiBridge) — one presented frame, then the mission starts | mission ready |
| Playing | world + HUD. Scripts open the mission brief themselves (missions 2–8: `show_scenario_brief`; mission 1 has it commented out, as in the original). Cut scenes (letterbox, dialog boxes) come from the mission scripts; Esc skips a running cut scene | finish 1 / 2 |
| Won | mission won screen + game-end menu → CONTINUE starts the next campaign mission with the carried profile (S1L1, S3L1, S2L1, S4L1, S5L1, S9L1, S7L1, S6L1) | CONTINUE / quit |
| Lost | game-over box: RETRY (restart mission), LOAD, QUIT TO MAIN MENU | |
| Ending | after mission 8: credits, then the main menu | |

The world simulation runs at the original fixed step of 33 ms (`World::kTickMs`); rendering interpolates between the
last two snapshots. It is paused while any menu, the mission brief, the message log, the quick-load prompt or an end
screen is open.

## Controls (defaults = STD.SET DEFAULTK, remappable in Options → Input → Remap Keys, stored in options.ini)

| Category | Keyboard + mouse |
|---|---|
| Movement | W forward, S back, A / D strafe, Shift walk, mouse X turns Joan and the camera |
| Combat | LMB light attack, RMB heavy attack, C energy attack, Q defend, Space jump, R interact, Z range mode, F3 / F4 HP / EP item |
| Command | F2 Action/RTS toggle (from mission 5, as the original), `` ` `` switch character, 1–7 select commanders |
| Menus | Esc in-game menu, F1 character details, F7 save, F8 load, F9 options, F12 mission brief, M message log |
| On screen | Ctrl close attention message, F11 name/HP bars, Tab zoom minimap, O objectives |
| Camera | wheel / NUMPAD+ NUMPAD- zoom, `,` `.` rotate (RTS) |
| Misc | F5 quick save, F6 quick load (prompt), F10 capture screen |

RTS mode: LMB click / drag-box selects, RMB on the ground moves (Ctrl = attack move), RMB on an enemy attacks, screen
edges scroll, middle-drag / `,` `.` rotate, wheel zooms.

Gamepad (SDL positional buttons, identical for Xbox and PlayStation layouts; table in `src/engine/game_input.hpp`):
left stick move (half tilt walks), right stick camera, South (A/Cross) jump, East defend, West heavy, North light,
RT light / LT heavy attack, LB / RB HP / EP item, Start in-game menu, Back RTS toggle. Dead zone and sensitivity come
from settings.ini `[controller] deadzone` / `sensitivity`.

Mouse yaw keeps the sub-degree remainder between ticks (no lost counts on high polling rates); `--camera-fix` /
`[controller] camera_fix=true` applies the community Camera Fix Patch turn rate (5x). docs/CAMERA_AND_WIDESCREEN.md.

## Saves

Native saves: `$XDG_DATA_HOME/joan-native/saves/*.jsav` (versioned CRC sections, 3 rotated backups,
docs/SAVES_AND_CAMPAIGN.md) plus a `META` section (description, mission, time). F5 writes `QuickSave.jsav`; F6 asks
"Load the last Quick Save game now?". Original `Save/*.SAV` files appear in the Load list and are imported read-only.

## Settings applied at startup

settings.ini (docs/LAUNCHER.md): `[renderer] backend/preset`, `[display] width/height/aspect/mode`,
`[controller] deadzone/sensitivity/camera_fix`, `[audio] master/music/sound/environment` (0–100, override the in-game
volumes), `[paths] game_dir`. Explicit CLI flags win. In-game options (shadows, volumes, input method, bindings) live in
`$XDG_CONFIG_HOME/joan-native/options.ini`.

## CLI shortcuts (tests, debugging)

```
--new-game [--mission N] [--difficulty D] [--seed S]   start a campaign mission directly
--load FILE                                            native .jsav or original .SAV
--scenario S1L1                                        the campaign mission of that map
--no-movies  --rts-unlocked  --camera-fix
--free-camera  --camera X,Y,Z[,H[,P]]                  fly camera (original units / degrees)
--headless --frames N                                  no window (NullRenderer): run N frames, print state + hash
--screenshot FILE.png --frames N                       hidden window, capture frame N
--autoplay-script FILE                                 timed synthetic input, see below
```

### Autoplay scripts (`src/engine/session_autoplay.hpp`)

One event per line `<frame> <verb> [args]`, `#` comments. With `--autoplay-script` the window is hidden and every
frame advances exactly one 33 ms tick, so scripts are deterministic with `--seed`.

```
0    log start
30   click 512 402            # pointer + left click in 1024x768 canvas units (menus)
100  cmd NewGame 1            # inject a UiCommand (NewGame, Load, Resume, QuickLoad, ContinueCampaign, ...)
120  key W down               # SDL scancode names
300  key W up
310  tap F5                   # down now, up 2 frames later
320  clickbtn left 3          # mouse button held 3 frames
330  mouse 40 0               # relative motion this frame
340  win                      # test hook: finish the WIN objectives (normal win delay follows)
350  shot work/run/combat.png # capture this frame
900  quit
```
Without `quit` the run ends two frames after the last event.

The acceptance play-through `work/PlayableGame/run/play.txt` (`play.sh gl|vulkan`, seed 1, `--rts-unlocked`) clicks
New Campaign → Normal, shows the loading screen and the opening cut scene, skips it, runs to the English camp and fights,
enters RTS mode (box select + move order), quick saves (F5), opens the Save menu (F7), quick loads (F6 + Return), forces
the win, clicks Next Mission / CONTINUE and starts mission 2 (S3L1, two map sections), whose script opens the mission
brief. GL and Vulkan end with the same mission hash.

## Tests

- `engine.flow_session` (headless): NEW CAMPAIGN → mission 1 runs 3000 ticks under keyboard/mouse input through
  `GameInput` → F5 → more play → F6 + OK: tick and `Mission::hash()` equal to the saved ones → Esc pauses → forced win →
  CONTINUE → mission 2 (S3L1) → QUIT TO MAIN.
- `engine.gl_session_hud` / `engine.vk_session_hud` ("Screenshot tests"): mission 1 after the cut scene at 1280x720
  (16:9) and 2520x1080 (21:9) vs `s04_04_mission1_hud.png`, masked SSIM per region sampled where the widescreen rules
  put it (3D view = centred 4:3 crop, HUD corners at the screen edges). Measured 0.52–0.60 in both backends;
  thresholds world 0.45, player panel 0.45, party 0.48, minimap 0.45. Lighting, grass, idle animation phase and the
  tutorial message in front differ from the Wine capture, so the thresholds guard framing and HUD placement.
- Module tests: `engine.game_input`, `engine.game_camera`, `engine.game_audio`, `engine.game_ui_bridge`
  (+ `gl_/vk_ui_bridge`), `engine.world_view_session`.

## Known gaps

- Startup movies are not played under `--autoplay-script` / `--screenshot` (hidden window); `--play-movie` captures them.
- RTS: no selection rings or drag box drawn; RMB on items/siege/gates, groups (Ctrl/Alt+digit) not ported.
- Save description entry: the Save screen gets no text input yet (Save New uses an empty description).
- F1 character details, F10 capture screen: not implemented.
- Joan always wears `joan_armorC` (armour-level model mapping unknown); projectiles are not drawn.
