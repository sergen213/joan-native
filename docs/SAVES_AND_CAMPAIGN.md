# Mission runtime, campaign flow and saves

Owner: MissionRuntime. Code: `src/game/mission*.{hpp,cpp}` (Mission orchestrator, MissionState,
MapObjects, Places, MissionUi), `src/game/script_bind*.{hpp,cpp}` (builtin host),
`src/game/campaign*.{hpp,cpp}` (campaign table, profile, runner), `src/game/savegame_*`
(native container, original import). Tool `tools/joa-campaign/`. Tests `tests/game/{mission,campaign,savegame}_*_test.cpp`
(`ctest -R game`).

RE references: [reverse-engineering/SCRIPT_VM.md](reverse-engineering/SCRIPT_VM.md) §8 (builtin table),
[reverse-engineering/GAMEPLAY_AI_TROOPS.md](reverse-engineering/GAMEPLAY_AI_TROOPS.md) Part D (triggers,
end conditions, scenario flow), [formats/CONTAINERS_WORLD_SAVES.md](formats/CONTAINERS_WORLD_SAVES.md) §7 (SAV / Player.dat).
Labels: **CONFIRMED** (read in code), **HIGH**, **HYPOTHESIS**.

## 1. Tick order

`Mission::tick` (one 33 ms tick) follows `Game_process_tick 0x4e5fc0` and `Game::process 0x471620`:

1. play time / scenario passed time += 33 while `finish == 0` (0x4e5fcc).
2. `MapObjects::tick` (modify_item 0x4afb20).
3. `World::tick_units` (game clock, UnitArray_process, projectiles, `Trigger_process` 0x4ef090 — the
   0x43-byte attack/event triggers live in the World, see below), then `process_world_events`
   (objective counters the engine increments, e.g. citizen kills in `Unit::die` 0x64f320).
4. `Places::tick` (place array 0x4c55d0, facilities 0x46d4a0, territories 0x4e4bb0).
5. `Vm::tick`: challenge timers, one exec of the step script (skipped once lost), count times /
   time limit (`FUN_004cd5d0`), gated by the cut-scene flag.
6. End conditions + win delay (`MissionState::process_end_conditions`, §3).
7. `MissionUi::tick`, `World::tick_physics` (integrator, after the Game object in the original).

Load (`Mission::start`) = `Battle::start_scenario 0x41cc80` + `Battle::load_world 0x41c810`: compile
the step script, run `_des` once (end conditions), load map objects (.stg), places (.pla/.ter/.trk),
units of every world node (`<node>.uni`, `Uni_load_create_unit 0x558a60` + the team pass of
`0x558890`), restore the profile (item-hint lists, gold, play time, command points), then
`Vm::load_pass()` — the extra exec of the script root that `load_world` does before tick 1.

**Determinism.** Fixed 33 ms ticks; one explicit seed for the shared RNG stream (VM + World, the
original seeds `time(NULL)`); every unit is processed every tick (the original's CPU-budgeted
round robin, `UnitArray_process`, is a documented GameSim deviation); no wall clock anywhere in
the simulation.

## 2. Triggers

The 0x43-byte trigger records (`TriggerSys` 0xa5ccd0) are owned by the World (GameSim) because
the melee/arrow damage path *is* the trigger system. The mission layer posts script triggers
through `World::post_trigger` (`add_gate_trigger` 0x20, `break_wall` 0x40) and receives triggers
aimed at map objects (`target_kind 2`) through `World::object_trigger`.
**Sort bug.** `TriggerSys::process` qsorts with a comparator that reads byte +0x12 of the
*element address* (a pointer byte) instead of the trigger priority, so the original order is
effectively arbitrary (CONFIRMED, Part D §2.5). The port keeps creation order and walks it from
the newest index down like the original loop; this is deterministic and equals the original
whenever the qsort happened to leave the array unchanged.

## 3. End conditions, victory, defeat

`MissionState` mirrors Game+0x309 (count, **max 40**) and the 0x1a-byte records (type 0 WIN,
1 LOSE, 2 SIDE quest, 3 IMPERATIVE / must survive; description, territory link, finish 0/1/2,
para1/2, enabled, gold award, experience award, awarded flag). Per tick, after the script:

- every described WIN/SIDE record with finish 1 not yet awarded: gold += award (party gold
  DAT_00a596c4), experience award to every live hero (rank > 3, class flag hero) via
  `World::add_xp(u, exp, share=1, quiet=0)` (FUN_005229a0(exp,1,0));
- all enabled described WIN records finished → win timer starts (else resets); an enabled
  described LOSE record with finish 2 → `finish = 2` immediately;
- `finish = 1` when `now - win_start >= win_delay` (unsigned) and no cut scene: enabled
  IMPERATIVE records → finished, unfinished described SIDE records → failed (not in scenario 5),
  `Victory` event; the WinMsg stops every unit (`AiSystem::set_idle`), except leaving scenario 5: GameOver_win_freeze_units
  0x6e7900 only `Unit_stop`s every unit there (`DAT_00a58fa7 == 5`), as S9L1 plays on in that world.
- Time limit out (VM) or a LOSE record → `Defeat` event. Main-character death is script
  convention (`S1L1_finish.sce` sets its LOSE record and calls `call_gameover_msg`).

## 4. Campaign flow

`scenario/scenario_01..08.scr` (file number = campaign index, `StepNo 1` everywhere):

| no | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| script | S1L1 | S3L1 | S2L1 | S4L1 | S5L1 | S9L1 | S7L1 | S6L1 |

Win → (win message, game-end menu) → **CONTINUE** (`GameEndMenu_input 0x720ca0`,
`CampaignRunner::continue_after_win`): store the profile, `finish = 3`, start scenario no + 1
(mode 0xd → `Battle::start_scenario(no+1, 1, keep)`); after scenario 8 the ending plays instead.
**RETRY** = restart the same scenario with the unchanged profile.

What persists between missions (profile, `Save/Player.dat` in the original; CONFIRMED from
`FUN_0051ff00`, `FUN_004c90a0`, `FUN_004c96f0`, `FUN_004727b0`/`FUN_00472b00`, `FUN_004c8810`):

- **Heroes**: every live unit with rank > 3 at CONTINUE is stored under its class code (0x77-byte
  profile entry). Before storing, bag items of item type 0xf are removed (except leaving
  scenario 5). The stored copy (`copy_persistent_unit`) is name, title, rank, kingdom, race, team
  id, the +0x11c..+0x16e block (lives, level, experience, skill/attribute points, HP/EP and their
  base maxima, STR/DEX/DEF/LEAD, armour, loyalty, morale, reputation, mode, attack behaviour),
  the learned-upgrade list and the inventory. Kills/medals are **not** copied. The entry has a
  member list but nothing ever fills it: **troops are not carried**.
- `load_unit(code, nation)` (FUNC 86 → `FUN_004c91c0`) is the only way back: it returns 0 until a
  CONTINUE has written the profile (`DAT_00a58f95`), so the first mission falls back to
  `add_unit`; otherwise it creates the class, copies the stored subset, sets the nation, marks the
  unit AI-controlled (+0x102) and refills HP/EP.
- Item-hint lists (Game+0x71c / +0x728: item ids whose pickup hint was shown once), party gold,
  total play time, command points (DAT_00a5b9f0).
- Difficulty is the global config byte 0xa57fc2 (0 easy … 3 very hard), carried by the profile
  and set on each World.
- A new profile holds 100 gold (shipped `Player.dat` +0x38 = 100; the mission-1 autosave's Game
  object shows gold 100).

## 5. Native save format

```
"JOANSAVE" | u32 version (1) | u32 section count
section: char tag[4] | u32 size | u32 crc32(data) | data
u32 crc32 of every preceding byte
```

Little-endian; any CRC mismatch, truncation or a newer version is rejected. Older versions go
through `save_migrations()` (one upgrade function per version step; empty in version 1).
Sections of a mission save:

| tag | content |
|---|---|
| MISN | scenario no, seed, last finish, test-hook flag, command points, campaign profile (heroes as full Unit records) |
| WRLD | `World::serialize` (units, ground items, triggers, projectiles, clock, controller) |
| AISY | `AiSystem::serialize` (orders, tasks, teams, formations) |
| VMST | `Vm::save` (every node cursor / flag / variable of the compiled scripts, RNG, ScenarioState) |
| MSTA | MissionState (end conditions, counters, gold, clock/date, lists) |
| OBJS / PLCS / UIST | map objects, places/facilities/territories, UI runtime (dialogs, cut scene, per-call-site WAIT state) |

Loading = start the same scenario with the saved seed (recompiles the same scripts, so VM
structure hashes match), then restore every section. Saves are written atomically (temp file,
fsync, rename, directory fsync) after rotating `name` → `name.1` … `name.3`, under
`$XDG_DATA_HOME/joan-native/saves/` (fallback `~/.local/share/joan-native/saves/`); writes into the
original install (a directory holding `joan.exe`/`JoanofArc.exe`, or its `Save/`) are refused.
The script `save_game(name, desc)` (the original's only autosave, `S1L1_tactic.sce`) requests a
save that `Mission::tick` writes after the tick to `<name>.jsav`. Saves are bit-exact: a save at
tick N, loaded and continued, gives the same state hash as the uninterrupted run
(`game.savegame_mission`: mission-1 start, mid-mission, after the transition to mission 2).

## 6. Original save import

`Save/*.SAV` bodies are raw memory dumps (CONTAINERS_WORLD_SAVES.md §7.1). Decoded reliably:

- Header (description, FILETIMEs, save-object +0x19a = scenario number).
- Section 4 (`FUN_00484780`): Game object 0x745 bytes (scenario no/current step at +0x17/+0x1b,
  finish +0x308, end-condition records +0x30b, gold +0x734, win delay +0x738, win start +0x73c,
  play time +0x740, count times Scenario+0x1ca..), the Scenario record 0x289, every step record
  0x1b2 (bit flags +0x18a..+0x196, stage +0x19a, time limit +0x1a6/+0x1aa, mode +0x1ae) followed by
  its name string (the description slot repeats the name: original bug in `FUN_00483b90`), then
  each end condition (0x1a record + description + territory link).
- Section 13 unit records (u16 0x432 + raw Unit): hero progression (level, experience, points,
  HP/EP, base stats, lives, morale, kills) by class.

`apply_original_save` starts the mission fresh from its map and restores bit flags, stage, mode,
time limit, count times, end conditions, gold, play time, win delay and the heroes' progression.
Shipped `Mission1-AutoSave.SAV`: scenario 1, stage 2, bits 0x02000008, gold 100, 7 end
conditions, "The Arrival".

**Limits.** The original does not save script-tree state either (it recompiles the script and
resumes from flags/stage), but it does restore the whole world; the import does not decode units'
positions/deaths, items, places, AI, triggers or inventories (heap pointers, per-class virtual
save layouts), nor the item-hint lists when non-empty, nor saves made while scenario generals are
listed. `Player.dat` profiles (any N) parse and write byte-exact (`formats/save_sav`), and
`to_player_dat` / `from_player_dat` (campaign.cpp) convert them to/from `CampaignProfile`.

## 7. Builtins

Ownership by slice (ids → handler file): see the generated table in `work/mission/slices.md`
(objects, places, units_query, units_state, troops, mission, ui). Coverage and deliberately
unimplemented ids: §7.1.

### 7.1 Coverage

`game.mission_coverage` parses every shipped `.sce` and checks the table: **270** builtin ids are
called by shipped scripts (25 VM-internal, 245 host); **245/245** host ids have handlers. In total
363 host handlers + 36 VM builtins = 399 of 407 ids. Deliberately unimplemented (none used by
shipped scripts): 55 `is_facing_enemy` (bearing helper 0x576da0 convention unconfirmed),
63 `get_character_recno` (mission scripts have no owning object), 261 `switch_controllable_unit`
(hero cycling 0x432300 not modelled), 314 `drop_item_on_floor` (hand-item model), 351
`mount_siege_tower_to_wall` (wall link 0x4633e0), 353 `unit_dist_to_track` (track object 0x49ab40),
362 `set_item_path_item_owner` (no PATH items in shipped maps), 379 `get_minimap_icon_display_enable`
(minimap resource table). A call to an id without handler is counted by `MissionHost::unimplemented_calls()`
and returns 0 / 0.0 / ""; `game.mission_s1l1` fails on any.

Per-slice deviations / HYPOTHESIS choices are commented at each handler (grep `HYPOTHESIS` in
`src/game/script_bind_*.cpp`, `mission_*.cpp`). Notable: hand-held weapon items are not modelled
(313, 358/359 partly); horses/carts boarding lives in the mission layer (World does not simulate
them); headless UI interfaces finish after `MissionUi::auto_ms`; give_order's WAIT (323) only waits
inside cut scenes, as in 0x661ca0; bribe / crate pickup / citizens-saved counters are not fed
because the World has no events for those engine paths.

## 8. Tool

```
joa-campaign [--mission N] [--ticks N] [--seed S] [--save-at T]... [--save-dir DIR] [--load FILE.jsav]
             [--import FILE.SAV] [--win-at T] [--continue] [--game-dir DIR] [--trace] [--autopilot] [--retries N]
```
Prints every MissionEvent (`t=<tick> <kind> a b text`), save paths/hashes, and at the end the
scenario, tick, finish, gold, state hash and any unimplemented builtin calls / script errors.
`joa-campaign --mission 1 --ticks 9000 --save-at 3000`.

`--autopilot` lets the campaign bot play instead (`--ticks` = budget per attempt, a loss is retried
`--retries` times, `--continue` chains missions with carried progression) and prints one report line
per mission: result, ticks, attempts, deaths, kills, finished/enabled WIN+SIDE objectives, hash, and
for a loss/timeout the blocking WIN objective, the bot's goal and the player position. With `--save-dir DIR`
the chain writes `DIR/bot-<stem>.jsav` at every mission start (resume one mission with `--load`).

### 8.1 Campaign bot (`src/game/autopilot_player.{hpp,cpp}`, test/diagnostic only)
Drives the player through `Mission::tick(InputFrame)` with only player inputs (W + camera yaw along a
navmesh path, A/B combos, Q guard / down attack, R at the interact indicator, F3) plus the
character-screen attribute allocation. Strategy (from the mission scripts and the Steam walkthrough):

- **Points:** every attribute point into HIT_POINT (+4 max HP each); every skill point into ADD on the lowest
  of the A / AA / AAA / AAAB combos it swings (combo level ×(lvl+10)/11 damage); a stronger one-hand sword in
  the bag goes into the hand (mission 1's archer captain drops JOAN_SWORD_LV2). Another hero's points are
  spent the way the in-game hint says: with no enemy within 400, switch to him (`Mission::switch_hero`, the
  key path), spend on his character screen, switch back (mission 2: Jean's 10 points, 50 → 90 HP).
- **Melee:** an English swing lands `anim_start + script StartTime` (~350 ms) after it starts. Above 45 %
  HP the bot attacks instead of guarding (every hit flinches its targets and A A A B cleaves; in a ring of
  attackers a guard only blocks the front — mission 2 kills went from 3–8 to 15–33 per run); below it, it
  guards swings landing within 400 ms (aimed at their middle, the controller does not turn while
  guarding), steps back when two are in reach. A knocked-down foe in reach is down-attacked (Q).
  Surrounded by 3+ within 110 it uses the energy attack (C) when learned (ENGATK skill 0xd; Joan's
  record is not combo-available, so not before ENERGY_ATTACK_UP) and affordable, with F4 (apples) for EP.
- **Food:** F3 below 50 % HP; crates (`BOX` map items with content) are broken and floor / dropped
  items walked over within 1200 when no enemy stands within 500 of them (the mission-1 tutorial).
- **Rest:** below 70 % HP, enemies that have not noticed the player are left alone (backed away from);
  the bot stands and regenerates. Below 30 % with no food it retreats to the nearest allied hero.
- **Allies:** a following ally hero (FOLLOW_UNIT) more than 700 behind, unless he is busy with a foe, and
  the **letter bearer** (a friend in the player's team dropping a MISSION item: mission 1's guards only open
  to him, and only on the first visit) more than 700 away are waited for standing, 30 s at most per wait
  (walking back turned the follower around too: mission 1 Joan and Jean walked back and forth for minutes).
  The bearer is a ward (below). A switchable ally hero within 1000 of a ward that dies for good when he
  becomes commandable (mission 2: Jean beside the Bastard as the attack starts) is put on hold there
  (Shift + his key, `AiSystem::toggle_hold`, his men too; the walkthrough holds Jean).
- **Objectives:** the nearest active minimap way point not yet stood at for 10 s (mission 2: blacksmith
  before the shop, so the buy screen comes before the shop talk that starts the attack); near it, NPCs
  showing the talk icon (`can_interact`) are walked up to and talked to once; then the open WIN
  objective's territory, the nearest enemy, items.
- **Shopping:** while the buy screen is open (`start_buy_interface`), once per opening, the bot makes
  the click handler's purchase (FUN_006c5a00: `World::give_item` of the item's Quantity at the stock
  level, gold −= Price) from `shop_stock()` (mission_ui.hpp, the stock list the engine buy screen also
  reads): explosive arrows up to 20 (packs of 5 for 500), then a heavier armour than worn (then worn,
  `World::use_item`; mission 2's JOAN_ARMOR_LV2 costs 3000, all the Normal gold, which left Joan without
  explosive arrows), plain arrows (10 for 100) with the rest.
- **Bow:** a foe 150–800 away (arrow damage is full up to FALL_S 800; also up/down a wall, |dz| < 700)
  with nobody within 100 (200 if after the player): Z, camera yaw/pitch at the target's middle, A to
  draw, A in SHTIDLE to release. **Explosive volley** first: the densest group of 2+ (within the blast's
  150) 60–800 away, also the crowd around the player (the ground blast skips the shooter and friends,
  knocks every enemy in it down). A shot is only released on a **clear shot** (`clear_shot`): the target
  is on its feet (an arrow aimed at a standing body flies over a knocked-down one and explodes far behind:
  before this check mission 2 landed 2 of 15 explosive arrows, after it 8–9 of 10) and the release lock
  (`range_fire`: nearest hostile within d/100 + AIM_R of the aim ray) picks the target itself. With
  explosive arrows left and nobody on the player, the bot walks to the densest group of 5+ within 3000
  (squads gathered at a rally point). Explosive ammo is swapped in by the character-screen drag
  (`World::equip`). Z again leaves range mode. A foe on another level (a wall top) is shot, never circled
  underneath (mission 2 timed out with Joan walking under a wall-top English for 26000 ticks).
- **Targets:** the current foe while within 700, else the nearest within 3000 (450 while a marker pulls),
  with whoever targets the player ×0.5, archers within 350 ×0.3 and **enemies after a citizen ×0.4**.
  With nobody in reach the bot hunts the enemy (not a cannon or horse: mission 2 Normal timed out next
  to the stage-5 cannons) with the smallest distance to the player + to `home()` (centroid of the units not
  hostile to the player: the town under attack before the next towns' garrisons), enemies on another level
  +5000.
- **Interact:** the indicator is computed before the physics step moves the player, so the bot stops and
  presses R only once it stayed lit 3 ticks (R while walking landed out of reach: mission 2 never got the
  shop talk). Gates are never toggled in passing.
- **Gates:** a way point on an open interactable gate (mission 2 stage 3: "Someone must approach and close
  the gate") is closed (walk to 100 inside it, towards `home()`, R) 30 s after it appears: after John's
  10 s mark (closing then sends him and team 11 off) and long before the 180 s reinforcements (16 more
  units while the gate is open). `travel()` (way points, territories, hunting) opens a closed gate whose
  shut passage ends the navmesh path short of the goal (or on another level); a gate the bot opened
  itself is not closed again while an enemy lives within 4000 outside it.
- **Navigation:** stuck twice on the same line (house corners the path mesh cuts) → run off sideways,
  alternating sides, further each time, then replan.
- **Loss:** a finish 2 or the game-over box (`call_gameover_msg`, e.g. a hero that must live died or too
  many citizens were killed; it freezes every unit even when no LOSE record ends the mission).

Hero switching is `Mission::switch_hero` (the key path, RTS commander pick and rts_deinit call it); hold is
`AiSystem::toggle_hold` (Shift+N). Tried on mission 2: the bot as Jean with Joan on the AI (the script's
DEFEND_PLACE at the gate) lost Joan in 5/6 seeds; Joan anchored at the Bastard (enemies > 1500 from him
ignored) lost on citizens in 10/12 seeds; neither is used. Jean's RAPIDFBOW (damage 16 vs Joan's BOW 8)
with his 90 arrows is the untried switching use (the bot would play Jean, Joan held). Not done yet: learning
new upgrades (RUSH from level 4, BB), selling (the buy screen sells at Price × SellingRatio), hero-to-hero item
transfer (not modelled by the port), siege use, RTS orders, mounting, bribes, choice dialogs (headless answers
choice 1). The interact prompt (R) also recruits. `JOA_BOT_TRACE=N` prints a status line every N ticks (with
the ally hero's task and target) plus hits taken / dealt, hits on friendly heroes (attacker team), deaths
(killer team), spawns, interact prompts and the player's shots / projectile hits; `JOA_BOT_DBG=T` prints 200
ticks of per-frame inputs and nearby enemies from tick T.

`game.campaign_legit` (CTest fixture `campaign_checkpoints`) runs the chain once from a new game (Normal, 150000 ticks per
attempt, 2 retries), writes `campaign_checkpoints/bot-<stem>.jsav` at every mission start and `campaign_checkpoints/chain.txt`
(one report line per mission) into the build tree (`build*/src/game`), checks no script errors / unimplemented builtins and
asserts `kExpectedWins` (3). `game.campaign_legit_mission_N` (N = 1..8) loads mission N's checkpoint, replays it and must
reproduce the chain's line exactly (result, ticks, hash: determinism through save / load); skipped when the chain did not
reach N. Iterate on one mission without rerunning the chain: `ctest -R legit_mission_4 -FS campaign_checkpoints`. Report
(seed 1, Normal) and `work/cb8/chain.sh` (120000 ticks) on Easy:

| mission | difficulty | result | ticks | attempts | kills | cause |
|---|---|---|---|---|---|---|
| 1 S1L1 | 0 Easy | WON | 20902 | 1 | 35 | — |
| 1 S1L1 | 1 Normal | WON | 20571 | 1 | 44 | — |
| 2 S3L1 | 0 Easy | WON | 65199 | 1 | 117 | — |
| 2 S3L1 | 1 Normal | WON | 70364 | 1 | 126 | — |
| 3 S2L1 | 0 Easy | WON | 68057 | 1 | 191 | — |
| 3 S2L1 | 1 Normal | WON | 109154 | 1 | 268 | — |
| 4 S4L1 | 1 Normal | timeout | 150000 | 1 | 131 | chain: side objectives only (2/11); a scratch run from the older chain checkpoint (`work/cb9/chain1`) finished 3, 4, 9 (t43000 / 78000 / 115000); Saint Augustin, Tourelles never |

**Mission 4 (S4L1, from `S4L1_tactic.sce`).** Win = objectives 1 Tourelles (no ENGLISH in place Tourelles), 2 Saint
Augustin (no ENGLISH in place Augustin and event bit 24: all six `abbow` arbalests dead or the Abtower wall at 0 HP),
3 priest (talk; the wagon at Église de lumière is talked to, follows, and must be within 600 at the second priest talk),
4 councillor (after 3; talk, then the five "Missing Villager" talks in Forêt Fierbois, then the councillor again; both
answer "Not now!" while English stand in Fierbois), 9 Jean de Metz (Talbot's retreat drops `keyS4_02`; talk to Jean with
it), 8 (enabled only if `ab_gate` is opened with Glasdale's `keyS4_03` before the wall falls). The four French cannons
shoot the Abtower only after Jean is free and the attack started (bit 31: wall damaged or the player inside Abtower
Inside), and turn on the Tourelles gate once Augustin is captured. Bot changes: a WIN territory without 500 headway in
60 s is skipped for 5 min, its height chosen on a linked region (Tourelles lies behind its shut gate); **talk errands**:
with no marker, friends the script gave the talk icon during the mission, or standing in a territory an open WIN objective
names / named, are talked to once and again after news their own talk did not bring (objective, talk icon, follower,
MISSION item); a follower is never talked to (a second word stops the wagon); shopkeepers once; an **escort** (citizen /
cart following the player) is taken back to the NPC whose talk brought its icon, after that NPC's other new talkers, and
waited for while it closes in; enemies within 2000 of an errand NPC count as on a ward; the talk give-up (30 s) counts
from 400 off; holds only for territories the player's nation owns. Open: two far foes scoring alike, chosen in turn,
can root the player (seen in Forêt Fierbois; a 1.3x hysteresis fixed it but cost mission 3 its seed-1 win).

**Port bug fixed (cannons never hit):** OrderExec_shoot 0x4bd090 stores a SHOOT order's map-item target in unit +0x2dd
(clearing +0x2db; `Unit::range_item`), and ThrowItem_launch 0x5133e0 / Unit_compute_aim_dir 0x50aca0 solve the shot to that
item (aim height: STC/CAN by ItemRes CollisionType +0x1e9 — sphere radius / box half height / cylinder half height — else
model bound radius / 4 or ItemRes Height; PROJ/EXPL model bound radius or Height). The port fired item SHOOTs along a stale
aim vector, so S4L1's cannons (script `give_order SHOOT 2 0 2 abtower`) and every RTS_ATTACK_ITEM archer / siege weapon
missed. Now every ball hits the Abtower mesh and both 0x40 events (object + area) take ranged_base_damage / 2
(`game.siege_crew`: Cannon 1 vs the Abtower, 1000 → 840 in 4 balls). The untargeted AI shot now uses the actor's forward
(+0.15 z for CANNON / FAUCONNEAU) as 0x50aca0 does.

**Mission 4 bot (`autopilot_plan_S4L1.cpp`):** the plan leads the errands itself instead of the generic talk / escort logic:
shopping at the Fierbois sellers within 4000 (explosive arrows up to 120, plain up to 300, F3 food up to 20: no more 10000-tick
rests), the priest's wagon (talk at the church, lead back, wait standing while it closes in, walk back to it when it stalls
600+ off for 20 s), the councillor and his five villagers (the talks alone deliver them: the generic escort waits cost 40000+
ticks), Fierbois cleared when the priest / councillor answer "Not now!" (retreat), Jean de Metz once both errands are done,
the Abtower shot, the English in Augustin (only once the tower is down: before that they stand on its walls), the Tourelles
gate, the English in Tourelles. Fierbois (nav level 1) and the Loire forts (level 2) are crossed at (16384, 27264) (the
level detour, re-planned after each fight, went round by Saint Jean le Blanc). In Forêt Fierbois only foes within 1500 or after
the player are fought (its place covers the church, the wagon track and the villagers: every English in it counted as near
an errand NPC from 13000 off and rooted the bot for 76000 ticks). Generic talk errands stop after the two errands (plan hook
`use_talker`). Core knobs: `explosive_reserve` (generic volleys, group shots and the plain-ammo fallback leave that many
explosive arrows alone: 90 for the gate while it stands), `plan_talk` (the NPC a plan walks up to is talked to again 300 ticks
after the last word).

**The Tourelles gate.** The cannons' default task turns them on it after the tower falls (same yaws as the original,
DIFFERENTIAL_VERIFICATION round 4). Round 6 (GateMesh) settled the collision: the gate's Door.AM1 never meets the
structure ray (the original's shared AM1 vertex buffer holds no positions); a ball stops on the gate's CollisionType box
(−15, −200, 0)..(15, 200, 265) in Physics_sweep_actors' item pass (`MapObjects::sweep_items`). Now, as in the original,
Cannon 2 and Cannon 3 hit it with every ball (−40 each, 2 per volley) and Cannon 1 / 4 lose theirs on the rise in front
of them (`game.siege_crew`). The cannon crews attach only near Joan, and after Augustin's capture the script's
RTS_ATTACK_ITEM (`unit_range` kind 2 = 1600 < ~2700, as the original 0x544690) drives them off. The earlier f_closed
mesh pose is gone; the gate box covers the same opening (mission 2: the English cannons outside still break the Castle
Gate, so the bot's `clear_shot` keeps its own-gate test, now against the boxes). The bot shoots explosive arrows into the portcullis from 350 outside (Gate_take_ranged, ~15 HP per arrow from the
BOW: ~90 arrows), then clears Tourelles.

Status (Normal, chain checkpoint, `work/MissionBot4/m4.cpp`): the west is done by ~90000 (Talbot 48000, priest 73000,
councillor 90000, Jean 99000; before: priest 60000, councillor never). One scratch run from the t40000 save **won** (Augustin
118690, gate ~125000, Tourelles 178696, victory 179151); from the checkpoint itself gold runs out: ~19000 buys food, arrows
and 120 explosive, and either Augustin's last wall archers (z 325..629, plain arrows only) or the gate's last 100–200 HP
outlast the ammunition (runs 8/9: gate 108 HP, Augustin 1 left at 260000). Not won in the chain yet: `kExpectedWins` stays 3.
Next: the cannon launch point / structure sweep (the gate would fall for free), or cheaper arrows (blot / crossbow).

**Mission plans** (`src/game/autopilot_plan.hpp`): `autopilot_plan_<stem>.cpp` defines `make_plan_<stem>()`; CMake globs the
files into the registry. Hooks: `first`, `before_targets`, `before_hunt` (return true = this tick's input is set),
`foe_weight` (target score factor, 0 = skip; the base skips siege weapons and horses), `after_tick`. The Autopilot helpers
(travel, fight, shoot / shoot_at, region, ...) are public for them.

**Mission 7 (S7L1, from `S7L1_des` / `S7L1_tactic` / `S7L1_finish.sce`).** Enabled at start: WIN 1 (capture Saint-Pierre:
no fightable enemy in places "St Pierre", "St Pierre2", "St Pierre3" *and* Gough dead or his heavy troop on the move), LOSE 9,
IMPERATIVE 10 / 11 (Alencon, Jean must live). Enabled on the way: 3 Varzy + 6 (100 `S7_GOLDBAR` barrels, `gold_crates`; 137 on
the map) by the gold-cart cut scene near Saint-Pierre; 4 (Matthew Gough) at the first entry into Saint-Pierre; 24 (Martin) by
the monastery-hill cut scene (place "st pierre firework"), which also gives both retreat -2 (killable); 14 on entering des
champs du nord; 15 (merchant cart) only if Varzy is seen before Saint-Pierre; 7 on entering St-Thomas (three mercenary squads
5 min later, 15 dead citizens = game over), won by the councillor scene, which enables 16 (talk to him with 100 barrels).
Once every *enabled* WIN of 1/3/24/4/6/7/14/15/16 is finished, event bit 2:16 deletes the invisible wall "inv" and enables 2
(La Charité: no fightable enemy in "St La Charite", x < 15262) and 5 (Gressart, retreat 1 then -2 at "gressart retreat 2");
the La Charité cut scene adds 25 / 26 (Begley, Hudleg). La Charité's gates open when a wall is damaged; walls take only
stones (0x40), kegs and rams in S7L1; gates also take explosive bolts / arrows (0x2000) and La Hire's melee (x3).
Start checkpoint: `game_campaign_S7L1_bot_test --checkpoint OUT [D] [FROM.jsav]` = CampaignRunner forced wins
(`finish_objectives`, awards included) up to mission 7. Main: from the legit chain's `bot-S4L1.jsav` (Joan level 12, 300 HP,
JOAN_SWORD_LV4, ~40 food, 36220 gold); pessimistic: from a new game (Joan level 9, STR 9, sword LV1, no food, 23600 gold).
Plan (`autopilot_plan_S7L1.cpp`): the Bastard follows Joan (RTS assign); Gough / Martin x0.05 target weight and hunted when
their region is linked; the inner wooden gate "StPierre gate 2" (closed by Gough's gate cut scene, shuts the hill off) is broken
by switching to La Hire (key 3, the walkthrough's way: RTS attack-item on a gate does nothing, every AI RTS_ATTACK_ITEM routine
only hits boxes) and back; gold barrels nearest first in linked regions (80 s each at most, St-Thomas' left alone while 7 is off);
St-Thomas defence / gold return errands; stall watchdog (a fight / hunt / volley target whose HP does not move in 30 s is given
up on); best bow / crossbow into the ranged slot (Martin's DRAGONCBOW); shop caps (60 explosive, 150 plain, keep 6000 gold).
Result (seed 1, Normal, main checkpoint): not won. Best run: La Hire breaks gate 2 at ~t35000, both Goughs die by ~t41000
(objectives 4 and 24), ~55 barrels by t150000; then Joan runs out of HP food (no food is sold before Saint-Pierre falls: the
potion shop opens on capture) and is worn down by the ~40 wall / gate archers of Saint-Pierre, which must all die for objective 1;
one run lost Joan at t90616. A shopping trip to St-Thomas' weapon seller was tried and dropped (the path detours never reached
him in 27000 ticks). Pessimistic start: Joan stuck resting (regen ~1 HP / 150 ticks without food). Not done: recruiting (the
barracks screen commit, level-20 troops), siege (workshop1 at La Charité is gold-built and usable by Joan once captured; IRON /
WOOD workshops only by Alencon), objectives 3 / 6 / 7 / 16 / 2 / 5. No port bug found in this mission. Bot core fixes made for it
(missions 1–3 still won, mission 4 5/12): loot give-up counted per target, `densest` skips given-up foes, the volley walk gives a
group up after 60 s without headway, `shop_explosive_max` / `shop_plain_max` / `shop_gold_keep` and crossbow ammo kinds in `shop()`.

**Mission 7 update (MissionBot7b).** Plan additions: the Gate Guard is bribed (2200 gold, walkthrough's quiet way) right after
the gold-cart cut scene, so the alarm's ~36 wall archers never spawn (event bits 14 / 13); capture hunts per place
("St Pierre"/"2"/"3", "eng camp", "eng 2 trigger") limited to reachable regions or clear shots; HP food bought at the potion
shop once Saint-Pierre falls; Joan's troop refilled at a French barracks through `game::FacilityOrder` (the barracks /
workshop screen copy, `more()` / `less()` / `commit()`, shared with the engine's `FacilityWindow`); La Charité phase (after
event_2 bit 16): siege-house cut scene, its mercenaries, a TREBUCHET built at "workshop1" and RTS-ordered at "wall2"
(any wall damage opens both gates), then the "St La Charite" hunt (~440 fightable mercenaries, untested). **Port bug fixed:**
Unit_update_location 0x4fee60 places an attached siege crew on the floor tracked under the attach point
(Phys_track_poly + PathSpace_poly_height); the port used the terrain height map, dropping every wall-walkway arbalest crew
~400 below its weapon where nobody could reach it (objective 1 never finished). `World::attach_z` / `IUnitPhysics::floor`;
test `game.siege_crew` (wall_crew_part). Result (seed 1, Normal, legit-chain checkpoint, 400000 ticks): Goughs dead by
t21000, Saint-Pierre captured t282000, Varzy captured, 100 barrels t354000 (5/6 WIN); objective 14 (des champs du nord,
entered while wandering) was left open — its hunt was added afterwards and not re-run; La Charité not reached. Easy not
measured. Pessimistic start: bribe only.

**Mission 7 update (MissionBot7c).** **Port bug fixed:** corpse removal (`World::tick` unit loop → `World::delete_unit`)
never took the dead out of its leader's team (GAMEPLAY_AI_TROOPS: `remove_member` + Unit_deinit 0x461f10 leave_team /
disband before the delete), so leaders kept deleted recnos in `members` forever (S7L1: Joan's 9 "members" were all deleted by
t20000, her troop never refilled). `IUnitBrain::removed`, called by `World::delete_unit`, runs `AiSystem::on_unit_removed`;
test `game.troop` (deleted member left the team). Plan fixes: `plan_talk` reset each tick (a stale one re-talked the potion
seller forever: the shop cut scene looped from t359000), HP food trip at < 10 (cap 60), light infantry (up to 3) in every
recruit, trebuchet crew = Joan's light infantry within 2000 (RTS assign branch A takes only `*LIGHT_INFANTRY*`; crew sent
from afar stops at (30425,15345) and its reservations block the attach points, so a fresh crew is preceded by
`rts_dismiss_crew`). Result (seed 1, Normal, legit-chain checkpoint): 7/10 WIN by t403000 (1 / 3 / 4 / 6 / 7 / 14 / 16 / 24 /
25: Begley dead, siege house captured, trebuchet built); not won. Open: the trebuchet gets 2 of 3 F crew (the third keeps
rejoining Joan's team), so it never fires at "wall2", the gates (event bit 26) stay shut, ~390 mercenaries, Gressart
(retreated to "gressart retreat 2") and Hudleg remain. The S7L1 test still asserts the bribe only.

**Mission 5 (S5L1, from `S5L1_des` / `S5L1_tactic` / `S5L1_finish.sce`).** No `SCRIPT` call reaches `S5L1_tactic2/3`
(Meung / Beaugency belong to S9L1, which continues in this world). Enabled at start: WIN 1 (Castle_Jargeau captured: the player
in `Castle_Jargeau` or `castleJargeau2` and `number_of_fightable_enemy_in_place` 0 in both; also sets the Jargeau barrack /
territory, which S9L1's main thread needs), LOSE 5 Joan, 14 La Hire, 17 Jean, 30 Alencon (game-over box on death), SIDE 18
(meet Dunois, way point 2; enables 19 d'Illiers, way point 3). WIN 34 William de la Pole (enabled by a French unit in
`meetSuffolk` or the `crossBridge` cut scene, finished at his retreat −1) and WIN 33 John de la Pole (enabled in `reachLord`,
finished at his death) join later; win delay 15 s. `autopilot_plan_S5L1.cpp`: hunts the fightable English left in the two castle
places without the generic 30 s give-up (the inner bailey is reached over the wall walk, a ~190-point path round the east walls),
ignores foes on the other side of the shut middle gate and way point 1 (at that gate) once inside, keeps swinging at William / John
above 40 % HP (they regenerate while the generic bot rests), eats or walks out of range when hurt under unreachable tower/wall
archers, and brings the West Left tower (inside `Castle_Jargeau`; its fall deletes team 80) down with the French cannons by an RTS
order (F2, select, right-click the tower, F2: `AiSystem::rts_attack_item`). Start checkpoint: the legit chain's `bot-S4L1.jsav`,
S4L1 forced (`joa-campaign --load bot-S4L1.jsav --win-at 10 --continue --save-at <transition+1>`; `game.campaign_S5L1_bot
--checkpoint OUT`): Joan level 11. Results (seed 1, 200000 ticks): Normal and Easy reach 18, 19, 33, 34 (John and William beaten,
t67000–166000) but time out on objective 1 — reachable outer-bailey remnants and the inner-bailey north-wall tower archers / polemen
(teams 57, 58, 79) are left; ordering the cannons straight after d'Illiers' talk lost them and d'Illiers (now only from inside the
castle, unverified). Pessimistic start (forced chain from a new game, Joan level 7, no food): Joan killed at t40729 on the way.

**Port bug fixed:** Unit_process 0x52c490 phase 0 clears the interact unit / item (+0x2e3 / +0x2e5, `mov word [edx+0x2e3],0`
at 0x52c4c4, `[eax+0x2e5]` at 0x52c4d0) before Unit_auto_pickup; the port never cleared them, so S4L1 (which, unlike
S1L1–S3L1, never calls `set_interact_unit_recno(main, 0)`) re-ran a shopkeeper's talk handler forever (buy screen
opening and closing, the bot frozen). `game.interact` checks the next tick clears both.

Seed sweeps (one attempt each, 90000 ticks): mission 1 **Easy 10/12** (seed 11 dies in the first tutorial fight,
seed 3 times out), **Normal 12/12**. Mission 2 from the chain checkpoints (`work/cb7/eval.sh`, RETRY seeds 1–12):
**Easy 2/12** won (seeds 2, 4), 1 timeout in stage 3, 8 lost on citizens, 1 the Bastard; **Normal 2/12** won
(seeds 3, 11), 8 lost on citizens, 2 the Bastard. A win takes 50000–81000 ticks (stage 4: the de Bourges / de
Blois garrisons; stage 5: William de la Pole, 580 HP). Holding Jean beside the Bastard took Normal from 0/12 to
1–2/12; buying explosive arrows before the armour from 0/12 stage-3 survivors to 1/12.

**Mission 3 (S2L1, from `S2L1_tactic.sce`).** A camp is captured (`set_place_captured`) once no unit of the camp's
nation (trigger mask 0x2443) and at least one hostile to it stands inside the place (`search_for_unit_in_place` "F" /
"E"); it is lost again the same way, and objectives 1 / 2 go back to NORMAL with it. Camp du pied fragile was captured
in every run (~t9000); the timeouts were the bot's: it walked off, and the three English waves sent after the capture
(dock teams 100–102, 120, 121, two minutes apart) retook the empty camp; it then chased foes it could not reach (the
prison archers behind the locked gate, which needs `KeyS2_01` or Jean, guards on the G hill) or stood at the closed
front gate of camp du beau vallon (English, 0 HP: the interact never opens a foreign gate without HP, FUN_0041af80),
whose way in is the back gate through the next nav level. No port bug was found on this mission. Bot changes:
**holds** (a finished WIN territory: enemies within 1500 count as on a ward, 4+ there call the bot back), **regions**
((nav level, path-mesh component) per point; level seams link regions of adjacent levels, ladders link a wall walkway to
the ground; `detour()` routes over them by Dijkstra and walks to the seam on its own level, then straight across, since
the engine plans a goal on another level over the coarse world mesh), ladders climbed / descended on purpose (R at the
foot facing it or at the top facing out, W / S on the rungs; never R at a ladder indicator in passing), gates the
player cannot open (foreign without HP, needing a key) never chosen, foes the path does not reach or chased 30 s in
all given up on for 60 s (doubling), the hunt ranked by distance to the open objective's territory (not `home()`) plus
region penalties (ladder +2000, other region +10000, unlinked +20000) and, while a territory objective is open, kept
30 s; on the way to that territory (> 2000 off) only foes within 800 are engaged; a follower not back within a 30 s
wait is not waited for again for 2 min. Seed sweeps from the chain checkpoints (`work/cb8/ev.sh`, 120000 ticks, RETRY
seeds 1–8): **Easy 8/8**, **Normal 4–6/8** (losses: Joan killed; timeouts: wandering between the camps).

**Mission 8 (S6L1, plan `autopilot_plan_S6L1.cpp`).** Win = 1 Auxerre (no ENGLISH in `S6L1_2_Check_Enemy_Place`, then Charles
inside it or in `S6L1_2_After_Auxerres_Place`), 2 Troyes (no ENGLISH in `S6L1_3_Capture_Troyes_Place`, Charles there or in
`S6L1_3_After_Troyes_Place`), 4 William de la Pole + Jean de Luxembourg dead (Reims), 7 Charles in `S6L1_8_Gate_Place` after a
Frenchman entered `S6L1_8_English_Spawn_Place`, 11 reputation > 79 (start 50; +3 Glasdale, +5 Auxerre, +2 elders, +2 montagne
du lion, +5 Troyes, +2/+3/+2/+3/+2 camps and towns, +5 Chalons, +3 Sept Saulx, +3 William), 13 Glasdale, 14 Fastolf; 6 / 9
(Charles, Alencon) must live. Troyes' walls fall to the montagne du lion trebuchets (camp captured + 3 LIGHT_INFANTRY attached
'F', tactic l.3990), gates only to La Hire / rams / siege fire. After 15 min without a count reset, Joan more than 11585 from
Charles spawns four level-22 assassin squads at him. Heroes never move unless the player orders them (RTS, scenario 5+): the
plan does the RTS round trip (Session::toggle_rts: select with team, right-click Joan = FOLLOW_UNIT 0x44f750, ground =
RTS_MOVE_TO), parks Charles/Alencon while enemies are within 1800 of them and outside the Auxerre trap at (19300,24790),
waits for / fetches the king, and shoots tower archers. Checkpoint: `work/MissionBot8/ck/chain.sh` (legit mission-4 checkpoint,
forced wins 4–7; Joan 13) and `ck/chain_pess.sh` (forced wins 1–7; Joan 10, 60 HP). Status (Normal, chain checkpoint): Glasdale
killed and Charles/Alencon alive through Auxerre, but Auxerre is never captured: the 12 archers on the four corner towers
(teams 203/205/216/223, z 524) are not reached by the nav paths and arrows from the yard hit the tower model (630 shots, 0 hits);
the script deletes a tower's team only at tower HP 0 (siege fire). Not done: tower access, the trebuchet crew (an assigned crew
standing at (40000,14500) gets LARGE_WEAPON_PUSH with a 1-point path and never moves), Troyes onward.
Auxerre towers (MissionBot8b): the tower tops are single nav polys linked to nothing (no ladder items in Auxerre), and the
four towers are the "middle" ones the Steam guide breaks with a Duke-built fauconneau/cannon (HP 200, stones only). The
archers stand in a roofed chamber (floor z 524, ceiling 664, windows ~z+50..+110). A player bow arrow (PROJ) is aimed at
pos.z + 0.8·Height = +80 = the top of the 80-high cylinder; its structure test is one FT-ms look-ahead ray along
(vx, vy, vz − V0) cast at launch, and from 800 off the solver lobs (minSin 0.25) into the ceiling. The plan now shoots
from walkway spots whose look-ahead ray is clear past the archer (within 700), but hits stay rare: in the port even a
stationary height-100 target on open ground takes only ~15 of 28 arrows (`work/MissionBot8b/ground.cpp`), because
`integrate_projectiles` tests the arrow as a point against [z, z+80]. The original's Shape_sweep_pair 0x6a1bb0 needs a
mover sphere (radius ≠ 0, else no hit) and FUN_00699a20 also tests the caps with that radius, and 0x514c40 rejects hits
only above z+Height(100). That suggests a port bug (arrow body radius missing), but the arrow actor's radius is not
decoded and no original measurement exists yet (recipe: tools/reference/diff/diffrun.py, `Orig(mission=4)`).

Mission 2: the bot meets the Bastard, buys 15 explosive + 40 plain arrows at the blacksmith (1980 →
0 gold), talks to the shop (stage 3), then 50 English/Burgundian units (teams 11–15, 31–33, levels 4,
John 10) spawn outside the gate and flood the city along four ATTACK_PLACE routes. They are frozen with
everyone else by the cut scene and then cross the ~2000 to the gate in ~100 ticks: units more than 4000
from the player run ×1.8 (Unit_move 0x4f0b50 LOD factor, CONFIRMED) and Joan is ~5000 away at the shop,
so ~37 are inside before anyone can reach the gate. Stage 4 needs every ENGLISH unit inside
First_Wave_Place — a box that also covers the spawn area outside the gate — dead, John gone (gate closed
after 10 s or John beaten) and the gate closed; objective 1 then finishes once no enemy is in the city.
Citizens (39) die at up to one per 3 s while ~30 enemies roam; the Bastard (level 3, 87 HP, two guards)
falls ~90 s after the attack when Joan is away from the town hall. Not done: stage 4's Orleans talk / the
two towns (the gate must be reopened), stage 5's cannon assault. A bot RETRY restarts with seed + attempt
(the original reseeds from `time(NULL)`).

**S3L1 after stage 3 (from `S3L1_tactic.sce` / `S3L1_finish.sce`).** Stage 4 (tactic l.2020): count time > 30 s,
objective 5 done, John gone, gate closed. Stage 4 → 5 (l.2095–2510): talk to the Bastard at the town hall
(way point 6578,5544: deletes `S3_Block`, enables 11 / 12), clear every ENGLISH unit from `S3L1_2_Town_1_Place`
(de Bourges) and `S3L1_2_Town_2_Place` (de Blois) (event bit 1, the way point comes back), talk to the Bastard
again (Tiger's eye, wall archers, 10 s pause), then with Joan and Jean inside `Check Enemy Place` / `Check Enemy
Place 2` and the gate shut the second wave (William, John, cannons, six reserve squads) spawns: stage 5. If Joan
is outside or the gate open at that check the Bastard says once "Return to the castle immediately, and close the
gate!". Stage 5: opening the gate (or the cannons breaking it) sends every squad at the town hall. Win: stage 6
= objectives 3 and 4 (`S3L1_finish.sce`): after 10 s of stage 5, `get_unit_retreat` of William and of John is
−1 (beaten with the retreat flag the script sets: Unit_die leaves them at 1 HP), each then gets a cut scene,
`del_unit` and WIN.

Bot changes for it: `travel()` also treats a path that leaves the walkable mesh (a step of > 200 in z: the wall
top to the coarse world-mesh leg outside the closed gate) as blocked and opens the gate; a gate the bot opened
is closed behind it once the player is 60+ inside (gate facing = outside; the interact refuses foreigners in
front) and heading inside; it is not opened from inside while 8+ enemies stand within 2500 outside (stage 5);
the path is replanned whenever any gate changes state; a re-shown way point is a new errand; an NPC is talked
to again after 100 s (the 30 s give-up timer restarts per approach); the "close the gate" message sends the bot
back through the gate it opened; resting with > 10 food eats; wards (allied heroes with retreat 0, friends a
way point follows) pull enemies on them to ×0.3 from up to 4000 and, swarmed by 4+, are run to; MISSION items
lying anywhere are fetched.

**Port bug fixed:** AI_energyAttack 0x4bca60 charges the EGATK_MP / EGATK_HP price only to units NOT hostile to
the player (Unit_isHostileToPlayer 0x5094d0); the port charged the hostile side and spared allies. William / John
at ≤ 20 % HP killed themselves with it (HP 0 without Unit_die, deleted next tick), so objectives 3 / 4 could
never finish (`game.ai_energy_cost`, GAMEPLAY_AI_TROOPS.md §20). Re-verified on the disassembly (0x4bcc75–0x4bcce6:
`ecx` = UnitArray_get(player), `push` the unit's recno, call 0x407ced → 0x5094d0, nonzero clears the pay flag) and
Script_can_energy_attack 0x652c10 (returns 1 for the player's enemies before any price check). The EP price is
`EP_CONSUME` (Inventory_aggregate_effect 0x15, 100 % without items) × max EP × EGATK_MP / 100 / 100 in both
(0x4bcc05–0x4bcc3d), now also for AI units (was the "100 %" hypothesis).

**Port bug fixed:** Unit_auto_pickup 0x49da30 only lets units of the player unit's nation (0x49da91: UnitArray_get
(player) +2 == unit +2) with LOD tier ≤ 3 (+0x22, within 8000 of the player) pick up floor items; the port let every
general (enemy captains too) pick them up. Jean picking up mission 1's dropped Imperial Scroll is original behaviour
(French, a hero: MISSION items are hero-only), and once he withdraws (`del_unit_team 2`, `jean_recno = 0` in
S1L1_tactic.sce) the gate check `get_item_quantity_in_unit_inventory(scroll, jean_recno)` fails, so the original
soft-locks the same way; the script's only other routes are the bearer himself inside "Messenger Detect" on the
first visit to "Castle Gate Place" (cut scene 3) or Joan carrying the scroll (cut scene 4). The bot therefore
escorts the bearer (waits for him, guards him as a ward) and fetches a dropped MISSION item itself.

Port bug fixed with the bot: F3 / F4 (`World::use_recovery_item`, 0x43b490 / 0x43b990) now go
through the potion effect `World::recover` (0x50b4f0 / 0x50ba00 / 0x50bcd0, called with force 0):
refused and kept at full HP / EP, team share (a leader gives amount/2 to every live member, a
member's surplus heals its leader's most hurt members), red 0x15 preferred over 0x19 (EP restored
too). The inventory right-click (`use_item`, force 1), the pickup of a fitting red potion (0x49da30,
0x50b4f0 force 1) and `is_in_danger` share that one implementation (`game.items_recovery`).
Earlier fixes: GAMEPLAY_IMPLEMENTATION.md "AI riders" (move-record lookup order, combo END_TIME,
last-waypoint radius, Unit_deinit mount unlink) and the `PHYS` save section.

**Mission 6 (S9L1, from `S9L1_tactic.sce` / `S9L1_finish.sce`; S5L1's world kept).** No objectives of its own
(`S9L1_des` is empty): S5L1's records stay, S9L1 enables WIN 2 Meung, 9 every north village (Fountainbleu, Rouvray, Checy,
Saint-Florent, Chateauneut, Blois, Bourges, Janville, Patay, Tours, Beaugency, Meung), 27 every south village (St-Aignar,
Chateaudun, Loches, Montargis, Jargeau), then 3 Beaugency (once Meung falls), 32 Talbot, 16 / 24 / 25 Glasdale, Fastolf, Scales
(spawned at levels 23-25 when a Frenchman enters `getReinforcement`); imperatives 14 / 17 / 30 / 36 (La Hire, Jean, Alencon, the
Bastard). A place is liberated when the player's hero (`search_for_unit_in_place` "P": Joan or a hero the player switched to)
stands in it with no enemy left in its box (`number_of_enemy_in_place`, siege weapons
and barracks queues count; Meung / Beaugency count fightable units only: 318 / 268 at the start). Meung, Loches and Montargis
are only scripted inside `if Castle_Jargeau captured` (l.2292), which only S5L1's capture code sets (S5L1_tactic l.4259): a
`--win-at` S5L1 win makes S9L1 unwinnable. Meung's gates (English, 1500 / 3000 HP) take only La Hire's melee, explosives
(ranged base damage / 2: ~5 per arrow for Joan) and siege shots; the scripted ways in are the spy (talk to Simon Ferron after
Loches, pay Elya Tinchor 3000 at Selles-en-Berry, keep French units out of `attackMeung` until he opens the south gate) or the
Bastard's cannons (controllable via RTS after meeting him at `meetOrlean`). Beaugency additionally needs bit 19 and bits_2 30 / 31.

Plan `autopilot_plan_S9L1.cpp`: the nearest open place to the player's hero (prerequisite captured, Meung only with a gate open;
skipped 5 min when its remaining enemies are all unreachable, after 90 s without headway or 20 s inside without capture), its
garrison first (foe weight 0.3, siege weapons 0.5 in towns), then a walkable point inside the box; a walk without headway for
20 s takes `Autopilot::detour`'s first level-seam crossing (the engine plans a goal on another level over the coarse world mesh,
from Orleans straight over the Loire); markers other than 8 (spy) / 9 (final camp) ignored (`MissionPlan::use_waypoint`); a
buy screen buys food (keeping 3000 gold for the spy), but no one is sent shopping (every food trip ended badly: Joan and Jean
to Jargeau's shop through the English villages died, Jean at Orleans' shop ended on its wall walkway with no way down).
**Army:** units in a map level the player is not near are deactivated (`NavWorld::stream`, Unit_deactivate 0x41fcc0) and ignore
their orders, so the heroes at Orleans never come to Joan at Jargeau (FOLLOW_UNIT also paths with a 1000-node budget); the
march between the two killed the hero leading it every time. The player therefore switches to the sturdiest hero at Orleans
(`Mission::switch_hero`, Jean) and plays from there; Joan waits in Jargeau (frozen). Every other hero within 4000 is assigned
with his squad to follow the player (RTS select with team + right-click the leader, Rts_cmd_assign 0x44f750 branch B); a hero
below 150 max HP stays home (Alencon: 106 HP, died in the first fight east of Orleans); one below 35 % HP is RTS-moved with
his squad to where the plan first saw him until above 80 %. Any hero's death is game over (S9L1_finish `call_gameover_msg`).
Not used: barracks recruits. Refilling the leader's troop from Orleans' barracks by RTS (`FacilityOrder::barracks` / `more` /
`commit`, macemen) paid 150 gold every 30 ticks while the troop stayed short (the recruits spawn behind Orleans' shut gates,
far from the troop at Rouvray), and the run that recruited lost Jean at tick 8294 with nothing liberated. Not done: the spy
route and the Bastard's cannons into Meung, Beaugency and the bosses.

**S5L1 win of the start checkpoint (not a port bug).** `game.campaign_S9L1_bot` builds its start from a new game: missions 1-4
won by `finish_objectives`, S5L1 by removing Jargeau's garrison and putting Joan (with her squad) on the castle ground; the
script then captures it and finishes objective 1. The chain failed with finish 2: the WIN fires only after the 15 s win delay,
and the idle award-only Joan (level 7, 60 HP, no input) was killed by two ENGLISH_LIGHT_INFANTRY walking in from ~1300 away
(LOSE 5 "Joan of Arc is defeated"; `process_end_conditions` as documented in §3). The hook also removes hostiles within 2500
of the castle. Result (Normal, seed 1, 90000-tick budget, ~3 min): not won; Rouvray liberated, 18 kills, lost at tick 41293
(Jean, the leading hero, killed near the eastern bridge). The heroes (levels 8-10, award experience only) are outmatched by
the English garrisons (soldiers level 12+, bosses 23-25); the test asserts a win or at least one place liberated.

**Port bug fixed (mission 6):** the win freeze set every unit's task to IDLE prio 127 also when leaving scenario 5, so in S9L1
no enemy ever fought back (Joan took 0 damage in 20000 ticks). GameOver_win_freeze_units 0x6e7900 skips the order clear and
`Unit_set_task(7, 0x7f, 1)` when `DAT_00a58fa7 == 5` and only calls `Unit_stop(0, 1)` (`game.campaign_S9L1_world`).

## 9. Gaps

- Multi-node worlds (scenarios 2–8 have several `.wor` nodes): terrain and nav mesh come from
  the first node only; node coordinate offsets are not applied to `.uni` placements (HYPOTHESIS:
  coordinates are world-absolute as in the engine's world scene).
- `change_scenario_step`: every shipped campaign file has one step, so step switching is not
  exercised.
