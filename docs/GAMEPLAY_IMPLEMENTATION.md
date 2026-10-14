# Gameplay implementation (spec → code traceability)

The headless simulation in `src/game/` reimplements the rules in
[reverse-engineering/GAMEPLAY_CHARACTER_COMBAT.md](reverse-engineering/GAMEPLAY_CHARACTER_COMBAT.md) ("RE" below).
Section references are to that document. Addresses are joan.exe VAs. The confidence column repeats RE's
label for the rule; **DEVIATION** marks a deliberate difference from the original.

Rendering never feeds gameplay. The renderer reads `World::snapshot()` and `World::events()` and nothing else.

## Modules

| file | content |
|---|---|
| `unitclass_db.{hpp,cpp}` | `GameData::load(game_dir)`: GAMEPLAY.RES constants (`Consts`, `ThrowConsts`), `unit/unit.res` (`UnitClass`), `item/item.res` (`ItemDef`), `item/dropitem.res`, `Resource/animscr.res` (`AnimScript`), STD.SET `ANIMATE`→move tables (`MoveRecord` = 0x99-byte animgrp record), `LEVEL`, `UPGRADE`, `UPITEM`, `NATION`. AM2 clip lengths and events are loaded lazily per move table. |
| `unit_state.{hpp,cpp}` | `Unit`: plain struct, field comments give the original offset. `Act` action enum (0x415820), action groups. `UnitMission`: fields used by scripts. |
| `items_inventory.{hpp,cpp}` | `Inventory` (unit+0x1c1), `Inventory::bonus` = Inv_bonus 0x497e10, recovery items, `give_item`, `equip`. |
| `world_sim.{hpp,cpp}` | `World`: Unit_init, SetAction, scheduler, anim-end handler, movement, regen, trigger dispatch, physics, snapshot, serialization. |
| `combat_melee.cpp` | Attack power, skill multiplier, damage, hit validation, reactions, knockdown, death, critical sequence. |
| `projectile_sim.cpp` | Ranged damage, projectile spawn/launch/solver/integration/hit, manager. |
| `progression_xp.cpp` | EXPUP, LEVELUP, set-level, upgrades, AI auto-upgrade, manual allocation. |
| `controller_player.{hpp,cpp}` | `InputFrame`, `PlayerController` (keyboard+mouse mode 3), `DuelPad` (a stand-in opponent used only by tests and the tool). |
| `terrain_query.{hpp,cpp}` | `ITerrainQuery`; `GriTerrain` (scenario .GRI); `FlatTerrain`. |
| `rng_lcg.hpp` | `rand15`, `random_range`, … on the shared `Rng` (script_vm.hpp). |
| `tools/joa-sim` | Headless duel runner. |

## Determinism and timing

| rule | code | RE | conf |
|---|---|---|---|
| Fixed 33 ms per logic tick: `now += 33` and `dt = 33` in every timer. The original uses this only in video-capture mode. **DEVIATION**: normal play in the original is not fixed-dt. | `World::kTickMs`, `tick_units` | §1.1 | CONFIRMED |
| Controller runs before the clock advances. One InputFrame per tick: frame = tick. **DEVIATION**: the original controller runs per drawn frame. | `tick_units` | §1.1, Part B §1.2 | CONFIRMED |
| Tick order: controller and pads → `now += 33` → UnitArray_process → projectile manager (#9) → Trigger_process (#10) → (mission layer: places, game timers, script VM) → physics integrator. `tick()` = `tick_units()` + `tick_physics()`. The mission layer calls the two halves separately, with its own work in between. | `tick_units`, `tick_physics` | §1.2 | CONFIRMED order |
| Scheduler. Pass 0 runs recno N..1: LOD tier by 2-D distance to the player (2000/4000/8000), alive-or-busy test, delete when done, then phases 0 and 1 for tier ≤ 1. Passes 1–12 use the original's filter table and run ascending. Generals and kings near the player get phase 3 twice. **DEVIATION**: the per-pass CPU budget is never exhausted, so every unit is processed every tick. This matches the original whenever its budgets are not hit. | `tick_units` | §1.3 | CONFIRMED |
| Cut-scene special mode (`cut_scene`): units with `m.cutscene_process` get phases 0–3; the others are stopped and get phases 0–1. `stop_unit_process` skips the scheduler. | `tick_units` | §1.3 | HIGH |
| Dead units are deleted 3000 ms after their DIE animation ends. HYPOTHESIS: this stands in for the actor fade. | `tick_units` | §1.3 | HYPOTHESIS |
| One gameplay RNG (`World::rng()`), explicitly seeded. `use_rng(&vm.rng())` shares it with the script VM. Draw order follows the original call sites listed below. **DEVIATION**: cosmetic draws use the separate `cosmetic_rng()`. These are the grass, wind, particles and idle-variant changes, none of which are simulated yet. | `World` | §2 | CONFIRMED |
| Gameplay draws: Unit_init `random_range(0,3)` AI combo buffer → drop roll `random(100)` → drop-list weighted roll; AI think stagger `random(500)` (no brain set); FATAL_KILL `random(100)` on every melee damage; ATTACKED/ATTKED2 `random(2)`; knockdown variant `random(n)`; lying time `random(2500)`; energy/rush hit `random(2)`; down-attack ignore `random_range(50,100)`; arrow spread `random(50)`, `random(2)`; ARROW_DEFLECT_OFF `random(100)`; critical sequence pick; AI auto-upgrade combo pick. | various | §2 table | CONFIRMED |
| Serialization: every struct exposes `visit(V&)`. `World::serialize/deserialize` cover clock, RNG seeds, units, ground items, triggers, projectiles, hostility, controller and pad state, HUD state and flags. `hash()` is FNV-1a over those bytes. Pad source functions and brain/physics pointers are not state. | `World::Impl::visit` | — | — |

## Data loading (atoi quirks)

| rule | code | RE | conf |
|---|---|---|---|
| Integer GAMEPLAY.RES keys go through `atoi`: STR_DAM 3.0→3, **DEX_DAM 1.5→1**, **DEF_RAT 0.3→0**. Float keys go through `atof`. EASY_B..VDIFF_B are never read; `_A` feeds BLOCKPER and `_D` feeds damage. THROWI MXD/MND/TR are atoi'd and then converted to float. V0/FT are derived as in 0x552776. | `GameData::load` | §3, Part D §1 | CONFIRMED |
| Item fields: Damage +0x4e, IncDamage +0x52, MinLevel +0x36, Range +0x32, DamageType +0x4a, AdditionalEffectType/Value/IncValue list. The effect-name enum order follows 0x4a45f0 (RUSH 0xe, DOWN 0xf, JUMP_B 0xd). Misspelt keys stay unread, as in the original (`ARROW` "Inc Damage"). | `GameData::load` | decomp 0x4a4200, 0x4a45f0 (read for this port) | CONFIRMED |
| Unit attach points use `atoi`, then a float conversion. | `GameData::load` | 0x525080 | CONFIRMED (MissionRuntime) |
| Move table: the STD.SET `ANIMATE` CODE→DBF row columns POSITION … SHOW_CRIT. Clip = `anim/<UPPERFILE>.am2`, clip `UPPERNAME`, matched case-insensitively. Length = clip end − start. | `GameData::moves` | Part A §1d, Part C §1.1 | CONFIRMED |

## Units, stats, regeneration

| rule | code | RE | conf |
|---|---|---|---|
| Unit_init copies the class record: name, rank (rec+0x149), lives, level, stats, armour level and item, morale, loyalty, falling→balance, vision. Every unit gets an inventory: PWeapon→melee 0 (mode 1), SWeapon→melee 1, PRange→ranged (mode 2 if no melee), SRange→ammo (quantity = item Quantity). The level-0 upgrades are unlocked and applied for free. XP = LEVEL[level−1] for level ≥ 2. HP and EP start full. A spawn level above the class level runs set-level 0x523190. | `spawn_unit` | Part A §1, §2, Part D §4.1 | CONFIRMED |
| Drop roll: rank ≥ 4 always drops. Rank 3 uses `*CAP_DI` and rank < 3 uses `*_DI` for classes 1–6 (`random(100) <= pct`). On success the weighted DropList roll walks from last to first. | `roll_drop` | Part A §2, Part C §6 | CONFIRMED |
| The stat getters add item effects 1–6. `set_hp` clamps to [0, max]; at 0 with lives left it uses a life and refills. `dead = HP<1 && lives==0`. | `max_hp`… `set_hp` | Part A §1 | CONFIRMED |
| Regen every 10 s: EP +RECOV_MP; HP `trunc(HP + max(DEF·0.1, 1))`, skipped when retreat == −1. Balance every 1 s while < 100: knocked → `+ (DEF·0.2+1)·10`, otherwise +3. | `regen` | Part A §3 | CONFIRMED |
| Morale setter quirk: a drop is ignored when morale > MORALE_BASE; MORALE_DROP items give immunity. | `set_morale` | Part A §5 | CONFIRMED (code), HIGH (effect) |
| Movement: per-state speed from the class record; DEX bonus `(min(DEX,200)−30)/170+1` when DEX > 30, except strafes and back-step; ×AI_MOVE for AI; ×1.3; **1 speed point = 46.1 u/s** (HYPOTHESIS). Strafe directions are ±45/90/135/180° from facing. | `unit_move` | Part B §5 | CONFIRMED / speed unit HYPOTHESIS |
| Physics without an `IUnitPhysics`: gravity 9.81·GravityLevel m/s², terrain height snap, refusal of non-walkable cells (GRI `valid` byte). **DEVIATION**: knock-back and stopped-locomotion velocity decay ×0.5 per tick on the ground. The original moves the body only for the duration of the last move request or force pulse. AiTroops' physics replaces this through `set_physics`. | `integrate_units` | Part C §5.3 | HYPOTHESIS |

## Actions and animation

| rule | code | RE | conf |
|---|---|---|---|
| SetAction 0x501190 selects the record by posture, action, ITEM_USING and SPECIAL (DEFAULT fallback). ATTACK/H_ATTACK match the combo string + letter. The IDLE row matches the AI 'A'×n buffer first (HYPOTHESIS: the IDLE A/AA/AAA rows of soldier tables). | `set_action`, `find_move` | Part C §1, §2.1 | CONFIRMED / IDLE variant HYPOTHESIS |
| Refusals: a non-hit action during a flinch waits for the flinch animation to end (siege exempt). Knocked AI units refuse actions while `knock_ms > 0`. **DEVIATION (HYPOTHESIS)**: DIE is always accepted. | `set_action` | Part C §5.2, §5.4 | CONFIRMED / DIE HYPOTHESIS |
| Remaps: a hit while knocked → DNATKED; DOWN after JATTACKB/JATTBI → JATTBD; IDLE after JATTACKB → JATTBI; ATTACKED↔ATTKED2 alternation, otherwise a `random(2)` pick when ATTKED2 exists. | `set_action` | Part C §2.4, §5.2 | CONFIRMED |
| ClearCombo for every action except ATTACK/H_ATTACK. The player's chain level resets for every action except ATTACK/H_ATTACK/CHAIN. Window = START_TIME/END_TIME. Gravity scale = ftol(script GravityLevel), default 3. | `set_action` | Part C §2.1, §2.3 | CONFIRMED |
| KNOCKDN*/H_DROP: lying time = FELL_DUR/5 + `random(FELL_DUR)`; rank ≥ 4 → invulnerability = max(cur, INVC_DUR 6000). | `set_action` | Part C §5.4 | CONFIRMED |
| Attack triggers for actions 0x1c, 0x1d, 8, 0x19, 0x1e–0x21, 0x49, 0x53, 0x46, 0x44: radius = script Range + max weapon reach; start = now + StartTime; a unit trigger plus a map-object twin. DOWNATK uses type 0x400 with Range only (quirk: no reach). | `set_action` | Part C §3.1 | CONFIRMED |
| Animation timing: remaining = clip length − (now − start). Animation end (0x7c7ab0): ATTACK/JATTACKB/DOWNATK/CHAIN/RUSH/ENGATK4 → combo index −1, lock = now, idle. ENGATK → ENGATK2/3/4 when the next record is free or the ENERGY_ATTACK skill is learned. Flinches, STANDUP and landings → idle. GUARD1/GUARDR are held. | `update_action` | Part C §2.2, §7 | CONFIRMED |
| Get-up: AI decrements the lying timer after the knock animation and then stands (or DIE if dead). The player stands as soon as the animation ends. | `update_action` | Part C §5.4 | CONFIRMED |
| Sound cues: move-record SOUND/SOUND2 at SOUND_TIME, plus AM2 clip events at their clip time. Each is emitted while the action is unchanged. | `schedule_cues`, `emit_cues` | Part A §1d | CONFIRMED data |

## Player controller (keyboard + mouse, mode 3)

| rule | code | RE | conf |
|---|---|---|---|
| Dispatcher early-outs (DIE, STANDUP, knocked, H_DROP). Order of evaluation: items → range mode → down attack → block → energy → attacks/rush → jump → evade → chain → movement → face movement direction → mouse yaw → forward locomotion. Each step returns when it consumed input. | `PlayerController::process` | Part B §3.1, §3.2 | CONFIRMED order |
| Double-tap: checked before this frame's tap is recorded; window `now − t < 300 ms`. The recorded tap is the first of W/S/A/D pressed on a key event. | `process` | Part B §3.2 | CONFIRMED (decomp 0x430480) |
| Attack input 0x4367c0, as decompiled: button phases, the fresh flags, the 250 ms lockout, rush gates (learned path scaled by EP_CONSUME; fallback path unscaled), queue window `[START_TIME, END_TIME)` since attack start, queued hit at END_TIME. The follow-up search walks records from last to first, needs the combo exactly one character longer, and requires it free or learned. B costs ENERGY_PT×EP_CONSUME/100. | `attack_input` | Part C §2.2 | CONFIRMED |
| Energy attack: EP cost = ftol(EP_CONSUME·(maxEP·40/100)/100), HP cost = ftol(maxHP·20/100); requires HP > HP cost. | `energy_attack` | Part B §3.9 | CONFIRMED |
| Block (GUARD1 while held; release → idle), down attack (enemy knocked within 60 u, bearing in the script arc), jump (dir byte ·0.711; 1400·magnitude, uphill ·n.z; vz 5000..10000), evades JUMP_B/R/L, chain (remaining ≤ CHAINPRO‰·len/chain). | helpers | Part B §3.10–3.12 | CONFIRMED |
| **HYPOTHESIS**: jump/knock-back force values become m/s by dividing by 1000. The unit body mass and the force units were not recovered. | `jump`, `on_attack_message` | Part B §3.12 | HYPOTHESIS |
| Range mode: enter on Z, draw SHTLOAD → SHTIDLE after len·100/(RATE_OF_FIRE+100), target pick by the perpendicular-distance rule (AIM_R 25, AIM_MXD 2500), fire SHTOUT; exit on Z. **HYPOTHESIS**: the camera eye is placed at unit z + class Height. | `range_input` | Part B §3.7, Part D §4.5 | CONFIRMED / eye HYPOTHESIS |
| Mouse yaw: `d = ftol(dx·((s+1)·0.7+0.2)·0.2)`, clamped ±45 during ATTACK; facing is truncated to integer degrees. | `process` | Part B §3.4 | CONFIRMED |

## Melee combat

| rule | code | RE | conf |
|---|---|---|---|
| Trigger_process: each trigger is delivered once, on the first tick with now ≥ start. Insertion order is kept: the vector is scanned in order and fired triggers are removed stably. Units are scanned in recno order. Target mask = class TriggerType. Melee senders must still be attacking. The sphere test (area types 0x40/0x2000/0x4000 use a cylinder) is followed by the handler; down attack and throw stop the scan. Kind-2 (map object) triggers go to `World::object_trigger`, owned by the mission layer. | `process_triggers`, `deliver_trigger` | Part C §3.2, Part D §6 | CONFIRMED |
| Melee_tryHit 0x5048b0: dead, super-armour, light hits on downed units, down-attack-only-on-downed, invulnerability, active scene, no friendly fire, reach (slot 1 else slot 0, quirk), horizontal arc with ±360 retry, block ±BLOCKANG inclusive (GUARDR restart), defIgnore 0/20/40 by bearing (down attack `random_range(50,100)`), threat +10. | `try_melee_hit` | Part C §3.3 | CONFIRMED |
| RelAngle sign: positive = target clockwise (right) of facing; yaw is counter-clockwise (cos, sin). | `rel_angle` | Part C §3.3 | HYPOTHESIS (RE open item) |
| Attack power: `((STR·STR_DAM) + (WEAPON_DAMAGE-scaled weapon − WP_DAMG + 100))·DAMAGE/100`, × WEAPON_DAMAGE_MULTIPLIER, × `*_D` difficulty if hostile to the player. All units get the weapon terms, because every unit has an inventory (Part D §4.1 settles Part C's "heroes only"). | `melee_attack_power` | Part C §4.1 | CONFIRMED |
| Skill multiplier: `(lvl+10)/11` × item damage bonus, by state table. | `attack_skill_mult`, `skill_level` | Part C §2.5 | CONFIRMED |
| Damage 0x509950 in float32: FATAL_KILL, morale factors, armour·AMR_DEF, DEF·DEF_RAT (=0), penetration, sustain bonuses, player crit ×2, chain ×N, F_ATTACK/F_DEFEND by the player's side, leader LEAD bonus, HP = trunc(HP − dmg). The game-type-8 branch is not modelled. | `apply_melee_damage` | Part C §4.2 | CONFIRMED |
| Reactions: retarget; player combo counter; energy/rush 50 % knockdown; directional flinch, and non-player units turn to face the attacker; balance −(H+V)/250, or 0 when H+V ≥ 6000 or V ≥ 3000; knock-back away from the attacker (×2 H_DROP, ×0.7 DNATKED); death. | `on_attack_message` | Part C §5.2, §5.3 | CONFIRMED |
| Knockdown 0x509000: not siege/cart, not mounted, variant `random(n)`, threat ≪ 1. | `knockdown` | Part C §5.4 | CONFIRMED |
| Unit_die 0x7c6240: retreat==1 survives at HP 1 (`MISSION_EVENT_DEFEAT` event 7); drop item; killer EP 6/15/30; medals, kills and leader counters; XP `Experience·level·m` (CRITEXP, 1/2^Δlevel, combo bonus, min 10) shared to the team; morale ±max(k·0.3, 1) to same nation and troop; DIE; death impulse. | `unit_die` | Part A §4, Part C §6 | CONFIRMED |
| Critical sequence: HP-trail − HP > CRI_DIFF starts it. The record is a random SHOW_CRIT row re-rolled until COMBOAVAIL Y. Keys advance or reset; completion sets the player crit flag (×2), cleared by the next attack input; expiry after CRI_TIME. Combo-hit counter resets after > 1999 ms. | `crit_check`, `crit_key` | Part C §4.4, §4.5 | CONFIRMED / list HYPOTHESIS |

## Ranged and siege

| rule | code | RE | conf |
|---|---|---|---|
| Ranged base `(DEX·DEX_DAM+100)·W/(ammo?200:100)` with weapon and ammo item damage; ranged damage 0x509e90 with RANGE_ATTACK skill, distance falloff FALL_S/FALL_E/DIST_P (not on siege victims), AMREXDEF, SIE_UDAM, sides, leader. | `ranged_base_damage`, `apply_ranged_damage` | Part D §4.2, §4.3 | CONFIRMED |
| Arrow hit 0x5051e0: ARROW_DEFLECT_OFF roll, super-armour, knocked filter, invulnerability, block ±70 (GUARDR), threat +5. Reactions: downed → DNATKED; stone/explosive knock unmounted units down; otherwise flinch; knock-back 3000/500; balance −ARR_FALL (types 2 and 0x40). | `try_ranged_hit`, `on_attack_message` | Part D §4.3 | CONFIRMED |
| Fire state machine: SHTLOAD → (SHTLOAD2) → SHTIDLE/H → SHTOUT/H; spawn once when remaining ≤ release (650 / 1667 cannon / 0 trebuchet / 1300 AI crossbow); trebuchet SHTOUT2. | `update_action` | Part D §5 | CONFIRMED |
| Projectile type from the ammo ITEMTYPE (0xd/0x10 ARW, 0xe/0x11 PROJ, 0xf/0x12 EXPL, 0x14 STC, 0x13 CAN). ARW DL = RANGE_DISTANCE·DL/100 (integer). Launch z stored as 0 (quirk). ARW: force dir·FC for 100 ms. PROJ/EXPL/STC/CAN with a target: solver 0x496d40 (MXD clamp, t = d/AV, raise the arc from t = 0.1 by 0.2 until sin ≥ minSin), 1 ms pulse. Spread only for non-player PROJ/EXPL beyond 300 u. Only the player consumes ammo. The shooter turns to the shot heading. | `spawn_projectile` | Part D §3.1–3.3, §4.4 | CONFIRMED |
| **HYPOTHESIS**: launch point = shooter pos + 0.75·cylinder height. The bone origin is not available headless. | `spawn_projectile` | Part D §3.2 | HYPOTHESIS |
| Integrator: 2 (ARW) or 5 sub-steps, trapezoid, force-pulse split, gravity GL·9.81. Swept segment vs the target unit's class cylinder; shooter, non-enemy and downed units ignored; hit valid below head height; terrain ground flag. | `integrate_projectiles` | Part D §3.4 | CONFIRMED |
| Hit events: ARW/PROJ → type 2 on the victim (radius TR); EXPL → 0x2000 area; STM/STC/CAN → 0x40 area, plus map-object twins. Ground: EXPL and STM/STC blast radius 150; CAN and arrows do nothing. Manager removes on DL, TL, collision, hit or ground. | `projectile_hit_unit`, `projectile_ground`, `process_projectiles` | Part D §3.5, §3.6 | CONFIRMED |
| Explosive keg on units: a 0x4000 trigger from a map object (src_kind 2) with `param` = keg item damage A; `dmg = (A/2 + max(0,(A−D)/2))·sustain`, F_DEFEND on the player side, threat +10. Senders are only skipped when they are units. **HYPOTHESIS**: unmounted units are knocked down, as with explosive arrows. | `on_attack_message` | Part D §7.4 | CONFIRMED formula / reaction HYPOTHESIS |

## XP, levels, upgrades, items

| rule | code | RE | conf |
|---|---|---|---|
| EXPUP: siege units get none; leader shares MEM_EXP split across members; members give LD_EXP to the leader (extra, not deducted); player command points capped at MAX_COM; then LEVELUP. | `add_xp` | Part A §4 | CONFIRMED |
| LEVELUP loop: unlock the rows for the level being left, level++, arrow, ATT and SKI points, repeat while XP ≥ the next threshold (30→31 edge kept), full heal, members dragged up. | `level_up` | Part A §4 | CONFIRMED |
| set-level, upgrade apply (STR/DEX/DEF/LEAD/HIT_POINT; skill types are only marked applied), AI auto-upgrade (last→first, REQUIRE check, random NEW_COMBO level-ups), manual allocation (STR_PER…EP_PER, combo level, undo). | `set_level`, `apply_upgrade`, `ai_auto_upgrade`, `allocate_point` | Part A §4 | CONFIRMED |
| Inv_bonus: sources are melee slot 0 (unless it is the ranged item), slot 1 and ranged (weapon mode 1 only), then ENHANCER bag items, highest level per id. Combination rules per type; the integer `/` is kept. **HYPOTHESIS**: the per-unit applicability test 0x4a42f0 (EffectiveRequirementType) is not applied. | `Inventory::bonus` | Part C §4.3 | CONFIRMED / applicability HYPOTHESIS |
| F3/F4: the bag potion (type 0x15/0x19, or 0x17 for EP) whose amount is closest to the missing amount, ties to the lowest index. **HYPOTHESIS**: it adds its amount (0x50bcd0/0x50b4f0 not read). | `use_recovery_item` | Part B §3.13 | CONFIRMED pick / HYPOTHESIS effect |

## Extension seams

- `IUnitBrain` (`set_brain`): `execute_orders` runs in phase 2 for every unit; `think` runs in phase 3 for every AI-controlled unit, and the brain owns the 0x49d810 timer. AiTroops implements it and keeps its task/order/team state keyed by recno.
- `IUnitPhysics` (`set_physics`): replaces `integrate_units` in the physics slot.
- `ITerrainQuery`: GRI implementation here; a PFD-backed one plugs in through the same interface.
- `World::object_trigger`: receives kind-2 (map object) triggers. `post_trigger` posts script and object triggers.
- `attach_pad`: drives any unit through a `PlayerController` from a frame source. `DuelPad` is the stand-in used by tests and `joa-sim`.

## Tests and tool

| CTest | checks |
|---|---|
| `game.world_rng` | Seed 0 → `random(100)` = 0, 1, 0, 33, 3, 35; `rand15`; `random_range` bounds. |
| `game.items_bonus` | Inv_bonus combination rules, enhancer dedupe, MORALE_DROP immunity, slot getter (synthetic items). |
| `game.combat_damage` | Hand-computed damage: Joan A 3.4944 → 61 HP; crit+chain 13.9776; English A on Joan 3.4104 → 56; Hard 5.3256; DEF ignored (DEF_RAT 0); block arc ±69 blocked, 71/180 not; reach 82/84; balance −12, second hit → knockdown; B knocks down at once; rank ≥ 4 knockdown → 6000 ms invulnerability. |
| `game.controller_combo` | Queued AA fires at t0+330 (END_TIME 317); late press not queued; combo end at anim end (583 ms) then 250 ms lockout; double-tap back within 99 ms → JUMP_B, 363 ms → none; W W+A fallback rush costs 5 EP. |
| `game.progression_level` | LEVEL table rows, level-up thresholds and points, multi-level jump, allocation and undo, kill XP 50/12/10 (min), EP_NOR, level-3 English soldier auto-upgrades (HP 75, STR 6, DEF 6, 4 points left). |
| `game.projectile_arrow` | THROWI values; ranged base 3.12; English archer PROJ arrow hits Joan for 2.2008 → 57 HP; ARR_FALL; AI ammo not consumed. |
| `game.interact` | Bribe cost/no-gold message/side switch/counters, prisoner freed (and ignored behind the player), talk trigger writes the player's +0x2e3, indicator event. |
| `game.world_duel` | S1L1 GRI duel Joan (player controller) vs a level-3 English light infantryman (pad): identical event log and hash across runs; a decision within 900 ticks; save at tick 150 → load → continue gives the same final hash. |

`joa-sim --scenario S1L1 --duel joan english_light_infantry --ticks 900 [--seed N] [--difficulty 0-3] [--level-a N] [--level-b N] [--quiet]` prints the event log, the final unit states, the winner and the World hash.

## Deviations (summary)

1. Fixed 33 ms ticks, and the controller runs once per tick (not per drawn frame).
2. All units are processed every tick; the original's CPU-budgeted passes are not modelled.
3. A separate cosmetic RNG. No cosmetic system consumes the gameplay stream.
4. Corpses are removed 3000 ms after the DIE animation (the fade timing is unknown).
5. Knock-back, jump and force units are converted at force/1000 = m/s; ground friction is ×0.5 per tick.
6. Projectile launch point at 0.75·cylinder height; range-mode camera eye at the class height.
7. DIE is accepted during flinch and knocked locks.
8. Only keyboard+mouse control (mode 3) is implemented; horse mode A only (0xa5802c = 0).
9. Horse body motion: the ridden horse's WALK reaches the gear cap (+0x2a4: 5 m/s, 10 m/s after 1 s of run gear) at once; the accelerating body model of 0x6a45a0 is not reproduced. Seated riders take the horse position (actor attach). H_DROP unlinks rider and horse.
10. Player mount (0x43c7d0) sets ride mode only when the mount succeeded (the original sets it regardless).
11. Face_move_dir's ATTACK re-aim (±45° steps from +0x126a) is not modelled; the heading is left alone during ATTACK. IDLEDN ends in IDLE and JATTACKB is stopped by Unit_stop (both kept by the original Unit_stop, which ends them elsewhere).

## Gaps

- Mounted AI: implemented (see "AI riders" below). Not modelled: AI-initiated mounting (no AI caller of 0x4fc220 exists), H_RUN charge gear.
- Keyboard-only, joystick, point-and-click and RTS controller modes; camera auto-rotation is left to the camera system.
- Idle-variant changes (IDLE2_CH, IDLE_CC, STOP_CC); grass and wind RNG consumers.
- Structures: gates, walls and kegs are the mission layer's, via `object_trigger`. The structure look-ahead sweep 0x693750 is not done here.
- Armour items' effect on armour level, and the Inv_bonus applicability check 0x4a42f0.
- Morale effects of killer-less deaths (0x64f320), nation-strength update, game type 8.
- No dynamic verification against the running original yet (DynamicRef observations match the initial Joan stats: L1, HP 60, EP 30, STR 9, DEF 8, DEX 10, LEAD 15).

## Turning, horses, interact (GameplayGaps)

| rule | code | RE | conf |
|---|---|---|---|
| Two headings: `yaw_goal` (+0x1e, Unit_set_yaw_deg 0x50abd0, written by executors, controller, reactions) and the body `yaw_deg` (+0x1a, Unit_set_yaw_rad 0x4ff870, read by movement and rel_angle). `set_yaw` writes the goal; `set_yaw_now` both (mouse yaw, range aim, mount). | `World::set_yaw`, `set_yaw_now` | decomp 0x50abd0/0x4ff870, 0x40437c, 0x42222f | CONFIRMED |
| Turn 0x4f2190 (phase 1 after Unit_updateAction): ≤ rate·dt/33 per tick toward the goal; rate by Special: 4/8/9/10/11/13 → 1°, 5 → 2°, 6 → 5°, else 20°; cap 25° (LOD < 2) / 90°. Hit states (0x23–0x26, DIE, H_HITED..H_RHITED) reset the goal. Attached units (+0x384) never turn. Siege weapons turn only fully manned (usage bit 1, or any slot for Special 6/10) and carry their crew to the rotated attach points (pushers PUSH / PUSH_L for cannons, others WALK, stop when < 1° left); carts play T_LEFT/T_RIGHT. While turning (step ≤ remaining) an AI unit does not move and every turning body loses its velocity. | `World::turn_step`, `unit_process` | decomp + asm (ftol on diff/360−diff) | CONFIRMED |
| Unit_move: DEX bonus from the raw DEX (+0x157) of the team leader (own for leaders/loners); LOD tier 2 ×1.4 (speed < 20), tier ≥ 3 ×1.8 when not mounted. | `unit_move` | 0x4f0b50 | CONFIRMED |
| Unit_stop 0x508b90: idle-class states unchanged; rider H_IDLE + horse IDLE; cart/driver CA_IDLE; climber C_IDLE_A/B; UP → IDLEDN; mode 1/2 → IDLE_BAT; bound prisoner IDLE_T; else IDLE. | `World::stop` | decomp | CONFIRMED |
| Horses: mount/dismount 0x4fc220 (side by rel_angle, 30/45 offset, H_MNT_L/R, H_DMNT_L), rider link 0x482930, mount search 0x44f0b0 (radius 70, last-to-first, +0x3c0), sync 0x5174e0 (dead rider/horse → H_DROP + −10000 pulse; horse follows the seated rider's heading), anim ends 0x7c7ab0 0x47–0x52 (seat → +0x1ea, H_IDLE; dismount steps 30 sideways, heading −90/−270), rider stop 0x4533e0 (STOP_CC roll always drawn). Player horse controller 0x43c5c0: R → interact handlers, then dismount on a standing horse; H_ATTACK A/B (B needs a free "B" record or HORSE_ATTACK_B); turn mode A ≤ TURN_ANG per frame relative to the horse, only while moving, S ignored; gear: W → run gear, W+walk → walk, S/S+A/S+D → back (MOVE_B), A/D alone or no key → rider stop (asm 0x43d9b9). | `mount_ride.{hpp,cpp}` | decomp | CONFIRMED / gear unit HYPOTHESIS |
| Face_move_dir 0x4352f0: DOWN/UP face the input; UP with > 180 ms left re-aims the jump push to dir·mag·1400 (zero without input); airborne apex (+0xa6 1→2, canAct) → combo cleared, IDLEDN; air steer: one ≤ 30° heading step toward the input with the horizontal velocity rotated onto it. | `PlayerController::face_move_dir` | decomp | CONFIRMED |
| Interact key (R): `World::interact` = `install_interact` (interact_player.cpp, installed by Mission::start). Handlers in order, first success wins: free prisoner 0x43bef0 (search 0x471c80: not hostile to the player, +0x2e1 set, +0x2e9 == 0, ≤ 100, bearing ±90 → Unit_free_prisoner, clear task, vision 2000, citizen_saved++), talk 0x43bd40 (0x46c1f0: +0x2e9 != 0, class mask 0x200, ≤ 100, ±90 → TRIGGER_INTERACT 0x200 r 100; receiver 0x75d540 sets the sender's +0x2e3), bribe 0x43be40 (0x46fa80: hostile, +0x2eb == 1, \|dz\| ≤ 100, ≤ 200, ±90 → Unit_bribe 0x4e8500: cost ftol(HP·(rank<3 ? BRI_COMM : BRI_CAPT)·max(0.5,(lvl−21)/10+1)), ×1.6 for MER_EX_ARCHER(_CAPTAIN) only; no gold → one-shot message; else pay, free siege slot, leave team (leader: all members), nation = briber's, clear task/orders/targets, join the briber if rank < 3 and room, +0x2eb = 2, counters), recruit 0x43c210, item 0x43bc50 (0x4d6320 over the modify array from the top: interact 0x200 or LADDER; foreign gates only from their back side with hp > 0 and StructionType != 5; gates need state < 2 and (no key or closed); ladders by foot/top facing test ≤ 90; others nearest ≤ 200 with \|dz\| ≤ 66 → TRIGGER_INTERACT to the item), then recruit screen 0x43bfe0 (RECRUIT facility of the unit's nation whose box holds it) and workshop screen 0x43c080 (WORKSHOP; IRON/WOOD only for ALENCON-as-player within 1024). On foot the result is ignored (0x430480: mount 0x43c7d0 and ladder 0x42d300 still see R); Ctrl_horse stops at the first success. Indicator FUN_006d3590 every tick: talk/bribe/prisoner/mount/recruit, then item/recruit screen/workshop → `MissionEvent::InteractPrompt` on change; results → `MissionEvent::Interact`. Deviations: candidates scanned in recno order (original: the level actor list); in RTS mode (game mode 4) the indicator is always hidden; the original's mode-4 branch shows/hides it from its own +0x1d field (RTS hover target), not modelled. | `interact_player.cpp`, `controller_player.cpp` | decomp + asm 0x4e8500 | CONFIRMED |
| Unit_onMessage 0x75d540 head: task-locked units (+0x2e1) take only TRIGGER_INTERACT unless +0x358; busy units (+0x1fd ladder climbers) take no message at all, and projectiles pass through them (body off, 0x4ac910). Climbers are therefore never hit: C_DROP (0x67) has no setter anywhere in joan.exe (no Unit_set_action/AnimTable lookup with 0x67; only Anim_parse_movement names it). | `on_attack_message`, `integrate_projectiles` | decomp + asm scan | CONFIRMED |
| Root motion (Unit_process_movement 0x52daa0): at move-table load (FUN_00415090) every record whose action is CHAIN, ATTACK, JATTACKB, ENGATK..4, ATTACKED/ATTKED2/ATTKEDR, THROW, THROWN, STANDUP, RUSH, DOWNATK or JUMP_B/R/L gets the "Bip01" x/y track of its clip as a root track (FUN_00417f20 / 0x418820 merge, keys relative to the clip-start key); per tick the delta root(t) − root(t − dt) rotated by the heading is the body's move request (`Unit::root_vel`, added to the velocity by both integrators), not while airborne. Data: Joan JUMP_B −78.7 (back), JUMP_R −88 (right), JUMP_L +79.8, A +21, RUSH +209; English ATTACKED −15.5. The original strips those Bip01 x/y keys from the drawn clip (renderers subtract `root_offset`). | `GameData::build_root`, `World::root_motion` | decomp + asm 0x52e1cd..0x52e300 | CONFIRMED |
| THROW (0x3a → TRIGGER_THROW 0x10, victim THROWN) is unreachable in the shipped game: no Unit_set_action call site passes 0x3a (all 213 direct sites and every computed one resolved), no AnimTable_findAction/canUseSpecial asks for it, the only producer of the id is Anim_parse_movement (move-table load); the input step at its slot in all three controllers, 0x436380, is an empty stub returning 0. Reads of action == 0x3a (Ctrl_down_attack, Trigger_process, Unit_set_action, Unit_process, movement) are dead guards. The THROWN end and the 0x10 receiver are kept for completeness. | — | asm scan of joan.exe | CONFIRMED |
| Balance audit (`game.balance_units`): unit.res/item.res == GameData == Unit_init for E/F light infantry (HP 65/100/32/50, STR 4/5/4/5, DEX 1/2/5/5, swords Damage 40 Range 45). Per hit 1.2·P − 0.6; reach = animscr Range + weapon Range: English A 83 / AA 87, French A 74 / AA 132; the AI stops 60–120 away and swings within 150 (0x534090), so French AA lands where English swings miss. The ~20:1 `game.ai_battle` outcome is original data, not a port bug. | `tests/game/balance_units_test.cpp` | decomp + data | HIGH |
| Save/Player.dat fully decoded (0x48 profile image: +0x38 gold, +0x3c play ms, +0x40 command points; name block; N entry heads + bodies: 0x432 Unit image, +0x16f block, upgrades, inventory 4 slots + 3 grids; two hint lists; trailing command points); `to_player_dat`/`from_player_dat`. | `formats/save_sav.cpp`, `campaign.cpp` | FUN_004c8810/4c8bb0/4c93e0/4c9460/4c9880 | CONFIRMED |

### Siege crews, carts, ladders (siege_crew.cpp, ladder_climb.cpp)
- Crew mirror (Unit_set_action 0x501190 head, CONFIRMED): weapon special 4/5/6/9/10 SHTLOAD/SHTIDLE(H)/SHTOUT(H) → 'F' crew SHTLOADC/SHTIDLEC/SHTOUTC, IDLE..IDLE_BAT → crew stop; ram ATTACK → 'F' crew BATTER_C. Crew hit/knocked/dizzy/DIE → slot +0x378/+0x37c freed, weapon stopped.
- Movement gate (0x52daa0 head, CONFIRMED): WALK/RUN stops unless every 'M' slot is manned (0x50d550 mask 1); bombard never moves; crew PUSH_L(cannon)/PUSH/WALK, ram 'F' crew IDLE.
- Crew placement (Unit_update_location 0x4fee60, CONFIRMED): after physics, crew at the rotated attach point. Cart driver (0x6b1350 + 0x454b20): cart pos − fwd·80, cart yaw; cart WALK/RUN/T_*/IDLE → driver CA_WALK/CA_RUN/CA_IDLE (0x459970).
- Weapon death (Unit_die 0x7c6240, CONFIRMED): crew unlinked + stopped; ram 'F' crew and cart driver die with it. `ai_shoot` fires with ammo only (siege weapons have no launcher item).
- Ladders (0x42d300/0x5183d0/0x4fab10/0x4ac910/0x518890/0x445000/0x7c7ab0, CONFIRMED; facing axis HIGH): interact within 100 of a LADDER foot/top, C_UP_SA/C_DN_SC mount, W/S step one rung (per-hero constants), C_UP_CS exits top, C_DN_AS exits at the foot. Climbers have no body.
- Deviations: ladder states run at the start of the tick (MapObjects::tick); only W/S step on ladders; the AI's reserved slot (+0x380) is not reset when crew are unseated; mount_siege_tower_to_wall (351) unimplemented (no campaign script calls it); the S1L1 Castle Gate has no poly at its centre, so its nav block is a no-op.
- Test: `game.siege_crew` (manned cannon turns/fires/kills, unmanned refuses; ram breaks the S1L1 gate 1500 → 0 in 24 hits; cart + driver; ladder climb 187 u; determinism).

### Campaign chain (campaign_runner.cpp, mission.cpp)
- Campaign 5→6 keeps the world (CONFIRMED Battle_deinit 0x41c780 / Game_deinit 0x471010 / Battle_load_world 0x41c810): S9L1 runs in S5L1's world, only the scripts restart (`Mission::continue_in_world`): finished objectives dropped, per-unit kill/troop counters cleared, UI/win delay/cut scene reset, clock and RNG continue (`Vm::continue_from`), profile restored. Gap: RETRY of mission 6 starts without a player (HYPOTHESIS: same in the original).
- `.uni` heroes and the `add_unit` builtin go through `Mission::add_unit` = UnitArray_add 0x51dfc0 (restores a stored hero of that class; CONFIRMED via Uni_load_create_unit 0x558a60).
- Tests: `game.campaign_full_run` (missions 1–8 × 9000 ticks with a deterministic autopilot, twice: equal hashes, no unimplemented builtins, no script errors; S5L1 is forced to a win only after its first script passes — forcing at tick 0 finished S5L1's first-game objectives 2/9/27 before the script disabled them, and S9L1 re-enables them, which ended S9L1 in an immediate win: a harness artifact, not a script/end-condition bug); `game.campaign_full_chain` (forced wins 1→8, transitions, gold and the four heroes carried, Ending after 8, save/load in mission 4 reproduces the hash); `game.campaign_full_playerdat`.

## AI and troops

Spec: `docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md` (Part A §A3–A5, Part B, Part C). Owner: AiTroops.

| rule | code | RE | conf |
|---|---|---|---|
| `.PFD` layout (models, points, polys, links, height grid, sub-grids); all 38 files parse to EOF. Ground vertices are stored with z = 0 and get the grid height at load (**HYPOTHESIS**). | `formats/pfd_mesh`, `NavMesh` | Part C §1.1 | CONFIRMED |
| Navmesh runtime: neighbours from links; "narrow" polys (sum of linked edge lengths < 170·n) are closed to radius ≥ 200; connected components; point-in-poly (`cross > 0` = outside) with wall-edge clearance (map-border edges exempt); poly height = triangulated grid (checkerboard diagonals) or walkway plane. | `NavMesh` | Part C §1.1, §2.2, §2.4, §4 | CONFIRMED |
| A*: integer `isqrt` costs, f = g + h, ties put the newest node first, 100 expansions, partial path to the best `h + g/16` node clipped on its edge toward the target; smoothing shrinks portals by the radius and runs a funnel over ≤ 8 portals, then drops waypoints < 0.5 apart. | `find_path` | Part C §2.1, §2.3 | CONFIRMED / funnel HIGH |
| LOS: `.POR` anti-portal quads (2nd pair swapped) block the eye(0.8·height)→target segment. | `NavMesh::segment_blocked` | Part C §6 | CONFIRMED |
| Character physics: g = 9.81·GravityLevel·46.1 u/s², 20 ms sub-steps, 80-unit step-up refusal, navmesh walls with x/y slide, unit–unit block that removes the approach component (slide), idle blockers pushed sideways for 4 frames, airborne when > 0.5 m above ground, every normal flattened (`NavTerrain::normal` = up). | `CharPhysics`, `NavTerrain` | Part C §4–§5 | CONFIRMED constants / HIGH order |
| Tasks: strict-priority `set_task`, lazy expiry, members inherit the leader's task except RTS_MOVE_TO, para/state accessors read the leader's when the own task is 0, hold marker `{0, p1 = 1}`. | `AiSystem::set_task`, `curr_task*`, `toggle_hold` | Part B §1 | CONFIRMED |
| Orders: queue sorted by priority (FIFO among equals), duplicates rejected, BLOCK refresh / APPROACH_ENEMY retarget, expiry purge, LOCK freeze, interruptibility table, all 22 executors; `give_order` team copies at prio −1 with issuer (FOLLOW_FORMATION suppressed when a member already has one; APPROACH_ENEMY never overrides a fight). | `orders_queue`, `orders_exec`, `give_order` | Part B §2, decomp 0x661340 | CONFIRMED |
| Think timing: per class ScriptDuration, first think at now + rand(500), only with an empty order queue. | `AiSystem::think` | Part A §A4, §3A | CONFIRMED |
| Target search: T/E/EP/P/S/F/L/O, radius 0 → 1600, sticky target kept with rand(100) < 96, LOS, targetable flag, siege excluded from E, citizen radius / 1.2, yaw/pitch cones, candidate cap by radius, five attack behaviours with their 51 %/81 % replacement rolls and the 0x527790 priority table, final rand(100) < 51 target update. 'L' without a leader returns −1 (original quirk). | `search_for_unit` | Part A §A5.2, decomp 0x54b480 | CONFIRMED |
| Ranges per class, aggression base 90/30/50 × EASY_A..VDIFF_A, block chance BLOCKPER × the same factors. | `unit_range`, `attack_chance`, `block_chance` | Part A §A5.3–A5.4 | CONFIRMED |
| 15 class routines and their task routing (ai_class from unit.res ScriptFile). | `ai_routines_{light,heavy,siege}.cpp` | Part B §3A–§3C | CONFIRMED / per-routine notes in code |
| Teams: create/join/leave/disband, max 20, `max_troop_size` = min(20, max(9 + max(0, (lead−20)/5), size)) with the TROOP_SIZE item %, auto-absorb of free (team 9999) same-nation units within 300, RECRUIT_TROOP order, script team helpers, leader death without re-election. | `troop_team.cpp` | Part B §4 | CONFIRMED |
| Formation: rows of ceil(√n), 100 apart, first row 100 behind the anchor, re-anchor when the leader is ≥ 400 away, slot picker by class group (light/mace, heavy/pole, ranged/siege) with 10-unit hysteresis; follow distance 200 + 100·k. | `formation_grid.cpp` | Part B §4.7 | CONFIRMED |
| RTS: selectable test, click-select with team, formation move (teams 400 apart, individual grids 80/200), attack unit/item, assign (join/follow/siege crew), enter/exit hand-over; third-person hold toggle and recruit-by-interact (100 units, ±90°). | `rts_*`, `toggle_hold`, `recruit_nearby` | Part B §5 | CONFIRMED |
| Path following: waypoint advance with REACH_DT × LOD/siege factors, walk below WK_DIST 50 else run, stuck > 500 ms → wait counter (INIT_W_C 5 … MAX_W_C 30) → one sidestep (±90+rand(25)°, rand(100)+20) then replan; follow-leader path copy within 320. | `path_follow.cpp` | Part C §3 | CONFIRMED / sidestep HIGH |

Tests (`ctest -R 'game|formats.pfd'`): `formats.pfd_mesh` (38 files, S1L1 counts), `game.nav` (every mesh; S1L1 heights vs `.GRI`, LOS quad, full path stays on the mesh, 100-node cap gives a partial path closer to the target, determinism), `game.physics` (10 m fall in 25 ticks = √(2h/g); block + push-aside), `game.troop` (size table, formation slots, members reach slots, absorb 300, hold, RTS move, recruit, serialize round trip), `game.ai` (ranges, aggression × difficulty, selection cases incl. cone, priority, sticky 96 %, LOS, queue rules, team propagation), `game.ai_battle` (20 v 20 English vs French light infantry for 3000 ticks: same hash on two runs, different seed differs; 100 v 100 profile).

Performance (this machine, -O2): 20 v 20 ≈ 0.03 ms/tick while fighting; 100 v 100 ≈ 1.1 ms/tick average, ≈ 4 ms worst tick (whole World tick incl. combat, AI, physics), i.e. ~3 % of the 33 ms budget.

Deviations:
1. Every unit runs phases 2/3 every tick (no CPU-budgeted passes); target search scans units in recno order instead of scene sectors, so tie-breaking rand rolls see candidates in a different order.
2. (resolved) Executors write the desired heading +0x1e; the body turns at the 0x4f2190 rate (`World::turn_step`, see "Turning, horses, interact").
3. `get_rotation` returns the absolute heading toward the target (what the TURN executor compares with), see `rotation_to`.
4. A* re-opens improved closed nodes instead of propagating g through the subtree (0x66ddb0); BVH replaced by a bucket grid; overlapping polys are resolved by the surface closest to z.
5. Path search always gets 100 expansions; the 500/1000/… "budget" argument of `path_to` is ignored (meaning unknown). Formation lateral/width offsets in the funnel are not modelled.
6. Push-aside speed 100 u/s (magnitude unknown); pending pushes are saved in the optional Mission save section `PHYS` (written only while one is active, so hashes of push-free states are unchanged).
7. (resolved) AI riders are driven, see "AI riders".
8. RECRUIT_TROOP matches the recruit type by class kingdom (race +0x14d is not loaded); news-log entries of set_task are not generated.

Gaps: `search_for_item` is a simplified reading of 0x503680; can_energy_attack ignores the energy-cost item effect; `.por` anti-portals are not loaded.

### AI riders (S6L1 Charles VII on "Charlies Horse")
| rule | code | RE | conf |
|---|---|---|---|
| A horse with a rider (+0x1e6) does not think: 0x49d810 sends it to Unit_player_control like the player. | `AiSystem::think` | decomp 0x49d810 | CONFIRMED |
| Path following of a seated rider: positions are the horse's (mount_sync); each step re-aims the rider (the horse follows its heading) and calls the run gear FUN_00466d50 (`horse_gear(2)`) unless the rider is in H_RUN; steering continues during H_RUN / H_ATTACK; arrival and the stuck stop use Unit_rider_stop 0x4533e0. | `update_movement` | decomp 0x52daa0 | CONFIRMED |
| Last waypoint radius: 2·REACH_DT for riders, siege (class 8) and LOD > 1 (it replaces the 5×/10× running radius; previously the port multiplied it, stopping riders/siege 10·REACH_DT short), REACH_DT for LARGE_WEAPON_PUSH. | `update_movement` | decomp 0x52daa0 | CONFIRMED |
| MOVE_TO_TARGET / MOVE_TO_RELATIVE_TARGET / MOVE_ON_TRACK end: a rider in H_WALK/H_BACK/H_RUN gets Unit_rider_stop, others Unit_stop. | `cleanup` | 0x4c1670, 0x4c17a0 | CONFIRMED |
| COMBO_ATTACK mounted branch: idle rider → p3−1, H_ATTACK 'A'; in H_ATTACK at record END_TIME (+0x28): p3 < 1 → Unit_stop + done, else p3−1 and another H_ATTACK 'A' ("AAA" wraps to "A"); anim end → stop + done; H_HITED..H_RHITED wait for expiry; DIE ends. p3 is therefore the swing count. | `execute_order` | 0x4bbf20 | CONFIRMED |
| Combo follow-ups (foot and mounted) wait for record +0x28 = END_TIME (u.win_close); the port used START_TIME. | `execute_order` | 0x4bbf20 + animgrp layout | CONFIRMED |
| Move-record lookup scans the table from the last record: an exact-SPECIAL match returns at once, SPECIAL-0 fallbacks keep the lowest index; SPECIAL 4 counts as 0 outside WALK/RUN. CHARLIE7 has two identical-key H_ATTACK A/AA/AAA groups: the first (DEFAULT script, DAMAGE 0) never hit; the original picks the second (charlies_HOR_ATK_A/B). | `World::find_move` | Unit_findCurrentAnimRec 0x504010 | CONFIRMED |
| Unit_deinit 0x461f10: deleting a rider frees its horse (0x482930(0)); deleting an active ridden horse dismounts the rider (0x4fc220(0)). Corpse deletion now goes through `World::delete_unit`. | `World::delete_unit` | decomp | CONFIRMED |
| The AI never mounts or dismounts on its own (0x4fc220 callers: player mount/dismount, Unit_deinit, script set_unit_ride_horse); riders leave the horse only by H_DROP (dead rider/horse, balance 0) or script. | — | xrefs | CONFIRMED |

Test `game.ai_rider`: CHARLIE7 rides CAMP_HORSE 2000 u under MOVE_TO_TARGET (horse WALK ~144 ticks, stops within 2·REACH_DT with rider_stop, ridden horse never gets orders), COMBO_ATTACK p3 = 2 → 2 H_ATTACK swings, one hit + knockdown on an English soldier, dead horse → rider H_DROP + unlinked, deleted horse → rider unlinked, deterministic.

### Multi-node worlds (missions 2–8) — src/game/multimap_nav.{hpp,cpp}
- NavWorld = one NavMesh per level (`<node>.pfd` at the node origin, box = node rect from .wor) + world mesh `<world>.pfd` (S9L1 uses S5L1w). World coords in, global poly ids (level base + local). `Mission::nav()` is non-null for every mission.
- find_path = Unit_path_to 0x505dd0: local path if the target is in the start's loaded level, else world mesh (/32, radius BODY_R/32, back ×32 +16) with the leading run inside the level replanned locally and spliced (CONFIRMED).
- find_poly_xy searches every level whose box holds (x,y), closest height (006aae70, simplified). Gates toggle their poly via find_poly_xy(..., ignore_blocked) so re-opening / reloading finds the same poly.
- Level streaming (World_draw 0x44c800): loaded = inside player±8000 || (loaded && inside ±16000); a level change sets `u.m.active` (Unit_activate/deactivate). Inactive units: no phase-1 action/turn/movement, no physics body (CONFIRMED 0x52c490 gates on +0x2f5), ignored by hit tests. Paths from unloaded levels are world-mesh only. Saved in the optional "NAVL" section.
- Deviations: camera = player position (no player → all loaded); A* budget stays 100.
- Test: `game.multimap_nav` (all 7 missions: load, cross-node path, AI border crossing, gate in a non-first node, determinism + save round trip).
