# Development notes

Test coverage and acceptance play-throughs of the game client. Moved from [PLAYING.md](PLAYING.md). Paths under
`work/` are local working directories (ignored by git) and are not part of the repository. Building and running the
tests: [BUILDING.md](BUILDING.md).

## Acceptance play-throughs

Autoplay scripts (format: [PLAYING.md](PLAYING.md) "Autoplay scripts").

The acceptance play-through `work/PlayableGame2/run/play.txt` (`play.sh gl|vulkan`, seed 1, `--rts-unlocked`; shots in
`work/PlayableGame2/run/<renderer>/`) clicks New Campaign → Normal,
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
