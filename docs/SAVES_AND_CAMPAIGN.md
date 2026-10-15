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
  the gate") is closed (walk to 100 inside it, towards `home()`, R) `gate_delay` after it appears (default 30 s; the
  mission-2 plan closes it at once). `travel()` (way points, territories, hunting) opens a closed gate whose
  shut passage ends the navmesh path short of the goal (or on another level); a gate the bot opened
  itself is not closed again while an enemy lives within 4000 outside it.
- **Navigation:** stuck twice on the same line (house corners the path mesh cuts) → run off sideways,
  alternating sides, further each time, then replan.
- **Loss:** a finish 2 or the game-over box (`call_gameover_msg`, e.g. a hero that must live died or too
  many citizens were killed; it freezes every unit even when no LOSE record ends the mission).

Hero switching is `Mission::switch_hero` (the key path, RTS commander pick and rts_deinit call it); hold is
`AiSystem::toggle_hold` (Shift+N). **Mission 2 plan (`autopilot_plan_S3L1.cpp`, measured 2026-10-15 on the mission-2
checkpoint, one attempt per seed, after the hit-time food fix below and ItemTransfer's `World::transfer_item`).** Stage 3
(the walkthrough's route): Joan closes the gate at once (never closing: 0/48 seeds survive), hands Jean her explosive
arrows on the character screen (`World::transfer_item`, minimap zoomed out: zoom 0 maps the whole map onto one 200-px
tile, so the transfer's 100-px reach covers the town), stays on hold there and the player takes Jean (key 2): the
Bastard's attackers first, then every foe within 6000 of him by distance / (1 + citizens within 1500); none left: the
generic hunt. Survives stage 3 in **22/48** seeds (Joan as the player, Jean held: 9/48; without the transfer 3/48; Joan
not held 12/48). Back to Joan for stage 4 (Jean playing it too: 3/48 won instead of 11/48). Stage 4: Achard's 20 explosive
arrows are kept; his buy screen spends the gold on plain arrows first (`shop_plain_first` 250, before the JOAN_ARMOR_LV2).
Stage 5 (walkthrough): Jean (key 2, RAPIDFBOW, 90 arrows; Joan held inside) then Joan shoot from the wall walk beside the
gatehouse ((7640,11300) or (9280,11320), z 517, inner edge: the gatehouse deck dropped a knocked-back shooter outside) one
crewman of every manned cannon (it fires only with `siege_slots_full`): the gate then holds at ~800–1400 HP for the whole
stage. A far lord gets one pull shot every 30 s and then walks up to the gate's outer face, where he is shot within 900;
nothing to shoot, no arrows or < 35 % HP: wait just inside the gate. When Jean's turn ends his bow and arrows go to Joan
(the walkthrough gives her the L2 bow): the lords now eat their food when hit and William, back at full HP, otherwise
waited outside the gate once Joan's arrows ran out (6 → 11 of 48 whole-mission wins on one checkpoint). Limits: a lord
idle at the gate blocks arrows from the front (`missile_block`, GUARD within BLOCKANG), ~2.5 damage per plain arrow; Joan
enters stage 4 at a lower level (Jean took the stage-3 kills) and dies there in ~6 of 22 runs. Whole mission: **6–11 of
48 seeds** won depending on the checkpoint (per attempt 12–23 %). **After the path-budget fix** (AI A* gets each
order's budget, `AiSystem::move_to`; the stage-4 strays now outlive the town fights): Achard's buy screen gets bag room
first (`make_room`; it had opened on 42 items and bought nothing); after the second Bastard talk (stage 4, objectives
11 / 12 done, his talk flag cleared) the bot guards him and leaves foes outside alone (hunting them kept Joan out, and
the script's "Return to the castle" comes once), Jean — held wherever stage 3 left him, often outside the gate or pinned
in its passage — is walked inside (key 2; R at the gate to get out of its passage / shut it behind him) and held there,
so stage 5's check (Joan and Jean in Check Enemy Place, gate shut) passes; crews beyond 900 are shot with explosive
arrows when no plain ones are left; out of arrows with a lord's escort down to 3, Joan goes out through the gate and
beats him (William had waited at the gate at full HP for 110000 ticks). 24 seeds on the chain's mission-2 checkpoint:
4 won, 11 lost in stage 3 (citizens), 9 lost in stage 5, **no timeouts** (were 5–7). Not done yet: learning
new upgrades (RUSH from level 4, BB), selling (the buy screen sells at Price × SellingRatio), siege use, RTS orders,
mounting, bribes, choice dialogs (headless answers
choice 1). The interact prompt (R) also recruits. `JOA_BOT_TRACE=N` prints a status line every N ticks (with
the ally hero's task and target) plus hits taken / dealt, hits on friendly heroes (attacker team), deaths
(killer team), spawns, interact prompts and the player's shots / projectile hits; `JOA_BOT_DBG=T` prints 200
ticks of per-frame inputs and nearby enemies from tick T.

`game.campaign_legit` (CTest fixture `campaign_checkpoints`) runs the chain once from a new game through missions
1..`kExpectedWins` (4) (Normal, 200000 ticks per attempt, up to 8 retries of a lost mission: the game-over RETRY a player
would press, reseeded seed + attempt; a timeout is not retried), writes `campaign_checkpoints/bot-<stem>.jsav` at every
mission start — the next mission's too (`bot-S5L1.jsav`, where the later missions' own segment fixture chains start) — and
`campaign_checkpoints/chain.txt` (one report line per mission) into the build tree (`build*/src/game`), checks no script
errors / unimplemented builtins and asserts all `kExpectedWins` won. `game.campaign_legit_mission_N` (N = 1..`kExpectedWins`;
the CMake RANGE follows it) loads mission N's checkpoint, replays it and must reproduce the chain's line exactly (result,
ticks, hash: determinism through save / load); skipped when the chain did not reach N. A later mission's owner raises
`kExpectedWins` (and the RANGE) once it wins reliably within these budgets. Iterate on one mission without rerunning the
chain: `ctest -R legit_mission_4 -FS campaign_checkpoints`. Report
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

**Port bug fixed (AI heroes never ate):** Unit_onMessage 0x75d540 calls Unit_is_in_danger 0x4fd980(1) after every hit
(melee, down attack, arrows / stones / explosive, keg), before its death check. A non-player unit below INDANGER (0.2)
eats: a hero (rank > 2) from his personal items (inventory +0x16), then his troop supply (+0x1a); a soldier only from his
leader's troop supply (the port's bag: items on grid 2). The argument also lets 0x50b4f0 feed a unit the blow took to 0 HP,
so a hero carrying food survives it. The port only ran it from the unused script builtin, so AI Jean / the Bastard / La Hire
and the bosses (John, William, Talbot, Glasdale, Alexander carry 1–3 food) never healed (`World::hit_danger`,
`game.hit_danger`). Effects: mission 2's AI Joan at the gate survives the walkthrough's route; the bosses now outlast a few
more arrows / blows.

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

Status (MissionBot4e, Normal, chain checkpoint: Joan level 10, JOAN_SWORD_LV1, 31 food, 16540 gold; `work/MissionBot4e/w4.cpp`
plays a save per seed): **won** in the chain (attempt 2, t184671), 6 of 12 seeds per attempt within 200000 ticks (wins at
109000–197000), no deaths on the last sweep; after the path-budget fix (MissionBot4f): attempt 1 (t146254), 7 of 12 seeds
(3 deaths before the Loire, 2 timeouts). Plan changes: before Talbot, the two Tours sellers once on the Loches road (y >
11000; from the north the walk led through Glasdale's camp) — JOAN_SWORD_LV4 (1480 gold: 4x the LV1 damage), food best-heal
first (meat / chicken heal fully), 100 plain arrows, no explosive ones (the 30 carried go into volleys); bag room through
`Autopilot::make_room` (apples beyond 5 and spare weapons dropped, food into the troop grid, the personal grid repacked
bigger-first: the sword's 2x4 block never fit between 29 loaves); Talbot (940 HP, eats a full-heal meat below 20 %, retreats
uphill at 50 % behind 14 level-15 crossbowmen and regenerates) is fought whenever wounded within 1500 on her level while she
has food or 40 % HP (the generic bot had given him up and rested beside him while he hit her); rests stop at 50 % and eat
from 4 food (`Autopilot::rest_pct` / `rest_food`; the 70 % rests at 1 HP / 150 ticks cost 20000+ ticks each); out of food
below 30 % she rests in place until 60 % fighting only attackers (the generic retreat ran back to La Hire through the
English); food up to 30, a potion seller from anywhere below 3 food after Talbot; explosive arrows up to 40 with 5 kept for
the Abtower; once the tower is down she stands by Place Cannon 1 for up to 12000 ticks so Cannons 2 / 3 break the gate before
Augustin's capture drives them off (a gate still up after the capture cost 40000+ ticks of arrow trips); a hunted foe without
300 of headway in 3000 ticks is given up on. Pessimistic start (MissionBot4d's checkpoint: level 10, no food) wins 1 seed of
4: the other three die on the way to Talbot (Patrol Area 1 / Alexander's camp) before any shop. `kTicks` is 200000.

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

**Mission 7 update (MissionBot7d): S7L1 won.** The trebuchet's third man was not a port bug: Joan's interact key took him back.
`Unit_find_recruitable_unit` 0x48dfb0 (decompiled, GAMEPLAY_AI_TROOPS §4.4) has no task test, so a light infantryman walking to
the weapon (LARGE_WEAPON_PUSH, not yet attached) shows the recruit prompt within 100 in front of the player, and the core
pressed R on every prompt (gdb: `recruit_nearby` → `join_team(#1407, Joan)` every 300 ticks). The core now skips a recruit
prompt on a man with that task. **Port bug fixed:** Rts_cmd_assign 0x44f750 branch A (GAMEPLAY_IMPLEMENTATION "Siege crews")
frees the point of a stale reserver when no free one is left, detaches a man from another weapon and takes only ENGLISH /
FRENCH / MER_LIGHT_INFANTRY; the port did none of it (crew stuck on the way kept the points reserved — the plan's dismiss
workaround is gone); test `game.siege_crew`. Plan fixes: no barracks trips once the gates are open (42 % of the time went
on them); supply trips to Alfonse (HP food, now up to 120) and Wardley Orfroisier next to him (bolts: the whole La Charité
phase had been played with none — its wall archers and tower crews are shot, not reached), a trip gaining < 200 in 3000 ticks
given up for a while (a platform inside La Charité walked the seam detours for good); the stall watchdog restarts on 100 of
headway (it gave up the long walk to the summit poleman at (1329,13017,4642) after 30 s); a hunt that shot 20 s without
effect from up to 1500 (bolts drop, `clear_shot` sees no rock) closes to 500 first. Result (Normal, seed 1, from the legit
chain's `bot-S4L1.jsav`): gates open t300990 (`--charite`: 408 s wall), Hudleg t338988, Gressart t544414, La Charité
t739631, **won t740086**, no deaths. Test: `game.campaign_S7L1_charite` (fixture) plays to the gates and saves
`campaign_checkpoints/bot-S7L1-charite.jsav` (asserts the bribe without the alarm); `game.campaign_S7L1_bot` plays on from it
and must win with objectives 2 / 5 / 26 done within 700000 ticks (439096 ticks, 270 s wall). Easy and the pessimistic
start (Joan level 9) not measured.

**Mission 7 update (MissionBot7f): S7L1 won again.** No port bug. Why the bot had stopped winning:
- **Hunt stall:** Joan stood at (40012,19788) on a Saint-Pierre wall from t60000 to t382500 "hunting" the garrison in the
  yard 350 below. The cause was the plan's `hunt_in`, which shot at anything in a clear line, with an empty ammo slot. The
  chain brings no arrows. Each target was given up after 30 s and the next one taken, so the hunt cycled through 18 of
  them. `Autopilot::travel` was not involved. Fix: the hunt shoots only after `load_ammo` succeeds, otherwise it walks in.
- **Supply trips:**
  - The resupply walk out of La Charité had no headway check. From Gressart's platform it circled, ate every food item
    and died.
  - A rest 700 from Alfonse counted as a stalled trip, so the bot left without buying.
  - A barracks trip to the start camp from the walled-in hill stood 1.3 M ticks.
  - Fix: one `trip_stuck` clock (200 gained in 3000 walked ticks, restarted after any interruption) for shop,
    resupply and barracks trips.
- **Inner gate:** Joan killed the Goughs on the hill side of the shut "StPierre gate 2" and was walled in there.
  `gate_job` now also sends La Hire when Joan is within 4000 of the gate and not linked to his region.
- **Gold and strength:**
  - Recruit refills other than the trebuchet crew keep 15000 gold back for food.
  - Joan's attribute points go into STRENGTH up to 60 (new `Autopilot::attr_kind`, default unchanged = HIT_POINT).
    Melee attack is STR × 3 + sword + 100, and the chain's Joan has STR 9.

Start: the chain now wins missions 1-4, so the fixture starts from `bot-S5L1.jsav`, with missions 5-6 forced. That Joan
is level 18 with JOAN_SWORD_LV4, no arrows, 9 men.

Result (Normal, seed 1):

| Test | Ticks | Wall time | What happened |
|---|---|---|---|
| `game.campaign_S7L1_charite` | 197129 | 285 s | Goughs dead t18513, Saint-Pierre t63369, gates open t197129 |
| `game.campaign_S7L1_bot` | 269068 | 220 s | Begley t202150, Hudleg t392293, Gressart t395722, La Charité t465742, **won t466197**; no deaths, 485 kills |

Tick caps: 400000 for the fixture, 500000 for the main test. Both tests have a 600 s timeout.

Still not done:
- The bot's buy screen restocks on every visit. The engine's `CharacterWindow` keeps the stock left per shop for the
  window's lifetime, and the original's persistence across visits is not confirmed.
- The run from `bot-S4L1.jsav` (Joan level 12, sword LV1) has not been measured with the final plan.

**Mission 5 (S5L1, from `S5L1_des` / `S5L1_tactic` / `S5L1_finish.sce`).** No `SCRIPT` call reaches `S5L1_tactic2/3`
(Meung / Beaugency belong to S9L1, which continues in this world). Enabled at start: WIN 1 (Castle_Jargeau captured: the player
in `Castle_Jargeau` or `castleJargeau2` and `number_of_fightable_enemy_in_place` 0 in both; also sets the Jargeau barrack /
territory, which S9L1's main thread needs), LOSE 5 Joan, 14 La Hire, 17 Jean, 30 Alencon (game-over box on death), SIDE 18
(meet Dunois, way point 2; enables 19 d'Illiers, way point 3). WIN 34 William de la Pole (enabled by a French unit in
`meetSuffolk` or the `crossBridge` cut scene, finished at his retreat −1) and WIN 33 John de la Pole (enabled in `reachLord`,
finished at his death) join later; win delay 15 s. `autopilot_plan_S5L1.cpp`: hunts the fightable English left in the two castle
places without the generic 30 s give-up, nearest by navmesh path length (straight-line picks bounced between the wall walk and the
west gatehouse archers above it), ignores foes behind the shut middle gate and archers on a standing guard tower, drops way point 1
(at the middle gate) once Joan has been inside (walking back to it whenever a hunt led past the north wall cost ~40000 ticks),
keeps swinging at William / John above 40 % HP (they regenerate while the generic bot rests), eats or walks out of range when hurt
under unreachable archers, rests at the siege camp before the west walls when out of food below 30 % (the generic retreat walked
to Dunois' camp 15000 off). RTS every 20 s once Joan is inside (F2, select, right-click, F2): the crewed cannons at a tower
holding castle foes (West Left: its fall deletes team 80), else at the middle gate; every other selectable troop (no hero, not
Dunois / d'Illiers: sent along both died by t30000) attack-moved at the nearest ground castle foe a path reaches; a troop that
dropped its order without moving (units in unloaded far levels) is left 5 min. Tower archers are unreachable in the original
too: the West Left top is a 2-poly nav island with no .pfd link and S5L1 has no ladder items. Start checkpoint: the legit chain's
`bot-S4L1.jsav`, S4L1 forced (`game.campaign_S5L1_bot --checkpoint OUT`): Joan level 11, ~31 food. Results (seed 1):
**Easy won** at t158169 (Jargeau captured; 18 / 19 / 33 / 34 done; Dunois alive, d'Illiers dead), S9L1-start save
`work/MissionBot5b/bot-S9L1-easy.jsav`. The plan stocks up first: with no foe near and bag room it breaks any heal-food box
within 3000 (8000 below 10 food) and walks over the drop (147 food boxes on the map; Orleans alone holds 18, behind its
East Gate, which the plan's walk hands to the generic interact to open and close), drops its spare apples (blue EP food,
5 kept) and spare swords at the Orleans potion seller and buys his heal food, the best heal first, as far as the
personal grid holds (World::buy_item; one cell a loaf since the bag-room rules), visits the weapon seller (way points
wait, 5000 gold kept; no arrows are bought since the bag rules: the bag is full of food by then), then leaves by the East
Gate: ~51 food (23 at the start; 37 with the apples kept). The tower blocker (MissionBot5c: West Left at 40/200 HP with
every cannon's crew dead; arrows do the tower 0 damage, S5L1 walls take only stone / keg hits; S5L1 has no kegs) gets a
re-crew: while fewer than 3 cannons within 8000 of the target are manned or have a crew walking to them, the nearest
commandable French light infantry (loaded level, not task-locked: Louis Barbier's team 582 is `no_react`) are assigned to
the idle cannon nearest it (Rts_cmd_assign). It only helps while such infantry live: by the time the crews are dead the
light infantry near Jargeau usually are too (Joan's two left in Orleans never leave their unloaded level). 40 explosive
arrows from the bailey took one tower archer from 123 to 7 HP (they regenerate), so the bow is no way out either.
Results, Normal (difficulty 1) from the `work/MissionBot7/legit2` mission-4 start (before the re-crew rule): **seed 1 won**
at t187953 (tower down
t40000 by d'Illiers' crews, Jargeau captured, 18 / 19 / 33 / 34 done, Joan level 20; S9L1-start save
`work/MissionBot5d/bot-S9L1.jsav`, which `game.campaign_S9L1_bot` now prefers), **seed 3 won** at t218708, seed 2 lost
(Joan killed at t156877 in the inner bailey, 3 food left), seed 4 stuck on the West Left tower at 128 HP (no French light
infantry left near Jargeau to crew a cannon). Easy, seed 1: won at t237100.
`game.campaign_S5L1_bot` plays 40000 ticks (a win takes 160000-240000, past a test's time) and checks progress there: not
lost, ≥ 10 heal food bought in Orleans, 18 / 19 met (d'Illiers at ~t34000), ≤ 200 fightable English left (122-186).

**Bot fix (bag rules):** `Autopilot::loot` walked to floor items the bag could not take (Unit_auto_pickup leaves them
lying): Joan stood 45000 ticks beside her dropped apples. It now only goes for gold, MISSION items, red food that fits the
HP missing, or what `World::give_item` takes on a copy of the unit.

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
Re-measured with the sphere-vs-cylinder arrows (MissionBot8c, `work/MissionBot8c/scan.cpp` / `boom.cpp`): from 128
walkway / yard spots around tower 1 only 3 put a plain arrow into its chamber (about one hit in six shots there), and
explosive arrows splash ~20 HP per shot over a tower's 3-4 archers (≈170 explosive arrows, 85000 gold, for the four).
The plan takes the walkthrough's route instead: when Joan passes the hut the player switches to the duke (IRON workshops
open only to Alencon within 1024, 0x43c080), who orders two CANNONs at `S6L1_2_Iron_Workshop` (4000 iron, 1500 each);
two of Joan's FRENCH_LIGHT_INFANTRY (the only crew class, Rts_cmd_assign 0x44f750) crew the first at once. An idle
crewed cannon goes for any enemy item within 4000 (AI_cannon_default APPROACH_ITEM), i.e. the gate under the gate-tower
archers (teams 224/225, TOWER_ATTACK range 2000) who killed the crew, so it waits at (16300,26300). With only tower
archers left it is RTS-moved to a gun spot whose muzzle-to-aim line meets the tower first (from most of the yard a
building takes the stone) and sent at the tower: five stones (40 HP each) per tower, the script deletes its team. The
wards park at (18700,25300), out of the gate archers' reach (the old (19300,24790) spot got Alencon shot dead). Status
(Normal, legit chain checkpoint, drive loop): Glasdale t20807, elders t26666, all four towers t45000–58000, Auxerre
captured and Charles there t58068 (rep 60), Fastolf (objective 14) t91836; then the king died: following Joan past
Croncels (Troyes' wall crossbowmen), or parked west of Croncels to the 15-min assassin squads while Joan fought inside
Troyes. A food trip to the start's potion shop never opened the buy screen: the plan's `first()` steered every tick, so
the generic interact (after `first()`) never pressed R at the seller's prompt; it now stops within 250 of the seller.
game.campaign_S6L1_bot was then a single 120000-tick run checking no defeat, Glasdale dead and a cannon built / tower
down; MissionBot8f split it into segments (below).
MissionBot8d: the "test vs drive loop" divergence was no nondeterminism. The same start reached in process (as the test)
and saved/loaded (as drive) hashes identically for 70000 ticks (`work/MissionBot8d/div.cpp`), and drive from the
current fixture sticks at "cannons at tower 59" exactly like the test. 8c's good run started from an older fixture
(`ck-l.jsav`) and older code. That stall had three causes: (1) the trap shut the gate (now English) on the crewed
cannon outside, and the RTS move then had no path (path 0, RTS_MOVE_TO forever); Joan, 500 off it on the walkway, never
travelled. She now opens the gate from 150 inside it (the interact opens a foreign gate from its back side). (2) Every
light infantryman died (crew under the gate-tower archers, the rest in the trap fights), and nothing else crews a gun.
The cannon now stays parked while anyone hostile is within 2200 of the gate. (3) The 14 gate-tower archers (teams
224/225, z 544) stood until heroes on the walkway happened to kill them. Joan now walks up and fights them
(`gate_towers`). From there Auxerre fell at t73867 (rep 58). The crew can still die to a corner tower's archers at a gun
spot 1400 off (tower 58, r9). Spots beyond their TOWER_ATTACK 2000 are not tried yet.
After Auxerre (plan code in place, not yet seen through): shop errands (start camp: JOAN_SWORD_LV6, 60 dmg against
the chain's LV1 10, and food. Auxerre: RECURVEBOW 38 vs RAPIDFBOW 16, JOAN_ARMOR_LV3. Troyes food. Bussy JOAN_SWORD_LV9)
and Auxerre recruits. Troyes' men are left alone while Wall3 stands: unprovoked, she fought in through the gate to
Fastolf, ate all her food and fled at 4 % HP. The wards park in Auxerre's yard and then at (36000,16000), which keeps the
hill camp and Troyes within the 11585 that triggers the assassin squads; an archer killed the king at the old
(33000,25000) park. They also get Joan's food (`transfer_item`). Then the six light infantry at
`S6L1_3_Join_Troop_Place`, the hill camp hunt, three crew per trebuchet, and the Troyes hunt. Seen working so far: the
join (six spawn, wards walk south) and the start of the hill approach.
MissionBot8e: shops never sold: the talk prompt needs the seller within 100 and ahead (talk_handler), the plan stopped at
250, and the chain's Joan arrives with a full bag (now `Autopilot::make_room` first; the start camp sells
JOAN_SWORD_LV6 and arrows: Joan had none, so she could not shoot the hill camp's tower archers). Joan's troop is full
(9/9), so Auxerre's barracks recruits nobody for her. Cannon crews (`unstick_crews`): crewmen who neither attach nor get
nearer their gun for 60 s were seen (a) by the gun with its point off the walk, (b) 17 off a free point that a
second crewman's stale `push_slot` shadowed, (c) blocked by the second cannon parked beside it. The whole crew is stood
down and assigned afresh once, then the cannon is given up on; only one cannon is crewed at a time. Joan's light
infantry join the king's troop at the start (all nine died in her fights before). Auxerre fell at t60359 in one run
from the mission-8 start (r4). The 15-min assassin squads come from count_time[0] (restarted at Auxerre's / Troyes' fall
and after each squad): past 840 s the wards follow Joan instead of parking and she stays within 10000 of the king (the
ring road to the hill camp ran 13300 from kSouthPark and squads hit the park). The six Troyes light infantry
(join_troop_flag = event bit 11) move to the king's park at once (the six died where they spawned). Plan code for the
road north is written but has not run yet: Bussy, camp du marais, par le lac and Sept Saulx, plus camp du Champagne / Chalons
while reputation is short, then the two lords and the king to Reims' gate (`road_stage`). Errands for shops more
than 15000 away are skipped (a plan started from a mid-mission save walked back to the start camp).
MissionBot8f. **Stuck crews are faithful, not a port bug:** OrderExec_move_to_target 0x4bfa10 ends a MOVE_TO_TARGET
within 2 x REACH_DT (30) for a unit at LOD > 1 (+0x22 >= 2: beyond 2000 of the player), while AI_large_weapon_push_inf
0x535020 attaches only within REACH_DT (15); Unit_update_location 0x4fee60 (crew snapped onto its point) and
get_nearest_attach_point 0x656dc0 match the port. A crew assigned away from Joan stands ~17-30 off its point until she
comes within 2000. `unstick_crews` gave up both cannons for exactly that; it now judges only crewmen within 2000 of her
(LOD 1). Auxerre's last two wall archers (BUR_ARCHER, team 218, (29600,22300,512)) stood 11300 from the king's park
while the assassin rule pulled Joan back at 10000 (35000 ticks wasted); the rule now uses the script's 3-D distance
(get_unit_dist 0x653150) with 11400. Auxerre (objective 1) from the mission-8 start: t63821 (was t98143). In the Troyes
stage the wards stay parked at kSouthPark (following her they were led into the hill fights and pulled her off the
trebuchets). The trebuchet crew is led (`lead_crew`): every crewman walking alone to the hill died to the middle-ring
squads (teams 405/424-427, ~16 men at (38800,10350)); "stopped at the hill's foot" was a port bug (below);
the biggest bunch of free light infantry is RTS-moved ~1000 at a time along the bot's own path with Joan beside them,
foes within 1200 of the road hunted first, until three stand within 1500 of the trebuchet. Segment tests:
game.campaign_S6L1_auxerre (start -> objective 1, `bot-S6L1-auxerre.jsav`), game.campaign_S6L1_troyes (-> objective 2,
`bot-S6L1-troyes.jsav`), game.campaign_S6L1_bot (-> win + Ending), each its own fixture, TIMEOUT 600.
MissionBot8g. **Port bug fixed: AI path budget.** `AiSystem::move_to` ignored the path_to budget and searched 100 polys;
the original passes it to PathSpace_astar as the expansion cap (OrderExec_* 500/1000; docs GAMEPLAY_IMPLEMENTATION AI
deviation 5). The crews stuck 1820 below trebuchet #912 (z 995 vs 2005, 242 polys) now climb (regression in
game.multimap_nav). With faithful AI walks: Auxerre t65843 from ck8 (wall shooters inside Auxerre on reachable walls,
TOWER_ATTACK, are fought before the cannons go in: team 202 shot two crews); Troyes from the pre-fix Auxerre save at
t184219 (the trebuchets crewed and Wall3 down). Plan changes for the road north: Joan's light infantry are handed to the
king every 10 s (recruits died following her through the camp), she fetches them from him once the camp is French;
wards wait only on busy foes within 1800 / any within 1000 (an idle squad at Bussy held the king for good) and hold
within 3000 of her fights; she fights foes at the wards first; hysteresis on "back to the king" (8000 out, 5000 in:
she turned on the spot at the 8000 line); assassin margin 10000 on the road; a hunt's stall check counts walking
headway (Troyes' last wall-crossbow crews, reachable from inside, were given up on the long walk). Last runs: the
Troyes segment from the new Auxerre save lost the duke to an ambush at the south park (t264154, Troyes' two
wall-crossbow crews left); the road from a Troyes save reached rep 74 (Champagne, par le lac) before the king died to
an ambush squad while Joan fought 11400 off (the 10000 margin and the walk stall fix are not yet run). Not yet seen:
Reims, the win, the Ending.

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

Plan `autopilot_plan_S9L1.cpp`: the nearest open place to the player's hero (prerequisite captured, Meung only with a gate
destroyed: gates the English open for a sortie shut behind the player; 90 s / 3 min without headway, or 20 s inside without
capture, skip it for 5 min), its garrison first (foe weight 0.3), then a walkable point inside the box; a walk without headway
for 20 s takes `Autopilot::detour`'s first level-seam crossing. Siege weapons take only stone / explosive hits (unit.res
TriggerType 0x6040: Joan's sword did nothing to Loches' bombard for 25000 ticks), so towns' bombards and the foes no path
reaches (tower archers) are shot (explosive / plain arrows) from within 700 with a clear line. Only marker 9 (final camp) is
followed (`MissionPlan::use_waypoint`). **Supplies:** Joan buys at the nearest French seller (Jargeau's; Meung's / Beaugency's
once taken) when below 12 food or 8 arrows of a kind, and stocks up before Meung (half of 300 plain, 40 explosive, 60 food):
its walls hold ~40 archers and crossbowmen. Also JOAN_SWORD_LV5 at Jargeau. 3000 gold is always kept (the spy's price).
**Army:** units in a map level the player is not near are deactivated (`NavWorld::stream`, Unit_deactivate 0x41fcc0) and ignore
their orders. A Joan sturdier than every hero (the legit start: level 21, 640 HP against 106-206) plays alone: Jean following
her into Saint-Aignan's fights was killed. A weaker Joan (the forced start, level 7) switches to the sturdiest hero at Orleans
(`Mission::switch_hero`, Jean); every other hero (150+ max HP) within 4000 is assigned with his squad to follow (Rts_cmd_assign
0x44f750 branch B); one below 35 % HP is RTS-moved home until above 80 %. Any hero's death is game over.
**Meung (Bastard's cannons):** with no other place open, Joan stocks up, walks into `meetOrlean` (cut scene, event bit_3 11, the
Bastard's men and four cannons become RTS-selectable), stays by the cannons and sends every manned one at MeungNorthGate
(Rts_cmd_attack_item, re-given every 20 s). A crew walks to its attach point only near the player: MOVE_TO_TARGET ends within
REACH (x2 at LOD > 1, beyond 2000 of the player) while attaching needs REACH_DT (15, 0x535020), so fCannonA's second crewman
stood 23 off his point for good while Joan was away (a weapon is selectable only fully manned). The Bastard (147 HP, not a
hero class) is RTS-kept at his camp and foes within 1500 of him are fought first: walked into Meung's fights he was killed by
its wall archers (imperative 36). **Beaugency:** once Meung is captured Joan walks into `getReinforcement` (scenario bit 1:
Glasdale, Fastolf, Scales and Richemont spawn; Glasdale's cut scene raises the north gate); bosses are fought first (foe
weight 0.05). Not done: the spy route (marker 8 ignored).
**Shared fix (bot):** `Autopilot::fight` backed off forever from a foe on another level when it had no arrows (Joan stood 100000
ticks under Meung's wall crossbowmen whose walkway the path reaches); without ammo it now closes in along the path.
Results from the legit Easy start (`work/MissionBot5b/bot-S9L1-easy.jsav`, Joan level 21, seed 1): every south village (27 WIN)
and Chateauneut, Saint-Florent, Fountainbleu, Rouvray, Checy, Bourges, Blois in ~190000 ticks (~355 kills by 150000); Meung
entered through sortie-opened gates down to 11 fightable English, then (from that save) the four cannons manned with Joan by
them broke the north gate and Meung was cleared to 6: four archers on a platform at z 524 (path ends ~100 short) that the hunt
alternates between instead of shooting. Not won.
**Meung's last six (MissionBot6d).** Five are archers (teams 170 / 171, task TOWER_ATTACK) on MeungRightTower / MeungLeftTower
(WALL, 200 HP, `broken_b_g42.SMO`): each top is a 2-poly nav island at z 521 with no ladder item and no .pfd link (as Auxerre's),
inside a parapet up to z 664 with 570..650 embrasures, so the 80-tall archers are hidden from most angles; the sixth (a poleman on
the wall walk) is reachable. Not a nav bug. The script deletes each team when its tower's hp reaches 0 (S9L1_tactic l.6560), and
a WALL takes ranged damage from stone only outside scenario 4 (Wall_take_ranged 0x492b10: explosive arrows do nothing here), so the
intended tool is the Bastard's cannons (range 1600). From the bot's checkpoint they could not get there: APPROACH_ITEM drove them
into the north gate hole, a 10-poly island with no path west. MissionBot7e later found the probable cause, a port bug since fixed:
loading a save left the poly of a gate destroyed before the save blocked. That route was not tried again. Arrows: a locked
player arrow (spawn_projectile → solve_aim to feet + 0.8 height, lobbed to normalised z ≥ 0.25 from 800 off) stops at the
first structure the look-ahead ray predicts (Physics_sweep_structures 0x693750: an FT-ms (1015) ray along (vx, vy, vz − V0),
re-cast from frame starts). A predictor built on exactly that (`lob_margin` in the plan) matched the physics on 81/81 test shots.
It finds perches (about 1500 off, outside the west wall or on the east walls) from which arrows land; 16-20 damage each. The
plan walks to the nearest perch, shoots only while the predicted flight is clear (the archers pace their tower tops) and moves
to the next perch after 15 s without harm. `solve_aim` is now declared in world_sim.hpp for it. Tested from the Meung
checkpoint, no kill yet: the archers moving made shots that were clear in the test misses in play. Also: tower archers no
walk reaches have foe weight 0 (they were chased forever), boss priority applies only within 1500 (Talbot, 15000 off at
Beaugency, pulled Joan out of Meung), and the bag is cleared before shopping (one cell per loaf since give_item became
Unit_add_item: 30 apples and a crossbow left no room for arrows). Shop purchases use `World::buy_item`.

**Test chain (MissionBot6f).** `game.campaign_S9L1_bot` no longer reads `work/` saves or forces a start: CTest fixture segments
`game.campaign_S9L1_seg1..12` (`src/game/CMakeLists.txt`, `--segment IN OUT [D]`, 60000 ticks each, 150-230 s measured) play
the bot from the legit chain's `campaign_checkpoints/bot-S5L1.jsav` (seg1 sets Easy), a win CONTINUEs into the next mission and
ends the segment, each saves `campaign_checkpoints/bot-S9L1-seg<k>.jsav` (the last `bot-S9L1-last.jsav`); the test proper plays
one more segment and must have won S9L1. The bot's memory starts fresh at every segment. Measured with a seeded
`bot-S5L1.jsav` (`ctest -R S9L1_bot -FS campaign_checkpoints`): S5L1 won in seg4 (Easy), seg5-7 took 13 places, seg8 lost
(Joan killed, below). **No unbroken S9L1 win yet** from the chain's start, on Easy or Normal.
Unbroken runs from the chain's `bot-S5L1.jsav` (Joan level 18, 6460 gold): **S5L1 won on Easy** (t181973, t198050) **and on
Normal** (t223842, after the fixes below). S9L1: Easy reached 15/16 places and Janville's last two wall archers (no bag room
for arrows; fixed: won from that save in 37100 ticks), another Easy run lost the Bastard (fixed), the rest died out of food
walking to Orleans' sellers with a garrison at their heels; Normal died out of food at Loches (Luxembourg's host: 32 food
gone in 15000 ticks) or on the way to a seller.
- **S5L1 Normal stall (fixed):** the walk to Dunois stood 300000 ticks at Orleans' shut South Gate: the marker stands 200
  up, so the path end counted as blocked "by height" and the unlinked-region gate rule never ran (`Autopilot::travel` now
  treats a path ending under the goal as whole; the S5L1 plan sets `gate_unlinked`); an English group outside the East Gate
  made the volley walk flicker with the walk out (no volley walk in town: `explosive_reserve` 1000 there).
- **S5L1 Normal death (fixed):** 5000 gold kept for S9L1's spy (never paid) left 1400 for food: 4 loaves, out of food at
  t120000; the rest at the siege camp ended at 60 % while the generic retreat holds to 70 %, so she walked on to Dunois' camp
  and was killed. Now 1000 kept, rest to 70 %.
- **S9L1:** Janville's last archers: ENHANCER gems and food above 30 are dropped for a 1x4 arrow column; the Bastard's
  attackers are fought first also before he is met; no spy reserve (1500 kept from the weapon seller for food); sellers with
  4+ English within 2000 skipped; food trips at < 20 between fights, mid-fight only < 3, food sellers only when starving.
  Open: big brawls (Blois / Bourges, Loches) burn 20-30 food, the garrisons chase Joan 18000 to Orleans' sellers and kill her.
- **Village slowdown (6 → 1-3 places in 120000 ticks), bisected:** the one-cell-per-loaf bag rule (shared, faithful) left
  Joan's bag full (30 apples, 26 loaves scattered over the 6x8 grid), so every shop visit bought nothing and she walked to a
  seller again every 5 min; dropping apples did not help because `Autopilot::loot` picked them straight back up. Fixed in
  the bot: `Autopilot::discard` (dropped items go into loot's skip set) and `Autopilot::make_room` (S4L1's: apples > 5 and
  spare weapons dropped, food to the troop grid, grid repacked) before every purchase. Easy start: 7 places at 120000.
- **Meung's tower archers:** with the gate save/load nav fix the cannons drive through the broken north gate; the plan sends
  them at MeungRightTower, then MeungLeftTower (`tower()`, Rts_cmd_attack_item); each falls in ~4000 ticks and the script
  deletes its archers.
- **Beaugency / final camp:** marker 9 (final camp) lies behind Janville's gates, which open only with Beaugency's capture
  (S9L1_tactic l.2845); the plan walks there itself only after it (the generic marker walk stood 50000 ticks at the gate).
- **Travel fix (opt-in `Autopilot::gate_unlinked`, set by the S9L1 plan; on for every mission it lost mission 2):** a goal in a region no walk links to with a path that looks whole (the coarse world-mesh leg runs
  through a shut gate) opens a gate near the player (Joan stood 90000 ticks at Orleans' shut east gate).
- Plan: sellers only within 20000 and linked; out of food (< 3) Joan breaks off fights for the seller (the Normal start was
  killed at Chateaudun with no food); explosive arrows for archers when no plain are left; perches recomputed at most every
  10 s.

**Port bug fixed (mission 6):** the win freeze set every unit's task to IDLE prio 127 also when leaving scenario 5, so in S9L1
no enemy ever fought back (Joan took 0 damage in 20000 ticks). GameOver_win_freeze_units 0x6e7900 skips the order clear and
`Unit_set_task(7, 0x7f, 1)` when `DAT_00a58fa7 == 5` and only calls `Unit_stop(0, 1)` (`game.campaign_S9L1_world`).

## 9. Gaps

- Multi-node worlds (scenarios 2–8 have several `.wor` nodes): terrain and nav mesh come from
  the first node only; node coordinate offsets are not applied to `.uni` placements (HYPOTHESIS:
  coordinates are world-absolute as in the engine's world scene).
- `change_scenario_step`: every shipped campaign file has one step, so step switching is not
  exercised.
