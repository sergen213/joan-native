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
  `Victory` event; the WinMsg stops every unit (`AiSystem::set_idle`).
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

- **Points:** every attribute point into HIT_POINT (+4 max HP each).
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
- **Allies:** waits for (regroups with) an ally hero only while it follows the player (FOLLOW_UNIT).
- **Objectives:** the nearest active minimap way point not yet stood at for 10 s (mission 2: blacksmith
  before the shop, so the buy screen comes before the shop talk that starts the attack); near it, NPCs
  showing the talk icon (`can_interact`) are walked up to and talked to once; then the open WIN
  objective's territory, the nearest enemy, items.
- **Shopping:** while the buy screen is open (`start_buy_interface`), once per opening, the bot makes
  the click handler's purchase (FUN_006c5a00: `World::give_item` of the item's Quantity at the stock
  level, gold −= Price) from `shop_stock()` (mission_ui.hpp, the stock list the engine buy screen also
  reads): a heavier armour than worn (then worn, `World::use_item`), explosive arrows up to 20 (packs of 5
  for 500: 15 with mission 2's 1980 gold), plain arrows (10 for 100) with the rest.
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
  With nobody in reach the bot hunts the enemy with the smallest distance to the player + to `home()`
  (centroid of the units not hostile to the player: the town under attack before the next towns'
  garrisons), enemies on another level +5000.
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
with his 90 arrows is the untried switching use (the bot would play Jean, Joan held). Not done yet:
spending skill points (Learn / ADD: combo levels ×(lvl+10)/11 damage, RUSH from level 4), selling (the buy
screen sells at Price × SellingRatio), hero-to-hero item transfer (not modelled by the port), siege use,
RTS orders, mounting, bribes, choice dialogs (headless answers choice 1). The interact prompt (R) also
recruits. `JOA_BOT_TRACE=N` prints a status line every N ticks plus hits taken / dealt, hits on friendly
heroes (attacker team), deaths (killer team), spawns, interact prompts and the player's shots / projectile
hits; `JOA_BOT_DBG=T` prints 200 ticks of per-frame inputs and nearby enemies from tick T.

`game.campaign_legit` runs the chain from a new game (Normal, 40000 ticks per attempt, 2 retries) twice,
checks determinism and no script errors / unimplemented builtins, and asserts `kExpectedWins` (1).
`joa-campaign --autopilot --difficulty D` measures other difficulties (seed 1, 40000 ticks, 2 retries):

| mission | difficulty | result | ticks | kills | HP dealt / taken | cause |
|---|---|---|---|---|---|---|
| 1 S1L1 | 0 Easy | WON | 18024 | 38 | 2803 / 95 | — |
| 1 S1L1 | 1 Normal | WON | 26530 | 38 | 2665 / 178 | — |
| 2 S3L1 | 0 Easy | not won | 40000 | — | — | see the seed sweep below |
| 2 S3L1 | 1 Normal | not won | 40000 | — | — | see the seed sweep below |

Mission 2 from the chain checkpoints (the bot's own mission-1 result), RETRY seeds 1–12, 40000 ticks each
(`work/cb5` probe): **Easy** 6/12 survive the first wave into stage 4 (6 lost on citizens); **Normal** 3/12 reach
stage 4, 8 lose the Bastard (swarmed at the town hall ~t9000–11000 by team 32's ten polemen and team 31, while
Joan fights at the gate), 1 on citizens. Stage 4 runs then time out: Joan hunts the de Bourges / de Blois
garrisons (opening the gate herself) but no run reached stage 5 in the budget. Before this round's fixes
(clear shots, wall-top foes, gate travel) Normal lost 11/12 in stage 3.

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

Port bug fixed with the bot: F3 / F4 (`World::use_recovery_item`, 0x43b490 / 0x43b990) now go
through the potion effect `World::recover` (0x50b4f0 / 0x50ba00 / 0x50bcd0, called with force 0):
refused and kept at full HP / EP, team share (a leader gives amount/2 to every live member, a
member's surplus heals its leader's most hurt members), red 0x15 preferred over 0x19 (EP restored
too). The inventory right-click (`use_item`, force 1), the pickup of a fitting red potion (0x49da30,
0x50b4f0 force 1) and `is_in_danger` share that one implementation (`game.items_recovery`).
Earlier fixes: GAMEPLAY_IMPLEMENTATION.md "AI riders" (move-record lookup order, combo END_TIME,
last-waypoint radius, Unit_deinit mount unlink) and the `PHYS` save section.

## 9. Gaps

- Multi-node worlds (scenarios 2–8 have several `.wor` nodes): terrain and nav mesh come from
  the first node only; node coordinate offsets are not applied to `.uni` placements (HYPOTHESIS:
  coordinates are world-absolute as in the engine's world scene).
- `change_scenario_step`: every shipped campaign file has one step, so step switching is not
  exercised.
