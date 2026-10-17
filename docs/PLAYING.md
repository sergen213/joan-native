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
| Command | F2 Action/RTS toggle (from mission 5, as the original), `` ` `` switch character (Joan → Jean → La Hire → Alencon, those alive, controllable and inside the minimap view; from mission 2), 1–4 switch to Joan / Jean / La Hire / Alencon, Shift+1–7 hold/resume that commander |
| Menus | Esc in-game menu, F1 character details, F7 save, F8 load, F9 options, F12 mission brief, M message log |
| On screen | Ctrl close attention message, F11 name/HP bars, Tab zoom minimap, O objectives |
| Camera | wheel / NUMPAD+ NUMPAD- zoom, `,` `.` rotate (RTS) |
| Misc | F5 quick save, F6 quick load (prompt), F10 capture screen |

Character details (F1): click an item to put it on the pointer (or hold and drag it), click a grid cell or slot to
put it down, right click puts it back / eats food / wears armour. Clicking a commander's portrait (bottom right)
shows his window; releasing an item on a portrait hands it to that commander, as in the original ("SHARING ITEMS
AMONG YOUR COMMANDERS"): a free melee / ranged / ammo slot takes it first, otherwise a free cell of his personal,
then troop inventory (stacks merge). Refused without room, for a commander under script control, and for one
outside the minimap view (his portrait is drawn faded). Releasing an item outside the inventory, right of x 800 or
below y 600 (over the minimap / HUD side), drops it on the ground beside the shown commander ("DISPOSING ITEMS");
hero armour, letters, keys and a few unique weapons cannot be dropped. Inventories are grids as in the original:
with no room a floor item stays where it lies (walking over it again picks it up once there is room), the shop
answers "no room" (BUY 9; purchases go to the personal inventory only), and a scripted gift that does not fit is
put on the ground next to its receiver. Weapons, bows and arrows picked up or given fill an empty hand / quiver
first. Food does not stack (one cell per loaf), potions and arrows do.

RTS mode (selected units stand in the original's `select*.SMO` ring, the RTSICON cursor shows the right-click
command): LMB click / drag-box selects (Shift adds, a leader brings his team), Ctrl+1..9 stores the selection as a
group, Alt+1..9 recalls it; 1–7 select a commander (press twice / double-click his portrait = centre the camera,
Shift+key = hold/resume + add; pressing Joan / Jean / La Hire / Alencon's key a second time also makes him the
player, and F2 out of RTS switches to the commander chosen last). F3 / F4 use an HP / EP item on every selected
commander. RMB: ground = move (Ctrl = attack move), enemy = attack, hostile gate/wall/keg/box =
attack item, floor item = move there, own siege weapon with light infantry selected = crew it, friendly leader =
join his team (a full team asks "General … can only command …" first, the game pauses), commander = follow him,
interactable unit near the player = talk, own barracks / siege workshop (BARR / WEAPON / IRON / WOOD cursor; the
IRON / WOOD workshops for Alencon only) = the recruit / workshop screen for the selected leader. Orders are
acknowledged by the original RTS voices. Double-click: own siege weapon =
dismiss its crew, own/neutral gate = open/close, interact box = open. Minimap: left click jumps the camera, right
click moves (Ctrl = attack move). Screen edges scroll, middle-drag / `,` `.` rotate, wheel zooms. Groups 1–9, the
commander choice and the RTS camera are saved with the game (native save section `VIEW`).

### Keyboard-only and joystick input methods

Options → Input → Input Method switches the controller (also in a running mission; in RTS it applies when leaving
RTS). Keyboard & Mouse is the default (table above). The others are the original's own controllers:

| Keyboard (K_KEY column) | |
|---|---|
| Movement | ↑ run forward, ← / → turn to camera-left / -right and run (the camera swings after), ↓ turn and run toward the camera; Left Shift + arrows strafe / step back (Shift+↑ walks), Shift alone centres the camera |
| Combat | Q light attack, W heavy attack, S energy attack, E defend (down attack on a downed enemy), R interact, Z range mode (arrows aim, Q shoots, Z leaves), F3 / F4 HP / EP item. Space is bound to Jump but never jumps in this mode (original quirk, kept) |
| Double taps (< 300 ms) | ↑↑ rush, Shift+↓↓ / Shift+←← / Shift+→→ evade back / left / right |
| Other | as Keyboard & Mouse (F2, `` ` ``, 1–4, Shift+1–7, Esc, F1, F7–F9, F12, M, Ctrl, F11, Tab, O, F5/F6/F10), NUMPAD+ / NUMPAD- zoom; no mouse turning |

Gamepad/Joystick (listed only with a pad; nothing moves without one): the left stick direction is camera-relative and
its tilt the speed (under ¼ stands, ¼–⅝ walks, beyond runs) and the jump length; North / RT light, West / LT heavy,
both at once energy attack, South jump, East defend, D-pad down interact, LB / RB items; hold L3 for range mode
(stick aims, North / RT shoots, release leaves); hold R3 to strafe with the stick and to centre the camera. Flick the
stick to full tilt twice within 300 ms: up = rush, with R3 held left / back / right = evade. The right stick only
pitches the camera (the original joystick mode has no camera stick); Start, Back and the Back layer as below.

Point & Click (listed last; port label for the original's "mouse-driven control mode" 2, which its Options never
offered — only CONFIG.DAT 0x6B = 2 reached it; such a CONFIG.DAT imports as Point & Click). The mouse is a pointer:

| Point & Click | |
|---|---|
| Left click | ground: walk/run there; enemy (unit, or an enemy gate/wall): run to it and attack until it dies or you click elsewhere — a second click on it while attacking with ≥ 40 EP spends 40 EP on a fury round of random follow-ups; item lying on the floor: walk to it (picks it up); your own gate: walk to it and open it; your own units: nothing |
| Right click | enemy: run to it, then one energy attack (light attack without 40 EP) |
| Keys (K_KEY column) | F3 / F4 HP / EP item, R item interact only (boxes, levers, gates within 200), Esc menu. Every other key is dead in this mode, as in the original (no F2 RTS, no hero switch, no zoom, no quick save) |

### Gamepad

SDL positional buttons, identical for Xbox and PlayStation layouts (tables in `src/engine/game_input.hpp`). The whole
game is playable with a pad only; mouse/keyboard keep working and the last-used device wins.

In game with Input Method Keyboard & Mouse (port extension; J_JOYPAD column of DEFAULTK, remappable; Gamepad/Joystick
method: see above): left stick move (half tilt walks), right stick camera (yaw follows
the in-game Mouse Sensitivity, pitch also Invert Mouse), South (A/Cross) jump, East defend, West heavy, North light
attack, LB / RB HP / EP item, D-pad up character details, D-pad down interact, L3 range mode, R3 centre camera.
Fixed: RT light / LT heavy attack (pull both together = the original's A+B energy attack), Start in-game menu (also
skips a cut scene), Back tapped alone = RTS toggle. **Back layer** (hold Back + button; reaches the actions the
original's pad column leaves unbound): South switch character, East close message, West energy attack, North
objective, LB quick save, RB quick load, D-pad up/down zoom, D-pad right/left rotate camera, L3 zoom minimap, R3 hit
point bars, Start mission brief, LT capture screen, RT message log. Save/Load/Options are in the Start menu.

RTS mode: the right stick moves a cursor (South = left button: click / hold + move = box select; West = right
button: move / attack order), the left stick scrolls, Back + D-pad rotates / zooms, HUD buttons and the minimap take
the cursor too.

Menus and overlays (main/difficulty/options/remap/load/save/in-game menu, mission brief, message log, quick-load
prompt, dialog choices, game over / victory / de-brief): D-pad or left stick moves a focus drawn exactly like the
original's mouse-hover state (no cursor shown), South activates (also advances dialog text), East or Start = Escape /
back, left/right adjust a focused slider or combo, LB / RB turn pages (save list, message log, remap category). The
first pad input only shows the focus.

Character details (D-pad up = F1), shop and ARMORY windows: the focus walks the attribute rows (South = the row's ADD
while points are left), the CB/WP button, every inventory grid cell and equipment slot (melee 1 / melee 2 / ranged /
ammo), the combo rows (South = ADD), LEARN, the scroll arrows and the commander portraits. South on an item picks it
up, South on a cell or slot drops it there (same World equip / unequip / bag moves as the mouse drag), South on a
portrait shows that commander and hands him a held item, North (Y / Triangle) drops a held item on the ground; West
(X / Square) uses the focused item (right click) or puts a held one back. The focused cell/item shows the window's
hover state and its tooltip below it.
LB / RB step inventory → combo list pages → inventory. Shop: South buys the focused stock item / sells the focused
own item. East = Escape closes (also while an item is held); D-pad up inside the window moves the focus instead of
closing it.

Options → Input lists Input Method "Gamepad/Joystick" only while a pad is connected (as 0x7435e0). With it selected
Remap Keys shows the joypad column (JOYPAD n names, J_KEY otherwise); selecting a row and pressing a pad button binds
it (the original's joystick capture through RemapKey_assign; Start cancels), stored as `keyN=` in options.ini.
settings.ini `[controller] deadzone` (radial, both sticks), `sensitivity` (camera + RTS cursor speed) and `rumble`
(default true: rumble on the player's hits taken / landed; the original had no force feedback).

Mouse yaw keeps the sub-degree remainder between ticks (no lost counts on high polling rates); `--camera-fix` /
`[controller] camera_fix=true` applies the community Camera Fix Patch turn rate (5x). docs/CAMERA_AND_WIDESCREEN.md.

## Saves

Native saves: `$XDG_DATA_HOME/joan-native/saves/*.jsav` (versioned CRC sections, 3 rotated backups,
docs/SAVES_AND_CAMPAIGN.md) plus a `META` section (description, mission, time) and a `<stem>.bmp` thumbnail (512x256,
the world view without HUD at the moment Esc / F7 opened the menu or F5 was pressed, as the original's Save/*.BMP).
Save screen: SAVE NEW or SAVE on a slot opens the description box (typing with caret, Backspace/Delete,
Left/Right/Home/End, click/drag in the box; 63 characters, cp1252); Return / CONFIRM saves and the list refreshes,
Escape / CANCEL aborts. The Load/Save lists re-read the folder every time they open, so files copied in or removed
outside the game show up. F5 writes `QuickSave.jsav`; F6 asks "Load the last Quick Save game now?". Original
`Save/*.SAV` files appear in the Load list (with their Save/*.BMP) and are imported read-only.

F10 (Capture Screen) writes the presented frame, UI included, as `JOAN000.png`, `JOAN001.png`, ... (the original's
`ScreenShot\JOAN%03d.JPG` numbering, never overwriting) into `$XDG_PICTURES_DIR` (environment, else
`~/.config/user-dirs.dirs`, else `$XDG_STATE_HOME/joan-native/screenshots`).

### Test saves (`tools/make_test_saves.py`)

`tools/make_test_saves.py` writes ready-made saves for every campaign mission into the native save dir: per mission
`Test_<MAP>_00_start.jsav` ("TEST <MAP> - start", right after the opening) plus one per key place
(`Test_<MAP>_<NN>_<place>.jsav`, "TEST <MAP> - <place>": camps, towns, castles, gates, objectives; the list is
`MISSIONS` in the script). Each comes from a headless run (`--scenario`, then `--load` of the start save, autoplay
`warp <place>` and `cmd Save 0 <file> <description>`) and is checked by loading it (`--load <file> --frames 60`:
right mission, hero alive where she was saved). S9L1 starts from S5L1 forced won + CONTINUE (its world is S5L1's,
so S5L1's places are untaken). Regenerate after save-format or mission changes:

```
tools/make_test_saves.py                       # all missions, build/src/engine/joan-native, ~30 s
tools/make_test_saves.py --missions S3L1,S6L1 --binary ~/.local/share/joan-native/app/current/bin/joan-native
tools/make_test_saves.py --out /tmp/saves      # elsewhere (--game-dir defaults to the installed game)
```
Existing `Test_<MAP>_*` files of the chosen missions are replaced. The Load list shows them newest first, 10 a page
(the page arrows wrap).

## Settings applied at startup

settings.ini (docs/LAUNCHER.md): `[renderer] backend/preset`, `[graphics]` image-quality keys (`aa`, `msaa`,
`anisotropy`, `render_scale`, `sharpness`: any preset, Original keeps its look), `[display] width/height/aspect/mode`,
`[controller] deadzone/sensitivity/camera_fix/rumble`, `[audio] master/music/sound/environment` (0–100, override the in-game
volumes), `[paths] game_dir`. Explicit CLI flags win (`--preset original --graphics msaa=4,aa=smaa`; only non-quality
`--graphics` keys switch to `custom`). In-game options (shadows, volumes, input method, bindings) live in
`$XDG_CONFIG_HOME/joan-native/options.ini`.

## CLI shortcuts (tests, debugging)

```
--new-game [--mission N] [--difficulty D] [--seed S]   start a campaign mission directly
--load FILE                                            native .jsav or original .SAV
--scenario S1L1                                        the campaign mission of that map
--no-movies  --rts-unlocked  --camera-fix
--free-camera  --camera X,Y,Z[,H[,P]]                  fly camera (original units / degrees): move keys fly, Shift fast,
                                                       mouse look (Options mouse sensitivity / invert), no HUD
--headless --frames N                                  no window (NullRenderer): run N frames, print state + hash
--screenshot FILE.png --frames N                       hidden window, capture frame N
--autoplay-script FILE                                 timed synthetic input, see below
--renderdoc  --renderdoc-capture N                     RenderDoc in-app API; capture frame N (see below)
```

### Developer tools (docs/GRAPHICS_ROADMAP.md "Developer tools")

- RenderDoc: `--renderdoc-capture N` loads RenderDoc (`RENDERDOC_LIB=/path/to/librenderdoc.so` for an extracted
  tarball, else the installed one) and writes frame N to `/tmp/RenderDoc/joan-native_<date>_capture.rdc` (path logged).
  OpenGL restarts the process once with RenderDoc preloaded. Vulkan needs RenderDoc's layer registered (installed
  package, or `VK_ADD_IMPLICIT_LAYER_PATH=<tarball>/etc/vulkan/implicit_layer.d` with the manifest's `library_path`
  pointing at the tarball's `lib/librenderdoc.so`). Example:
  `RENDERDOC_LIB=~/renderdoc_1.46/lib/librenderdoc.so joan-native --scenario S1L1 --screenshot s.png --frames 31 --renderdoc-capture 30`.
  RenderDoc's own hotkeys (F12/PrtScn) also work in a visible window while it is loaded.
- Dev overlay (release builds and `-DJOA_DEV_OVERLAY=ON`): Ctrl+F12 toggles it (plain F12 stays the Mission Brief).
  It shows timings, draw/unit counts, a day/night section (the mission clock; "override game clock" with a
  time-of-day slider, a speed in game minutes per real second and 06:00..night presets) and graphics preset/feature
  toggles. While it is shown the mouse is released (no mouse look) and the overlay draws its own pointer; the launcher's
  Graphics tab "Debug menu" checkbox (`[debug] overlay`) opens it at start.
- Tracy (builds configured with `-DJOA_TRACY=ON`): connect the Tracy profiler/`tracy-capture` v0.11.1 to the running
  client; zones `sim.tick`, `render.submit`, `render.present`, `audio.mix`.
- Day/night: the world lighting follows the mission clock (24 game minutes per real minute), blending the world's
  `.day` stages (Normal_06 .. Normal_Night: sun colour, ambient, sun elevation 45° .. 135°, fog scattering) between
  their start times. `JOA_WORLD_CLOCK=<minutes>` pins it, e.g. `JOA_WORLD_CLOCK=1450` = `Normal_Night`.
- `JOA_CAPTURE_FPS=<fps>`: `--screenshot` / `--autoplay-script` runs advance 1/fps s per frame instead of one 33 ms
  game tick (render interpolation as on a high-refresh display, still deterministic); scale script frame numbers by fps/30.

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
342  give JOAN_ARMOR_LV3      # test hook: item.res code into the player's bag, then equipped / used (armour, weapons)
344  drop SMALLRED 90 30      # test hook: floor item 90 units from the player, 30° left of its heading
346  crit                     # test hook: critical attacks on + HP trail > CRI_DIFF: next tick starts a crit sequence
350  shot work/run/combat.png # capture this frame
360  type Before Orleans      # text input (rest of the line), e.g. the save description
362  warp Camp 1 Place        # test hook: hero to a facility point / place centre (by name, map/*/*.pla)
364  cmd Save 0 T.jsav TEST 1 # save <native save dir>/T.jsav, description = rest of the line ("TEST 1")
370  pad south tap            # gamepad button (south/east/west/north = a/b/x/y, lb/rb, start, back, dpup, ...): down|up|tap [frames]
380  axis lefty -1            # gamepad axis (leftx/lefty/rightx/righty/lefttrigger/righttrigger), held until changed
900  quit
```
Without `quit` the run ends two frames after the last event.

The acceptance play-through `work/PlayableGame2/run/play.txt` (`play.sh gl|vulkan`, seed 1, `--rts-unlocked`; shots in
`work/PlayableGame2/run/<renderer>/`, F10 captures in `/tmp/pg2-play-<renderer>/pictures/`) clicks New Campaign → Normal,
shows the loading screen and the opening cut scene, skips it, drops two floor items ahead (test hook `drop`) that Joan
picks up running (SMS line), runs to the English camp and fights, plays a critical attack (`crit` hook), opens F1
(bag sword + XP hooks, drag-equip, ADD Strength, tooltip, combo page), swaps armour LV2/LV3, enters RTS (box select,
Ctrl+1, move order — units walk —, deselect, Alt+1, Ctrl attack-move, minimap jump, 2 2 → Jean, F2 out as Jean, `` ` ``
back to Joan), quick saves (F5), saves "Before Orleans" typed on the Save screen (F7 → SAVE NEW), opens the Load list
with its thumbnail (F8), captures the screen (F10), quick loads (F6 + Return), forces the win, clicks Next Mission /
CONTINUE and starts mission 2 (S3L1), whose script opens the mission brief. GL and Vulkan end with the same mission
hash.

Pad-only play-through `work/PlayableGame2/GamepadPlay/play_gamepad.txt` (replace `SHOTDIR`; same flags + `--no-movies`, no
`--new-game`): D-pad + South through main menu → New Campaign → Normal, Start skips the cut scene, left stick runs,
right stick turns, North/RT/West attacks + Back+West energy attack, Back tap → RTS with the stick cursor (South box
select, West order), Back+LB quick save, Start menu + East resume, Back+RB quick load prompt + South OK, forced win,
South Next Mission, D-pad to CONTINUE → mission 2. One `give SMALLRED` test hook heals Joan after the fight (she is
AI-driven in RTS). GL and Vulkan end with the same mission hash.

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
- `engine.save_capture` (headless, real client): F7 → SAVE NEW → typed description with caret edits → Return: META
  description, 512x256 `<stem>.bmp`, list order; F10 twice → JOAN000/001.png; folder re-scan on screen open; thumbnail
  crop; free-camera mouse look. `engine.gl_save_thumbnail` / `engine.vk_save_thumbnail`: description entry shot +
  Load list with the selected save's thumbnail (1:1 vs its BMP, screen vs `s04_07_load_menu.png`).
- `engine.gamepad` (headless): pad-only main menu → New Campaign → Normal (NewGame 1), Escape by East, mouse takes over
  again, options page + slider/combo adjust + East back (OptionsChanged), Gamepad/Joystick listed only with a pad, remap
  capture binds J_JOYPAD 4 / Start cancels / LB-RB category, in-game menu East resumes; GameInput Back layer + tap,
  deadzone, stick sensitivity; UI repeat timing; RTS cursor; rumble; autoplay pad verbs. `engine.gl_gamepad` /
  `engine.vk_gamepad`: pad-focused main menu, options root, in-game menu, remap (KBM + joypad column) vs the original
  hover shots and vs the port's own mouse hover.
- `engine.character_details` (headless): F1 pauses, ADD on Strength, bag sword dragged onto the weapon grid
  (World::equip), right-click bread (World::use_item), CB/WP page switch, F1 closes; shop buy (gold/bag), BUY 8 on
  missing gold, Esc closes; ItemTransfer: Joan's explosive arrows clicked onto Jean's portrait (window shows Jean, the
  arrows arrive), dragged back onto Joan's portrait, a task-locked Jean refuses (no input in his window), a potion
  dragged outside the window lands on the floor beside Joan, a CanDrop 0 letter stays; then pad only (PadWindows):
  D-pad up through GameInput opens it, ADD Strength, South picks the bag sword and drops it on melee 1, West eats a
  bread, RB/LB pages, East closes, South buys in the shop, South on Jean's portrait hands him arrows, North drops a
  potion. `game.items_transfer`: World::transfer_item rules (rank, control, task lock, minimap view, slot-first
  placement, bag room, self hand-over), give_item / buy_item / floor pickup with full bags and partial stacks,
  drop_item distance, discard_item CanDrop.
  `engine.gl_/vk_character_details`: window vs s04_06_character_details (masked SSIM 0.95), combo page and shop
  shots. `engine.gl_/vk_gamepad` also write `gamepad_<gl|vk>_charwin.png` (pad focus on a bag cell + tooltip).
- `engine.recruit_workshop` (headless, mission 7): barracks opened by the Interact event pauses, pad right adds /
  East leaves unchanged, aw_r / aw_l clicks, Enter commits (gold, population, queue) and two light infantry join
  Joan's troop; workshop BUILD queues a cannon that spawns after 2 s; bag cells (personal and troop bag, and the
  campaign profile's heroes) survive Mission::save/load and the character window draws them in their grids.
  `engine.gl_/vk_recruit_workshop`: barracks and workshop screen images. `game.savegame_import`: Jean de Metz's
  equipment and bag cells from Save/Mission1-AutoSave.SAV.

## Known gaps

- Startup movies are not played under `--autoplay-script` / `--screenshot` (hidden window); `--play-movie` captures them.
- RTS: map items are picked by their model's bounding sphere (no box / cylinder kinds). In RTS the hero is AI-driven:
  from mission 1 she may run off toward the English (her mission task), as in the original. The original has no
  formation keys or order buttons. Not ported: the box-select voice is not suppressed while the same voice still
  plays (0x473140), non-hero commanders are selectable outside the minimap's revealed area (FUN_00717910), the
  script `quit_rts_mode` does not run the Rts_deinit hero switch. Hero-switch demo
  `work/PlayableGame2/RtsHero/play_rts_hero.txt` (mission 1 `--rts-unlocked`: box select, Ctrl+1, F3, move voice,
  2 2 → Jean, F2, `` ` `` → Jean and back; the RTS log line prints `voice <code>`).
- F1 character details (pauses; attribute ADD, drag weapons/bow/ammo onto their slots, right-click potions, combo
  page with combo-level ADD / LEARN), the shop (click a stock item to buy, click an own item to sell) and the ARMORY
  window on first pickup of a ShowArmoryInterface item: bag cells are saved (INVG section), troop inventory is
  display-only, no hero switch from the window, no armory 3D item (details: UI_SYSTEM.md "Character details window").
  Autoplay test hooks `<frame> bag <code>` (give without equipping) and
  `<frame> xp <n>` (gain XP); demo `work/PlayableGame2/CharDetails/play_chardetails.txt`, pad-only demo
  `work/PlayableGame2/PadWindows/play_padwindows.txt`.
- Barracks recruit and siege-workshop screens (R inside a RECRUIT / WORKSHOP facility of the player's nation; pause
  the game; aw_r / aw_l or pad left/right on a row, RECRUIT|BUILD / Enter commits, CANCEL / Esc leaves; HELP = G_POPUP
  text box; build bar over the workshop while a weapon builds). Mission 7's workshop1 is English until the script's
  capture; its IRON / WOOD workshops need Alencon as the controlled unit (key 4 / `` ` ``). Test hooks
  `<frame> warp <facility | place name>`, `<frame> gold <n>`, `<frame> clear <place name>` (kills the hostiles in
  it), `<frame> owner <nation> <place name>` (set_place_nation_by_code), `<frame> attr <type> <n>` (spend attribute
  points, 5 = leadership); demo `work/PlayableGame2/RecruitFix/play_recruit.txt` (mission 7): leadership raises the
  troop maximum to 15 and two recruits join; workshop1 cleared + made French, HELP box, two cannons built (bar over
  the workshop, "Siege weapon built.").
- Equipment visuals: hero meshes follow the armour used (JOAN_ARMOR_LV1/2/3 → joan_armorC / Joan_of_Arc / joan_armorB),
  carried weapons swap, projectiles fly with the ammo's flight mesh, dropped items lie on the floor and are picked
  up by walking over them (generals). Box contents and script-added map items are drawn and picked up the
  same way (a broken box plays its break clip and disappears; destroyed map items vanish); pickups by the player
  post the original SMS line ("You have picked up ...", gold-crate counter) bottom
  left. Not done: arrow trails, item pickup radius is an estimate (15), and the bow shown while aiming
  (the hero is hidden behind the first-person aim camera, as in the original).
- Gamepad: no on-screen keyboard (a pad-only save gets no typed description); in RTS the commander keys 25..32 have no
  pad binding (select with the cursor); the `--ui-screen` viewer (app.cpp) takes no pad input.
