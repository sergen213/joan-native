# Launcher (`joa-launcher`)

Code: `launcher/main.cpp` (Qt 6 Widgets + Concurrent), links only `joa_install`; no engine code.
Built when `JOA_BUILD_LAUNCHER=ON`. Runs on Wayland and X11 through Qt's platform plugins; Wayland app-id / desktop id `joan-native` (`packaging/desktop/joan-native.desktop`, icon `joan-native.svg`).

## Features

| Tab | Controls |
|---|---|
| Game | Play; Import Game Folder…; Install from GOG Installer…; "Allow Wine-assisted install" (opt-in); Verify Files; Repair… (defaults to the folder recorded in `.joa-install.json`, otherwise asks); Open Logs |
| Graphics | Renderer (OpenGL/Vulkan), preset (Original/Enhanced/Ultra/Custom), ray tracing reflections/shadows/GI (enabled only with Vulkan), resolution, aspect, display mode |
| Controller | Stick deadzone, stick sensitivity |
| Updates | Channel, Check for Updates, Update Game, Update Launcher, Roll Back Update, release notes (Markdown from `joa-updater check`) |

Import/verify/repair run `joa_install` on a worker thread (`QtConcurrent`) with progress in the bar and results in the log pane. Every settings change is saved immediately (atomic write).

## Executables

The launcher looks for `joan-native`, `joa-updater` (and the updater swaps `joa-launcher`) next to its own executable first, then on `PATH`.

- **Play**: `joan-native --game-dir <dir> --settings <settings.ini>`, started detached, working dir = game dir; `<dir>` is `[paths] game_dir` or `$XDG_DATA_HOME/joan-native/game/current`. stdout+stderr go to `${XDG_STATE_HOME:-~/.local/state}/joan-native/logs/game-<UTC ts>.log`.
- **Updates** (contract agreed with the updater):
  - `joa-updater check --channel stable|experimental --json` → `{"current","latest","available":bool,"notes"}`
  - `joa-updater apply --channel X --json`, `joa-updater rollback --json`
  - stderr `progress N message` lines drive the progress bar; other stderr lines go to the log; exit 0 = ok, otherwise stdout `{"error":…}` is shown.
  - Update Game = `apply`. Update Launcher = `apply`, then relaunch `$XDG_DATA_HOME/joan-native/app/current/bin/joa-launcher` (else `$APPIMAGE`, else the sibling `joa-launcher`) and quit.
- `--screenshot FILE [--tab N]`: render the window with `QWidget::grab()` after 200 ms, save PNG, exit (CTest `launcher.screenshot` uses `QT_QPA_PLATFORM=offscreen` and private XDG dirs). This mode never writes settings.
- `--play [-- ENGINE_ARGS]`: start the game exactly like Play (saved settings, log file, extra engine arguments appended), without showing the window, and wait until joan-native exits (inside an AppImage the game's files disappear when the launcher process ends). Desktop action "Play" of `joan-native.desktop`; used by `release.install_flow` (`--play -- --no-movies --frames 120`).

## settings.ini schema

`${XDG_CONFIG_HOME:-~/.config}/joan-native/settings.ini`. Written by the launcher, read by `joan-native`. Reference reader/writer: `src/install/settings.{hpp,cpp}` (`joa::install::Settings`). Plain INI: `[section]`, `key=value`, `#`/`;` comments. Missing file/keys and invalid or out-of-range values fall back to the default; unknown keys are ignored. Explicit engine CLI flags override the file.

| Section | Key | Values | Default |
|---|---|---|---|
| `renderer` | `backend` | `opengl` \| `vulkan` | `opengl` (engine Vulkan backend not available yet) |
| `renderer` | `preset` | `original` \| `enhanced` \| `ultra` \| `custom` | `enhanced` (launcher); the engine without a settings file uses `original` |
| `graphics` | `hd_packs` | comma list of pack names / absolute dirs, first wins ([HD_ASSETS.md](HD_ASSETS.md)); works with every preset | empty |
| `graphics` | `pbr`, `normal_maps`, `ibl`, `hdr`, `ssao`, `volumetric_fog`, `particle_lighting` | bool; read only when `preset=custom` (unset keys = Enhanced values) | Enhanced |
| `graphics` | `exposure` 0.05–16, `shadow_map_size` 0\|1024\|2048\|4096\|8192 (0 = original stencil volumes), `shadow_softness` 0–8, `shadow_distance` 200–8192, `ssao_radius` 1–500, `ssao_intensity` 0–4, `aa` none\|taa, `fog_density` 0–10, `upscale` none\|fsr1, `render_scale` 0.5–1, `sharpness` 0–1 | custom only; see GRAPHICS_ROADMAP.md "Remaster" | Enhanced |
| `raytracing` | `reflections`, `shadows`, `gi` | `true` \| `false` (`1`/`0` accepted); any remastered preset, Vulkan + ray-query GPU only, else logged and the raster path runs ([RAYTRACING.md](RAYTRACING.md)) | unset = preset (Ultra on, others off) |
| `raytracing` | `rays_per_pixel` 1–4, `max_roughness` 0–1, `denoise` bool, `probe_density` low\|medium\|high | quality of the above | 1, 0.6, true, medium |
| `display` | `width`, `height` | integer 0–16384; `0` = desktop resolution; if only one is 0 both become 0 | `0` |
| `display` | `aspect` | `auto` \| `4:3` \| `16:9` \| `16:10` \| `21:9` | `auto` |
| `display` | `mode` | `windowed` \| `fullscreen` \| `borderless` | `fullscreen` |
| `controller` | `deadzone` | real 0.0–0.9 | `0.15` |
| `controller` | `sensitivity` | real 0.1–5.0 (camera stick and RTS cursor speed) | `1.0` |
| `controller` | `rumble` | `true` \| `false` — pad rumble on the player's hits (engine reads it; no launcher widget yet) | `true` |
| `update` | `channel` | `stable` \| `experimental` | `stable` |
| `paths` | `game_dir` | absolute path; empty = `$XDG_DATA_HOME/joan-native/game/current` | empty |

Example:

```ini
[renderer]
backend=opengl
preset=enhanced
[raytracing]
reflections=false
shadows=false
gi=false
[display]
width=2560
height=1440
aspect=auto
mode=borderless
[controller]
deadzone=0.15
sensitivity=1
[update]
channel=stable
[paths]
game_dir=
```

## Gaps

- Controller button remapping lives in the game (Options → Input → Input Method Gamepad/Joystick → Remap Keys, stored
  in options.ini, docs/PLAYING.md "Gamepad"); the launcher only edits deadzone/sensitivity (no `rumble` checkbox yet).
- The `joa-updater` integration is untested end to end here (the updater is built separately).
- No in-launcher log viewer; Open Logs opens the folder in the desktop file manager.
