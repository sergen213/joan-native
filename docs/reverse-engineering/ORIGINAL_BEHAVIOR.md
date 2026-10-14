# Original behavior (dynamic reference capture)

Observed behavior of the ORIGINAL GOG build (joan.exe 2004-01-19) under Wine. Reference only; Wine is
never a runtime dependency of the port. Decompiler output cited here is not verified source.

## Environment and reproduction

| Item | Value |
|---|---|
| Wine | wine-11.19 (WoW64), isolated prefix `work/wine/prefix` |
| Game copy | `work/wine/drive_c/joa` (`cp -a --reflink=auto` of the install; linked as `C:\joa`). The original install is never written: `CONFIG.DAT`/`Save/` mtimes stay 20:43 (pre-capture) after all runs. |
| X server | private `Xvfb :91` 1920x1080x24, GLX = Mesa 26.2.4 llvmpipe (LLVM 23.1.1); d3d8 via wined3d/OpenGL. wined3d reports the adapter as "NVIDIA GeForce GTX 470" (its default for unknown vendor). |
| Input | xdotool (XTest) |
| Capture | ImageMagick `import -window root`, ffmpeg x11grab |

```
python3 tools/reference/capture.py launcher vsetting devicelog session resolutions fileorder nomode combat
python3 tools/reference/file_order.py work/reference/logs/s06_fileorder.log
```
`capture.py setup` (run implicitly) is idempotent: X server + RandR modes, copy, prefix, registry keys.
Every scenario restores `CONFIG.DAT` and `Save/` in the copy from the original first, and writes
`work/reference/logs/<scenario>.steps.tsv` (timestamp, action, artifact).

Harness requirements found (all CONFIRMED by failed/successful runs):
- **RandR modes**: Xvfb exposes only its screen size; joan.exe then exits (see "Display mode"). The
  harness adds 640x480, 800x600, 1024x768, 1280x720, 1280x1024 (`cvt` 60 Hz) to the Xvfb output.
- **Xvfb physical size**: Xvfb reports 0 mm; after `xrandr -s` Wine's next mode switch died with X error
  `BadValue` on `RRSetScreenSize` (first 640x480 run). Fixed by `xrandr --fbmm 508x286` after each reset.
- **Focus**: with no window manager the game window never gets focus; DirectInput (foreground) then ignores
  the mouse while the keyboard still works. The harness calls `xdotool windowfocus`.
- **Mouse is relative-only**: the menu cursor is driven by DirectInput deltas, not the X pointer position.
  The harness pushes it into the top-left corner (it clamps) and moves by (x, y); measured 1:1 at default
  sensitivity.
- **Key taps**: ~12 ms xdotool taps were sometimes missed (F1/F2/F12); keys are held 0.25 s.
- `winemenubuilder` disabled (`WINEDLLOVERRIDES`): the first `wineboot` regenerated wine-extension
  `.desktop`/mime cache files in `~/.local/share` (all pre-existing; regenerated content).

## Startup sequence

1. `JoanofArc.exe` → `WinExec("JOAN.EXE", SW_SHOWNORMAL)`, or `WinExec(<cmdline>)` if a command line is
   given, then exits. CONFIRMED (`s01_launcher.log`: `CreateProcessInternalW ... cmdline L"JOAN.EXE"`
   187 ms after launcher start) + decompile `FUN_00408540` / string `JOAN_EXE` @0x437e9c (HIGH CONFIDENCE).
   No registry, CD or version check observed.
2. `joan.exe` loads DLLs in import order (`s06_file_order.tsv`): v3d_87.dll, OpenAL32.dll, then
   `V3D_SSE.DLL` (CPU-specific v3d variant; a `V3D_OLD.DLL` lookup misses), opens `CONFIG.DAT`, scans
   `MUSIC/`, `INT8X6/`, `TEXTURE/*` directories, then `RESOURCE/PAL_STD.RES`, cursors, `A_WAVE*.RES`.
3. Direct3D 8 init (see below), window "Wars & Warriors: Joan of Arc", fullscreen.
4. `MOVIE/ENLIGHT.WMV` (t≈1.4 s after start of file activity) then `MOVIE/INTRO.WMV` (played via
   wmvcore/winegstreamer under Wine). Escape skips **the current** movie (one press per movie).
5. Main menu, music `MUSIC/JOA_In_your_name.wma`.

`CONFIG.DAT` is rewritten at startup (mtime changes) with identical bytes in the default flow. CONFIRMED.

## Direct3D 8 device (CONFIRMED, `s03_devicelog.log`, `s05_*.log`)

| Field | Observed |
|---|---|
| `Direct3DCreate8` | SDK 0x78 (DX 8.1) |
| Mode probing | `GetAdapterModeCount` + `EnumAdapterModes` over **every** mode, `GetDeviceCaps`, then `EnumAdapterModes(idx of 1024x768 X8R8G8B8)` |
| Checks | `CheckDeviceFormat(X8R8G8B8, RENDERTARGET)`, `CheckDeviceType(HAL, X8R8G8B8, X8R8G8B8, windowed=0)`, `CheckDeviceFormat(D24S8, DEPTHSTENCIL)`, `CheckDepthStencilMatch` x2 |
| `CreateDevice` | adapter 0, `D3DDEVTYPE_HAL`, flags `0x40` = `D3DCREATE_SOFTWARE_VERTEXPROCESSING` (under llvmpipe caps; may differ on real HW — UNKNOWN), present params at static VA `0x00A5243C`, device pointer stored at `0x00A52428` |
| Swapchain | fullscreen, 1024x768 @60 Hz, back buffer `X8R8G8B8` x1, auto depth-stencil `D24S8`, no multisampling |
| Swap effect / presentation interval | not visible in Wine traces — UNKNOWN (read `0x00A5243C`+0x14/+0x30 in the decompile) |

HIGH CONFIDENCE: `0x00A5243C`/`0x00A52440` are BackBufferWidth/Height — the decompile calls
`View3D_set_viewport(..., 0, 0, DAT_00a5243c - 1, DAT_00a52440 - 1)`; `DAT_00a52444` (BackBufferFormat)
is passed as the format to `D3DXCreateTexture`.

### Display mode / resolution (CONFIRMED)

- `VSetting.exe` only lists 1024x768 modes: its mode loops (`FUN_00404ef0`, `FUN_00404260`) accept
  `Width == 0x400 && Height == 0x300` only; the dropdown shows `1024 x 768 x 32 60Hz` and `... x 16 60Hz`.
- VSetting writes `CONFIG.DAT` u32 LE: width @0x82, height @0x86, bpp @0x8a, refresh @0x8e (globals
  `DAT_00411f0a..f16` minus buffer base `DAT_00411e88`); original values 1024/768/32/60. HIGH CONFIDENCE.
- joan.exe **ignores** these fields: with CONFIG.DAT set to 640x480, 800x600, 1280x720, 1280x1024,
  1920x1080 (all present in the X mode list) and 1024x768x16, it always selected the 1024x768 X8R8G8B8 mode
  and Wine switched the X screen to 1024x768 (`s05_cfg*`, `s07_1024x768x16`). Screenshots all 1024x768.
- With no 1024x768 mode available (`s07_no1024_mode`) joan.exe calls `Direct3DCreate8`, enumerates the
  modes, `Release`s and exits silently within ~0.1 s — no message box, no window content.
- Requested capture at 640x480/800x600/1280x720/1920x1080 therefore impossible without patching the
  executable: the original renders 1024x768 only (manual: "Monitor capable of 1024x768 resolution").

## VSetting.exe

Requires `HKLM\Software\Enlight Software\Joan of Arc` with `Version` = `"1.00"` (compared against
string @0x41088c) and `Path`; otherwise it shows "You haven't installed Joan of Arc" and exits. The GOG
build does not create these keys (fresh prefix). CONFIRMED (`s02_vsetting.log`, decompile `FUN_00403580`).
Dialog "Video Settings": 3D Device, Video Mode (1024x768 x32/x16), Reflection (None/Modulate/High),
Bump Map Detail (No/Yes); buttons Default, Save, Launch Game, Cancel.

## File load order (CONFIRMED, `logs/s06_file_order.tsv`, 1870 paths, WINEDEBUG=+timestamp,+file)

Paths are relative to the game dir; times relative to the first game-dir access (tracing slows the game).

| t (s) | Phase | Files |
|---|---|---|
| 0.18–0.38 | config/UI init | `CONFIG.DAT`, `RESOURCE/PAL_STD.RES`, `I_CURSOR.RES`, `CURSOR.RES` |
| 0.8–0.96 | sounds, UI textures, fonts | `RESOURCE/A_WAVE2.RES`, `A_WAVE1.RES`, `T_BASIC.TXR`, `T_INTERF.TXR`, `INT8X6/*font*.DDS`, `FONT/*.FIN` + matching `INT8X6/*.DDS` |
| 1.02–1.27 | global game data | `RESOURCE/INTERFAC.SET`, `SCRIPT/PSCRIPT/`, `MODEL/` (259 hits), `ITEM/ITEM.RES`, `DROPITEM.RES`, `SELLITEM.RES`, `UNIT/unit.res`, `UNIT/RECRUIT.RES` |
| 1.27 | saves | `SAVE/Mission1-AutoSave.SAV`, `SAVE/Player.dat` |
| 1.38 / 18.7 | movies | `MOVIE/ENLIGHT.WMV`, `MOVIE/INTRO.WMV` |
| 20.6 | menu music | `MUSIC/JOA_In_your_name.wma` |
| 92.6 | New Campaign → mission 1 | `SCENARIO/scenario_01.SCR`, `SCRIPT/SCENARIO/S1L1.SCE`, `S1L1_des.SCE`, `S1L1_desEX.res` |
| 100.6–103.3 | map | `MAP/scenario1/S1L1w.WOR`, `S1L1w.TER`, `scenario1.res`, `S1L1.MAP`, `MAP/common.lgt`, `common.sca`, `special.lgt`, `special.sca`, `S1L1W.day`, `S1L1.PFD`, `.trk`, `.cm`, `.PLN`, `.GRI`, `.POR` |
| 103.5+ | units/items | `ANIM/*.AM2`, `ITEM/Object/Models_Items/**.SMO` + `TEXTURE/ITEM/*.DDS` (1158 ITEM, 347 TEXTURE paths) |
| ~107 | shaders | `SHADER/nvlinker.dll` |

Paths are opened with mixed case (`RESOURCE/`, `ITEM/`, `script/`, `model/`); the port needs
case-insensitive lookup on Linux. CONFIRMED.

## Menus (screenshots `s04_0[1-3]_*`)

- **Main menu**: New Campaign, Load a Campaign, Options, Credits, Quit; animated Joan portrait;
  "Copyright © 2004 Enlight Software Ltd." Buttons slide in when returning to it.
- **Options** → Video (Brightness, Contrast, Gamma, Draw Distance, Dynamic Grass On/Off, Dynamic Grass Draw
  Distance, Dynamic Grass Draw Density, Shadow "High Shadow"; Default/Back), Audio (Music, Environmental,
  Sound Volume, 3D Sound On), Input (Input Method "Keyboard & Mouse", Mouse Sensitivity, Invert Mouse,
  Riding Camera Rotation Angle, Riding Mode "Mode A"; Remap Keys/Default/Back), Game (Game Level "Normal",
  Melee Auto-Targeting On). No resolution option in-game.
- **Credits**: scrolling credits over a 3D battlefield scene; Escape returns.
- **Load a Campaign**: "LOAD A SAVED GAME", Page 1 of 1, slot list ("Mission 1 Auto Save"), thumbnail
  panel, File Information, Back.
- **New Campaign**: Easy / Normal / Hard / Very Hard / Cancel → loading screen "Mission One" / "The
  Arrival" with LOADING bar (~20 s on llvmpipe) → letterboxed in-engine cutscene with portrait subtitles
  (Jean de Metz) → Escape skips → gameplay.
- **In-game menu (Esc)**: Load Game (F8), Save Game (F7), Mission Brief (F12), Message Log (M), Restart
  Mission, Options (F9), Quit To Main Menu, Quit To Windows, Back.
- **Save menu (F7)**: slots + Save New/Back. **Load menu (F8)**: slots + Back.
- **Quick load (F6)**: modal "Load the last Quick Save game now?" OK / CANCEL.
- **Mission brief (F12)**: Background, Objectives, Townships, Commanders, Battleground, Strategic Command
  and Siege Weapon (greyed in mission 1); Mission Time / Total Time counters.
- **Message log (M)**: book UI, "Mission 1", pages of portrait + dialogue entries.
- **Character details (F1)**: name, level, Experience/Next Level (0/1000), gold 100, HP 60/60,
  EP 30/30, Strength 9, Defense 8, Dexterity 10, Leadership 15, kill/critical counters, Weapon / Range
  Weapon / Enhancers / Personal Inventory / Troop Inventory grids (Joan at mission 1 start, Normal).
- **Game over** (Joan killed): "GAME OVER — You have perished in your efforts to reach the city of
  Orléans - now all hope is lost!" with three buttons over the live scene (`s08_end`).

## Gameplay observations (mission 1, Normal)

- Third-person camera behind Joan; HUD: minimap top-right (Tab toggles a larger zoom), Joan portrait with
  HP/EP bars and level bottom-left, commander portraits (Joan, Jean de Metz) bottom-right, attention
  message panel above the bottom HUD (Ctrl closes), objective text top-left (O toggles: "Safely reach and
  enter Orléans", "Eliminate all enemies along the path").
- W/S/A/D run/back/strafe, Shift walk, mouse X turns Joan/camera, wheel zooms the camera
  (`s04_06_zoom_in/out`); LMB light attack and RMB heavy attack show sword trails; Q defend, Space jump,
  C energy attack (`video/s04_05_joan_move_attack.mp4`).
- Tutorial messages advance with progress: movement → "combo attacks ... AAAB" → "The enemy is just
  ahead" → barrels/crates contain food (LMB) → "To heal yourself, press F1 ... F3 to automatically heal".
- Map edge: "You cannot go beyond the battlefield"; Joan stops and turns back (`s04_08`).
- Running straight ahead from the start reaches a palisade/tent camp with red-armoured enemy infantry;
  a group engaged and killed Joan within ~30 s of button-mash combat → Game Over (`video/s08_combat.mp4`).
- F2 (Action/RTS toggle) produced no visible change at the start of mission 1 (Strategic Command is greyed
  in the brief); troop commands were not reachable here. UNKNOWN whether RTS is locked by the mission.
- F11 (hit point bars) showed no change with no units on screen.
- Saves: mission start writes `Save/Mission1-AutoSave.SAV` (189087 B) + `.BMP`; F5 writes
  `Mission-QuickSave.SAV` (191723–192291 B) + `.BMP`; `Player.dat` (114 B) rewritten. Thumbnails are
  512x256 24-bit BMP (393270 B). CONFIRMED (`logs/s04_session.save_dir.txt`).

## Controls (Remap Keys pages `s04_02_remap_0..6`; manual p.47)

| Category | Bindings (in-game defaults) |
|---|---|
| Movement | Move forward W, Move back S, Strafe Left A, Strafe Right D, Walk Shift |
| Control | Defend Q, Light attack LMB, Heavy attack RMB, Energy attack C, Jump Space, Interact R, Range mode toggle Z, Use HP recovery item F3, Use EP recovery item F4 |
| Command | Action/RTS toggle F2, Switch character `` ` ``, Joan 1, Jean de Metz 2, La Hire 3, Duke of Alençon 4, Bastard of Orléans 5, Sir Arthur de Richemont 6, King Charles VII 7 |
| Menus | Character Details F1, Save F7, Load F8, Options F9, Mission Brief F12, Message Log M |
| Onscreen | Close Attention Message Ctrl, Hit Point Bar F11, Zoom Minimap Tab, In-Game Objective O |
| Camera | Zoom in NUMPAD+, Zoom out NUMPAD-, Rotate CW `,`, Rotate CCW `.` (RTS) |
| Misc | Quick Save F5, Quick Load F6, Capture Screen F10 |

Manual-only (p.38–40, not exercised): RTS camera pans at screen edges, middle-drag rotates; select via
portrait click / commander key (twice = center); box-select with LMB drag; RMB move/attack; Ctrl+digit
defines a group, Alt+digit selects; Shift+commander key toggles hold/resume; Keyboard-only scheme maps
attacks to Q/W/S/E with arrows for movement. The manual's "Quick Load & Quick Save F5, F6" ordering is
contradicted by the remap screen (Save F5, Load F6), which matches observed behavior.

## Not captured / gaps

- Other resolutions: impossible with the unmodified exe (1024x768 only, above).
- RTS/troop commands, abilities beyond basic attacks, equipment upgrades, victory, mission 2+, horse riding.
- Swap effect / presentation interval (UNKNOWN). Real-GPU behavior (only llvmpipe used; frame rate low).
- Audio was not captured (no sound device in the harness).

## Corpus index (`work/reference/`, gitignored)

| Artifact | Content |
|---|---|
| `shots/s01_*` | launcher run: enlight logo, intro at 20 s/42 s, menu after Escape |
| `video/s01_startup_movies.mp4` | first 40 s of ENLIGHT.WMV/INTRO.WMV |
| `shots/s02_vsetting_*` | VSetting dialog and each dropdown (device, video mode, reflection, bump) |
| `logs/s03_devicelog.log` | first 5000 lines of +d3d8,+d3d (device creation) |
| `shots/s04_01_*` | movies, main menu |
| `shots/s04_02_*` | options, video/audio/input/game options, remap pages 0–6 |
| `shots/s04_03_*` | credits, load-a-campaign |
| `shots/s04_04_*` | difficulty, loading, mission 1 cutscene, HUD, tutorial message |
| `video/s04_05_joan_move_attack.mp4`, `shots/s04_05_after_moves` | movement/attack/defend/jump/energy/walk/strafe |
| `shots/s04_06_*` | F1 details, M log, F12 brief, O objective, Tab minimap, F11, wheel zoom, F2 attempts |
| `shots/s04_07_*` | quick save, save/load menus, in-game options, Esc menu, quick-load prompt and result |
| `video/s04_08_advance_and_fight.mp4`, `shots/s04_08_*` | map boundary message |
| `shots/s05_cfg*_main_menu`, `logs/s05_cfg*.log` | CONFIG.DAT mode sweep (all render 1024x768) |
| `shots/s06_*`, `logs/s06_fileorder.log`, `logs/s06_file_order.tsv` | +file run, load order |
| `shots/s07_*`, `logs/s07_*` | no-1024x768 exit; 16-bit config |
| `video/s08_combat.mp4`, `shots/s08_*` | run to enemy camp, combat, Game Over |
| `logs/*.steps.tsv` | per-scenario timestamped steps |
