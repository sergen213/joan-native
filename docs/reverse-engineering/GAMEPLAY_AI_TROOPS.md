# Gameplay: AI, troops, pathfinding, collision, triggers, mission flow (joan.exe)

Static reverse engineering of `joan.exe` (GOG build, MSVC 6 debug-style, image base 0x400000) for the native port. Every claim carries an address and a label: **CONFIRMED** (disassembly/decompile read end to end, constants checked in disasm), **HIGH CONFIDENCE**, **HYPOTHESIS**, **UNKNOWN**. Decompiler output is not verified source. Nothing here was observed at runtime.

Conventions: offsets are bytes into `#pragma pack(1)` structs. "Unit" = record returned by `UnitArray_get 0x41b3c0(recno)` (1-based). Times are ms of game time (`DAT_00a500ba` = game time ms, `DAT_00a500be`/`DAT_00a500b6` = frame delta ms); one logic tick = 1/30 s ≈ 33 ms. World units: 46.1 units ≈ 1 m (Part C). `rand(n)` = `Misc_random 0x590540`.

Related docs: script language / FUNC table / unit+item data / damage formulas — `docs/formats/SCRIPTS_AND_GAME_DATA.md`, `docs/GAMEPLAY_SYSTEMS.md` (ScriptsGameplay). Save format — `docs/formats/CONTAINERS_WORLD_SAVES.md`. Names for all functions below: `tools/ghidra/names_ai.tsv`.

Document map:
- Part A (this part): tick order, RNG, unit scheduler, per-unit process phases, enemy AI decision layer (target selection, perception ranges, aggression, flee/retreat, group behaviour), GAMEPLAY.RES constants, unit struct quick reference.
- Part B: tasks and orders (state machines), all native AI routines per class, teams/recruiting/formations, player commands, RTS mode, key bindings.
- Part C: pathfinding (navmesh A*), movement, ground height, character physics/collision, line of sight, walls/gates, projectiles, entity containers/limits.
- Part D: trigger system, end conditions (victory/defeat), scenario/campaign transition, autosave.

---

## A1. Per-tick update order — `Game_process_tick 0x4e5fc0` (ECX = Sys 0xa50088) — CONFIRMED (disasm `mov ecx,imm` before each call)

Object names come from the `"Loading <name>"` strings pushed before each object's init in `0x46f3a0`/`0x46fc30`/`0x470d40` (CONFIRMED for every object named that way; others HIGH/HYPOTHESIS as marked).

Precondition: `Sys+0x44 (float, game speed) != 0 && Sys+0x151 (suspend) == 0`. World block runs only when `Battle(0xa51294)->state > 9` (`0x41c7e0`: `*this > 9`, HIGH: "scenario loaded") and `DAT_00a500c2 != 5`. The same predicate gates `DAT_00a596d0/00a59181 += dt` (when `DAT_00a59298 == 0`).

World simulation (in this order):

| # | call | `this` | object / meaning | conf. |
|---|---|---|---|---|
| 1 | `0x476bb0` | 0xa47ba8 | game clock: `minutes += dt_ms*0.0004` (1 real s = 0.4 game min; day = 1440 min = 3600 s), day rollover → `0x4953d0` | HIGH |
| 2 | `0x4b0780` | 0xa506e0 | modify_light_array | CONFIRMED name |
| 3 | `0x4afb20` | 0xa50530 | modify_item_array | CONFIRMED name |
| 4 | `0x51e280` | 0xa47c08 | **unit_array → `UnitArray_process`** (A3) | CONFIRMED |
| 5 | `0x487960` | 0xa3ce90 | glowing_light_array | CONFIRMED name |
| 6 | `0x547030` | 0xa527c8 | water_array | CONFIRMED name |
| 7 | `0x431f20` | 0xa48aa8 | water_wave_array | CONFIRMED name |
| 8 | `0x464740` | 0xa45d30 | dynamic_grass | CONFIRMED name |
| 9 | `0x4913c0` | 0xa48500 | throw_item_array (projectiles, Part C §9 / App. A) | CONFIRMED |
| 10 | `0x4ef090` | 0xa5ccd0 | **trigger_system** (Part D §2) | CONFIRMED |
| 11 | `0x4c55d0` | 0xa484d0 | place_array | CONFIRMED name |
| 12 | `0x46d4a0` | 0xa5d100 | facility_array (barracks/workshops, Part B §4.6c) | CONFIRMED |
| 13 | `0x4e4bb0` | 0xa52940 | territory_array (town capture) | CONFIRMED name |
| 14 | `0x471620` | 0xa58f90 | **Game::process** — end-condition evaluation, scenario switch (Part D §3.3) | CONFIRMED |
| 15 | `0x63c650` | 0xa51298 | script engine: runs every live script instance (`0x63b5e0`) | HIGH |

Then, only when `Sys+0x3a (screen mode) == 9` (in-game): UI processes (main_game_interface 0xa4d618, cut_scene_interface 0xa5d380, 0xa48a38 timed flag `0x761630`, generic_message 0xa57030, objective_menu 0xa53cb0, game_over 0xa53ff8, vict_interface 0xa40470, unit_attribute 0xa56398, upgrade 0xa58f75, buy 0xa4fc98, military_balance 0xa5ccf0, message_array 0xa55a88), 3-D sound environment `0xa50298` (`0x68a170(dt_s, view, 1|2)`, `0x68c2a0`, `0x68a010`; listener = player unit + 250 height — HYPOTHESIS for the role), animation_particle_effect_array 0xa57078. Then by screen mode: 9 → camera array `0x5634e0` (0xa48cc0) + physis_sys `0x6ad660` (0xa570a8, character physics, Part C §5); 0xe → intro; 0xf → credits. Then menu/interface processes for modes 9 and 3, then quit (0xa5cec8) and ending (0xa57c98); finally if not suspended: sound_man `0x470f80` (0xa5cf98) and **adaptive music `0x4e6fe0`** (ECX = Sys).

Consequences for the port: units move and decide (4) before projectiles (9) resolve, triggers (10) consume hit events produced in 4 and 9, and end conditions (14) are evaluated after all world updates; scripts (15) see the post-update state and their effects apply on the next tick. Character physics (`physis_sys`) integrates *after* all logic, in the mode-9 block.

## A2. RNG — CONFIRMED (`0x590540`, `0x590510`, `0x5904e0`)

One global generator, object `0xa45fa8` (ilib "Misc"), seed at `+0x7a` (int32). All 255 call sites of `0x590540` pass ECX = 0xa45fa8 (CONFIRMED by scan).

```c
int32 seed;                          // misc+0x7a
int rand(int n) {                    // Misc_random 0x590540, ret 4
    seed = seed * 0x015A4E35 + 1;    // imul/add, 32-bit wrap
    return (uint32)(n * ((seed >> 16) & 0x7FFF)) >> 15;   // imul then SHR (unsigned)
}
```
Seeded once at start-up with CRT `time(0)` (`0x5904e0` → `0x590510`, called from WinMain for objects 0xa45fa8 and 0xa50490). The seed is never written anywhere else (no stores to 0xa46022 in .text) and is not part of save games → original gameplay is non-deterministic across runs. For deterministic simulation the port should seed explicitly and reseed on scenario load/save-load (port decision, not original behaviour).

## A3. Unit scheduler — `UnitArray_process 0x51e280` (ECX = unit_array 0xa47c08) — CONFIRMED

Skipped when `DAT_00a06bf4 == 0` or `DAT_00a596ab != 0` (script `set_stop_unit_process_flag`, handler `0x65c500`).

**Cut-scene branch** (`Game+0x744 != 0`, `0x4301c0`): for recno = size..1: if `unit+0x2e` (can-process-in-cut-scene, script `set_unit_can_process_in_cut_scene`) → `unit+0x22 = 1`, `Unit_process(0..3)`; else if `!unit+0x2e1` → `0x508b90(0,1)` (stop/idle) then `Unit_process(0)`, `Unit_process(1)`.

**Normal branch:**

1. Pass 0 (every unit of the current world, `unit+0x335 == DAT_00a45f3e`, recno 1..N):
   - `unit+0x22 = Unit_lod_tier()` (`0x508880`): distance² in **x/y only** from the player unit (`DAT_00a5b9e8`; camera position instead when `DAT_00a5b9f8 == 4`, RTS view): ≤4e6 (2000 u) → 1, ≤1.6e7 (4000) → 2, ≤6.4e7 (8000) → 3, else 4; the player unit itself = 1; no player = 4.
   - **alive test** `A = unit+0x102 == 0 (player-controlled) || hp(+0x135) > 0 || +0x120 != 0 || retreat(+0x114) == -2 || (actor(+0x2f5) && actor+0x30 (fade rate) != 0 && now − actor+0x38 ≤ 1000/|actor+0x30|)` (`0x7b1f6b` = CRT fabs, HIGH). Dead units keep processing while their death fade runs (`set_unit_dead` sets rate −0.1 → 10 000 ms).
   - If `A`: when tier ≤ 1 → `Unit_process(0)`, `Unit_process(1)` immediately.
   - Else (dead and faded): remove from its team (`0x5288f0` via `0x4216d0`), delete (`0x51e1f0(recno)`).
2. Twelve budgeted round-robin passes. Each pass `k` keeps a persistent cursor `short this[0x40+2k]` and budget `short this[0x58+2k]`; it walks recnos from the cursor (wrapping N→1), processes units matching its filter, stops when it returns to the cursor or after `budget+1` processed units, and stores the cursor back.

| pass | cursor/budget | filter (tier = +0x22, rank = +0x104) | phases | budget floor | K |
|---|---|---|---|---|---|
| 1 | 0x40/0x58 | tier 2 | 0,1 | 50 | 1.0 |
| 2 | 0x42/0x5a | tier ≥3 | 0,1 | 50 | 1.0 |
| 3 | 0x44/0x5c | tier ≤1, rank >3 (GENERAL/KING) | 2 | 1 | 1.0 |
| 4 | 0x46/0x5e | tier ≤1, rank ≤3 | 2 | 10 | 1.0 |
| 5 | 0x48/0x60 | tier 2 | 2 | 10 | 1.0 |
| 6 | 0x4a/0x62 | tier 3 | 2 | 10 | 1.0 |
| 7 | 0x4c/0x64 | tier 4 | 2 | 10 | 0.5 |
| 8 | 0x4e/0x66 | tier ≤1, rank >3 | 3 | 1 | 1.0 |
| 9 | 0x50/0x68 | tier ≤1 | 3 | 3 | 1.0 |
| 10 | 0x52/0x6a | tier 2 | 3 | 3 | 1.0 |
| 11 | 0x54/0x6c | tier 3 | 3 | 3 | 1.0 |
| 12 | 0x56/0x6e | tier 4 | 3 | 3 | 0.5 |

   After each pass: `ms = rdtsc_delta*1000/cpu_hz` (`0x45e750` rdtsc, `0x45e7f0` → float ms using cpu-usage object 0xa485a8 +0x44); `if ms != 0: budget = (n*K/ms > floor) ? (short)(n*K/ms) : floor` — i.e. each pass is allowed ≈1 ms (0.5 ms for tier 4) of CPU per tick. Note pass 9 (tier ≤1 phase 3) also re-runs generals already handled by pass 8.

Port guidance: the budget is CPU-speed dependent, so the original is not reproducible tick-for-tick. A faithful-enough port can process every unit's phases 0–3 each tick (what a fast CPU converges to for small armies), or emulate the passes with fixed budgets (e.g. the floors × 4) if far-unit cost matters. HYPOTHESIS: original designers intended far units to "think" at reduced rates; the 1 ms budget on a 2004 CPU processed ~50–200 units per pass.

## A4. Per-unit process — `Unit_process 0x52c490(phase)` (ECX = unit) — HIGH

Returns 0 if `unit+0x335 != current world`.

- **Phase 0 — upkeep:** clear `+0x2e3`/`+0x2e5` (short); `0x49da30` auto-pickup (generals, rank>3, within reach of items: potions when hp short, gold/food, mission items); if player unit `0x416680`; actor sound handles `+0x32d/+0x331` (`0x687a60`); `0x518b90`; if `+0x2e1` set and anim state `+0x215 != 7` and `+0x2f1` → stop and play IDLE/7 anim; unit type subclass 0xb with attached `+0x339` → "BAT_TRK" sound; **task LARGE_WEAPON_PUSH (15)**: pusher stays attached while within `max(200, weaponBound+20)` of the weapon (bound by actor collision kind +0xb8: 1 sphere r +200, 2 box half-diagonal, 3 cylinder r +0xf0), else detach (`actor+0xb8 = 0`, `+0x38e = 1`); **task RETREAT (11)**: `task.para3 (+0x2b2) -= dt` while > 0.
- **Phase 1 — timers/regen:** `0x5174e0`, `0x454b20`; level-up arrow flag `+0x130` cleared after `LEVELARR` 5000 ms from `+0x131`; actor update `0x7c7ab0` (action animation, projectile release), movement-state `0x4f2190`; `Unit_update_movement 0x52daa0` when no action owner or AI-controlled (Part C §3.2); sector/actor re-registration (`+0x325`); MP regen: every `RECOV_T` 10 000 ms (+0x151 timer) `mp(+0x145) += RECOV_MP 1` while `mp < max`; morale/upkeep every 1 000 ms while `+0x1cb < 100` (`0x4fea80`); HP regen: every `RECOV_HP` 10 000 ms (+0x118 timer) if `hp < maxhp (0x523320)` and `+0x114 != -1` → `set_hp(ftol(hp + (def*DEF_HP_R > 1 ? def*DEF_HP_R : 1.0)))` via `0x4fe510`, `def = Unit_defense 0x5238c0 = +0x159 (short) + item bonus slot 5` (CONFIRMED disasm 0x52cebb–0x52cf26; DEF_HP_R 0.1 is read with atof) (both regen paths suppressed when `DAT_00a5d384`); invulnerability timer `+0x1dd -= dt`; AI auto-upgrade after `UPG_TIME` 600 000 ms (+0x1bc) for non-hero types (`type+0x120 == 0`) → `0x4a95a0` (unitlvl.cpp); `+0x2a = now`.
- **Phase 2 — orders:** return early if `0x6e7b50()` (game paused by UI); `Unit_auto_join_team 0x4655a0`; if `0x44a660()` (unit is a team leader) → `Team::update_formation 0x4fa0f0(0,1)`; **order executor** `0x4baf80(unit)` on the order queue `+0x2a1` (Part B §2); then idle-animation upkeep depending on LOD tier `+0x22` (tier 1: `0x6ad980(actor,1)` when idle; tier 2–4: freeze idle anim for most states).
- **Phase 3 — AI think** `Unit_ai_think 0x49d810`: player main character with `+0x102 == 0`, or `+0x1e6 != 0` → `Unit_player_control 0x4fd630` (HYPOTHESIS). Otherwise, if the order queue is non-empty → return (AI only plans when idle). If `+0x247 == 0` (normal): first call sets `+0x29d = now + rand(500)`; then every `unittype+0x121` ms: `0x528440`, select AI program `unittype+0x125` (`0x484540`), run `UnitAI_dispatch 0x5467b0`; if it returned 0 and `+0x299` (script object) → run that script (`vtbl+0xc`); `+0x29d = now`. If `+0x247 != 0` the AI runs every tick.

## A5. Enemy (and friendly NPC) AI — native code

### A5.1 Why `script/unit/unitAI.res` is inert — CONFIRMED
`unitAI.res` (18 records: name[34] + int offset, loaded by `UnitScriptRes_load 0x5462b0` into 0x28-byte records `{short id; char name[32]; ... short ai_class @+0x26}`) contains only `exit` bodies; the original script logic survives as comments. The developers compiled it to C++: `ai_class = UnitAI_class_from_name 0x546520(name)` and `UnitAI_dispatch 0x5467b0` switches on it:

| class | name(s) | dispatcher |
|---|---|---|
| 1 | human_light_infantry | `0x531f90` |
| 2 | human_light_general | `0x5322c0` |
| 3 | special_general | `0x532520` |
| 4 | human_heavy_infantry | `0x532780` |
| 5 | human_heavy_general | `0x532a00` |
| 6 | human_archer, human_archer_ex | `0x532c60` |
| 7 | archer_gen, archer_ex_gen | `0x532ef0` |
| 8 | human_cannon | `0x533170` |
| 9 | autobow | `0x533370` |
| 10 | batterram | `0x533440` |
| 11 | camp_horse | `0x5335e0` |
| 12 | cart | `0x5336e0` |
| 13 | citizen | `0x533810` |
| 14 | human_trebuchet | `0x533920` |
| 15 | pig | `0x533b10` |

The routines call the script FUNC handlers directly (e.g. `0x6517c0` search_for_unit, `0x661340` give_order), so the AI = "task routing + engine primitives". Each dispatcher switches on the current task (`get_curr_task 0x6520d0`, inheriting the leader's task when the unit has none; Part B §1) — per-class routing tables are in Part B §3A–3C.

### A5.2 Target selection — `Unit_search_for_unit 0x54b480(type, radius, centerOnLeader, place, maxYawDeg, maxPitchDeg)` (ECX = unit; script wrapper `0x6517c0(unit, …)`) — CONFIRMED

- `radius == 0` → 1600. Returns a unit recno or 0.
- `type[0]`: `'T'` = current target `+0x2d7` if still valid; `'E'` enemy (`Unit_is_enemy 0x5094d0`); `'P'` the player unit (`DAT_00a5b9e8`); `'S'` enemy siege weapon (type class 8 with flag `0x40` in type+0x11c, not visible); `'F'` friend (`0x5095e0`); `'L'` team leader (returns leader if within radius); `'O'` units with `+0x10c != 0`. `type[1] == 'P'` restricts candidates to place `place` (`0x456c70`/`0x60d5f0` point-in-place).
- Search centre = own position, or the team leader's when `centerOnLeader` and the unit has a leader.
- **Sticky target ('E')**: if the current target is valid, not retreated (`+0x114 != -1`), within radius, passes the angle limits and is not hidden behind an anti-portal (`Level_segment_blocked 0x560d80`, Part C §6), keep it unless `rand(100) >= 96` (96 % keep). Targets riding inside a cart/siege slot (`type+0x118 == 0xb` and slot flag 2) are skipped.
- Candidate set: scene-sector query over the AABB `centre ± radius` (`0x5ef610`), scene objects of type 0xb (unit); skip self, dead (`hp ≤ 0 && +0x120 == 0`) or `+0x114 == -1`. Units in different "rooms" (actor+0x212/0x20e interior ids differ) are skipped unless the searcher is ranged (class 5/6, or class 8 with subclass 4,5,6,9,10,12).
- Enemy acceptance additionally requires LOS (`0x560d80`, unit eye height `ftol(...)`), target type flag bit0 (`type+0x11c & 1`, targetable), not class 8 (siege) — siege are found only via 'S'.
- Distance cap: citizen targets (class 0) must lie within `radius / 1.2` when radius ≥ 400 (`fdiv` by double 1.2 at `0x54c3da`) — CONFIRMED.
- Optional view cone: `|yaw to target| ≤ maxYawDeg` (`0x505960`, wrapped to ±180) and `|pitch| ≤ maxPitchDeg` (`0x505b50`).
- Max accepted candidates before stopping: radius <200 → 2, <1000 → 5, <2000 → 10, <3000 → 20, else 100.
- **Selection by `unit+0x16b` attack behaviour** (set by `set_unit_attack_behaviour 0x651690`): 1 NEAREST: first candidate, then a nearer one replaces it with `rand(100) < 51`; 2 WEAKEST: lowest `maxhp*(+0x120) + hp` with 51 % replacement; 3 IMPORTANT: highest rank `+0x104` with 51 %; 4 NEAREST_ARCHER: prefers archer/crossbow class (5/6) targets — an archer replaces a non-archer unconditionally, nearer same-kind with 81 %, a non-archer replaces an archer only if nearer than half the best distance²; 5 NEAREST_HIGH_PRIORITY: highest type priority `0x527790(type)` first, equal priority nearer with 81 %.
- After an 'E' search: with `rand(100) < 51` the result becomes the new `+0x2d7` target, except a still-valid old target is kept if the new result is 0.

### A5.3 Perception / engagement ranges — `get_unit_range 0x65b280(unit, kind)` — CONFIRMED

By unit-type class (`type+2`: 0 CITIZEN, 1 LIGHT_INFANTRY, 2 HEAVY_INFANTRY, 3 POLEMAN, 4 MACEMAN, 5 ARCHER, 6 CROSSBOW_MAN, 7 CAVALRY, 8 SIEGE_WEAPON, 9 CART — `0x527220`):

| class | kind 1 (engage) | kind 2 (alert/aggro) | kind 3 (leash/chase) | kind 4 |
|---|---|---|---|---|
| 1 light inf | 800 | 1600 | 2500 | – |
| 2 heavy inf | 600 | 1200 | 2500 | – |
| 3 poleman | 600 | 800 | 2500 | – |
| 4 maceman | 1000 | 2000 | 2500 | – |
| 5 archer | 250 | 1200 | 1600 | – |
| 6 crossbow | 250 | 1400 | 1800 | – |
| 8 siege | 400 | 1600 | 1600 | 3200 |
| 0, 7, 9 | 0 | 0 | 0 | – |

Other fixed radii used by the AI: melee contact check 150, "almost contact" 200, leader-leash 1000 (follow formation if leader farther), auto-join-team 300 (`0x4655a0`: 90 000 = 300²), battle music `BATTLE_R` 1500 (`0x4e6fe0` searches 'E' around the player), minimap enemy visibility `VISION_R` 2000.

### A5.4 Aggression (attack chance) — CONFIRMED (`0x511e70`, `0x511f80`)

`base = 90` if `type+0x149 ∈ {4,5}`, else 30 for ARCHER/CROSSBOW class, else 50 (stored in `unit+0x2df`, short, reset to base after every successful attack roll). `chance = ftol(+0x2df × A[difficulty])` with `A = {EASY_A 0.5, NORM_A 1.0, DIFF_A 1.5, VDIFF_A 2.0}` (globals 0xa39184/0xa3919c/0xa391a8/0xa39190), difficulty = `Config` byte 0xa57fc2 (0..3). The GAMEPLAY.RES `_B` keys are never read (loader reads `EASY_A..VDIFF_A` twice into 0xa391a4/88/a0/98 — original key-name bug, CONFIRMED in `0x472ee0`).

### A5.5 Infantry decision routine — `AI_infantry_default 0x534090` (class 1; tasks 0,1,9,18,default) — HIGH (call/push sequence CONFIRMED in disasm)

Order calls are `give_order(unit, action, prio, team, p1, p2, p3, p4, expire_ms)` (`0x661340`, Part B §2). Unit "mode" `+0x167`: 1 ATTACK, 2 ACTIVE, 3 IDLE (`set_unit_mode 0x6515d0`; `is_attack_mode` = 1, `is_idle_mode` = mode > 1).

```c
tgt   = search(me,"T");                       // current target
if (leader = get_leader_recno(me)) {          // 0x6569f0
    if (!search(me,"L",1000)) {               // leader farther than 1000
        give_order(me, FOLLOW_FORMATION, 2, team=1, ..., 10000);
        set_unit_target_unit_recno(me, 0); return 1;
    }
    if (!tgt) give_order(me, FOLLOW_FORMATION, -2, 1, ..., 10000);   // low-prio regroup
}
e = search(me,"E",150);
if (!e) { set_mode(ACTIVE); e = search(me,"E",200); if (e && rand(50)==0) set_mode(ATTACK); }
else set_mode(ATTACK);

if (is_attack_mode(me)) {
    if (!e) return 1;
    give_order(me, TURN, 3, 0, get_rotation(me,e));       // face it
    return AI_infantry_melee_roll(me);                       // 0x535c20
}
if (is_idle_mode(me)) {                                      // ACTIVE or IDLE
    ltgt = leader ? search(leader,"T") : 0;
    set_attack_behaviour(me, NEAREST_HIGH_PRIORITY);
    r1 = get_unit_range(me,1);
    e  = search(me,"E", task_para1 ? r1/2 : r1, centerOnLeader=1);
    c  = search(me,"E", r1/2, 0);
    if (c && class(c) not in {ARCHER, CROSSBOW}) {         // enemy melee close: skirmish
        r = rand(10);
        if (r < 1)      give_order(me, APPROACH_ENEMY, 1, 0, e, 60, 60, 0, 30000);
        else { give_order(me, TURN, 1, 0, get_rotation(me,e));
               give_order(me, SHIFT, 1, 0, r<5 ? 1 : r<9 ? 2 : 3, 0,0,0, 1500); }  // side-step
        return 1;
    }
    if (e) { give_order(me, APPROACH_ENEMY, 1, 0, e, 60, 60, 0, 30000); return 1; }
    if ((!leader || ltgt) && !task_para1) {
        set_attack_behaviour(me, NEAREST_HIGH_PRIORITY);
        e = search(me,"E", get_unit_range(me,2), 1);       // aggro radius
        if (e) { give_order(me, APPROACH_ENEMY, 1, 0, e, 60,60,0, 30000); return 1; }
    }
    if (!task_para1 && tgt && get_unit_dist(me,tgt) <= get_unit_range(me,3))   // leash
        { give_order(me, APPROACH_ENEMY, 1, team=1, tgt, 60,60,0, 30000); return 1; }
    set_mode(me, IDLE);
}
return 1;
```

`AI_infantry_melee_roll 0x535c20` (CONFIRMED): return 0 if the unit is busy (anim state +0x215 ∈ {8,0x19,0x1c–0x22,0x46,0x49,0x53}). `r = rand(100)`; if `r < chance` (A5.4): reset `+0x2df` to base; `rand(3)==0` → `COMBO_ATTACK "A"` else `"AA"` (`give_order(me,1,2,0, string_to_order_para(s,1,…),…,1500)`), then with `rand(100) < 90` `SHIFT 3` (prio 1, 1500 ms). Else if `r < 90`: nothing. Else `SHIFT rand(3)+1` (prio 2, 1500 ms).

Variants (full pseudo-code / deltas in Part B §3A–3C): DEFEND_PLACE `0x534880` uses `"EP"` (enemy inside the task's place), leashes to targets inside the place, otherwise `APPROACH_PLACE(place)` prio −3 and IDLE; generals `0x536390` are identical but use the 135° yaw cone on the aggro search and their own attack roll `0x538340`; archers `0x540070` use pitch ≤ 65°, approach with stand-off `range2 − 400`, prefer barrels/items to shoot (`ENGLISH_EX_ARCHER` searches item codes), shoot via `0x541270` (`rand(10) < chance` → `SHOOT` prio 2, 1500 ms).

### A5.6 Flee / retreat / danger — HIGH

- Task FLEE (5), RECRUIT_TROOP (3), HELP_TROOP (4) and IDLE (7) are no-ops (`return 1`) in every class dispatcher; ATTACK_UNIT (2) and FLEE are never assigned by engine or shipped scripts (Part B §1.5). There is **no HP-based autonomous fleeing**.
- `RETREAT` (11) is script-issued: `AI_task_retreat 0x545d50`: `give_order(APPROACH_PLACE, prio 99, team, place=para1, 30000)`; when the unit is inside the place or `para3` timer (decremented in phase 0) ≤ 0 → `set_unit_dead` (fade out, "escaped"), set idle, stop task, clear orders.
- Retreat flag `+0x114` (script `set_unit_retreat`): value 1 = the unit cannot die — on lethal damage it gets an IDLE task one priority higher and the flag becomes −1 (Part B §1.6); −1 = "defeated/retreated" (excluded from all searches, no HP regen, fades out with alpha); −2 = keep processing.
- `is_in_danger 0x4fd980` (script FUNC 57): `hp/maxhp < INDANGER 0.2`; generals (rank > 2) and units whose leader has inventory first auto-drink the potion (item type 0x15, else 0x19) whose restore value best matches the missing HP, playing `spiritual.txt` particles, and return 0; else 1.
- `is_attacked 0x6512d0`: anim state ∈ {0x23–0x2a, 0x40–0x42} (being hit / knocked). `will_be_attacked 0x651430(victim, attacker)`: attacker anim ∈ {0x1c,0x1e–0x21,0x53} and victim within ±90° of the attacker's facing.
- Infantry/general dispatchers: if `is_attacked` and the current task is IDLE (7) with no para → `stop_curr_task` on itself and on its leader if the leader is also IDLE (the unit reacts by dropping a passive "idle" posting).

### A5.7 Group behaviour — HIGH
- `give_order(..., team=1)` (and always for APPROACH_ENEMY) copies the order to the team leader and to every member whose task is unset or equal to the leader's (priority −1, `issuer = me`); APPROACH_ENEMY is not copied to members that already have a target or an APPROACH_ENEMY order — focus-fire spreads through the troop but does not override existing fights (`0x661340`).
- Members inherit the leader's task (`get_curr_task` falls back to the leader's task unless that is RTS_MOVE_TO 12).
- Members out of leader range (>1000) abandon combat and run FOLLOW_FORMATION; idle members regroup with low priority (−2).
- Leaders automatically absorb leaderless same-nation units within 300 (`0x4655a0`, class not 0/8/9, subclass 0/1, team size < max).
- Formation geometry, troop size and leader-death handling: Part B §4.

### A5.8 Adaptive music — `0x4e6fe0` — HIGH
Music state: 2 "battle" when an enemy is within `BATTLE_R` 1500 of the player (search 'E', centre on leader) or a territory under attack; 6/7 by `DAT_00a59298` (lose/win); 8 when in an enemy territory; 9/10 cut-scene variants; extra layers every 30–60 s (`rand(30000)+30000`). Not gameplay-affecting.

## A6. GAMEPLAY.RES constants — CONFIRMED (`Resource/GAMEPLAY.RES`, readers `0x51c680`, `0x472ee0`, THROWI `0x5525b0`)

Text sections `{SECTION}` with `[KEY]value`. Integers are parsed with `atoi` (so `DEF_RAT 0.3` → 0, `STR_DAM 3.0` → 3; see GAMEPLAY_SYSTEMS.md); keys marked *f* use `atof`. The port should read this file rather than hard-code values. AI/movement-relevant keys (global VA in parentheses):

| key | value | use |
|---|---|---|
| UNIT BODY_R (0xa3cc9c) | 32 | unit body radius |
| MAX_W_C / INIT_W_C (0xa3ccdc/0xa3cd04) | 30 / 5 | blocked-path wait counters (Part C §3) |
| WK_DIST (0xa3ccd8), HORSE_WD (0xa3cbe0) | 50, 500 | walk vs run threshold |
| LD_FM_DT (0xa3cc74) | 160000 | leader→formation distance² (400²) before the formation anchor moves |
| REACH_DT (0xa3cc28) | 15 | arrival radius |
| FELL_DUR / INVC_DUR | 2500 / 6000 | knock-down, post-get-up invulnerability |
| AI_MOVE *f* (0xa3cc70) | 1.2 | AI movement-speed ratio |
| MINDEXS / MAXDEXS | 30 / 200 | dexterity→speed clamp |
| MOR_NOR, MOR_CH_U *f*, MOR_CH_T *f* | 50, 1.0, 0.3 | morale |
| BLOCKPER / BLOCKANG | 50 / 70 | AI block reflex (Part C App. A §10) |
| UPG_TIME (0xa3cc24) | 600000 | AI auto-upgrade delay |
| DANGER INDANGER *f* (0xa3cd24) | 0.2 | is_in_danger |
| MINIMAP VISION_R (0xa3cc44) | 2000 | minimap enemy radius |
| MUSIC BATTLE_R (0xa39180) | 1500 | battle music |
| GAME EASY_A..VDIFF_A | 0.5/1/1.5/2 | AI attack chance (A5.4) |
| GAME EASY_D..VDIFF_D | 0.7/1/1.5/2 | hostile damage (GAMEPLAY_SYSTEMS.md) |

Full key→global map (98 keys) is reproducible with the `cfgtable(0x51c680)` helper in `work/AITroopRE/re.py`; THROWI keys in Part C App. A §2.

## A7. Unit struct — quick reference of fields used by AI/troops/movement

Consolidated from all parts (offset → type → meaning → evidence). Full per-system tables: Part B §1.1, §2.1, §4.1; Part C §1.2/§5.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | short | recno | everywhere |
| +0x02 | short | nation id | `0x4655a0` same-nation test, `set_main_character` |
| +0x0e/+0x12/+0x16 | float×3 | position x, y, z (z up) | `0x54b480`, `0x508880` |
| +0x1a | float | facing (deg) | `set_main_character` camera |
| +0x22 | int | LOD tier 1..4 | A3 |
| +0x26 | short | — (uscrres record +0x26 is AI class, not unit) | `0x5467b0` |
| +0x2e | char | can process in cut-scene | A3 |
| +0x2f | char | not player-controllable (`set_control_flag`) | `0x6574a0` |
| +0x31 | short | team id (9999 = none) | `0x657fd0`, `0x4655a0` |
| +0x33 | Team* | team (Part B §4.2) | many |
| +0x3b | short | unit-type id → `0x42d0a0(id)` type record | many |
| +0x102 | short | 0 = player-controlled (main character) | `set_main_character 0x658f70` |
| +0x104 | int | rank 0 INFANTRY,1 ARCHER,2 CAVALRY,3 CAPTAIN,4 GENERAL,5 KING (`0x527130`) | scheduler, search IMPORTANT |
| +0x10c | int | 'O' search flag | `0x54b480` |
| +0x114 | int | retreat flag (1 can't die, −1 retreated, −2 keep alive) | A5.6 |
| +0x120 | int | multiplier in WEAKEST metric / alive test (HYPOTHESIS: "lives") | `0x54b480`, A3 |
| +0x135 | int | hit points | many |
| +0x13d | int | max hit points (cached) | `0x4fd980` |
| +0x145 | int | energy (MP) | phase 1 |
| +0x167 | int | AI mode 1 ATTACK, 2 ACTIVE, 3 IDLE | `0x6515d0` |
| +0x16b | int | attack behaviour 1..5 | `0x651690` |
| +0x1cb | short | morale (<100 regenerates) | phase 1 |
| +0x1dd | int | invulnerability ms | phase 1 |
| +0x1e6 | int | player-control override | `0x49d810` |
| +0x215 | short | current anim/action state (0x2a = dead) | `is_died 0x653c60` |
| +0x247 | short | think-every-tick flag | `0x49d810` |
| +0x299 | ptr | per-unit script object | `0x49d810` |
| +0x29d | int | last AI think time | `0x49d810` |
| +0x2a1 | OrderQueue* | order queue (0x3e bytes) | Part B §2 |
| +0x2a5 | Task (0x19) | current task; +0x2be second task slot | Part B §1.2 |
| +0x2d7 | short | current target recno | `0x54b480` |
| +0x2df | short | aggression (attack chance %) | A5.4 |
| +0x2e1 | char | locked (no task change / stop) | `0x432b90`, Part B |
| +0x2f5 | Actor* | 3-D actor (+0x30 fade rate, +0x34 alpha, +0x38 fade start, +0x5c physics body, +0xb8 collision kind) | A3, Part C |
| +0x335 | World* | owning world | A3 |
| +0x339 | Level* | map piece for LOS/height | Part C §6 |
| +0x384 | int | attached-to unit recno (cart/siege slot), +0x388 slot | `0x54b480` |

Unit-type record (`0x42d0a0(id)`, parsed by `0x525080` from `unit/unit.res`): +2 class, +4 code string, +0x118 subclass (0xb = mountable cart/siege tower), +0x11c flags (bit0 targetable, bit 0x40 siege target), +0x120 hero flag (non-zero = no auto-upgrade), +0x121 AI think interval ms, +0x125 AI program id (uscrres), +0x149 aggression kind, +0x1ad attach-slot count, +0x1b9 slot flags — offsets CONFIRMED, field names HIGH.

## A8. Cross-part notes, conflicts, gaps

- `FUN_0042e1a0`, named `Game_quick_save` in `names.tsv`, is the global key handler (`ViewCtrl_process_key`, Part B §5.4.1) — HIGH; names.tsv not edited (not owned).
- Autosave: `GameFile_make_name 0x477770(buf, auto)` is reached with auto=1 only from `Game_autosave 0x4779e0` (`_AUTO%03d.SAV`), which has no callers or pointer references → the engine never autosaves; manual saves use auto=0 from the save UI `0x74f230`. The only real autosave is the script `save_game("Mission1-AutoSave", …)` in `S1L1_tactic.sce` (Part D §4.5) — CONFIRMED.
- " troop goes to reinforce / attack / flees" are news-log strings written by `set_task` for message types 0x30–0x32; the formatter `0x4ba890/0x4ba930/0x4ba9d0` is reached only through that log (Part B §1.5). No strategic "AI general" exists.
- `set_charmove_detect_block_flag` writes a flag nothing reads (Part C).
- UNKNOWN: APPROACH_ENEMY p2/p3 (60, 60) exact semantics (Part B §2 executor `0x4bd420`), `+0x120` meaning, `0x416680`/`0x518b90`/`0x5174e0`/`0x454b20` phase helpers not decoded.
- Port note: the CPU-time budgets (A3) and time(0) seed (A2) are the only intentional sources of non-determinism found; all AI randomness flows through `rand()`.


### A8.1 Name aliases
Parts B–D were written in parallel; where they used a different name for the same function, `tools/ghidra/names_ai.tsv` keeps one (left column). Addresses are authoritative.

| addr | name in names_ai.tsv | also called in Parts B–D |
|---|---|---|
| `0x42ff90` | `Unit_is_playable_hero` | `Unit_is_hero` |
| `0x4301c0` | `Game_is_cutscene` | `ViewCtrl_is_paused` |
| `0x4655a0` | `Unit_auto_join_team` | `Unit_absorb_free_units` |
| `0x467c30` | `Unit_find_incoming_missile` | `ProjectileMgr_nearest_incoming` |
| `0x47e200` | `Trigger_init` | `Event_init` |
| `0x497e10` | `Unit_item_effect_sum` | `Inventory_get_effect_total` |
| `0x49d810` | `Unit_ai_think` | `Unit_think_ai` |
| `0x4bbbc0` | `Order_id_from_name` | `Order_name_to_id` |
| `0x4e4520` | `UnitTask_set` | `Task_set` |
| `0x4eef00` | `TriggerSys_add` | `Event_post` |
| `0x4ef090` | `TriggerSys_process` | `EventQueue_process` |
| `0x505dd0` | `Unit_path_to` | `Unit_move_to` |
| `0x5094d0` | `Unit_is_enemy` | `Unit_is_hostile_to` |
| `0x512070` | `Unit_block_chance` | `Ai_block_chance` |
| `0x51c680` | `GameplayRes_load_unit_consts` | `Gameplay_load_unit_vars` |
| `0x5229a0` | `Unit_add_exp` | `Unit_add_experience` |
| `0x546520` | `UnitAI_class_from_name` | `UnitAI_script_class_id`, `UnitScriptRes_name_to_native_id` |
| `0x5467b0` | `UnitAI_dispatch` | `UnitScriptRec_run_native` |
| `0x60d5f0` | `AABB_contains` | `Box3d_contains` |
| `0x661340` | `Unit_give_order` | `Script_give_order` |
| `0x6b8af0` | `GameOver_lose_freeze_units` | `GameOverBox_open` |
| `0x6e7900` | `GameOver_win_freeze_units` | `WinMsg_show` |
| `0x75d540` | `Unit_on_hit_message` | `Unit_on_event`, `Unit_on_trigger` |

---

# Part B — Tasks, orders, native AI routines, teams, formations, player commands (joan.exe)


## 0. Synopsis (what a reimplementation needs)
- **Task** (Unit+0x2a5, 0x19 B) = long-lived goal set by scripts/RTS/map loader; **Order** (Unit+0x2a1 OrderQueue) = short
  action (1..22) issued every AI think by the native per-class unit AI (§3) or scripts; executed each tick by
  `OrderQueue_process 0x4baf80` from `Unit_process 0x52c490` (§2).
- Members without a task run the **leader's task** (get_curr_task/para inherit, §1.8); the leader's Team (0x58 B, max 20,
  §4) is the troop. Formation slots: rows of ceil(sqrt(n)), 100 apart, 100 behind the anchor (§4 › 7).
- Player control: third-person mode has **no** follower order keys (only hold toggle + recruit-by-interact); orders are
  given in RTS mode (F2, scenario ≥5) → set_task 12/19/13/17/15/10 (§5 › 3.4). Default keys/remap in §5 › 5.
- No AI "general": tasks ATTACK_UNIT/RECRUIT_TROOP/HELP_TROOP/FLEE are never assigned; the "troop goes to …" strings are a
  news-log side effect of set_task (§1.5).

## 1. Core task model

### 1.1 Unit fields used by the task/order system
| off | type | meaning | evidence | conf |
|---|---|---|---|---|
| +0x002 | short | nation/side (copied into news msgs) | 0x528050 `movsx [ecx+2]` | HIGH |
| +0x031 | short | team id ("Group:" key in .uni) | 0x5590? .uni loader writes `+0x31 = atoi(Group)` (FUN_00558a60); stop_all_team_task 0x6523c0 matches it | CONFIRMED |
| +0x033 | Team* | non-null only on the team leader | FUN_0044a660 | CONFIRMED |
| +0x037 | short | leader recno | FUN_004216d0 | CONFIRMED |
| +0x114 | int | retreat flag: 1 = "retreat instead of dying" (see 1.6), -1 after it fired, -2 special | FUN_0064f320 | HIGH |
| +0x1dd | int | set 0x7fffffff when a retreat-flagged unit "dies" | FUN_0064f320 | HIGH |
| +0x29d | int | set 1 when task LARGE_WEAPON_PUSH(15) accepted (then FUN_0049d810) | 0x528213 | CONFIRMED |
| +0x2a1 | OrderQueue* | order (action) queue, lazily `new(0x3e)` | FUN_00661340 | CONFIRMED |
| +0x2a5 | Task (0x19 B) | current task | FUN_00528300 `add ecx,0x2a5` | CONFIRMED |
| +0x2be | Task (0x19 B) | secondary task slot; zeroed on every set/clear; FUN_005121c0/FUN_00432300/FUN_004f8550 write `(0,0,1,0,0,0)` into it | 0x5283e4, 0x5122xx | HIGH |
| +0x2d7 | short | current target recno (immediately after +0x2be slot) | known | CONFIRMED |
| +0x2e1 | char | "task locked": set_task refused while non-zero (FUN_00432b90) | 0x527fc3 | CONFIRMED |
| +0x2aa..+0x2ba | | Task fields of +0x2a5 (see 1.2) | | |
| +0x3c9 | int | copied from leader into member by get_curr_task when member inherits leader's task | FUN_006520d0 | CONFIRMED (meaning UNKNOWN) |
| +0x378/+0x37c | int*[] | (large weapon) per-push-slot pusher recno / aux arrays, indexed by pusher's +0x380 | FUN_00528300 | CONFIRMED |
| +0x380/+0x384/+0x388 | int | LARGE_WEAPON_PUSH: slot index (-1 none), +0x384, +0x388 cleared on release | FUN_00528300 | CONFIRMED |

### 1.2 Task struct (0x19 bytes, packed) — `FUN_004e4520(id, prio, p1, p2, p3, expire)` (ECX=&task)
```c
#pragma pack(1)
struct Task {            // at Unit+0x2a5 (current) and Unit+0x2be (secondary)
  int32  id;             // +0x00  0 = none (TASK ids below)
  int8   prio;           // +0x04  signed char
  int32  para1;          // +0x05  (Unit+0x2aa)
  int32  para2;          // +0x09  (Unit+0x2ae)
  int32  para3;          // +0x0d  (Unit+0x2b2)
  uint32 expire_ms;      // +0x11  (Unit+0x2b6) absolute game time DAT_00a500ba; 0 = never
  int32  state;          // +0x15  (Unit+0x2ba) per-task FSM state, reset to 0 by FUN_004e4520
};
```
Task ids (FUN_004e4610): 1 ATTACK_PLACE, 2 ATTACK_UNIT, 3 RECRUIT_TROOP, 4 HELP_TROOP, 5 FLEE, 6 DEFEND_PLACE, 7 IDLE,
8 TOWER_ATTACK, 9 SCOUT, 10 FOLLOW_UNIT, 11 RETREAT, 12 RTS_MOVE_TO, 13 RTS_ATTACK_UNIT, 14 APPROACH_PLACE,
15 LARGE_WEAPON_PUSH, 17 RTS_ATTACK_ITEM, 18 DEFEND_UNIT, 100+n SPECIALn. 16 has no name; 19 (0x13) is set only by the
RTS command code FUN_0044cfb0 (0x44dd6d, 0x44e097) — see §5.

### 1.3 Unit::set_task — FUN_00527fb0(Task* t) (ECX=Unit) — CONFIRMED
```c
bool Unit::set_task(Task* t) {
  if (!can_accept_task(t)) return false;              // FUN_00527f10
  switch (t->id) {                                     // news-log entry, msglog object 0xa50020
    case 2: case 13: if (valid(t->para1)) Msg_attack (0xa50020, type,tgt.type, nation,tgt.nation, ftol(tgt x,y,z)); break; // FUN_004ba1f0 -> msg 0x31
    case 4:          if (valid(t->para1)) Msg_reinforce(...same with target unit...); break;                              // FUN_004ba170 -> msg 0x30
    case 5:          Msg_flee(type, nation, ftol(own x,y,z)); break;                                                     // FUN_004ba270 -> msg 0x32
  }
  clear_task();                                        // FUN_00528300
  task = *t;  /* FUN_004e4520(t->id,t->prio,p1,p2,p3,expire) -> state=0 */  task2 = {0};
  if (t->id == 15) { this->f29d = 1; FUN_0049d810(); }
  return true;
}
bool Unit::can_accept_task(Task* t) {                  // FUN_00527f10
  if (task_locked /*+0x2e1*/) return false;
  return task.id == 0 || task.prio < t->prio || (task.expire_ms && task.expire_ms < now_ms);
}
void Unit::clear_task() {                              // FUN_00528300 (stop_curr_task)
  if (task.id == 15 && push_slot/*+0x380*/ >= 0 && valid(task.para1)) {
     Unit* w = get(task.para1); w->slot_unit[+0x37c][slot] = 0; w->slot_unit2[+0x378][slot] = 0;
     f384 = 0; f388 = 0; push_slot = -1; }
  task = {0}; task2 = {0};
}
void Unit::expire_task() { if (task.expire_ms && task.expire_ms < now_ms) task = {0}; }   // FUN_00528440 (from FUN_0049d810)
```
Equal priority does NOT pre-empt (strict `<`). Expiry is lazy: an expired task is only replaced when a new task
arrives or FUN_00528440 runs.

### 1.4 Script/engine entry points that set tasks (complete list of set_task callers) — CONFIRMED
| caller | task(prio, params) | context |
|---|---|---|
| FUN_006619d0 give_task ACTION 0x146 | (task, prio, p1, p2, p3, expire = duration?now+duration:0) | script; RETREAT(11) with p2==1 also given to every member of the unit's team |
| FUN_00661b40 give_team_task ACTION 0x147 | same | given to unit then leader's whole team |
| FUN_00558a60 .uni `CreateUnit` loader | `Task:` name → TOWER_ATTACK: (8, 1, p1=Range); other: (task, 1, p1=ftol(X+map.ox), p2=ftol(Y+map.oy), p3=Range); `none` → no task | map load |
| FUN_00467590 barracks spawn | (10 FOLLOW_UNIT, prio 5, p1=facility+0x13c follow recno) | §4 |
| FUN_0064f320 Unit::die | if retreat flag(+0x114)==1: (7 IDLE, prio cur+1) and unit survives | 1.6 |
| FUN_006b8af0 / FUN_006e7900 lose / win screen | every unit except player: clear orders, (7 IDLE, prio 0x7f, p1=1) | game over freeze |
| FUN_0044cfb0, 0044eb80, 0044f1f0, 0044f750, 007c6240 | RTS commands: 12, 19, 13, 17, 15, 10, 7 | §5 |
| FUN_00545d50 (native RETREAT routine) via give_task | | §3 |

Shipped data usage (counted over `script/scenario/*.sce`, `map/*/*.uni`): give_task names IDLE 604, DEFEND_PLACE 165,
FOLLOW_UNIT 157, LARGE_WEAPON_PUSH 134, APPROACH_PLACE 130, TOWER_ATTACK 121, ATTACK_PLACE 87, RETREAT 12,
RTS_ATTACK_ITEM 8, RTS_ATTACK_UNIT 3, SCOUT 2, SPECIAL1..5; .uni `Task:` none 5910, TOWER_ATTACK 182, idle 21;
`Range:` 500 (5933) / 2000 (180). **ATTACK_UNIT(2), RECRUIT_TROOP(3), HELP_TROOP(4), FLEE(5) are never assigned by any
native code or shipped data** — CONFIRMED by the exhaustive caller list above (FUN_004e4520 has 20 call sites; none
pushes 2..5 as a constant).

### 1.5 " troop goes to reinforce/attack/flees" — news log, vestigial AI-general feature
- Formatters FUN_004ba890 (type 0x30) " <type> troop goes to reinforce <type> troop.", FUN_004ba930 (0x31) "... goes to
  attack ...", FUN_004ba9d0 (0x32) "... troop flees" are entries of the msg-type table at 0xa018b7 (5-byte records
  {fnptr, byte flag}, index = msg type; flag=1 for 0x30..0x32). CONFIRMED (table dump).
- Records created by FUN_004ba170/1f0/270 → FUN_004b9a90(type, 0, nationA, nationB) on msglog object 0xa50020; record
  +0x18 short unit-type A, +0x1c unit-type B, then FUN_004b9c80(x,y,z) location. Log capped at 2000 entries (oldest
  dropped). CONFIRMED.
- Only producer: Unit::set_task for tasks 2/13 (attack), 4 (reinforce), 5 (flee). Since 2/4/5 are never assigned
  (1.4), only RTS_ATTACK_UNIT (13, player RTS command) ever produces a message in the shipped game. There is **no AI
  general decision logic** in the binary: the task values exist for the scripted designer (give_task) and the former
  unitAI.res scripts (`script/unit/unitAI.res` is all `exit` + commented source; ported to native, §3). HIGH CONFIDENCE.
- Reimplementation: keep the log entry generation in set_task; nothing else needed.

### 1.6 Death with retreat flag (FUN_0064f320 Unit::die, relevant part) — HIGH
```c
if (unit.recno == player_unit /*DAT_00a5b9e8*/ && unit.type != type("JOAN_OF_ARC")) FUN_00432300(0); // control back to Joan
clear_task();
if (retreat_flag /*+0x114*/ == 1) {          // scripted heroes that must not die
   f1dd = 0x7fffffff; FUN_004fe510(1); FUN_0042dd70();
   set_task({7 IDLE, prio = task.prio+1}); retreat_flag = -1; f118 = -1;
} else { ...normal death, fade actor (+0x2f5 alpha ramp -0.1f), etc. }
```
Also on death: releases large-weapon push slots (+0x1bc/+0x1be arrays, +0x1c2/+0x1c4 link), clears every pusher's task
and order queue.

### 1.7 Hold (player "stay") interaction with the AI — HIGH
`Unit_toggle_hold 0x5121c0` (§5) parks the current task in the +0x2be slot and sets own task = {id 0, prio 0, p1 = 1}.
Default-task routines (0x534090 infantry, 0x536390 general, 0x540070 archer, …) only search/engage enemies when
`get_curr_task_para(u,1) == 0` (e.g. 0x534090 decomp lines calling FUN_00652190(u,1) before FUN_0065b280/FUN_006517c0
search). Because members with task 0 read the LEADER's para1 (FUN_00652190), holding the leader makes the whole troop
hold. Toggling again restores the parked task from +0x2be.

### 1.8 Script accessors (FUNC ids 162-167, 325) — CONFIRMED
| FUNC | handler | behaviour |
|---|---|---|
| get_curr_task | 0x6520d0 | dead→0; own task if !=0; else leader's task unless leader task==12 (RTS_MOVE_TO) → 0; copies leader+0x3c9 to self |
| get_curr_task_para(u,i) | 0x652190 | i=1/2/3 → +0x2aa/+0x2ae/+0x2b2, leader's if own task 0 |
| get_curr_task_state | 0x652280 | +0x2ba (leader's if own task 0) |
| set_curr_task_state | 0x6522f0 | writes OWN +0x2ba (only if own task or leader exists) |
| stop_curr_task | 0x652370 | clear_task (own only) |
| stop_all_team_task(team) | 0x6523c0 | clear_task on every unit with +0x31==team |
| get_curr_order | 0x651260 | queue? queue->cur.action (queue+0x1d) : 0 |
Members without own task therefore *run the leader's task* through the native AI (§3) — the leader's task is the
troop's task.

## 2. Orders (actions): OrderQueue and executors
Scope 0x4baa30..0x4c2680 + helpers. Evidence = addresses (decomp + disasm checked where ECX/args matter).
0x4baa30/4baa70/4baab0/4bab00 are NOT order code (Nation-array helpers, ECX+0xc/+0x10) — ignore.
Config globals (GAMEPLAY.RES, see work/combat_re/constants.md): REACH_DT=15 @0xa3cc28, LD_FM_DT=160000 @0xa3cc74, STOP_CC=0.3 @0xa3cce4, CLB_IDLE=20 @0xa3cce8, EGATK_MP=40 @0xa3cd28, EGATK_HP=20 @0xa3cd1c.

### 1. Data layout

#### OrderRecord (0x21 bytes, packed) — CONFIRMED (setter 0x4bb950, add 0x4bac60)
| off | type | field |
|---|---|---|
| +0x00 | int | action id (0 = none) |
| +0x04 | int8 | priority (signed; script uses -3..110) |
| +0x05/+0x09/+0x0d/+0x11 | int | p1..p4 |
| +0x15 | int | from_leader flag ("use issuer's path") |
| +0x19 | int | issuer recno (leader that propagated it) |
| +0x1d | uint | expire game-time ms (= now + duration, ALWAYS set; 0x661340 `now+[ebp+0x28]`) |

#### OrderQueue (0x3e bytes, `new(0x3e)`; ctor 0x4bab50, dtor 0x4baba0) — CONFIRMED
| off | field |
|---|---|
| +0x00..+0x1c | DynArray<OrderRecord> (FUN_005843d0(0x21,10)); 1-based get FUN_00484570, insert-at FUN_005848d0(idx,rec), remove FUN_00584a10 |
| +0x1d..+0x3d | current OrderRecord: +0x1d action, +0x21 prio, +0x22 p1, +0x26 p2, +0x2a p3, +0x2e p4, +0x32 from_leader, +0x36 issuer, +0x3a expire |

Queue is kept sorted by descending prio (FIFO among equals). Executors use p-fields as scratch state (p3/p4 rewritten).

#### Unit fields used by orders
| off | type | meaning | evidence |
|---|---|---|---|
| +0x02 | short | nation id (Nation* = FUN_00509400) | 0x509400 |
| +0x04/+0x06 | short | Place id / slot index occupied | 0x4c2540 |
| +0x0a | int | current anim-record id (AnimSet lookup FUN_004168d0) | 0x5045f0 |
| +0x1a | float | heading, radians | 0x4ff850 |
| +0x1e | float | desired heading, DEGREES (setter 0x50abd0); turning done elsewhere | 0x50abd0 |
| +0x22 | int | LOD (>1 doubles reach radius) | executors |
| +0x102 | short | on-field/active flag (set 1 by recruit) | 0x4c0ae6 |
| +0x104 | int | control/sched level (5089e0 setter); >2 enables 1000-node path budget | 0x4bfbd0 |
| +0x10a | short | recruit family key (matches type+0x14d) | 0x4c0a24 |
| +0x135 / +0x145 | int | HP / MP (setters 0x4fe510 / 0x4fe5d0); max via 0x523320 / 0x523700 | 0x4bca60 |
| +0x151 | uint | last MP spend time (regen timer) | 0x4bccf4 |
| +0x1c1 | ptr | effect/skill object; FUN_00497e10(0x15,recno) = energy-cost % | 0x4bcc29 |
| +0x1e2 | int | mount (horse) recno, 0 = on foot (FUN_00430190) | |
| +0x1fd | int | busy/carry state (FUN_0042d690) — blocks DOWN/ENERGY/RUSH | |
| +0x20f | uint | last action time; can_act FUN_00500ba0 = now ≥ +0x20f + CLB_IDLE(20) AND anim ∈ {-1..6, 9..0x14, 0x2b..0x2d, 0x36, 0x37, 0x39, 0x43..0x46, 0x54..0x57, 0x59, 0x5a} (idle/move/guard/shoot-idle/horse/push); every other anim (attacks, hits, RUSH 0x53, EXTRA…) → false | 0x500ba0 |
| +0x213 | short | stance: 0 ground, 1 airborne ("STAND"/"JUMP", FUN_004157c0); setter 0x500d10 | |
| +0x215 | short | anim state (names FUN_00415820, table below) | |
| +0x227 | char[12] | combo key buffer | 0x4bbf20 |
| +0x233 | int8 | combo length | |
| +0x2a1 | OrderQueue* | | |
| +0x2d7 | short | current target unit | |
| +0x2db / +0x2dd | short | SHOOT target unit / item | 0x4bd090 |
| +0x2f1 | AnimSet* | anim library (has-anim FUN_00500d40, find FUN_00416dc0, combo-exists FUN_00416e30) | |
| +0x2f5 | Actor* | +0x4c anim start ms | |
| +0x339 | ptr | map/world; +0x5c track system | |
| +0x3c9 | int | track/route id being followed (0 = free pathing) | 0x4bfa10, 0x4bfd70 |
| +0x3d9 | vec3 | APPROACH_ENEMY cached goal point | 0x4bd420 |
| +0x3e6,+0x3ea,+0x3f2 | int | path state, cleared by Unit_clear_path 0x422f10 | |
| +0x3f6 ← +0x3fa | int | copied on every stop (path-sync stamp) | all cleanups |
| +0x412 | vec3 | formation offset (relative to Team anchor) | 0x448870 |

Team (Unit+0x33): +0x02 int8 member count (max 20 = 0x14), +0x38 vec3 formation anchor, +0x44 float formation angle (rad), +0x48 vec3 / +0x54 float snapshot of anchor/angle (written by FOLLOW_FORMATION on slot compute, 0x4be8a8).
Place (array 0xa484d0, get 0x456c70, invalid 0x456ca0): +0 short id, +0x2a vec3 centre (0x4c21c0), +0x3a AABB min / +0x46 max (contains-test FUN_0060d5f0 = AABB inclusive), +0xc2 int nslots, +0xc6+(i-1)*0xc vec3 slot pos (0x4c2470), +0x15e+i*4 float slot facing rad (0x4c24b0), +0x192+i*4 int slot occupant (assign 0x4c2540; nearest free/own slot 0x45e620).
Zone object (array 0xa5d100, get 0x43c450, invalid 0x44cf80): +0x29 vec3 centre (0x46d670), +0x39 AABB min / +0x45 max. Also carries an order template at vtbl+0x18 → rec+0x101..+0x11e (0x654d70).
Item (array 0xa460a0, get 0x425d20, invalid 0x425d50): pos 0x499930; vtbl+0x4c → state rec, +0xd1 char gate-open; +0x86 → sound emitter.
Track: FUN_0049ab40(id) → +0x46 int node count, +0x4a node array stride 0x54, node pos at +8 (vec3).

#### Anim states (+0x215) referenced (FUN_00415820 name→id)
1 IDLE 2 IDLE2 3 IDLE3 4 IDLE_BAT 5 IDLEUP 6 IDLEDN 8 JATTBI 9 TIPTOE 0xa WALK 0xb RUN 0xe MOVE_L 0xf MOVE_R 0x10 MOVE_B 0x15 JUMP_B 0x16 JUMP_R 0x17 JUMP_L 0x18 DOWN 0x1a UP 0x1c ATTACK 0x1e..0x21 ENGATK..ENGATK4 0x22 DOWNATK 0x23..0x28 ATTACKED/ATTKED2/ATTKEDR/BATTKED/LATTKED/RATTKED 0x29 DNATKED 0x2a DIE 0x2b GUARD1 0x2d GUARDR 0x30 SHTLOAD 0x32 SHTLOAD2 0x33 SHTOUT 0x34 SHTOUT2 0x36 SHTIDLE 0x38 SHTOUTH 0x39 SHTIDLEH 0x3c STANDUP 0x3e STOP 0x3f STOP_B 0x40..0x42 KNOCKDN1-3 0x43 H_IDLE 0x44 H_WALK 0x45 H_BACK 0x46 H_RUN 0x47 H_STOP_B 0x48 H_STOP 0x49 H_ATTACK 0x4a..0x4d H_*HITED 0x4e H_DROP 0x53 RUSH 0x59/0x5a C_IDLE_A/B, 0x6b+n EXTRAn.
- LOCK = {0x40,0x41,0x42,0x3c,0x29,0x4e} (knocked down / standing up / H_DROP).
- IDLE_CLASS FUN_00508ab0 = {1..6, 8, 0x43, 0x59, 0x5a}.
- BUSY_ATTACK FUN_00510f80 = {8, 0x19, 0x1c..0x22, 0x46, 0x49, 0x53}.

### 2. Lifecycle — CONFIRMED
Caller chain: `Game_process_tick 0x51e280` (30 Hz) → `Unit_process 0x52c490(phase)` (ECX=unit). Phases per unit: 0 upkeep, 1 regen/timers, **2 = order execution**, 3 = AI think (0x49d810 → dispatcher 0x5467b0, which issues orders). Phase-2 loop runs for every unit in round-robin buckets with per-bucket budgets (timer obj +0x40..+0x56 cursors, +0x58..+0x6e budgets): {LOD≤1 & +0x104≥4}, {LOD≤1 & +0x104<4}, {LOD==2}, {LOD==3}, {LOD>3} (0x51e8f9..0x51ef01), then the same 5 buckets for phase 3. Paused/scripted path (0x51e360) runs phases 0..3 back-to-back. So orders queued by AI in tick N execute in tick N+1 (or later if bucket budget exhausted).
Unit_process phase 2 (0x52d000): `if FUN_006e7b50() return; FUN_004655a0(); if (is_leader) FUN_004fa0f0(0,1); if (unit+0x2a1) OrderQueue_process(unit);`

```c
// 0x4baf80 OrderQueue::process(Unit* u)  (ECX=queue)
for (k = size; k > 0; --k) if (q[k].expire < now) remove(k);     // queued only; unsigned <
if (u->anim in LOCK) return;                                      // frozen while knocked down
if (cur.action == 0) { if (!pop_front()) return; }               // 0x4baf10: cur = q[1]; remove(1)
else if (size && q[1].prio > cur.prio && can_interrupt(u)) {     // strict >
    finish_current(u);                                            // 0x4bb660 cleanup, cur.action=0
    pop_front();
}
switch (cur.action) { 1:0x4bbf20 2:0x4bd090 3:0x4bf510 4:0x4bf240 5:0x4bf5f0 6:0x4bf780
  7: 0x4bfa10(u,(float)p1,(float)p2,(float)p3)   8: 0x4bfa10(u,p1+u.x,p2+u.y,p3+u.z)
  9:0x4bfd70 10:0x49cef0 11:0x4c25a0 12:0x4bd420(u,p1) 13:0x4bdbc0 14:0x4bdf90 15:0x4be770
  16:0x4bed40 17:0x4a71f0 18:0x4c09a0 19:0x4bc8a0 20:0x4bca60 21:0x4bcdb0 22:0x4c2680 }
```
(jump table 0x4bb2a6 verified.) An executor ends its order by `cur.action = 0` (with or without the cleanup function). There is NO generic expiry for the current order — each executor checks `cur.expire < now` itself.

```c
// 0x4bac60 OrderQueue::add(u, action, prio, p1,p2,p3,p4, expire, from_leader, issuer) -> int
rec = {action,prio,p1..p4,from_leader,issuer,expire};
if (cur.action == action) {
    if (action == 4 /*BLOCK*/) cur.expire = expire;                       // refresh only
    else if (action == 12 && (cur.p1!=p1 || cur.p2!=p2 || cur.p3!=p3)) {   // retarget
        ApproachEnemy_cleanup(u) /*0x4378a0, sets cur.action=0*/;
        cur.p1=p1; cur.p2=p2; cur.p3=p3; cur.p4=p4; cur.action=12;         // prio/expire/flags NOT updated
    }
    return 0;
}
idx = size+1;
for (k = size; k > 0; --k) {
    if (q[k].action == action) return 0;          // duplicate in queue → rejected (update branch is dead code)
    if (rec.prio > q[k].prio) idx = k;            // cmp 0x4bae70
}
insert_at(idx, rec); return idx;
```
Other queue ops: has_order(action) 0x4bb8d0 (cur or queued); remove_at(k) 0x4baee0; finish_current 0x4bb660 (switch → cleanup, then cur.action=0); can_interrupt 0x4bb3e0 (table below).
Issuers of add(): script give_order 0x661340 (+team propagation, see TroopCommand notes), scenario/trigger effect objects 0x467590/0x46a040 (vtable methods; fields +0x101 action,+0x105 prio,+0x106..+0x112 p1..p4,+0x11e duration; from_leader=issuer=0).
Order cancellation elsewhere (callers of 0x4bb660): stop_all_order 0x6618f0, win/lose 0x6b8af0/0x6e7900, hit handler 0x75d540 (on hit msg 1: if cur ∈ {12,15} and can_interrupt → finish; msg 0x10 (thrown) sets cur.action=0 without cleanup), 0x432300, 0x43c210, 0x4404c0, 0x44a690, 0x44cfb0, 0x44eb80, 0x44f1f0, 0x44f750, 0x4655a0, 0x4e8500, 0x5121c0, 0x64f320, 0x7c6240.

Script glue:
- 0x643d40 ScriptNode compile: cmd 0x143 give_order → arg2 string → FUN_004bbbc0 action id → node+0x22; cmd 0x146/0x147 give_task/give_team_task → FUN_004e4610 task id.
- 0x6612e0 ScriptNode(give_order)::poll (vtable; used by `WAIT{ ACTION("give_order",…) }`) → 0x661ca0(unit, action): unit dead/no queue → 1 (done). If game-state obj 0xa5d380+0xf ∈ {0} or ({6,7,8} and byte 0xa5d3bf==0) → cancel that order (cur: finish_current; queued: remove) and return 1. Else return !has_order(action). (Semantics of state values UNKNOWN.)
- 0x6511f0 string_to_order_para(str, i): 0x4bbac0 packs chars big-endian 4/int: p1=c0..c3, p2=c4..c7, p3=c8..c11; returns p_i (i∉1..3 → 0). Decoder 0x4bba00(i,k) = (p_i >> (4-k)*8) & 0xff.
- 0x651260 get_curr_order(recno) = queue ? cur.action : 0.

#### Interruptibility (0x4bb3e0) — CONFIRMED
| action | interruptible when |
|---|---|
| 0 | always |
| 1 COMBO | anim ∉ {ATTACK 0x1c, H_ATTACK 0x49} (0x4c0c40) |
| 2 SHOOT | anim ∉ {0x30,0x32,0x33,0x34,0x36,0x38,0x39} (0x4c0d90) |
| 3 JUMP | stance≠air, or anim time remaining==0 (0x4c0f30) |
| 4 BLOCK | anim ∉ {GUARD1, GUARDR} (0x4c0ee0) |
| 5 MOVE | anim ∉ {RUN, WALK, TIPTOE} (0x4c0f90) |
| 10 TURN | cur.expire < now (0x433a90) |
| 11 IDLE, 17 GATE, 18 RECRUIT | expire<now or anim remaining==0 (0x4c1090/4c10f0/4c1150) |
| 19 DOWN_ATTACK | anim ∉ {ATTACK, H_ATTACK} (0x4c0d40) |
| 20 ENERGY | anim ∉ ENGATK1-4 (0x4c0c90) |
| 21 RUSH | anim ≠ RUSH (0x43bbc0) |
| 6,7,8,9,12,13,14,15,16,22 | always (return 1) |

### 3. Shared executor idioms (CONFIRMED)
- `LOCKCHK`: if anim ∈ LOCK → cur.action=0 (no cleanup). Present in all except 9,17,18,22 (19/20/21 check it after other tests).
- `REACH(u)`: R = REACH_DT(15); R = 2*REACH_DT (30) if mounted or type class (type+2)==8 or LOD>1. Distance is 2-D XY squared, measured from the horse if mounted. Arrived iff d² ≤ R².
- `BUDGET`: path_to search budget param: 500; 1000 if d²≥2.25e6 (1500²) [FOLLOW_* unconditional; MOVE_TO/MOVE_ON_TRACK/APPROACH_ENEMY/ITEM additionally require u+0x104>2]. APPROACH_PLACE: d²≤250000→500, ≤2.25e6→800, ≤6.25e6→1000, ≤1.225e7→1500, else 2000.
- `path_to` = FUN_00505dd0(x,y,z,budget,bool use_leader_path,short leader) (ECX=unit; path planner, owned by PathCollide): if use_leader_path && leader alive && |u−leader|² ≤ 102400 (320²) reuse the leader's path offset by (u−leader) (returns 3 when leader has no remaining nodes); else plan own path (A* via 0x66cb40, stores goal in u+0x3cd..0x3d5, returns 1 = nodes exist, 2 = none, 0 = fail). HYPOTHESIS on exact return semantics.
- `LEADERFLAG`: after path_to: ret∉{0,3} → cur.from_leader=0 (15,16); for 7,9,12,13,14: ret==3 keeps flag only if issuer alive and (issuer has no queue or issuer.cur.action==15), else 0.
- `STOP` cleanup pattern: Unit_clear_path 0x422f10 (unless following a track with ≥2 nodes and u+0x3e6≤0); u+0x3f6=u+0x3fa; Unit_stand 0x508b90(0,1) (picks idle anim: IDLE/IDLE_BAT/C_IDLE/H_IDLE+horse etc.); cur.action=0. Mounted+moving (H_WALK/H_BACK/H_RUN) uses Rider_stop 0x4533e0 instead (STOP_CC=0.3 chance → H_STOP_B/STOP_B else H_STOP/STOP; WALK/MOVE_B → H_IDLE+IDLE).
- Movement executors only (re)plan when anim ∈ IDLE_CLASS and ∉ BUSY_ATTACK; while walking they do nothing (the path follower moves the unit). No speed constants live in the order code: speed comes from the anim chosen (WALK vs RUN) and the path follower — out of scope here.
- Facing: executors only write desired heading u+0x1e (degrees) via 0x50abd0; actual turning elsewhere.

### 4. Per-action table
| id | name | p1 | p2 | p3 | p4 | executor / cleanup | ends when | sets |
|---|---|---|---|---|---|---|---|---|
| 1 | COMBO_ATTACK | combo chars 0-3 | chars 4-7 | chars 8-11 (mounted: int hit counter) | – | 0x4bbf20 / 0x4c11f0 (stand) | combo string exhausted & anim done, expire in hit/air states, DIE/dead, no AnimSet | anim ATTACK(0x1c)+key, H_ATTACK(0x49,'A'); +0x227/+0x233 |
| 2 | SHOOT | target kind 1=unit 2=item 0=none | target id | scratch: 1=fired | – | 0x4bd090 / 0x435280 | after fire returns to idle; expire | +0x2db/+0x2dd, anim SHTLOAD(0x30) |
| 3 | JUMP | – | – | – | – | 0x4bf510 / 0x4c15e0 (anim DOWN 0x18) | expire while airborne | anim UP(0x1a) |
| 4 | BLOCK | attacker recno (0 any) | – | – | – | 0x4bf240 / 0x4c15a0 | mounted; attacker dead; expired and attacker no longer threatening | anim GUARD1(0x2b) |
| 5 | MOVE | gait 0/1 walk, 2 run | – | – | – | 0x4bf5f0 / 0x45c430 | expire (checked while gait anim playing) | stance=0, anim WALK/RUN |
| 6 | SHIFT | 1 MOVE_L 2 MOVE_R 3 MOVE_B 4 JUMP_B | – | – | – | 0x4bf780 / 0x43fbe0 | step anim finished or expire | stance=0, anim |
| 7 | MOVE_TO_TARGET | x | y | z (int world) | track id (→u+0x3c9) | 0x4bfa10 / 0x4c1670 | REACH; expire | path |
| 8 | MOVE_TO_RELATIVE_TARGET | dx | dy | dz | track id | same as 7, goal = offset + CURRENT pos each tick | expire (or |d|≤R) | path |
| 9 | MOVE_ON_TRACK | track id | 0 → first node, 1 → last node (scratch) | ping-pong flag | – | 0x4bfd70 / 0x4c17a0 | track invalid/<2 nodes, expire | u+0x3c9=track |
| 10 | TURN | heading deg | – | – | – | 0x49cef0 / 0x4c1860 (stand) | |Δ|≤1° (round) ; class-8 w/o crew | u+0x1e |
| 11 | IDLE | – | – | – | – | 0x4c25a0 / none | expire while idle anim running | idle anim |
| 12 | APPROACH_ENEMY | target recno | stand-off min dist | random band width | scratch: goal cached | 0x4bd420(u,p1) / 0x4378a0 | target dead; expire; d(u,target) in (p2, p2+p3]; at goal | u+0x3d9 goal |
| 13 | APPROACH_ITEM | item id | radius | random extra | – | 0x4bdbc0 / 0x4c1390 | within p2+rand(p3); item gone; expire | path |
| 14 | APPROACH_PLACE | place id | zone id (0xa5d100, overrides place) | – | – | 0x4bdf90 / 0x43da10 | inside zone AABB / reached slot / place gone / expire | slot claim u+4/u+6, u+0x1e |
| 15 | FOLLOW_FORMATION | scratch slot x | scratch slot y | scratch: 1=slot cached | – | 0x4be770 / 0x4c1480 | REACH (face team angle); expire; own target closer than slot | Team+0x48..0x54 snapshot |
| 16 | FOLLOW_UNIT | unit recno | angle offset deg (rel. to unit heading) | distance | – | 0x4bed40 / 0x4c14e0 | REACH (face followed heading); within p3 of followed unit; own target closer; unit dead; expire | u+0x1e |
| 17 | GATE_OPEN | gate item id | desired state (0 closed, 1 open) | – | – | 0x4a71f0 / none (0x42d410) | immediately | world event 0x20 |
| 18 | RECRUIT_TROOP | – | – | – | – | 0x4c09a0 / none (0x425c50) | immediately | spawns units |
| 19 | DOWN_ATTACK | – | – | – | – | 0x4bc8a0 / 0x4c1230 | anim==DOWNATK (next tick), expire, can't act | anim DOWNATK(0x22) |
| 20 | ENERGY_ATTACK | – | – | – | – | 0x4bca60 / 0x4c1270 | anim ∈ ENGATK (next tick); expire&interruptible | anim ENGATK(0x1e), HP/MP cost |
| 21 | RUSH_ATTACK | – | – | – | – | 0x4bcdb0 / 0x434210 | next tick after RUSH starts (can_act false in RUSH); ENGATK/LOCK; expire & anim≠RUSH; MP < cost | anim RUSH(0x53), MP cost |
| 22 | SPECIAL_MOVEMENT | EXTRA index n (anim 0x6b+n) | item id (sound source) | repeat count | – | 0x4c2680 / 0x4c18f0 | count exhausted & anim done; expire | anim EXTRAn |

Typical native parameters (decoded pushes at all 0x661340 call sites; prio, team_flag, p1..p4, duration ms):
- 12: prio 1, p2=p3 ∈ {60/60, 70/70, 100/100, 100/50}, team 0/1, 30000.  13: prio 1, (100,100)/(100,50)/(800,400), 30000.
- 15: prio 2 or −2, team 1, p=0, 10000.  16: prio −1/0/5, p3=200, 5000/20000.  14: prio −3/0/99, 30000.
- 6: prio 1, p1 1/2/3, 1000–1500.  4: prio 1/2/4/5, p1 attacker, 1000–2000.  2: prio 2/3, p1=2 (item) or var, 1500/5000.
- 7: prio −1/0/10/99/110, 5000–30000.  8: prio −1, 2000/5000.  10: prio 3/90, p1 325 or var, 2500/5000.
- 19/20/21: prio 2, 1000–5000.  22: (20,p1=1,p3=1,2000), (7,2,…,1,10000), (70,11/13,…,1,40000).
- Script examples: `give_order(u,"SHOOT",2,0,0,0,0,0,1500)` (fire in current facing), `give_order(u,"APPROACH_PLACE",1,1,place,0,0,0,3000)` inside WAIT{}.

### 5. Executors (pseudo-C)

#### 1 COMBO_ATTACK 0x4bbf20 — HIGH
```c
if (anim in LOCK || anim in {DOWNATK,ENGATK1..4}) { cur.action=0; return; }
key(i) = para_char(i/4+1, i%4+1);        // i-th char of packed combo string
if (!mounted) {
  if (anim == ATTACK) {
    for (i=0; i<combo_len && combo[i]==key(i); ++i);
    if (i==combo_len && key(i)) {                  // performed prefix matches, more keys pending
      if (!u->actor) { set_anim(ATTACK, key(i), 1); return; }
      if (!u->animset) { reset_combo; stand; cur.action=0; return; }
      rec = AnimSet::get(u->anim_id); el = now - actor->anim_start;
      if (anim_remaining(u)==0) { reset_combo; stand; done; return; }
      if (el < rec->chain_min_ms /*rec+0x28*/) return;   // chain window opens
      tmp = combo + key(i); if (!AnimSet::has_combo(ATTACK,tmp)) return;
      set_anim(ATTACK, key(i), 1); return;
    }
    if (anim_remaining(u)==0) { reset_combo; stand; done; }   // string finished
  } else if (anim in ATTACKED..RATTKED) { if (expired) done; }
  else if (anim==DIE || dead) done;
  else if (stance==air) { if (expired) done; }
  else if (expired) done;
  else if (anim in IDLE_CLASS) {                    // start combo
    reset_combo; first = key(0);
    if (u->animset && AnimSet::has_combo(ATTACK,{first})) { set_anim(ATTACK, first, 1); clear_path; u+0x3f6=u+0x3fa; }
  }
} else if (anim == H_ATTACK) {
  if (!u->animset || anim_remaining==0) { reset_combo; stand; done; }
  else if (elapsed >= rec->chain_min_ms) {
    if (cur.p3 < 1) { reset_combo; stand; done; }
    else { cur.p3--; set_anim(H_ATTACK,'A',1); if (!strcmp(combo,"AAA")) { combo_len=1; combo[1]=combo[2]=0; } }
  }
} else if (anim in H_*HITED) { if (expired) done; }
else if (anim==DIE || dead) done;
else { cur.p3--; set_anim(H_ATTACK,'A',1); }
```
reset_combo = `u+0x233=0; u+0x227=0`. Combo keys are letters ('A'…); new attack anim variant = key char (FUN_00501190(anim, variant, 1)).

#### 2 SHOOT 0x4bd090 — CONFIRMED
```c
LOCKCHK;
if (cur.p3 == 1) { if (anim in IDLE_CLASS) cur.action=0; return; }    // fired; wait to finish
if (!(anim in IDLE_CLASS)) {
  if (anim==DIE || dead) { done; return; }
  if (anim in {SHTLOAD,SHTLOAD2,SHTOUT,SHTIDLE,SHTOUTH,SHTIDLEH}) return;
  if (expired) done; return;
}
if (expired) { done; return; }
if (cur.p1 == 1) { u+0x2dd=0; if (unit p2 dead) { u+0x2db=0; goto fire; } u+0x2db=p2; a = rel_angle_to_unit(p2) /*0x505960, deg, (-180,180]*/; }
else if (cur.p1 == 2) { u+0x2db=0; if (item p2 invalid) { u+0x2dd=0; goto fire; } u+0x2dd=p2; a = rel_angle_to_item(p2) /*0x5059b0*/; }
else goto fire;
if (a < -10 || a > 10) return;          // wait until facing within ±10° (turning done by others)
fire: reset_combo; set_anim(SHTLOAD,0,1); clear_path; u+0x3f6=u+0x3fa; cur.p3 = 1;
```

#### 3 JUMP 0x4bf510 — CONFIRMED
`LOCKCHK; if (stance==1) { if (expired) done; } else set_anim(UP 0x1a,0,1);` Cleanup sets anim DOWN(0x18). (Landing logic is in anim/physics code.)

#### 4 BLOCK 0x4bf240 — CONFIRMED
```c
if (anim in LOCK || mounted) { done; return; }
if (p1 == 0 || unit_valid(p1)) {
  if (has_path) { clear_path; u+0x3f6=u+0x3fa; }
  if (anim in {GUARD1,GUARDR}) {
    if (expired) {
      if (!unit_valid(p1)) { stand; done; }
      else if (!will_be_attacked(u, p1) /*0x651430: p1 anim ∈ {ATTACK,ENGATK1-4,RUSH} && |rel_angle(p1→u)|<90°*/) {
        stand; done; if (type->ai_script /*type+0x125*/) AI_after_block(type ai obj, u) /*0x546970*/;
      }                                   // still threatened → keep guarding past expire
    }
  } else set_anim(GUARD1,0,1);
} else { stand; done; if (type+0x125) AI_after_block(...); }   // attacker died
```
add() with BLOCK while BLOCK current only refreshes expire. AI_after_block 0x546970 dispatches on AI script id (obj+0x26) to per-class counter routines (0x535c20, 0x538340, 0x53abd0, 0x53cb10, 0x53f670, 0x541270(u,1,0), 0x5432a0, 0x544c40, 0x544e00, 0x545380, 0x545d00 for id 14).

#### 5 MOVE 0x4bf5f0 — CONFIRMED
```c
LOCKCHK;
if (anim in {RUN,WALK,TIPTOE}) { if (anim_remaining==0) set_anim(anim,0,1) /*loop*/; else if (expired) done; }
else { stance=0; if (p1==0||p1==1) set_anim(WALK,0,1); else if (p1==2) set_anim(RUN,0,1); }
```
Pure "walk forward in current heading" for duration. Cleanup: stand.

#### 6 SHIFT 0x4bf780 — CONFIRMED
`if anim in LOCK∪{DOWNATK,ENGATK1-4} → done. if anim ∈ {MOVE_L,MOVE_R,MOVE_B,JUMP_B,JUMP_R,JUMP_L}: if (remaining==0 || expired) { stand; done; } else { stance=0; anim = p1: 1 MOVE_L, 2 MOVE_R, 3 MOVE_B, 4 JUMP_B }` (single side-step/back-step/back-jump).

#### 7/8 MOVE_TO_TARGET / MOVE_TO_RELATIVE_TARGET 0x4bfa10(u, gx, gy, gz) — CONFIRMED
```c
if (anim in LOCK) { done; u+0x3c9 = 0; return; }
if (expired) { cleanup_7(u); return; }
if (!(anim in IDLE_CLASS)) return;
d2 = XY dist²(u or horse, g); if (d2 <= REACH²) { cleanup_7(u); return; }
if (p4) u+0x3c9 = p4;                       // follow track/route p4
budget = (u+0x104 > 2 && d2 >= 1500²) ? 1000 : 500;
r = path_to(gx,gy,gz,budget,from_leader,issuer); LEADERFLAG(r);
// cleanup_7 0x4c1670: if (p4) u+0x3c9=0; mounted&&moving ? Rider_stop : stand; STOP path rule; done
```
Action 8 recomputes goal = p + CURRENT position every tick ⇒ never arrives unless |p|≤R: it is a "keep moving toward offset" order bounded by its duration (scripts use 2000–5000 ms).

#### 9 MOVE_ON_TRACK 0x4bfd70 — HIGH
```c
if (!p1 || !u->map || !u->map->tracks || expired) { cleanup_9; return; }   // 0x4c17a0: u+0x3c9=0, stop, clear path
t = track(p1) /*0x49ab40*/; if (t->n < 2) { cleanup_9; return; }
goal = (p2==0) ? t->node[0].pos : (p2==1 ? t->node[n-1].pos : <unset>);
d2 = dist²(u or horse, goal);
if (p3 && d2 <= REACH²) p2 = !p2;           // patrol ping-pong
u+0x3c9 = p1; budget=(u+0x104>2 && d2>=1500²)?1000:500;
r = path_to(goal, budget, from_leader, issuer); LEADERFLAG(r);
```
No LOCK check, no idle check; with p3==0 it keeps targeting the end node until expire.

#### 10 TURN 0x49cef0 — CONFIRMED
```c
if (anim in LOCK) { done; return; }
if (anim in BUSY_ATTACK) { done; return; }
t = type(u); if (t->class==8 && t->subclass∉{6,10} && !has_crew(u,1) /*0x50d550*/) { done; return; }
if (anim_remaining==0) stand;
diff = round(((p1 - heading_deg) mod 360) in [0,360));   // 0x590a20 round-half-away
if (diff <= 1 || diff >= 359) done; else u+0x1e = (float)p1;
```

#### 11 IDLE 0x4c25a0 — CONFIRMED
`LOCKCHK; if (!(anim in IDLE_CLASS)) stand; else if (anim_remaining==0) stand /*re-roll idle variant*/; else if (expired) done;` No cleanup.

#### 12 APPROACH_ENEMY 0x4bd420(u, target=p1) — CONFIRMED
```c
if (anim in LOCK) { done; return; }
if (!unit_valid(target) || dead(target) || expired) { cleanup_12(u); return; }   // 0x4378a0 = STOP
if (anim in BUSY_ATTACK) return;
U = u.pos; T = target.pos; extra = rand(p3);                 // Misc_random 0x590540
d2 = |U-T|²(3-D, length_sq);
if (p2² < d2 && d2 <= (p2+p3)²) { cleanup_12(u); return; }   // already inside the band
ang = dir_deg(T→U) + rand(270) - 135;                        // = heading - rel_angle(target→u) + U[-135,134]
r = p2 + extra; G = T + r*(cos ang, sin ang), G.z = T.z;     // byte-angle sin table (256/turn, 0x576ac0 cos /0x5768d0 sin)
if (p4 == 0) { u+0x3d9 = G; p4 = 1; } else G = u+0x3d9;       // goal chosen once
if (XY dist²(U,G) <= REACH²) { cleanup_12(u); return; }
budget = (u+0x104 > 2 && |U-G|² >= 1500²) ? 1000 : 500;
if (|T-G|² > (p2+p3)² || |T-G|² <= p2²) {                   // target moved: goal left the band
    clear_path; u+0x3f6=u+0x3fa; p4 = 0; }                   // re-pick next tick
r = path_to(G, budget, from_leader, issuer); LEADERFLAG_approach(r);
if (path nodes && XY dist²(last node, U) <= REACH²) cleanup_12(u);
```
Runs every tick (no IDLE_CLASS gate). Typical p2=p3=60 → stand-off ring 60..120 units.

#### 13 APPROACH_ITEM 0x4bdbc0 — CONFIRMED
```c
LOCKCHK;
if (item invalid || expired) { cleanup_13; return; }          // 0x4c1390 STOP
if (anim in BUSY_ATTACK || !(anim in IDLE_CLASS)) return;
I = item.pos; extra = rand(p3); R = p2 + extra;
if (|U-I|² <= R²) { cleanup_13; return; }
G = I + R*(cos a, sin a), a = rand(360) deg (byte angle, fresh each replan), G.z = I.z;
budget = (u+0x104>2 && |U-G|²>=1500²)?1000:500;  r = path_to(G,…); LEADERFLAG_approach(r);
```

#### 14 APPROACH_PLACE 0x4bdf90 — HIGH
```c
LOCKCHK;
if (place invalid(p1) || expired) { cleanup_14; return; }      // 0x43da10 STOP
if (!((u+0x3c9==0 || no path nodes) && !(anim in BUSY_ATTACK) && anim in IDLE_CLASS)) return;
P = place(p1); G = P.centre;
if (zone_valid(p2)) {                                            // zone overrides place
  Z = zone(p2); if (AABB(Z).contains(U)) { cleanup_14; return; }
  G = Z.centre; G.x = Z.min.x + rand(Z.max.x-Z.min.x); G.y = Z.min.y + rand(Z.max.y-Z.min.y);
} else {
  slot = 0;
  if (AABB(P).contains(U)) {
    if (is_leader(u)) {
      slot = Place_find_slot(P,u) /*0x45e620: own slot, else nearest free by 3-D dist; 0 if none*/;
      if (slot) { if (P.occ[slot] != u) { Place_assign(P,slot,u); Team_reform(u->team,1,1) /*0x4fa0f0*/; }
                  else Place_assign(P,slot,u);  G.xy = P.slot_pos[slot].xy; }
    } else if ((L = leader(u)) && L+6 /*slot*/ && L+4 == P.id) { slot = L+6; G = formation_pos(u) /*0x448870*/; }
  }
  if (!slot) {
    if (AABB(P).contains(U) && is_leader(u)) cleanup_14(u);      // NB: no return — still issues path below
    G.x = P.min.x + rand(P.max.x-P.min.x); G.y = P.min.y + rand(P.max.y-P.min.y);   // z = centre z
  } else if (XY dist²(U or horse, G) <= REACH²) {
    cleanup_14(u); u+0x1e = P.slot_facing[slot] * 180/π; return;
  }
}
budget = by |U-G|²: ≤500²:500, ≤1500²:800, ≤2500²:1000, ≤3500²:1500, else 2000;
r = path_to(G, budget, from_leader, issuer); LEADERFLAG_approach(r);
```
Followers inside the place take the formation position relative to the (slotted) leader; leaders claim the nearest free place slot and face its stored facing on arrival.

#### 15 FOLLOW_FORMATION 0x4be770 — CONFIRMED
```c
if (anim in LOCK) { done; return; }
if (expired) { cleanup_15(u); return; }                      // 0x4c1480 = STOP
if (anim in BUSY_ATTACK || !(anim in IDLE_CLASS)) return;
if (p3 == 1) G = {(float)p1, (float)p2, u.z};
else {
  G = formation_pos(u) /*0x448870: Team(leader).anchor(+0x38) + u+0x412; own pos if no formation (0x4b62b0==0), if u is leader, or leaderless*/;
  p1 = ftol(G.x); p2 = ftol(G.y); p3 = 1;
  if ((L = leader(u)) && L->team) { T=L->team; T+0x54 = T+0x44; T+0x48..0x50 = T+0x38..0x40; }  // snapshot
}
d2 = XY dist²(u or horse, G);
if (d2 <= REACH²) {
  cleanup_15(u);
  if (leader(u) && u+0x102) u+0x1e = leader(u)->team->angle(+0x44) * 180/π;    // face formation direction
  return;
}
if (issuer valid) {                                          // leader-path piggyback allowed only if:
  if (dead(issuer) || !issuer->queue || !has_order(issuer,15) || |u-issuer|²(XY) >= LD_FM_DT(160000 = 400²))
      from_leader = 0;
} else from_leader = 0;
if (target(u+0x2d7) alive && |u-target|² < d2) { cleanup_15(u); return; }   // enemy nearer than slot → break formation
budget = d2 >= 1500² ? 1000 : 500;
r = path_to(G, budget, from_leader, issuer); if (r!=3 && r!=0) from_leader = 0;
```
Slot is fixed once per order (p1/p2 cached), so formation changes need a fresh order. Issued by AI with team_flag=1, prio ±2, 10 s.

#### 16 FOLLOW_UNIT 0x4bed40 — CONFIRMED
```c
LOCKCHK; if (expired) { cleanup_16; return; }                 // 0x4c14e0 = STOP
F = unit(p1); if (!valid) { cleanup_16; return; }
if (anim in BUSY_ATTACK || !(anim in IDLE_CLASS)) return;
a = F.heading(rad) + p2 * 0.017453292;                        // angle stored as turn fraction (0x577340), cos/sin via fcos/fsin
G = {F.x + p3*cos a, F.y + p3*sin a, F.z};
d2 = XY dist²(u or horse, G);
if (d2 <= REACH²) { cleanup_16; if (leader(u)) u+0x1e = F.heading*180/π; return; }
dF = XY dist²(u, F);
if (dF < p3²) { cleanup_16; return; }                          // already within follow distance of F
if (target alive && |u-target|² < d2) { cleanup_16; return; }
budget = dF >= 1500² ? 1000 : 500;                             // NB: uses distance to F, not to G
r = path_to(G, budget, from_leader, issuer); if (r!=3 && r!=0) from_leader = 0;
```
Native use: p3=200 typical. (+0x1a heading radians; 0x9cfbf8=π, 0x9cfbe8=180.)

#### 17 GATE_OPEN 0x4a71f0 — HIGH
`if item p1 valid: s = item->vtbl[0x4c]()->open(+0xd1); if ((p2==0 && s==1) || (p2!=0 && s==0)) post_event(0x20 /*use gate*/, …, unit recno, pos=u.pos, 1.0f, now, now, 1) (FUN_004eef00); cur.action=0;` (one-shot toggle request).

#### 18 RECRUIT_TROOP 0x4c09a0 — CONFIRMED
```c
N = Nation(u+2) /*0x509400*/; if (!N || !is_leader(u)) { done; return; }
for (t = unit_type_count /*DAT_00a48d8c*/; t > 0; --t)
    if (type(t)+0x14d == u+0x10a && type(t)+0x149 == 0) break;  // highest matching rank-and-file type
if (t < 1) { done; return; }
n = 0;
while (team(u)->count < 20 && Nation_spend_recruit(N) /*0x4b5bf0(0,0,0)*/) {
    r = UnitArray_create(t) /*0x51dfc0*/; v = unit(r);
    v.set_ctrl_level(0) /*0x5089e0*/; v.set_nation(u+2) /*0x509250*/; v.set_world(u+0x335) /*0x4fed00*/;
    v.join_team(u.recno) /*0x438f80*/; v+0x102 = 1; new[n++] = r;
}
for each new v: v.set_pos(u.pos + v+0x412) /*0x4fedf0*/; v.set_heading(u.heading) /*0x4ff870*/;
done;
```
Nation_spend_recruit 0x4b5bf0: requires N+0x1371 ≥ 2.0f, N+0x14a5 ≥ 1.0f, N+0x1385 ≥ 1; spends resource 9 by 2.0 (0x4b5010(9,2.0f,0)), N+0x14a5 −= 1.0, N+0x1385 −= 1. Instant spawn at formation offsets, no walking.

#### 19 DOWN_ATTACK 0x4bc8a0 — CONFIRMED
`if (anim==DOWNATK || expired || anim in LOCK) { done; return; } if (!mounted && !(u+0x1fd) && can_act(u) && has_anim(DOWNATK)) { if (anim==ATTACK && combo_len && remaining==0) reset_combo; set_anim(DOWNATK,0,1); } else done;`

#### 20 ENERGY_ATTACK 0x4bca60 — CONFIRMED
```c
if (expired && can_interrupt_20(u)) { cleanup; return; }
if (anim in ENGATK1-4 || anim in LOCK) { done; return; }
if (mounted || u+0x1fd || !has_anim(ENGATK)) { done; return; }
if (anim==ATTACK && combo_len && remaining==0) reset_combo;
if (!can_energy_attack(u) /*0x652c10*/) { done; return; }
mp = ftol( maxMP*EGATK_MP/100 * effect_pct(0x15)/100 );  hp = ftol( maxHP*EGATK_HP/100 );
set_anim(ENGATK,0,1);
if (!(player valid && player.is_hostile_to_player(u) /*0x5094d0*/)) { set_mp(MP-mp); set_hp(HP-hp); }   // the player's enemies pay nothing
u+0x151 = now;
```
can_energy_attack 0x652c10: hostile to the player → 1 (no price check); else MP ≥ mp && HP > 3*hp && has ENGATK anim && (anim usable 0x416ed0 || crew/weapon 0x50d3d0(0xd,0)>0). 0x5094d0 is Unit_isHostileToPlayer: unit nation 0 → 0, else FUN_004b6860(player nation, unit nation) = the nation-relation hostility byte (same nation 0, either nation 0 → 1). The port used to charge the hostile side: a retreat-flagged boss (mission 2's William / John) at ≤ 20 % HP killed itself with its energy attack (HP 0 without Unit_die, deleted next tick), so its "defeat" objective never finished (`game.ai_energy_cost`).

#### 21 RUSH_ATTACK 0x4bcdb0 — CONFIRMED
```c
if (expired && anim != RUSH) { cleanup; return; }
if (anim in ENGATK1-4 || anim in LOCK) { done; return; }
if (mounted || u+0x1fd || !can_act || !has_anim(RUSH)) { done; return; }
if (anim==ATTACK && combo_len && remaining==0) reset_combo;
if (!u->animset) { done; return; }
cost = AnimSet::get(AnimSet::find(RUSH))->mp_cost(+0x18); if (u+0x1c1) cost = effect_pct(0x15)*cost/100;
if (cost <= MP) { set_mp(MP-cost); set_anim(RUSH,0,1); } else done;
```
Order lifetime ≈ 1 tick after RUSH starts: next tick can_act (0x500ba0) is false for anim 0x53 → `done` (no cleanup, anim keeps playing). Same pattern ends DOWN_ATTACK (anim==DOWNATK check) and ENERGY_ATTACK (anim∈ENGATK). CONFIRMED (0x500ba0 switch).

#### 22 SPECIAL_MOVEMENT 0x4c2680 — CONFIRMED
```c
if (expired) { reset_combo; stand; done; return; }
A = 0x6b + p1;   // EXTRA<p1>
if (anim == A) {
  if (remaining == 0) {
    if (p3 < 1) { reset_combo; stand; done; }
    else { set_anim(A,0,1); p3--; play_item_sound(p2); }
  }
} else if (anim in IDLE_CLASS && p3 > 0) { set_anim(A,0,1); cur.action = 22; p3--; play_item_sound(p2); }
// play_item_sound: if item p2 valid && (item+0x86 != 0 || *(item+0x86)+4 != 0 /*BUG: should be &&; null deref if +0x86==0*/)
//   0x5abe30(*(item+0x86)+4, AnimRec(u)->sound(+0x4d), now, 0,0,0)
```

### 6. Open points
- path_to (0x505dd0) return codes — HYPOTHESIS; full planner belongs to PathCollide. The budget (arg 4, `[ebp+0x14]`) is pushed as
  PathSpace_find_path's arg 10 at every call site (e.g. 0x5066a3) and forwarded to PathSpace_astar's 12th arg, its pop counter
  (`dec [ebp+0x34]` / `jle` at 0x66cfa5..0x66cfbc): the A* expansion cap. CONFIRMED (disasm).
- Game-state 0xa5d380+0xf values {0,6,7,8} semantics in 0x661ca0 — UNKNOWN.
- Who turns the unit toward +0x1e / SHOOT target (±10° gate) — outside order code.
- Order ids 1 COMBO_ATTACK, 3 JUMP, 5 MOVE never issued by native give_order call sites (script/player only).

## 3A. Native unit AI — classes 1–3 (light_inf, light_gen, special_gen)
Scope note: the parent (AITroopRE.TroopCommand) already covered the class mapping, 534090, 535c20, 536390, 534880, 545d50/545ea0/546050, search_for_unit, get_unit_range, aggression 511f80/511e70 and set_task. Those are only cited here.
Notation: `ORDER(act,prio,team,p1,p2,p3,p4,ms)` = give_order FUN_00661340(unit,act,prio,team_flag,p1..p4,duration_ms). All args were decoded from disasm pushes. `u` = self recno, `U` = self Unit*.
Every routine is `__thiscall(UnitScriptRec* this, Unit* U)` with `ret 4`. It returns 1 when handled; 0 makes 0x49d810 fall back to the unit's script object (U+0x299)->vtbl[3]. CONFIRMED (0x49d8d2..0x49d90d).

### Dispatcher context (new facts only)
- `this` in FUN_005467b0 is the **UnitScriptRes record**, not the unit. Global UnitScriptRes is at 0xa48cf8 = {int count; Rec* recs; char loaded}. Rec is 0x28 bytes: +0 short idx, +2 char name[32] (section name from SCRIPT\UNIT\unitAI.res), +0x26 short native_id = FUN_00546520(name). FUN_00484540(i) = &recs[i-1]. Loader is FUN_005462b0 / FUN_005461d0, free is 0x546140/0x546200. CONFIRMED.
- Caller FUN_0049d810 (Unit::think_ai, ECX=Unit). Per unit type: type+0x125 short = unitAI rec index, type+0x121 int = think interval ms. U+0x29d = last think time; its first value is now+rand(500). Each think calls FUN_00528440 (expire task) first. The player unit (recno==DAT_00a5b9e8 && U+0x102==0) and U+0x1e6!=0 skip the AI and call FUN_004fd630. A non-empty order queue (U+0x2a1) skips thinking. CONFIRMED.
- All unitAI.res sections begin with `exit`. The scripts are dead and only the section names matter.
- FUN_0042d0e0 (ECX=Unit) is_dead = `hp(+0x135) < 1 && U+0x120 == 0`. U+0x120 is an int that keeps the unit "alive" at hp≤0; it is written in ~20 places, likely a dying/revive grace counter (HYPOTHESIS). Script `is_died` 0x653c60 is different: anim(+0x215)==0x2a.
- FUN_006512d0 is_attacked(recno) = unit valid && anim +0x215 ∈ {0x23–0x28, 0x29, 0x2a, 0x40, 0x41, 0x42}, i.e. the unit is in a hit/knockdown/die reaction anim. It is not "was damaged recently". CONFIRMED.
- Main routines 1–3, attacked branch: `p1=get_curr_task_para(u,1); if task==7 && p1==0 {stop_curr_task(u); L=get_leader_recno(u); if L && get_curr_task(L)==7 stop_curr_task(L);} return 1`. The `p1==0` guard is native-only and absent in the script. CONFIRMED.
- FUN_00510f80 (ECX=Unit) is_busy_attacking = anim ∈ {8, 0x19, 0x1c–0x22, 0x46, 0x49, 0x53}. CONFIRMED.
- FUN_00512070 (ECX=Unit) block_chance = DAT_00a3cc90 × diffmul[DAT_00a57fc2 0..3] (floats at 0xa391a4/0xa39188/0xa391a0/0xa39198, all runtime BSS; default = raw a3cc90). Same pattern as aggression 511f80. CONFIRMED structure, values runtime.
- FUN_00467c30(ECX=Unit, char* side, int radius) nearest_incoming_missile. It scans DynArray 0xa48500; elements have +8 = item recno into ItemArray 0xa460a0 and +0x2a = shooter unit recno. Filters: shooter valid; self target (+0x2d7)==0 or ==shooter; side "E" requires shooter hostile (FUN_005094d0); |self.pos(+0xe) − item.pos(+0x56)|² ≤ radius². Returns item short +4 (recno) or 0. HIGH CONFIDENCE (array identity = projectiles is HYPOTHESIS).
- FUN_00523700 = max energy (U+0x149 + bonus if U+0x1c1). FUN_004fe5d0(v) sets energy U+0x145 = clamp(v,0,max). FUN_0065c8d0 is_unit_dead(recno). FUN_0065ddb0(recno) = float distance unit→its attach point on the weapon (−1 if none). FUN_0065dfa0(recno) = U+0x388 (attach slot).

### Class × task routing (classes 1–3)
| task | 1 human_light_infantry 0x531f90 | 2 human_light_general 0x5322c0 | 3 special_general 0x532520 |
|---|---|---|---|
| 0 none | 534090 inf_default | 536390 gen_default | 538c90 |
| 1 ATTACK_PLACE | 534090 | **536a40** gen_attack_place | **539a40** |
| 3/4 RECRUIT/HELP | ret 1 (no-op) | 536390 | 538c90 |
| 5 FLEE | ret 1 | ret 1 | ret 1 |
| 6 DEFEND_PLACE | 534880 | **5370d0** | **53a060** |
| 7 IDLE | ret 1 | ret 1 | ret 1 |
| 9 SCOUT | 534090 | ret 1 (default) | ret 1 |
| 10 FOLLOW_UNIT | leader? 534090 : **5377b0** | **5377b0** | **5392d0** |
| 11 RETREAT / 12 RTS_MOVE_TO / 14 APPROACH_PLACE | 545d50 / 545ea0 / 546050 | same | same |
| 13 RTS_ATTACK_UNIT | **535870** | **537f90** | **53a6d0** |
| 15 LARGE_WEAPON_PUSH | **535020** | ret 1 | ret 1 |
| 17 RTS_ATTACK_ITEM | **535a20** | **538140** | **53a9d0** |
| 18 DEFEND_UNIT | 534090 | 5377b0 | 5392d0 |
| 19 RTS_ATTACK_MOVE | **535e30** | **538730** | **53b1b0** |
| 101–105 SPECIAL | **ret 0 → script fallback** | ret 1 | ret 1 |
| other | 534090 | ret 1 | ret 1 |
Melee "attack roll" helper by class: 1 → 535c20, 2 → **538340**, 3 → **53abd0**.

### Task ids 16 / 19
- **19 = RTS attack-move**, HIGH CONFIDENCE. Its only setter is FUN_0044cfb0, the player formation-move command (called from 0x440b30 ×3). Arg param_4 = `DAT_00a51800 & 0xc` (modifier-key bits). For each troop member (not dead, task≠12, matching team), it computes a formation slot (spacing 200 if type class==8 else 80) and calls `task.set(param_4 ? 19 : 12, U.cur_prio(+0x2a9)+1, x, y, z, now+180000)`, then stop_task + set_task (0x44dd65/0x44e08f). Battering ram AI 0x533440 treats 19 == 12 (decomp line ~430754). It has no name in FUN_004e4610.
- **16**: UNKNOWN. No setter (no `push 0x10`/mov into a task struct before 4e4520/527fb0/6619d0/661b40) and no `case 0x10` in any class main routine. It is unused natively; class 1 would route it to the default 534090.

### 5377b0 FOLLOW_UNIT / DEFEND_UNIT (class 2; class 1 when no leader)
```
f=para1 (unit to follow); keep=para2 ?: 180
d = f valid ? (int)sqrt(dx²+dy²) using +0xe,+0x12 : 0
fd = get_follow_unit_distance(f)               // FUNC 0x6592c0
if d > fd+200: ORDER(16 FOLLOW_UNIT,0,team=1,f,keep,fd,0,5000); set_unit_target_unit_recno(u,0); return 1
tgt = search_for_unit(u,"T",0,0,0,0,0)         // own current target
e   = search_for_unit(u,"E",150,0,0,0,0)
if e: set_unit_mode(u,"ATTACK")
else { set_unit_mode(u,"ACTIVE"); e=search_for_unit(u,"E",200,..); if e && rand(50)==0: set_unit_mode(u,"ATTACK") }
if is_attack_mode(u):
   if e: ORDER(10 TURN,3,0,get_rotation(u,e),0,0,0,2500); return 538340(U)
   return 1                                    // item path dead (local never set)
if is_idle_mode(u):
   ftgt = f ? search_for_unit(f,"T",0,..) : 0  // followed unit's target
   set_unit_attack_behaviour(u,"NEAREST_HIGH_PRIORITY")
   e  = search_for_unit(u,"E",get_unit_range(u,1),1,0,0,0)
   en = search_for_unit(u,"E",get_unit_range(u,1)/2,0,0,0,0)
   if en>0 && type(en).class(+2) ∉ {5,6}:
       r=rand(10): r<1 → ORDER(12 APPROACH_ENEMY,1,0,e,60,60,0,30000)
                   r<5 → ORDER(10,1,0,rot(e),0,0,0,2500); ORDER(6 SHIFT,1,0,1,0,0,0,1500)
                   r<9 → same TURN; SHIFT p1=2
                   r==9→ same TURN; SHIFT p1=3
       return 1
   if e>0: ORDER(12,1,0,e,60,60,0,30000); return 1
   if ftgt:
       set_unit_attack_behaviour(u,"NEAREST_HIGH_PRIORITY")
       e = tgt ? search(u,"E",range(u,2),0,0,0,0) : search(u,"E",range(u,2),0,0,135,0)
       if e>0: ORDER(12,1,team=1,e,70,70,0,30000); return 1
       if tgt && get_unit_dist(u,tgt) <= get_unit_range(u,3): ORDER(12,1,1,tgt,70,70,0,30000); return 1
   set_unit_mode(u,"IDLE")
return 1
```
No task-state writes. CONFIRMED (disasm 0x5377b0–0x537df6).

**5392d0 (class 3 follow) delta**:
- Attack mode with enemy e. If `will_be_attacked(u,e)` (FUNC 0x651430) and own anim ∉ {0x1e,0x1f,0x20,0x21}:
  - if rand(200) ≤ aggression(U) → U+0x2df=200, then TURN + roll;
  - elif rand(100) ≤ block_chance(U) → ORDER(10,3,0,rot,…,2500) + ORDER(4 BLOCK,2,0,e,0,0,0,1000), return.
  - Otherwise: ORDER(10,3,…) then 53abd0.
- Idle mode, first: m=nearest_incoming_missile(U,"E",range(u,2)). If m && !is_busy_attacking(U) && rand(100) ≤ block_chance → ORDER(10,1,0,get_item_rotation(u,m),…,2500) + ORDER(4 BLOCK,1,0,0,0,0,0,2000), then continue (no return).
- Has no half-range SHIFT dance. First approach uses e=search(range1,flag1) → ORDER(12,1,0,e,**70,70**,0,30000).
- ftgt branch: both searches use 135→0, and the approach to own tgt has no range-3 distance check.

### 538340 gen melee attack roll (class 2), delta vs 535c20
```
r=rand(100); down=can_attack_down(u); fac= down ? is_facing_knock_down_enemy(u) : 0
if r<60 && down && fac: ORDER(19 DOWN_ATTACK,2,0,0,0,0,0,1500); return 1
if r < aggression(U):                       // 511f80
   U+0x2df = FUN_00511e70(U)
   r2=rand(14): 0-2 "A"(p3=1) | 3-4 "AA"(2) | 5 "AAA"(3) | 6-12 "AAB"(4)
       → ORDER(1 COMBO,2,0,string_to_order_para(s,1),0,p3,0,1500); if rand(100)<90 ORDER(6 SHIFT,1,0,3,0,0,0,1500)
   r2==13 && can_energy_attack(u) → ORDER(20 ENERGY_ATTACK,2,0,0,0,0,0,1500)
elif r>=90: ORDER(6,2,0,rand(3)+1,0,0,0,1500)
return 1
```
vs 535c20: the infantry version has an early `if is_busy_attacking return 0`, no DOWN_ATTACK, rand(3) only "A"/"AA" with p3=0, and no energy attack. CONFIRMED.

### 53abd0 special-general attack roll (class 3)
```
if is_busy_attacking(U) return 0                       // → script fallback
r=rand(100); down=…; fac=…
if r<60 && down && fac: ORDER(19,2,0,0..,1500); return 1
if r<60 && DAT_00a58fa7==7 && strcmp(type(U)+4,"MARTINGOUGH")==0:
    set_energy(U, max_energy(U)); if can_rush_attack(u): ORDER(21 RUSH_ATTACK,2,0,0,0,0,0,5000); return 1
if r >= aggression(U) return 1
pf=0; P=DAT_00a5b9e8 (player recno)
if P valid && u!=P && P->is_hostile(u) (FUN_005094d0 ECX=P) && (U+0x114==0 || U+0x114==-2): pf=1
prio=2
if U+0x2df >= (pf?100:120):
    if aggression(U)*(pf?1.3f:1.0f) >= rand(200): r=21, prio=3      // else r unchanged (<aggr)
else r=rand(24)
U+0x2df = FUN_00511e70(U)
r: ≤4 "A"(p3=1) | ≤8 "AA"(2) | ≤12 "AAA"(3) | ≤16 "B"(1) | ≤18 "AAAA"(4) | ≤20 "AAAB"(4)
   → ORDER(1,prio,0,str,0,p3,0,1500)
   ==21: can_energy_attack ? ORDER(20,prio,0,0..,5000) : ORDER(1,prio,0,"B",0,1,0,1500)
   ≤23: can_rush_attack ? ORDER(21,prio,0,0..,5000) : ORDER(1,prio,0,"A",0,1,0,1500)
   else nothing
return 1
```
- No post-combo SHIFT.
- DAT_00a58fa7 = current chapter/mission index (HYPOTHESIS); 7 is the Martin Gough boss level.
- U+0x2df short is a combo/aggression accumulator.
CONFIRMED (0x53abd0–0x53b080).

### 536a40 ATTACK_PLACE (class 2)
Same skeleton as 5377b0 without the follow-distance prelude. Attack mode → TURN(3) + 538340. Idle mode:
```
set_unit_attack_behaviour NEAREST_HIGH_PRIORITY
e=search(u,"E",range1,1,…); en=search(u,"E",range1/2,…) → same 10% approach(60,60) / 90% TURN+SHIFT 1/2/3 dance (class 5/6 excluded); return 1
if e>0: ORDER(12,1,0,e,60,60,0,30000); return 1
e = tgt ? search(u,"E",range2,1,0,0,0) : search(u,"E",range2,1,0,135,0)
if e>0: ORDER(12,1,0,e,60,60,0,30000); return 1
if tgt && get_unit_dist(u,tgt) <= range(u,3): ORDER(12,1,team=1,tgt,60,60,0,30000); return 1
set_unit_mode(u,"IDLE"); ORDER(14 APPROACH_PLACE,0,team=1,para1(place),0,0,0,30000)
```
**539a40 (class 3) delta**:
- Attack mode gets the same will_be_attacked/aggr/block block as 5392d0, with roll 53abd0.
- Idle mode adds the missile-block check and drops the SHIFT dance.
- range2 searches use 135→0 and the order uses team=1.
- The approach to own tgt has no range-3 check.

### 5370d0 / 53a060 DEFEND_PLACE generals (delta vs 534880)
**5370d0 (class 2) vs 534880**:
- No leader handling. Removed: search "L" 1000, the FOLLOW_FORMATION(15,−2,team1,…,10000) when no target, and the leader-target gate on the far-range search.
- Attack-mode TURN prio is 4 (inf: 3) and the roll is 538340.
- Second "EP" range-2 search passes a6=135. Range-2 approach uses team=1.
- End: `set_unit_mode IDLE; ORDER(14,0,team=1,place,0,0,0,30000)` (inf: ORDER(14,−3,team0,place,…) before IDLE).
- The `is_unit_inside_place(place,tgt) && dist ≤ range3` approach is kept.

**53a060 (class 3) vs 5370d0**:
- Adds the will_be_attacked/aggr/block block (attack mode) and the missile-block check (idle mode); roll is 53abd0.
- Drops the half-range SHIFT dance. 135→0.
- The target-inside-place approach has no range-3 distance check.

### 535020 LARGE_WEAPON_PUSH (class 1 only)
- p1 = weapon recno.
- p2 = preferred attach side: 0 → 'M' first, else 'F' first. Weapon type points have flag bits at type+0x1b9[i]: bit0 'M', bit1 'F'.
- U+0x380 = reserved attach point idx (−1 none). U+0x384 = attached weapon recno. U+0x388 = attach slot.
```
w=para1
if get_attaching_unit_recno(u) <= 0:
   if can_attach(w)==1:
      side = para2==0 ? 'M' : 'F'
      if U+0x380==-1: U+0x380 = get_nearest_attach_point(u,w,side)
      if U+0x380 < 0 (none): try other side; on success-path store U+0x380, set task para2 (U+0x2ae) = (para2==0 ? 1 : 0)
      if slot>=0: ORDER(7 MOVE_TO_TARGET,10,0,ap_x,ap_y,ap_z,0,10000)   // get_nearest_attach_point_x/y/z(w,slot)
                  attach_unit(u,w,side,DAT_00a3cc28)
else (already attaching):
   if FUN_0065ddb0(u) > (float)DAT_00a3cc28: detach_unit(w, U+0x388); stop_all_order(u)
   else if w invalid || is_dead(w):
        U+0x388=0; U+0x384=0; U+0x380=-1; FUN_00528300 (clear task); FUN_0050a480(0,0,0,0); get_default_weapon(u); stop_all_order(u)
   else stop_all_order(u)
return 1
```
- DAT_00a3cc28 = attach-distance tolerance (int, runtime BSS).
- get_nearest_attach_point (0x656dc0) also reserves the point: weapon+0x37c[i] = u. It needs weapon+0x38c != 0 and a free point (weapon+0x378[i] < 1 && +0x37c[i] < 1). It uses type+0x1ad count and +0x1b1 vec3 offsets.
CONFIRMED (fallback-side semantics HIGH).

### 535640 — unreferenced (dead code; no thunk caller)
- Hard-coded gate script. Targets A=(23074,5708,−105) and B=(22935,5908,−111).
- dA=get_distance(u,A), dB=get_distance(u,B).
- If dA<10 or dB<10:
  - TURN(10,90,0,325,…,5000);
  - near A → ORDER(22 SPECIAL_MOVEMENT,70,0,11,0,1,0,40000); near B → p1=13;
  - then add_gate_trigger(para1,0) and stop_curr_task.
- Else ORDER(7 MOVE_TO_TARGET,110,0,x,y,z of the nearer point,0,5000).
- Likely the old "close entrance" special. CONFIRMED.

### 535870 RTS_ATTACK_UNIT (class 1)
```
t=para1
if is_attacked(u) || !valid(t) || is_unit_dead(t): stop_curr_task(u); return 1
if get_unit_dist(u,t) < 200: set_unit_mode(u,"ATTACK")
else { ORDER(12,1,0,t,100,100,0,30000); return 1 }
if is_attack_mode(u): ORDER(10,4,0,get_rotation(u,t),0,0,0,2500); return 535c20(U)
if is_idle_mode(u): ORDER(12,1,0,t,100,100,0,30000)
return 1
```
- 537f90 (class 2) is identical with roll 538340.
- 53a6d0 (class 3) adds the will_be_attacked/aggr/block block before the TURN(4), with roll 53abd0.
CONFIRMED.

### 535a20 RTS_ATTACK_ITEM (class 1)
```
it=para1
if is_attacked(u): stop_curr_task; return 1
if !is_item_box(it) (item+0x4e != 10) || item_deleted(it) (FUN_00425d50 on ItemArray 0xa460a0): stop_curr_task; return 1
if get_item_dist(u,it) < 150: set_unit_mode ATTACK else { ORDER(13 APPROACH_ITEM,1,0,it,100,50,0,30000); return 1 }
if is_attack_mode: if !is_item_box(it) {stop_curr_task; return 1}; ORDER(10,4,0,get_item_rotation(u,it),…,2500); return 535c20(U)
if is_idle_mode: ORDER(13,1,0,it,100,50,0,30000)
return 1
```
538140 / 53a9d0 differ only in the roll (538340 / 53abd0). CONFIRMED.

### 535e30 RTS_ATTACK_MOVE task 19 (class 1)
```
e=search(u,"E",150); if e: mode ATTACK else {mode ACTIVE; e=search(u,"E",200); if e && rand(50)==0 mode ATTACK}
if is_attack_mode && e: ORDER(10,3,0,rot(e),…,2500); return 535c20(U)
if is_idle_mode:
   set_unit_attack_behaviour NEAREST_HIGH_PRIORITY
   e=search(u,"E",range1,1,…); en=search(u,"E",range1/2,…)
   en>0 && class∉{5,6} → 10%/40%/40%/10% approach(60,60,30000)/TURN+SHIFT1/2/3 dance; return 1
   e>0 → ORDER(12,1,0,e,60,60,0,30000); return 1
   (x,y,z)=para1..3; ORDER(7 MOVE_TO_TARGET,0,0,x,y,z,0,30000)
   if get_distance(u,x,y,z) <= 200: stop_curr_task(u)
   set_unit_mode(u,"IDLE")
return 1
```
- 538730 (class 2) uses roll 538340.
- 53b1b0 (class 3) adds the will_be_attacked/aggr/block block and drops the NEAREST_HIGH_PRIORITY call and the SHIFT dance.
No task-state writes in any focus routine.

## 3B. Native unit AI — classes 4–7 (heavy_inf, heavy_gen, archer, archer_gen)
Scope per parent: FOLLOW_UNIT / TOWER_ATTACK / RTS_ATTACK_UNIT / RTS_ATTACK_ITEM / task 19 in full; everything else as deltas
vs the class‑1/2/6 base routines (0x534090, 0x534880, 0x535870, 0x535a20, 0x535c20, 0x535e30, 0x536390, 0x536a40,
0x5370d0, 0x5377b0, 0x537f90, 0x538140, 0x538340, 0x538730, 0x540070, 0x541270).
Args below were decoded from the disasm pushes, not the decompiler (it mis-attributes pushes made before nested calls
such as `get_rotation`/`get_unit_range`). give_order = `FUN_00661340(unit, action, prio, team_flag, p1, p2, p3, p4, dur_ms)`.
**None of these routines write task state (+0x2ba)**: they're stateless per think tick. The only task mutation is
`stop_curr_task` (FUNC 166, 0x652370). CONFIRMED (no get/set_curr_task_state call in any of the 34 routines).

Shorthands: `T = search_for_unit(me,"T",0,…)` = own current target (+0x2d7) if valid; `rand(n)` = Misc_random(n) ∈ [0,n);
`R1/R2/R3 = get_unit_range(me,1/2/3)`; TURN(prio,tgt) = give_order(me,10,prio,0,get_rotation(me,tgt),0,0,0,2500)
(item variant uses get_item_rotation); BLOCK = action 4; SHIFT = 6; APPROACH_ENEMY = 12; APPROACH_ITEM = 13;
APPROACH_PLACE = 14; FOLLOW_FORMATION = 15; FOLLOW_UNIT = 16; MOVE_TO_TARGET = 7; COMBO_ATTACK = 1; SHOOT = 2;
DOWN_ATTACK = 19; ENERGY_ATTACK = 20.

### 0. Helpers used (new facts)

| addr | name | semantics | conf |
|---|---|---|---|
| 0x6517c0 | search_for_unit (FUNC) | `Unit_search_for_unit 0x54b480(type, radius, p3, p4, p5, p6)` on `me`. radius 0 → 1600. p3≠0: search centred on the **leader**'s position (if one exists and isn't me). p4 = place recno (used by "EP"). p5 = max \|yaw\| to the target (deg, `0x505a00`). p6 = max \|elevation\| (deg, atan2(dz, horizontal dist), `0x505ba0`). 0 = no limit. "T" returns the current target +0x2d7 if valid, else 0. "L" returns the leader recno if the leader is alive, +0x114 != -1, and within radius. | CONFIRMED |
| 0x6519d0 | search_for_item (FUNC 5) | `0x503680(types, radius, p3)`; "EGC" = enemy **G**ate, **C**losed (item kind +0x4e==1, +0xd1==0, hp +0xc2>0); "EK" = second enemy item class (decoding not finished) | HIGH / UNKNOWN for "EK" |
| 0x65b280 | get_unit_range (FUNC) | by unit-**type** class (type+2): 1: 800/1600/2500; 2: 600/1200/2500; 3: 600/800/2500; 4: 1000/2000/2500; 5: 250/1200/1600; 6: 250/1400/1800; 8: 400/1600/1600 (kind4 = 3200); others 0. kind1 = melee/near, 2 = engage, 3 = keep-target max | CONFIRMED |
| 0x6592c0 | get_follow_unit_distance | 200 if the followed unit has no Team; else 200+100*k where k = 1/2/3/4/5 for member count (Team+2) <2/<5/<10/<17/<26 **only if** Team+0x37 == 1, else k=0 | CONFIRMED |
| 0x512070 | Unit_block_chance | `BLOCKPER(0xa3cc90, =50) * {EASY_A .5, NORM_A 1, DIFF_A 1.5, VDIFF_A 2}[difficulty 0xa57fc2]` (ftol) | CONFIRMED |
| 0x5130a0 | Unit_line_of_fire_blocked(tgt) | ECX=me; steps 1 unit at a time along the 3D line me→tgt from me.z+50 over the terrain heightmap (world+0x339; +0x4c/+0x4e = dims, `0x4c59d0` = height); returns 1 if terrain is above the ray or tgt is invalid, else 0 | HIGH |
| 0x510f80 | Unit_is_in_attack_anim | anim state +0x215 ∈ {8,0x19,0x1c..0x22,0x46,0x49,0x53} → 1 | CONFIRMED |
| 0x65c8d0 | Unit_is_dead_recno | valid && FUN_0042d0e0() | CONFIRMED |
| 0x7c07b0 | strcmp | used against type+4 (type name) | CONFIRMED |

Return value: the routines return 1 = handled. The melee rolls return 0 when `Unit_is_in_attack_anim`; that 0 propagates out of
UnitAI_dispatch, and Unit_ai_think 0x49d810 then runs the unit's script object +0x299 (`vtbl+0xc`) as a fallback. CONFIRMED (0x49d8d7..0x49d90d).

### 1. Main routines (dispatcher ids 4..7)

All four: `if dead return 1; set_unit_attack_behaviour(me, B); task = get_curr_task(me);
if is_attacked(me) { if task==7 && para1==0 { stop_curr_task(me); L=get_leader_recno(me); if L && get_curr_task(L)==7 stop_curr_task(L);} return 1; }`
then switch. B = "NEAREST" (4,5), "WEAKEST" (6,7). Unlisted tasks return 1 (nothing). CONFIRMED.

| task | 4 heavy_inf 0x532780 | 5 heavy_gen 0x532a00 | 6 archer 0x532c60 | 7 archer_gen 0x532ef0 |
|---|---|---|---|---|
| 0 | 53b610 | 53d3b0 | 540070 | 541600 |
| 1 ATTACK_PLACE | 53b610 | 53db10 | 540070 | 5420a0 |
| 3/4 | – | 53d3b0 | – | – |
| 5,7 | – | – | – | – |
| 6 DEFEND_PLACE | 53beb0 | 53e240 | 540640 | 541b10 |
| 8 TOWER_ATTACK | (default) 53b610 | – | 540c10 | 5425d0 |
| 9,18 | 53b610 | 18→53e9d0, 9→– | 540070 | 541600 |
| 10 FOLLOW_UNIT | Unit_leader(me)? 53b610 : 53e9d0 | 53e9d0 | leader? 540070 : 542820 | 542820 |
| 11/12/14 | 545d50 / 545ea0 / 546050 (all four classes) ||||
| 13 RTS_ATTACK_UNIT | 53c700 | 53f260 | 540e80 | 542e60 |
| 17 RTS_ATTACK_ITEM | 53c960 | 53f4c0 | 5410b0 | 543090 |
| 19 | 53cda0 | 53fa60 | 541300 | 543330 |
| default | 53b610 | – | 540070 | 541600 |

`Unit_leader` = FUN_004216d0 with ECX=unit (0x532875 `mov ecx,[ebp+8]`): returns self for a leader. So a soldier in a team (or a team leader) given FOLLOW_UNIT runs its default AI (formation follow); only a team-less unit runs the FOLLOW_UNIT routine.
vs unitAI.res (script source, stale): the heavy_inf script routes 0||10→none and has no 9/18/19; HUMAN_HEAVY_GENERAL routes 3/4/5 to separate recruit/help/flee scripts and 10→follow. Native: heavy_gen 3/4 → default, 5 → nothing; 18 is added to 10. The archer script (HUMAN_ARCHER) uses an `exit`ed body. Native adds 9/18/19.

### 2. Common melee building blocks (heavy classes)

#### 2.1 Engage (attack-mode selection) — shared prologue of 53b610/53beb0/53d3b0/53db10/53e240/53e9d0/53cda0/53fa60
```
E = search_for_unit(me,"E",100)                 // light classes: 150
if E: set_unit_mode(me,"ATTACK")
else { set_unit_mode(me,"ACTIVE"); E = search_for_unit(me,"E",150);   // light: 200
       if E && rand(50)==0: set_unit_mode(me,"ATTACK") }               // 2%/think
if is_attack_mode(me):
    if !E return 1
    if will_be_attacked(me,E) && rand(100) <= Unit_block_chance():     // NEW in heavy classes
        TURN(4,E); give_order(me,BLOCK,4,0,E,0,0,0,1000); return 1
    TURN(3,E); return MeleeRoll(me)                                    // 53cb10 heavy_inf, 53f670 heavy_gen
```
"EP" variants (DEFEND_PLACE) use `search_for_unit(me,"EP",r,0,place)`. Each prologue also has a dead item branch
(`item` local is never assigned → `TURN(3,item); MeleeRoll` unreachable; the decompiler flags it as unreachable).

#### 2.2 Idle-mode enemy pick (shared tail, "near skirmish")
```
if is_idle_mode(me):
  set_unit_attack_behaviour(me,"NEAREST_HIGH_PRIORITY")
  A = search(me,"E", (para1==0 ? R1 : R1/2), centre_on_leader=1)   // heavy_gen/attack-place: always R1
  N = search(me,"E", R1/2)
  if N>0 && type(N).class ∉ {5,6}:           // not cavalry/siege-type class
      r = rand(10)
      r==0  : give_order(me,12,1,0,A,60,60,0,30000)
      1..4  : TURN(1,A); give_order(me,SHIFT,1,0,1,0,0,0,1500)
      5..8  : TURN(1,A); SHIFT p1=2
      9     : TURN(1,A); SHIFT p1=3
      return 1
  if A>0: give_order(me,12,1,0,A,60,60,0,30000); return 1
  …routine-specific tail…
```
(N == A's half-radius probe: a close non-cavalry enemy makes the unit side-step/strafe 90% of the time instead of charging.)

#### 2.3 MeleeRoll heavy_inf — 0x53cb10 (delta vs 0x535c20)
Identical structure (`if Unit_is_in_attack_anim return 0; r=rand(100); if r < Unit_attack_chance(): +0x2df = Unit_base_attack_chance(); k=…; else if r>=90: SHIFT(prio2, rand(3)+1, 1500)`), but
k = rand(**5**) (base: rand(3)): 0 → COMBO "A", 1..2 → "AA", **3..4 → "B"** (new heavy blow). Each combo: give_order(me,1,2,0,string_to_order_para(s,1),0,0,0,1500), then 90%: SHIFT(prio1,p1=3,1500). CONFIRMED.

#### 2.4 MeleeRoll heavy_gen — 0x53f670 (full; delta vs 0x538340 not diffed)
```
r = rand(100)
down = can_attack_down(me) ? is_facing_knock_down_enemy(me) : 0
if r<60 && can_attack_down && down: give_order(me,DOWN_ATTACK,2,0,0,0,0,0,1000); return 1
if r < Unit_attack_chance():
   +0x2df = Unit_base_attack_chance()
   k = rand(14)
   0..2  : COMBO "A"   p3=1 ; 3..4: "AA" p3=2 ; 5: "AAA" p3=3 ; 6..12: "B" p3=1
           = give_order(me,1,2,0,order_para(s,1),0,p3,0,1000); then 90%: SHIFT(1,3,1000)
   13    : if can_energy_attack(me): give_order(me,ENERGY_ATTACK,2,0,0,0,0,0,1000)
elif r > 89: give_order(me,SHIFT,2,0,rand(3)+1,0,0,0,1000)
return 1        // NO busy-anim check (never returns 0)
```
CONFIRMED (pushes 0x53f6f1..0x53f971). Durations 1000 ms (infantry 1500).

### 3. FOLLOW_UNIT

#### 3.1 heavy (classes 4 team-less, 5 for tasks 10/18) — 0x53e9d0 (full)
```
F = para1 (followed unit); ang = para2 ? para2 : 180          // para2 → FOLLOW_UNIT p2 (default 0xb4)
d = valid(F) ? (int)sqrt((me.x-F.x)²+(me.y-F.y)²) : 0          // +0x0e,+0x12 horizontal, ftol
fd = get_follow_unit_distance(F)
if d > fd + 150:
    give_order(me,FOLLOW_UNIT,0,1(team),F,ang,fd,0,5000); set_unit_target_unit_recno(me,0); return 1
T = own target
engage prologue (§2.1 with radii 100/150)            // attack-mode branch → BLOCK prio4 / TURN3 + MeleeRoll 53f670
if is_idle_mode:
    FE = F ? search_for_unit(F,"T") : 0              // the followed unit's target
    §2.2 (A uses R1, centre_on_leader=1)
    if FE:
        set_unit_attack_behaviour(me,"NEAREST_HIGH_PRIORITY")
        A2 = search(me,"E",R2)                       // base 0x5377b0: +FOV 135° when T==0
        if A2>0: give_order(me,12,1,1(team),A2,70,70,0,30000); return 1
        if T && get_unit_dist(me,T) <= R3: give_order(me,12,1,1,T,70,70,0,30000); return 1
    set_unit_mode(me,"IDLE")
return 1
```
So the unit joins the followed unit's fights: it only pulls the team into an attack (team_flag=1, dist params 70/70) when the leader/followed unit has a target. Deltas vs 0x5377b0 (light): radii 100/150 (vs 150/200), the block branch, MeleeRoll 53f670 (vs 538340), and no FOV-135 limit on the A2 search. CONFIRMED.

#### 3.2 archer (class 6 team-less, class 7 always) — 0x542820 (full)
```
F,ang,d,fd as 3.1
if d > fd+150: give_order(me,FOLLOW_UNIT,-1,0,F,ang,fd,0,5000); set_unit_target_unit_recno(me,0); return 1
T  = own target
E2 = search(me,"E",R2, 0,0,0, elev≤65)           // the T==0 / T!=0 branches are identical
E1 = search(me,"E",R1, 0,0,0, 65)
item = 0
if typename == "ENGLISH_EX_ARCHER_CAPTAIN":
    item = search_for_item(me,"EGC",R2) || search_for_item(me,"EK",R2)
if E1:                                          // enemy too close → back off
    give_order(me,12,1,0,E1,R1,400,0,30000); return 1
if E2||item: set_unit_mode(me,"ATTACK") else set_unit_mode(me,"ACTIVE")
if is_attack_mode:
    if E2:
        if !Unit_line_of_fire_blocked(E2): TURN(3,E2); ShootRoll(me,1,E2); return 1
        set_unit_mode(me,"ACTIVE")
    if item: TURN_item(3,item); ShootRoll(me,2,item); return 1
if is_idle_mode:
    A = search(me,"E",R2, centre_on_leader=1, 0,0, 65)
    if A>0: give_order(me,12,1,0,A,R2-400,400,0,30000); return 1
    if T && get_unit_dist(me,T) <= R3: give_order(me,12,1,1,T,R2-400,400,0,30000); return 1
    set_unit_mode(me,"IDLE")
return 1
```
ShootRoll = 0x5432a0 (byte-identical to 0x541270): `if rand(10) < Unit_attack_chance(): +0x2df = Unit_base_attack_chance(); give_order(me,SHOOT,2,0,kind(1 unit/2 item),tgt,0,0,1500)`. CONFIRMED.
Notes: archer FOLLOW_UNIT order prio is **-1** and not team-wide; APPROACH_ENEMY for archers uses (p2,p3) = (R1,400) to step away from close enemies and (R2-400,400) to close to firing range.

### 4. TOWER_ATTACK (task 8; archers posted on towers/walls) — 0x540c10 (class 6), full
```
radius = para1
set_unit_attack_behaviour(me,"NEAREST_ARCHER")
E = search(me,"E",radius, 0,0,0, elev≤65)
item = 0
if typename == "ENGLISH_EX_ARCHER": item = search_for_item(me,"EGC",R2) || search_for_item(me,"EK",R2)
set_unit_mode(me, (E||item) ? "ATTACK" : "ACTIVE")
if is_attack_mode:
    if E && !Unit_line_of_fire_blocked(E): TURN(3,E); ShootRoll_541270(me,1,E)
    if item: TURN_item(3,item); ShootRoll_541270(me,2,item)       // runs even after the unit shot (no return)
return 1
```
No movement orders: the unit never leaves its post. If both fire, the second TURN/SHOOT hit the add-order dedup (same action already current/queued → update/reject; OrderExec). CONFIRMED.
**0x5425d0 (class 7)**: same, except: type name "ENGLISH_EX_ARCHER_**CAPTAIN**"; item searches use radius = para1 (not R2); ShootRoll 0x5432a0. CONFIRMED.

### 5. RTS_ATTACK_UNIT (task 13, p1 = target unit)

#### 5.1 heavy_inf — 0x53c700 (full)
```
tgt = para1
if is_attacked(me) || !valid(tgt) || Unit_is_dead_recno(tgt): stop_curr_task(me); return 1
if get_unit_dist(me,tgt) >= 150: give_order(me,12,1,0,tgt,100,50,0,30000); return 1
set_unit_mode(me,"ATTACK")
if is_attack_mode:
    if will_be_attacked(me,tgt) && rand(100) <= Unit_block_chance(): TURN(4,tgt); BLOCK(4,tgt,1000); return 1
    TURN(4,tgt); return MeleeRoll_53cb10(me)
if is_idle_mode: give_order(me,12,1,0,tgt,100,50,0,30000)
return 1
```
Note: the RTS command is **dropped as soon as the unit is hit** (is_attacked). Delta vs 0x535870: APPROACH p3 = 50 (base 100); block branch added; MeleeRoll 53cb10 (base 535c20). CONFIRMED.
**0x53f260 (heavy_gen)**: same as 53c700 but TURN/BLOCK prio **5** in the block branch and MeleeRoll 0x53f670. The non-block TURN stays at prio 4. CONFIRMED.

#### 5.2 archers — 0x540e80 (class 6), 0x542e60 (class 7)
```
tgt = para1
if is_attacked(me) || !valid(tgt) || dead(tgt): stop_curr_task(me); return 1
if get_unit_dist(me,tgt) >= R2: give_order(me,12,1,0,tgt,R2-400,400,0,30000); return 1
set_unit_mode(me,"ATTACK")
if is_attack_mode:
    if !Unit_line_of_fire_blocked(tgt): TURN(4,tgt); ShootRoll(me,1,tgt)
    else set_unit_mode(me,"ACTIVE")
if is_idle_mode: give_order(me,12,1,0,tgt,R2-400,400,0,30000)
return 1
```
0x542e60 = 0x540e80 with ShootRoll 0x5432a0. CONFIRMED.

### 6. RTS_ATTACK_ITEM (task 17, p1 = item recno)

#### 6.1 heavy_inf — 0x53c960 (full)
```
it = para1
if is_attacked(me): stop_curr_task(me); return 1
if get_item_dist(me,it) >= 150: give_order(me,APPROACH_ITEM 13,1,0,it,100,50,0,30000); return 1
set_unit_mode(me,"ATTACK")
if is_attack_mode:
    if !is_item_box(it): stop_curr_task(me); return 1        // FUNC is_item_box (0x65b6a0) = item still attackable
    TURN_item(4,it); return MeleeRoll_53cb10(me)
if is_idle_mode: give_order(me,13,1,0,it,100,50,0,30000)
return 1
```
Delta vs 0x535a20: no up-front `stop_curr_task`/`is_item_box`/`FUN_00425d50` validity block (base checks item validity first); MeleeRoll 53cb10. **0x53f4c0** (heavy_gen) = 53c960 with MeleeRoll 53f670. CONFIRMED.

#### 6.2 archers — 0x5410b0 (class 6)
```
it = para1
if is_attacked(me): stop_curr_task; return 1
if get_item_dist(me,it) >= R2: give_order(me,13,1,0,it,R2-400,400,0,30000); return 1
set_unit_mode(me,"ATTACK")
if is_attack_mode:
    if !is_item_box(it): stop_curr_task(me); return 1
    ShootRoll_541270(me,2,it)                     // no TURN
if is_idle_mode: give_order(me,13,1,0,it,R2-400,400,0,30000)
```
**0x543090 (class 7)**: in attack mode it does `TURN_item(4,it); give_order(me,SHOOT,3,0,2,it,0,0,1500)`, i.e. an **unconditional** shot at prio 3, with no attack-chance roll. CONFIRMED.

### 7. Task 19 (unnamed; behaves as "attack-move": p1..p3 = destination x,y,z)

#### 7.1 heavy_inf — 0x53cda0 (full); 0x53fa60 heavy_gen is identical in call/arg trace, including the roll (CONFIRMED)
```
search(me,"T")                                   // result discarded
engage prologue §2.1 (100/150), but the TURN before MeleeRoll is prio 3
if !is_attack_mode || !E:
  if is_idle_mode:
    §2.2 (A = search(me,"E",R1,centre_on_leader=1), N = R1/2 probe, strafe/approach 60/60)
    x,y,z = para1,para2,para3
    give_order(me,MOVE_TO_TARGET 7,0,0,x,y,z,0,30000)
    if get_distance(me,x,y,z) <= 200: stop_curr_task(me)          // 0xc9: dist < 201
    set_unit_mode(me,"IDLE")
  return 1
block / TURN(3) + MeleeRoll_53cb10 (also in heavy_gen 53fa60)
```
Delta vs 0x535e30: radii 100/150 (base 150/200), block branch, MeleeRoll 53cb10. **Quirk**: 0x53fa60 (heavy_gen task 19) also calls the *infantry* roll 0x53cb10 (0x53fc3d), not 0x53f670. Every other heavy_gen routine uses 0x53f670. Keep this for fidelity. CONFIRMED.

#### 7.2 archers — 0x541300 (class 6), 0x543330 (class 7)
```
search(me,"T")                                   // discarded
E2 = search(me,"E",R2,0,0,0,65); E1 = search(me,"E",R1,0,0,0,65)
if E1: give_order(me,12,1,0,E1,R1,400,0,30000); return 1
set_unit_mode(me, E2 ? "ATTACK" : "ACTIVE")
if is_attack_mode && E2:
    if !LOF_blocked(E2): TURN(3,E2); ShootRoll(me,1,E2); return 1
    set_unit_mode(me,"ACTIVE")
if is_idle_mode:
    give_order(me,7,0,0,para1,para2,para3,0,30000)
    if get_distance(me,para1,para2,para3) <= 200: stop_curr_task(me)
    set_unit_mode(me,"IDLE")
return 1
```
0x543330 = 0x541300 + ShootRoll 0x5432a0 + a dead item branch (the item local is never set; flagged unreachable at 0x5434ea). CONFIRMED.

### 8. Default / ATTACK_PLACE / DEFEND_PLACE deltas

#### 8.1 0x53b610 heavy_inf default (tasks 0/1/9/18/default, and 8/10 for team members) vs 0x534090
The leader/formation block is the same: `if get_leader_recno(me)>0 { Ld = search(me,"L",1000); if !Ld { give_order(me,15,2,1,0,0,0,0,10000); set_unit_target_unit_recno(me,0); return 1 } if !T give_order(me,15,-2,1,0,0,0,0,10000) }`.
Deltas: engage radii 100/150 (vs 150/200); block branch (§2.1); MeleeRoll 0x53cb10; the leader-centred R2 search uses **FOV 135°** when T==0 (`search(me,"E",R2,1,0,135,0)`). The light base has no FOV there. Tail: `if para1==0 && T>0 && dist(T) <= R3: give_order(me,12,1,1,T,60,60,0,30000)`, else `set_unit_mode IDLE`. CONFIRMED.

#### 8.2 0x53beb0 heavy_inf DEFEND_PLACE vs 0x534880
Same four deltas as 8.1 ("EP" searches with p4 = place; radii 100/150; block; 53cb10; FOV 135 in the leader-centred "EP" R2 search when T==0). The tail is the same as the base: `is_unit_inside_place` check, then APPROACH_PLACE give_order(me,14,-3,0,place,0,0,0,30000). CONFIRMED.

#### 8.3 0x53d3b0 heavy_gen default (tasks 0/3/4) vs 0x536390
Radii 100/150; block; MeleeRoll 53f670. Leader-centred R2 search **without** FOV (base: FOV 135 when T==0), and the resulting approach is team-wide: `give_order(me,12,1,1,A,60,60,0,30000)` (base team_flag 0). Same keep-target tail (dist ≤ R3 → team approach 60/60). No final IDLE. CONFIRMED.

#### 8.4 0x53db10 heavy_gen ATTACK_PLACE vs 0x536a40
Same as 8.3 with these changes: the near probe always uses R1 (no para1 test); no `para1==0` gate on the R2 search; the R2 approach is team_flag 0; and the tail is `set_unit_mode IDLE; give_order(me,APPROACH_PLACE 14,0,1(team),para1,0,0,0,30000)`. Engage-side deltas as 8.3. CONFIRMED.

#### 8.5 0x53e240 heavy_gen DEFEND_PLACE vs 0x5370d0
`place=para1; get_place_unit_view_angle(place)` (result unused). "EP" searches (p4=place) at 100/150. In attack mode: `set_place_unit_view_angle(place,0)`, then block with TURN/BLOCK prio **5**, else TURN(4)+MeleeRoll 53f670. Idle: §2.2 on "EP", then leader-centred "EP" R2 search with no FOV (base: 135) → team approach (12,1,1,…,60,60); `if T && is_unit_inside_place(place,T) && dist ≤ R3` → team approach; else IDLE + give_order(me,14,0,1,place,0,0,0,30000). CONFIRMED.

#### 8.6 0x541600 archer_gen default vs 0x540070
No leader/FOLLOW_FORMATION block (archer generals lead). ShootRoll 0x5432a0. Items: "ENGLISH_EX_ARCHER_CAPTAIN" (EGC/EK within R2). Otherwise identical, including: E1 (R1) → back off (12,1,0,E1,R1,400); idle with para1==0 → leader-centred R2 → (12,1,0,A,R2-400,400); T within R3 → (12,1,1,T,R2-400,400); else IDLE. CONFIRMED.

#### 8.7 0x5420a0 archer_gen ATTACK_PLACE (class 7 task 1)
This is 0x541600 with these changes: the initial E2/E1 searches pass p4=place (para1) (with type "E" the place arg is probably ignored; HYPOTHESIS); there is no para1==0 gate; the idle R2 search is **not** leader-centred; and if nothing is found (`A==0` and T isn't within R3) the unit gets `give_order(me,14,0,1,place,0,0,0,30000)` instead of IDLE. CONFIRMED.

#### 8.8 0x540640 archer DEFEND_PLACE (class 6, full) / 0x541b10 (class 7)
```
place=para1; get_place_unit_view_angle(place) (unused); T=own target
if get_leader_recno(me)>0: search(me,"L",1000) (result unused); if !T: give_order(me,15,-2,1,0,0,0,0,10000)
E2 = search(me,"EP",R2,0,place,0,65); E1 = search(me,"EP",R1,0,place,0,65)
item = typename=="ENGLISH_EX_ARCHER" ? (EGC||EK within R2) : 0
if E1: give_order(me,12,1,0,E1,R1,400,0,30000); return 1
mode = (E2||item) ? ATTACK : ACTIVE
if is_attack_mode: set_place_unit_view_angle(place,0); shoot E2 if LOF clear (TURN3+ShootRoll) else ACTIVE; item → TURN3+ShootRoll
if is_idle_mode:
   A = search(me,"EP",R2,1,place,0,65); if A>0: give_order(me,12,1,0,A,R2-400,400,0,30000); return
   if T && is_unit_inside_place(place,T) && dist(T) <= R3: give_order(me,12,1,1,T,R2-400,400,0,30000); return
   set_unit_mode(me,"IDLE")
```
Unlike 53b610, the "L" result is ignored, so there's no "leader out of range → regroup" here. **0x541b10** deltas: no leader block; ShootRoll 0x5432a0; type "ENGLISH_EX_ARCHER_CAPTAIN"; after IDLE, `if A==0: give_order(me,14,0,1,place,0,0,0,30000)`. CONFIRMED.

### 9. Constants summary
| value | meaning |
|---|---|
| 100 / 150 | heavy melee engage radii (light classes: 150 / 200) |
| rand(50)==0 | 2% chance per think to engage the second-ring enemy |
| rand(100) <= BLOCKPER(50)·{.5,1,1.5,2} | block reaction (TURN+BLOCK prio 4, prio 5 for heavy_gen RTS/defend) |
| 60/60, 70/70, 100/50 | APPROACH_ENEMY p2/p3 for skirmish / follow-assist / RTS melee |
| R2-400 / 400 | archer approach p2/p3; back-off R1/400 |
| 65° | max elevation for archer target search |
| 135° | yaw FOV for heavy_inf leader-centred search |
| 150 + follow_dist | FOLLOW_UNIT leash; follow_dist = 200 + 100·k |
| 180 | default FOLLOW_UNIT p2 (task para2 == 0) |
| 200 | task 19 arrival radius (stop task) |
| 1000 | leader leash ("L" search) for FOLLOW_FORMATION regroup |
| durations | TURN 2500, BLOCK 1000, SHIFT 1500 (heavy_gen roll 1000), COMBO 1500/1000, SHOOT 1500, FOLLOW_UNIT 5000, FOLLOW_FORMATION 10000, approach/move 30000 |

## 3C. Native unit AI — classes 8–15 (cannon, autobow, batterram, camp_horse, cart, citizen, trebuchet, pig)
Scope: dispatcher cases 8..15 of FUN_005467b0 and their task routines. Shared 545d50/545ea0/546050 are in the parent notes (not redone).
Source reference: `script/unit/unitAI.res` sections CANNON(l.727), AUTOBOW(1534), 2nd "CANNON" = batterram (1673), CAMP_HORSE(1070), CART(1750), CITIZEN(1116), TREBUCHET(1298), PIG(123). Native = compiled script; deviations listed per class.

### 0. Common helpers (all CONFIRMED from decomp+disasm unless noted)

| fn | FUNC name | semantics |
|---|---|---|
| 006515d0 | set_unit_mode | unit+0x167 int: "ATTACK"=1, "ACTIVE"=2, "IDLE"=3 |
| 00651590 | is_attack_mode | +0x167 == 1 |
| 00651550 | is_idle_mode | +0x167 > 1 (ACTIVE **or** IDLE) |
| 00656bb0 | is_all_attached(u,"M"/"F") | mask M=1,F=2. 0 if unit+0x38c short==0 or type+0x1ad (slot count)<1. For every slot i with `type+0x1b9[i] & mask`: crew=unit+0x378[i] (int[]); crew<1 → 0; crew invalid → clear +0x378[i]/+0x37c[i], 0; crew+0x384 != self → clear slot, 0. All ok → 1. Bad unit → -1 |
| 006517c0 | search_for_unit(u,type,radius,from_leader,place,max_yaw,max_pitch) | → Unit_search_for_unit 0x54b480 (ECX=u). radius 0→1600. p5 = max \|horizontal bearing\| deg (FUN_00505a00), p6 = max \|elevation angle\| deg (FUN_00505ba0). 'T' = current target +0x2d7 if valid; 'S' = enemy unit whose type class==8 and type+0x11c&0x40 (siege weapon), with terrain LOS; 'L' = own leader within radius; 'E' enemy; 'F' friend; 'EP' enemy inside place p4 |
| 006519d0 | search_for_item(u,type,radius,from_leader) | → FUN_00503680. 'EC' = enemy item of kind 1 (+0x4e==1) with vfunc+0x4c rec: hp +0xc2 > 0 and +0xd1==0 (not destroyed/closed); 'EGC'/'EGO' = enemy gate closed(+0xd1==0)/open(+0xd1==1) |
| 00653150 | get_unit_dist(a,b) | int 3D \|pos_b-pos_a\| |
| 00653060 | get_item_dist(u,item) | int 3D \|item+0x56 - u.pos\| (item array 0xa460a0, FUN_00425d20) |
| 0065b800 | get_distance(u,x,y,z) | int 3D distance |
| 00652ee0 | get_rotation(u,tgt) | signed relative bearing deg (-180,180] to target unit (0x505a00) |
| 00653000 | get_item_rotation(u,item) | same for item+0x56 |
| 00652f80 | get_angle_diff(u,tgt) | same as get_rotation (0x505960→0x505a00), no validity of ret on bad recno: 0 |
| 0065f0f0 | (no FUNC) | signed relative bearing to item (0x5059b0) |
| 0065b280 | get_unit_range(u,kind) | by type class (+2): 1:{800,1600,2500} 2:{600,1200,2500} 3:{600,800,2500} 4:{1000,2000,2500} 5:{250,1200,1600} 6:{250,1400,1800} **8:{400,1600,1600,3200 (kind4)}**, other 0 |
| 0065c8d0 | (no FUNC) | is unit dead (FUN_0042d0e0: hp+0x135<1 && lives+0x120==0) |
| 005130a0 | (ECX=self) | terrain LOS blocked: step 1 world unit along self(z+50)→target; 1 if heightmap (+0x339 map, FUN_004c59d0) > ray z; 1 if target invalid |
| 006524a0 | get_gate_state(item) | kind1 → vfunc+0x4c rec +0xd1 (0 closed,1 open); else -1 |
| 0065b6a0/0065b700/0065b760 | is_item_box / is_item_gate / is_item_wall | |
| 006558d0 | set_unit_target_unit_recno | writes +0x2d7 |
| 006618f0 | stop_all_order | frees OrderQueue +0x2a1 |
| 0065b130 | get_cut_scene_flag | byte [0xa58f90+0x744] |
| 0065c050 | change_animation_special | |
| 00651010 | random_number(n) | = Misc_random(n) |

Shoot/attack micro-helpers (ECX unused):
- `00544c40 AI_cannon_shoot_roll(u,kind,tgt)`: `if (rand(10) < 4) give_order(u, SHOOT 2, prio 2, 0, kind, tgt, 0,0, 1500)` — 40%. CONFIRMED.
- `00544e00 AI_autobow_shoot(u,kind,tgt)`: `give_order(u,SHOOT,2,0,kind,tgt,0,0,1500)`. CONFIRMED.
- `00545d00 AI_trebuchet_shoot(u,kind,tgt)`: `give_order(u,SHOOT,2,0,kind,tgt,0,0,5000)`. CONFIRMED.
- `00545380 AI_ram_attack_roll(u)`: `if (is_all_attached(u,"F") && rand(10) < 5) give_order(u, COMBO_ATTACK 1, 2, 0, string_to_order_para("A",1), 0,0,0, 1500)`. CONFIRMED.

SHOOT p1 = target kind (1 = unit, 2 = item), p2 = target recno. TURN(10) p1 = relative bearing deg (from get_rotation/get_item_rotation). give_order signature: (u, action, prio(char), team_flag, p1,p2,p3,p4, duration_ms). Prio -1 = 0xff (char) = lowest.

None of these routines writes task state +0x2ba; the only "state" is unit mode +0x167 and the order queue. Task end = `stop_curr_task` (FUN_00652370 → FUN_00528300). CONFIRMED (no set_curr_task_state call in any routine below).

### 1. Dispatchers (classes 8..15) — CONFIRMED

Common prologue: `u=*unit; set_unit_attack_behaviour(u, B)`.

| id | fn | class | B | guards | routing |
|---|---|---|---|---|---|
| 8 | 533170 | CANNON | WEAKEST | dead(0x42d0e0)→ret; is_attacked→ret | M=is_all_attached M, F=..F; !M&&!F → stop_all_order. task 7→ret. if M: 12→545ea0, 14→546050, 19→543c10. if F: 0,11,1→543800; 13→5442d0; 17→544690. else nothing |
| 9 | 533370 | AUTOBOW | WEAKEST | dead→ret; attacked→ret | task 0→544cb0; everything else (incl. 7, 11) nothing |
| 10 | 533440 | BATTERRAM | WEAKEST | is_died FUNC(0x653c60)→ret; attacked→ret | !M&&!F→stop_all_order. 12 or 19: M ? 545ea0 : nothing. 17 && F → 5450b0. 101 → 544e50. else nothing |
| 11 | 5335e0 | CAMP_HORSE | NEAREST | is_died→ret; attacked→ret | `if (search_for_unit(u,"E",100,0,0,0,0) && rand(100) > 70) give_order(u, SPECIAL_MOVEMENT 22, prio 7, 0, 2, 0, 1, 0, 10000)` (scared) |
| 12 | 5336e0 | CART | NEAREST, anim "DEFAULT" | `if (unit+0x2e byte==0 && get_cut_scene_flag()) ret`; dead→ret | 0,7→nothing; 10→545430; 14→546050; 12→545ea0 |
| 13 | 533810 | CITIZEN | NEAREST, anim "DEFAULT" | dead→ret (no cut-scene check) | 0→545590; 7→nothing; 10→545900 **then falls through** (returns 1); 14→546050; 101→545a60 |
| 14 | 533920 | TREBUCHET | WEAKEST | dead→ret; attacked→ret | !M&&!F→stop_all_order; 7→ret; if M: 12,19→545ea0, 14→546050; if F: 0,11→543fd0, 1→545b80, 13→544500, 17→544aa0 |
| 15 | 533b10 | PIG | NEAREST, anim "DEFAULT" | dead→ret | see §9 |

Deviations from unitAI.res (native = truth): cannon drops tasks 6 (defend_place) and 8 (tower_attack), adds 19; autobow drops task 11; batterram's "can_fire → idle.txt" fallback dropped; camp horse "drink" branch (rand>98 → SPECIAL_MOVEMENT 3,…,1) dropped; cart/citizen "none" scripts for cart dropped (cart task 0 = nothing) and cart gains task 12; citizen has no cut-scene check; trebuchet drops tasks 3/4/5/6/8, maps 1/13/17 to new routines, routes 19 to rts_move. `unit+0x2e` byte = "run AI during cut scene" (HYPOTHESIS, only seen here).

### 2. CANNON default — 543800 (tasks 0, 1, 11) — CONFIRMED

```c
search_for_unit(u,"T",0,0,0,0,0);                       // result unused (dead)
if (get_leader_recno(u) > 0 && !search_for_unit(u,"L",1000,0,0,0,0)) {  // leader farther than 1000
    give_order(u, FOLLOW_FORMATION 15, prio 2, team 1, 0,0,0,0, 10000);
    set_unit_target_unit_recno(u,0); return 1;
}
range = 1600;
if (unit+0x309 /*weapon actor*/ && ModelArray(0xa570a8).get(actor+0x5c)->+4 == 11) range = 2500; // HYPOTHESIS: long-barrel model
if (!is_all_attached(u,"F")) return 1;
tgt  = search_for_unit(u,"S",range,0,0,0,65) ?: search_for_unit(u,"E",range,0,0,0,65);
item = search_for_item(u,"EC",3000,0);
set_unit_mode(u, (tgt||item) ? "ATTACK" : "ACTIVE");
if (is_attack_mode(u)) {
    if (item) { give_order(u,TURN 10,3,0,get_item_rotation(u,item),0,0,0,2500);
                give_order(u,SHOOT 2,2,0, 2,item,0,0,1500); return 1; }
    if (tgt) { if (!LOS_blocked(tgt)) { give_order(u,TURN,3,0,get_rotation(u,tgt),0,0,0,2500);
                                        AI_cannon_shoot_roll(u,1,tgt); return 1; }   // 40% SHOOT
               set_unit_mode(u,"ACTIVE"); }
}
if (is_idle_mode(u)) {
    if (get_curr_task_para(u,1)==0 && (it=search_for_item(u,"EC",4000,0)) > 0)
         give_order(u, APPROACH_ITEM 13, 1, 0, it, 800, 400, 0, 30000);
    else set_unit_mode(u,"IDLE");
}
```
Item priority > unit; item gets TURN+SHOOT every think (no roll); unit target 40%.

### 3. CANNON task 19 — 543c10 — CONFIRMED
Task 19 (name unknown; behaves "attack-move"): identical target/fire block to §2 (range 1600/2500, S then E, EC 3000), **no** leader-follow. Idle branch:
```c
if (is_idle_mode(u)) { x=para1,y=para2,z=para3;
   give_order(u, MOVE_TO_TARGET 7, prio 0, 0, x,y,z,0, 30000);
   if (get_distance(u,x,y,z) <= 200) stop_curr_task(u);
   set_unit_mode(u,"IDLE"); }
```
Only reached when M attached (dispatcher). Shared trebuchet/batterram task 19 → RTS_MOVE_TO instead.

### 4. CANNON task 13 RTS_ATTACK_UNIT — 5442d0 — CONFIRMED
```c
tgt = para1;
if (is_attacked(u) || !valid(tgt) || is_dead(tgt)) { stop_curr_task(u); return 1; }
if (get_unit_dist(u,tgt) >= get_unit_range(u,2) /*1600*/) {
    give_order(u, APPROACH_ENEMY 12, 1, 0, tgt, range2-2000 /*=-400*/, 2000, 0, 30000); return 1; }
set_unit_mode(u,"ATTACK");
if (is_attack_mode(u)) {
    if (!LOS_blocked(tgt)) { give_order(u,TURN,prio 4,0,get_rotation(u,tgt),0,0,0,2500);
                             return AI_cannon_shoot_roll(u,1,tgt); }
    set_unit_mode(u,"ACTIVE"); }
if (is_idle_mode(u)) give_order(u, APPROACH_ENEMY,1,0,tgt, range2-2000, 2000, 0, 30000);  // LOS blocked → reposition
```
p2 = range2-2000 and p3 = 2000 pushed verbatim (disasm 0x54437b..0x5443a9); meaning of APPROACH_ENEMY p2/p3 belongs to order executor notes.

### 5. CANNON task 17 RTS_ATTACK_ITEM — 544690 — CONFIRMED (angle quirk HIGH)
```c
item = para1;
if (is_attacked(u) || ItemArray(0xa460a0).invalid(item)) { stop_curr_task(u); return 1; }
hp = 999;  it = item_ptr;
if (it->kind(+0x4e)==1) hp = it->vf4c()->+0xc2; else if (kind==4) hp = it->vf58()->+0xc6;
if (hp < 1) { stop_curr_task(u); return 1; }
half = ItemTypeArray(0xa58f50).get(it->+0xa)->(short)+0x24 / 2.0f;     // footprint radius
if (get_item_dist(u,item) < get_unit_range(u,2) /*1600*/) {
    set_unit_mode(u,"ATTACK");
    if (is_attack_mode(u)) {
        if (!is_item_box(item) && !is_item_gate(item) && !is_item_wall(item)) { stop_curr_task(u); return 1; }
        rot = get_item_rotation(u,item);                         // relative bearing, deg
        d = round( Angle(rot deg) - Angle(unit+0x1a yaw rad) )  // Angle stores turns [0,1); d in [0,360)
        if (d <= 1 || d >= 359) give_order(u,SHOOT,3,0, 2,item,0,0,1500);
        else                    give_order(u,TURN ,3,0, rot,0,0,0,2500);
    }
    if (is_idle_mode(u)) give_order(u, APPROACH_ITEM 13, 1, 0, item, range2+(int)half, 400, 0, 30000);
} else give_order(u, APPROACH_ITEM, 1, 0, item, range2+(int)half, 400, 0, 30000);
```
Quirk: `rot` is already relative but absolute yaw is subtracted again (0x5448a6..0x544913) — reproduce verbatim. Angle helpers: 577340 set from radians (/2π, wrap), 5773e0 set from degrees (/360), 5775b0 subtract, 577450 → degrees. FUN_00590a20 = round half away from zero.

### 6. AUTOBOW task 0 — 544cb0 — CONFIRMED
```c
set_unit_attack_behaviour(u,"NEAREST_ARCHER");
tgt = search_for_unit(u,"E",3000,0,0,10,65);           // ±10° cone, fixed mount
set_unit_mode(u, tgt ? "ATTACK" : "ACTIVE");
if (is_attack_mode(u) && tgt) {
    a = get_angle_diff(u,tgt);
    if (a < 10 && a > -10) return AI_autobow_shoot(u,1,tgt);   // SHOOT prio2 1500ms, every think
    unit+0x2d7 = 0;                                           // drop target
}
```
No TURN: autobow never rotates.

### 7. BATTERRAM

#### 7.1 task 101 (SPECIAL+1, "batterram_move_attack") — 544e50 — CONFIRMED
```c
gate = para3;  set_unit_mode(u,"IDLE");
if (!is_item_gate(gate) || get_gate_state(gate) != 0 /*open or not gate*/) {
    set_unit_mode(u,"ACTIVE");
    give_order(u, MOVE_ON_TRACK 9, 1, 0, para1, para2, 0, 0, 30000); return 1; }
if (get_item_dist(u,gate) >= 300) {
    if (is_all_attached(u,"M")) { set_unit_mode(u,"ACTIVE"); give_order(u,MOVE_ON_TRACK,1,0,para1,para2,0,0,30000); }
    return 1; }
if (is_all_attached(u,"M")) give_order(u,TURN,4,0,get_item_rotation(u,gate),0,0,0,2500);
else if (abs(item_bearing(u,gate) /*0x65f0f0*/) > 30) return 1;
set_unit_mode(u,"ATTACK");  AI_ram_attack_roll(u);       // 50% COMBO_ATTACK "A" if F crew
```
Task params: para1/para2 = MOVE_ON_TRACK track params (forwarded as order p1,p2), para3 = target gate item. Ram moves along a track until the closed gate is within 300, rams until it opens/breaks, then resumes the track.

#### 7.2 task 17 RTS_ATTACK_ITEM — 5450b0 — CONFIRMED
```c
item = para1; if (item invalid) { stop_curr_task(u); return 1; }
set_unit_mode(u,"IDLE"); hp=999; reach=0;
if (kind==1) { if (get_gate_state(item) != 0) return 1;   // open gate: nothing to do (task kept)
               hp = vf4c()->+0xc2; reach = 300; }
else if (kind==4) { hp = vf58()->+0xc6; reach = 400; }
if (hp < 1) { stop_curr_task(u); return 1; }
if (get_item_dist(u,item) < reach) {
    if (M) give_order(u,TURN,4,0,get_item_rotation(u,item),0,0,0,2500);
    else if (abs(item_bearing(u,item)) > 30) return 1;
    set_unit_mode(u,"ATTACK"); AI_ram_attack_roll(u);
} else if (M) { set_unit_mode(u,"ACTIVE"); give_order(u, APPROACH_ITEM 13, 1, 0, item, 100, 100, 0, 30000); }
```

### 8. CART / CITIZEN follow — 545430 (cart, task 10), 545900 (citizen, task 10) — CONFIRMED
Identical except order duration: cart 5000 ms, citizen 20000 ms.
```c
lead = para1; keep = para2 ? para2 : 180;
d = get_unit_dist(u,lead);  cur = search_for_unit(u,"T",...);
if (d > 400) { give_order(u, FOLLOW_UNIT 16, prio 5, 0, lead, keep, 200, 0, DUR); set_unit_target_unit_recno(u,0); }
else if (!cur) give_order(u, FOLLOW_UNIT, prio -1, 0, lead, keep, 200, 0, DUR);
```
(`< 0x191` → d ≤ 400 is the near case.)

### 9. CITIZEN

#### 9.1 task 0 "citizen_none" — 545590 — CONFIRMED
```c
e = search_for_unit(u,"E",600,0,0,0,0);
if (e > 0 && valid(e)) {                                     // flee
    s = self.pos, p = enemy.pos;
    a = atan((s.y-p.y)/(s.x-p.x));                           // atan, NOT atan2: sign of direction lost (quirk)
    dz = rand(400) - 400;  r = rand(400);
    dx = (int)(r*sin(a)); dy = (int)(r*cos(a));              // sin→p1, cos→p2 (CRT 7b2404=fsin, 7b24b4=fcos)
    if (dz > 0) dz += 100; if (dz < 0) dz -= 100;            // dz ∈ [-500,-101] always
    stop_all_order(u);
    if (rand(5) <= 3) give_order(u, MOVE_TO_RELATIVE_TARGET 8, prio -1, 0, dx, dy, dz, 0, 2000);   // 80%
    else              give_order(u, SPECIAL_MOVEMENT 22, prio 20, 0, 1, 0, 1, 0, 2000);           // 20% (panic anim)
} else if (rand(100) >= 90) {                                // wander
    for v in (ox,oy,oz): v = rand(200)-100; v>0 ? v+=80 : v<0 ? v-=80 : 0;   // ±[81..180] or 0
    give_order(u, MOVE_TO_RELATIVE_TARGET, prio -1, 0, ox, oy, oz, 0, 5000);
}
```
#### 9.2 task 101 "citizen_walk_in_area" — 545a60 — CONFIRMED
```c
if (rand(100) >= 90) { r = para3;
   give_order(u, MOVE_TO_TARGET 7, prio -1, 0, para1 + rand(2r) - r, para2 + rand(2r) - r, 0, 0, 30000); }
```
para1/para2 = area centre x,y, para3 = half-size (square area, z passed 0).

### 10. PIG — 533b10 — CONFIRMED
```c
rel() { v=rand(200)-100; return v>0 ? v+50 : v<0 ? v-50 : 0; }   // ±[51..150]
if (is_attacked(u))                                        { anim "CAMP"; MOVE_TO_RELATIVE_TARGET(prio -1, rel,rel,rel, 5000); }
else if (search_for_unit(u,"E",100,...) && rand(100) > 80) { anim "CAMP"; same }
else if (search_for_unit(u,"F",100,...) && rand(100) > 95) { anim "CAMP"; same }
else if (rand(100) >= 80)                                  { same, no anim change }
```
give_order(u, 8, -1, 0, ox, oy, oz, 0, 5000). Thresholds from disasm: 0x533c8c `cmp 0x50; jle`, 0x533da8 `cmp 0x5f; jle`, 0x533e9b `cmp 0x50; jl`. "CAMP" = run animation set (string 0x9cf538).

### 11. TREBUCHET

#### 11.1 tasks 0/11 — 543fd0 — CONFIRMED
```c
search_for_unit(u,"T",...);  if (get_leader_recno(u) > 0) search_for_unit(u,"L",1000,...);   // both results unused: no formation follow
if (!is_all_attached(u,"F")) return 1;
tgt  = search_for_unit(u,"S",1600,0,0,0,65) ?: search_for_unit(u,"E",1600,0,0,90,65);   // E limited to ±90° front
item = search_for_item(u,"EC",3200,0);
set_unit_mode(u,(tgt||item)?"ATTACK":"ACTIVE");
if (is_attack_mode(u)) {
    if (item) { give_order(u,SHOOT,2,0, 2,item,0,0,5000); return 1; }       // no TURN
    if (tgt)  { if (!LOS_blocked(tgt)) return AI_trebuchet_shoot(u,1,tgt);  // SHOOT 5000ms, no TURN
                set_unit_mode(u,"ACTIVE"); } }
if (is_idle_mode(u)) {
    if (para1==0 && (it=search_for_item(u,"EC",4000,0)) > 0) give_order(u,APPROACH_ITEM,1,0,it,800,400,0,30000);
    else set_unit_mode(u,"IDLE"); }
```
#### 11.2 task 1 ATTACK_PLACE — 545b80 — CONFIRMED
```c
tgt = search_for_unit(u,"EP",16384,0, para1 /*place*/, 0, 65);
set_unit_mode(u, tgt ? "ATTACK" : "ACTIVE");
if (!is_attack_mode(u)) { if (is_idle_mode(u)) set_unit_mode(u,"IDLE"); return 1; }
if (tgt) { give_order(u,TURN,3,0,get_rotation(u,tgt),0,0,0,2500); AI_trebuchet_shoot(u,1,tgt); }
```
#### 11.3 task 13 RTS_ATTACK_UNIT — 544500 — CONFIRMED
```c
tgt=para1; if (is_attacked(u) || !valid(tgt) || is_dead(tgt)) { stop_curr_task(u); return 1; }
if (get_unit_dist(u,tgt) >= 3*get_unit_range(u,4) /*9600*/) return 1;   // never approaches
set_unit_mode(u,"ATTACK");
if (is_attack_mode(u)) { if (!LOS_blocked(tgt)) { give_order(u,TURN,4,0,get_rotation(u,tgt),0,0,0,2500); return AI_trebuchet_shoot(u,1,tgt); }
                         set_unit_mode(u,"ACTIVE"); }
```
#### 11.4 task 17 RTS_ATTACK_ITEM — 544aa0 — CONFIRMED
```c
item=para1; if (is_attacked(u)) { stop_curr_task(u); return 1; }
if (get_item_dist(u,item) < 3*get_unit_range(u,4)) {
    set_unit_mode(u,"ATTACK");
    if (is_attack_mode(u)) {
        if (!box && !gate && !wall) stop_curr_task(u);
        else { give_order(u,TURN,4,0,get_item_rotation(u,item),0,0,0,2500); give_order(u,SHOOT,3,0,2,item,0,0,1500); } } }
```
No hp check (unlike cannon §5); out of range → nothing (task persists until expiry/replacement).

### Gaps
- Task ids 16 and 19 names: 19 behaves as "move/attack-move" for siege (cannon: MOVE_TO_TARGET + fire; trebuchet/ram: rts_move). UNKNOWN name.
- SPECIAL_MOVEMENT p1..p3 meaning (camp horse 2/0/1, citizen 1/0/1), MOVE_ON_TRACK p1/p2, APPROACH_ENEMY p2/p3, MOVE_TO_RELATIVE_TARGET frame (citizen dz always negative) — order executor's domain.
- Model rec +4 == 11 (range 2500 for cannon) — model array 0xa570a8 type field not decoded.

## 4. Teams, recruiting, formations
Scope: Team struct (Unit+0x33), team membership, leader death, recruiting (RECRUIT_TROOP, barracks, recruit facility, bribe, absorb), formations (FOLLOW_FORMATION slot math).
Confidence tags: CONFIRMED (read in disasm), HIGH, HYPOTHESIS, UNKNOWN.
Coordinates: Unit +0x0e x, +0x12 y (ground plane), +0x16 z (height), +0x1a facing (float, radians).

### 1. Unit fields used by the team system

| off | type | meaning | evidence | conf |
|---|---|---|---|---|
| +0x02 | short | nation recno (0 = none) | 509250 set_nation, 6582f0 | CONFIRMED |
| +0x2f | char | "control locked" (1 = cannot be switched to / recruited). Cleared on leave_team | 658e10 writes `flag==0`; 432300 hero switch skips `+0x2f!=0`; 48dfb0 excludes; 42fcb0 clears | HIGH |
| +0x31 | short | team id. 0 = none, 9999 = "free unit, absorbable by a leader" | 657fd0, 4655a0 (`==9999`), 6bdc00 (9999 when no leader) | CONFIRMED |
| +0x33 | Team* | non-null only on a leader | 447600, 44a660 | CONFIRMED |
| +0x37 | short | leader recno (members only) | 438f80, 42fcb0 | CONFIRMED |
| +0x39 | short | "orphaned" flag: set 1 when team disbanded under it, 0 on join. Blocks auto-absorb | 42dd70, 438f80, 4655a0 | CONFIRMED (writes) / HIGH (meaning) |
| +0x102 | short | spawned-at-runtime flag (1 = barrack/recruit spawn). Gates corpse deletion (and team removal), face-team-dir after FOLLOW_FORMATION | 467590, 6584e0, 4c09a0 set 1; 51e280 tests; 4be770 tests | HIGH |
| +0x104 | int | rank: 0 INFANTRY,1 ARCHER,2 CAVALRY,3 CAPTAIN,4 GENERAL,5 KING (type+0x149 via 527130). rank>2 ⇒ leader (owns Team) | 527130, 5089e0 | CONFIRMED |
| +0x10a | short | race (from type +0x14d, 4ca400 race name→id) | 4c09a0 cmp type+0x14d | HIGH |
| +0x114 | int | 0 normal; 1 = "defeat ⇒ surrender instead of dying" (die path: hp=1, disband team, IDLE, → -1); -1 surrendered; -2 never delete corpse | 7c6240, 51e280, 4655a0, 432300 | HYPOTHESIS (semantics), CONFIRMED (values) |
| +0x120 | int | extra lives; dead ⇔ hp(+0x135)<1 && +0x120==0 | 42d0e0, 4fe510 | CONFIRMED |
| +0x124 | short | level; +0x126 int exp | 523190, 5229a0 | HIGH |
| +0x15b | short | base leadership (LEADERSHIP = +0x15b + item effect 6) | 523920 | CONFIRMED |
| +0x163 | short | morale 0..100 (setter 4fe8c0) | 4fe8c0, 7c6240 | HIGH |
| +0x1c1 | ptr | inventory (null = no items; gates item effects) | 448080, 523920, 50fd90 | HIGH |
| +0x1c5 | short | item dropped on death (0 = none) | 658d90, 50b390 | HYPOTHESIS |
| +0x2e1 / +0x2e2 | char,char | prisoner flag / prisoner pose kind. `FUN_00432b90` returns +0x2e1 (set_task refuses while prisoner) | 519800, 5199b0 | CONFIRMED |
| +0x2eb | short | bribe state: 1 bribeable, 2 bribed | 4e8500, 656a60 | CONFIRMED |
| +0x2ed | char | active/in-scene flag (deactivate 41fcc0 → 0, activate 442820 → 1) | 41fcc0, 442820 | CONFIRMED |
| +0x412 | vec3 f32 | formation slot offset, WORLD axes, relative to Team anchor (z +0x41a never written by 4fa0f0) | 4fa0f0, 448870 | CONFIRMED |
| +0x41e | float | formation width = (cols-1)*100 (also on leader) | 4fa0f0 | CONFIRMED |
| +0x422 / +0x426 | float | formation slot LOCAL (fwd, side) — used as slot identity for hysteresis; leader gets (0,0) | 4fa0f0, 45de50 | CONFIRMED |

### 2. Team struct (operator_new(0x58), ctor FUN_00528660 = memset 0; saved raw 0x58 bytes in unit save, load 47de30/47e580)

| off | type | field | evidence | conf |
|---|---|---|---|---|
| +0x00 | short | leader recno | 447600 `**(+0x33) = *unit` | CONFIRMED |
| +0x02 | char | member count (excludes leader), hard max 20 (0x14) | 438f80 `cmp eax,0x14`, 5287d0 | CONFIRMED |
| +0x03 | short[20] | member recnos, dense, insertion order (+0x03..+0x2a) | 5287d0 append, 5288f0 delete-shift (591070) | CONFIRMED |
| +0x2b | short[6] | member count per rank (index Unit+0x104) | 5287d0 / 5288f0 `+0x2b + rank*2` | CONFIRMED |
| +0x37 | char | formation type: 1 = grid (only value ever written; 0 ⇒ no slotting) | 447600 sets 1; 4fa0f0/451dc0/44a950/6592c0 test `==1` | CONFIRMED |
| +0x38 | vec3 f32 | formation anchor (x,y,z) | 4fa0f0, 658810 | CONFIRMED |
| +0x44 | float | anchor facing (radians) | 4fa0f0 (`= leader+0x1a`), 658810 (deg*0.017453292) | CONFIRMED |
| +0x48 | vec3 f32 | previous anchor snapshot | 658810, 4be770 | CONFIRMED |
| +0x54 | float | previous facing snapshot | 658810, 4be770, 451dc0 | CONFIRMED |

Team is persisted only by pointer-presence (unit save writes the struct after Unit 0x432 bytes if +0x33 != 0) (47de30).

### 3. Membership primitives (pseudo-C)

```c
// FUN_00447600  Unit::create_team()   (ECX = unit)
void create_team(Unit* u) {
  if (!u->team) u->team = new Team{};            // 0x58, zeroed
  u->team->leader = u->recno; u->team->ftype = 1;
  if (nation valid(u->nation)) nation(u->nation)->update_unit_lists(u);   // 4b5e40
  u->team->anchor = u->pos; u->team->angle = u->facing;   // +0x38..+0x44 = unit+0x0e..+0x1a
  u->team->prev_anchor = u->pos; u->team->prev_angle = u->facing;
}

// FUN_00438f80  Unit::join_team(leader_recno)   (ECX = member)
void join_team(Unit* m, int leader) {
  if (leader == 0) { leave_team(m); return; }
  disband_team(m);                               // 42dd70: if m itself leads a team
  Unit* L = UnitArray_get(leader);               // NOTE: L->team must be non-null (no check)
  if (L->team->count < 20 && (L->nation==0 || m->nation==0 || m->nation==L->nation)) {
    m->leader = leader; m->orphan = 0; m->team_id = L->team_id;
    L->team->add_member(m->recno);               // 5287d0 → recompute formation (1,1)
    nation(m->nation)->update_unit_lists(m);
    m->raise_to_level(L->level);                 // 523190: if m->level < L->level, add exp up to level threshold
  }   // silently fails otherwise. NOTE: does NOT remove m from a previous leader's member array — callers call leave_team first
}

// FUN_0042fcb0  Unit::leave_team()   (ECX = member)
void leave_team(Unit* m) {
  if (!m->leader) return;
  UnitArray_get(m->leader)->team->remove_member(m->recno);   // 5288f0 → recompute formation (1,1)
  m->leader = 0; m->team_id = 0; nation(...)->update_unit_lists(m); m->ctrl_lock(+0x2f) = 0;
}

// FUN_0042dd70  Unit::disband_team()   (ECX = leader)
void disband_team(Unit* L) {
  if (!L->team) return;
  for (i = L->team->count; i > 0; --i) { Unit* m = members[i-1]; if valid: leave_team(m); m->orphan(+0x39) = 1; }
  delete L->team; L->team = 0; nation(...)->update_unit_lists(L);
}

// FUN_005287d0 Team::add_member(recno) / FUN_005288f0 Team::remove_member(recno)
add:    if already present return; members[count++] = recno; rank_count[unit.rank]++; update_formation(1,1);
remove: if absent return; delete index (shift down); count--; rank_count[unit.rank]--; update_formation(1,1);

// FUN_005089e0 Unit::set_rank(rank)  (+0x104)
if (rank != cur) { if (rank > 2 && m->leader) leave_team(m); cur = rank;
                   if (rank > 2 && !is_leader) create_team(m); update nation lists; }
```

FUN_004216d0 leader_of(): self if team, else UnitArray_get(+0x37) if recno valid, else 0. FUN_004b62b0 in_team(): `+0x37!=0 || +0x33!=0`.
FUN_004b5e40 Nation::update_unit_lists(unit) (ECX = nation): keeps two sorted short arrays (bsearch 5911a0 cmp 4b5e10, insert 5910f0, delete 591070): leaders list `+0x152d count / +0x1531 ptr` (unit.nation==this && is_leader) and free-unit list `+0x1535 / +0x1539` (unit.nation==this && !in_team). FUN_004b5cc0 removes. CONFIRMED.

Team-id allocation: none. Team ids are author-supplied numbers (scenario/script set_unit_team, barrack leader/member records); 9999 = free pool; 0 = none. Leader lookup by id scans: 6581f0 (nation leader lists), 51fe60 (whole unit array). CONFIRMED (no allocator found; every write of +0x31 copies a given id).

#### Max troop size — FUN_00448080 Unit::max_troop_size()  (ECX = leader)  CONFIRMED (disasm)
```c
int lead = leadership();                         // 523920: +0x15b + item_effect(6 LEADERSHIP)
if (inventory) lead = (item_effect(0x17 TROOP_SIZE) + 100) * lead / 100;   // integer
int extra = max(0, (lead - 20) / 5);             // C int division
int n = extra + 9;
n = max(n, team->count);                         // never below current size
return min(n, 20);
```
TROOP_SIZE = item effect type 0x17 (FUN_004a45f0 parses item "EffectiveRequirementType/AdditionalEffectType/DamageType" names; 0x17 aggregated as percentage by FUN_00497e10: `acc += (acc+100)*v/100` over equipped items 0/1, carried item, and owned non-duplicate items). Read by: 4655a0 absorb, 48dfb0 recruit-nearest, 44f750 RTS assign, 4e8500 bribe, 65ef40 share_team_member, 6bf630 barracks UI. NOT read by recruit_team_member (6584e0), RECRUIT_TROOP (4c09a0), join_team (only hard 20).

### 4. Lifecycle: member death / leader death  (CONFIRMED unless noted)

- Death (`die` FUN_007c6240, ECX = victim, arg = killer recno):
  - morale (MOR_CH_T = 0.3, GAMEPLAY.RES): `w = rankweight(victim) + rankweight2(killer)` (527dd0: rank0-2→1, 3→2, 4-5→3; 527e60 same but GENERAL "JOAN_OF_ARC"→4). For every unit u: if u.nation==killer.nation && killer.team_id!=0 && u.team_id==killer.team_id → `morale = ftol(morale + max(1.0, w*0.3))`; else if u.nation==victim.nation && victim.team_id!=0 && u.team_id==victim.team_id → `morale = ftol(morale - max(1.0, w*0.3))` (setter 4fe8c0 clamps 0..100; leader items MORALE_DROP(0x13)/MORALE_BASE(0x12) may block/raise floor). Disasm 7c7010..7c7142.
  - killer stats: if killer is a member, its leader +0x1bb++ and +0x1b9++ (kill tallies).
  - victim's task cleared (528300). Team NOT touched — dead leader keeps Team*, dead members stay in the member array.
  - special: victim+0x114 == 1 ⇒ not killed: +0x1dd=0x7fffffff, hp=1, disband_team(victim), task IDLE(prio+1), +0x114 = -1.
- Corpse removal (UnitArray process FUN_0051e280, per tick): unit deleted when `+0x102!=0 && hp<=0 && +0x120==0 && +0x114!=-2 && actor fade elapsed (now - actor+0x38 > 1000/actor+0x30)`. Before delete: `leader_of(u)->team->remove_member(u)` (for a leader this is a no-op on its own team). Then FUN_0051e1f0 → Unit dtor FUN_00416580 → deinit FUN_00461f10: `if (+0x37) leave_team(); if (leader) disband_team();`.
- ⇒ Member death: slot/array entry persists until corpse deleted (spawned units only; scenario-placed units with +0x102==0 are never deleted, so their corpses stay members forever). Formation still assigns slots to corpses (4fa0f0 only checks recno validity via 428b70, not 42d0e0).
- ⇒ Leader death: NO re-election. Team keeps dead leader; leader task cleared so members (which inherit leader task via get_curr_task) idle; anchor stops moving (leader corpse static). When (spawned) leader corpse deleted → disband: every member leave_team (team_id := 0, +0x2f := 0) and +0x39 := 1 (orphan; not auto-absorbable since absorb needs team_id==9999 && +0x39==0). Re-grouping only via player recruit (48dfb0/44f750), scripts, bribe.
- Exp sharing (FUN_005229a0 add_exp(e, share, flag)); skipped in game mode 2 and for class 8 siege: if share: member gains e ⇒ alive leader gains `ftol(e*LD_EXP)` (0.4); leader gains e ⇒ each alive member gains `ftol(ftol(e*MEM_EXP)/count)` (MEM_EXP 0.4). Recursion with share=0. CONFIRMED (disasm 5229fc/522acd).

### 5. Script handlers (team)

| FUNC/ACTION | addr | behaviour |
|---|---|---|
| set_unit_team(recno, id) | 657fd0 | leader: create_team, +0x31=id, every other unit with +0x31==id → join_team(leader) (even other leaders → their teams disband); ret 1. Non-leader: if a leader with that id exists → join_team(leader), ret 1; else +0x31=id, ret 0. |
| get_unit_team_size(recno) | 658160 | in_team ? leader->team->count (members only) : 0 |
| get_unit_team_leader_by_team_id(id) | 6581f0 | scan nations' leader lists, first valid leader with +0x31==id; else 0 |
| set_team_nation(id, nation_name) | 6582f0 | leader by id; nation = 4b67e0(4b91a0(name)); RAW writes +0x02 on leader+members (no nation-list/score bookkeeping) |
| set_team_nation2(id, name) | 658420 | RAW +0x02 write on every unit with +0x31==id |
| recruit_team_member(recno, type_name, n) | 6584e0 | L=leader_of(unit); repeat n while L.count != 20: spawn 51dfc0(type), set_rank(type+0x149), set_nation(unit.nation), set_world(unit+0x335), join_team(L), +0x102=1. No position set (spawn default) — pair with set_team_member_pos. Ignores max_troop_size. |
| deactivate/activate_team_member(recno) | 658650/658730 | for each valid member of leader_of(unit) (not leader): 41fcc0 / 442820. Always ret 0 |
| set_formation_pos(recno, x,y,z, deg) | 658810 | anchor=(x,y,z), angle=deg*0.017453292, prev=current, update_formation(1,0); ret recno |
| set_team_member_pos(recno) | 658920 | teleport each valid member: world=leader world, pos = leader pos + member+0x412, facing = leader facing (4ff850/4ff870) |
| kill_team(id) | 658ad0 | leader by id: each alive member with type flag TRIGGER_ATTACK(+0x11c&1) and the leader: +0x120=0, set_hp(0), die(killer = player recno DAT_00a5b9e8). No leader: same over all units with +0x31==id |
| disable_team_drop_item(id) | 658d90 | +0x1c5 = 0 for all units of id |
| set_team_control_flag(id, f) | 658e10 | +0x2f = (f==0) for all units of id |
| del_unit_team(id) | 659080 | leader: delete all members then leader via 655420 (clear task, delete mount +0x1e2, rank>3 → 50b390, 51e1f0). No leader: delete all units of id (rank>3 → 50b390) |
| del_unit_team_member(id) | 659210 | delete every non-leader unit with id |
| set_team_prisoner(id, a, b) | 65dc20 | every valid non-prisoner unit of id: 519800(a,b): stop move, anim 7 if (+0x2f1 && b) else 1, +0x2e1=a, +0x2e2=b, hide weapons (50a480×2) |
| free_team_prisoner(id) | 65dcf0 | every prisoner of id: 5199b0: re-show weapons, clear +0x2e1/+0x2e2, stop |
| get_leader_recno(recno) | 6569f0 | leader recno, 0 if self is leader / none |
| contain_team_member(id) | 656a60 | 1 if any unit (alive or dead) with +0x31==id and +0x2eb!=2 (not bribed) |
| stop_all_team_task(id) | 6523c0 | 528300 clear task on every unit with id |
| share_team_member(dst, src, n) [id 0x183] | 65ef40 | move up to n valid members src→dst while dst.count < dst.max_troop_size: leave_team, join_team(dst), RTS-group cleanup (+0x31d) |
| give_item_to_unit_troop(item, lvl, qty, unit) [id 0x14e] | 65c770 | = give_item_to_unit (65c5f0) but passes flag=1 to 50fd90/495f40 (skips merge-into-existing-stacks branch). Not team-iterating. HYPOTHESIS on purpose |
| get_follow_unit_distance(recno) | 6592c0 | 0 if recno 0/invalid; 200 if no Team; else 200 + 100*cols (cols per §7; ftype≠1 → 200) |

### 6. Recruiting

#### 6a. RECRUIT_TROOP — task 3 and order action 18
- Task 3: routed by per-class AI to the default/"0,3,4" routine (no dedicated native task handler; see dispatcher table in known facts). UNKNOWN beyond that.
- Order 18 executor FUN_004c09a0 (ECX = OrderQueue, arg unit; dispatched by 4baf80 case 0x12). CONFIRMED:
```c
Nation* N = unit->nation_obj();                  // 509400
if (!N || !unit->team) { finish; return; }
for (t = type_count(DAT_00a48d8c); t > 0; --t)   // LAST matching type wins
  if (type(t)->race(+0x14d) == unit->race(+0x10a) && type(t)->rank(+0x149) == 0) break;
if (t < 1) { finish; return; }
n = 0;
while (unit->team->count < 20 && N->try_pay_recruit(0)) {   // 4b5bf0
  r = spawn(t); m->set_rank(0); m->set_nation(unit->nation); m->set_world(unit->world);
  m->join_team(unit->recno); m->spawned(+0x102) = 1; ids[n++] = r;
}
for each spawned: pos = unit->pos + m->slot_offset(+0x412); facing = unit->facing;   // instant placement in formation
finish (current order action := 0);
// Nation::try_pay_recruit(dry): requires gold(+0x1371 f32) >= 2.0 && pool(+0x14a5 f32) >= 1.0 && pop(+0x1385 int) >= 1;
//   if !dry: spend 2.0 gold (4b5010 category 9), pool -= 1.0, pop -= 1.     (field names HYPOTHESIS)
```
Ignores max_troop_size (hard 20).

#### 6b. Other ways units enter a team
- Leader auto-absorb FUN_004655a0 (from unit process 52c490 phase 2, every processed tick): if alive leader, +0x114==0, count < max_troop_size: query spatial grid box ±300 (x,y,z); each unit-actor (type 0xb, not self) with +0x114==0, alive, no leader/team, same nation, +0x39==0, +0x31==9999, subclass(+0x118)∈{0,1}, class∉{0 citizen,8 siege,9}, 3D dist² ≤ 90000 (300): join_team, clear order queue, stop (508b90), clear task. CONFIRMED.
- Player "recruit nearby" FUN_0043c210 (player controller; tried after other interactions): target = FUN_0048dfb0(100): nearest unit u with u.nation==self.nation, u.team_id != self.team_id, rank<3, not player, +0x2f==0, not mounted(+0x384==0), class∉{0,8,9}, subclass∈{0,1}, not prisoner, alive, dist ≤ 100, relative bearing (505960) in [-90°,90°]; requires self has world, team_id!=0, is leader, count < max, alive. Then u.leave_team, join_team(self), clear task/orders, RTS group cleanup. CONFIRMED.
- RTS "assign selection to leader" FUN_0044f750 (RTSCommand domain): purges dead members; if count+new > max and not confirmed → dialog; else keeps nearest members (qsort by distance) and adds nearest selected. Selected leaders (rank>2) under a GENERAL target get task FOLLOW_UNIT(10, prio+1, para1=target). Siege target: light infantry get task 15 LARGE_WEAPON_PUSH. HIGH.
- Bribe FUN_004e8500(briber): requires +0x2eb==1. cost = ftol(hp(+0x135) * (rank<3 ? BRI_COMM(5) : BRI_CAPT(10)) * max(0.5, (level-21)/10 + 1)); unless type is MER_EX_ARCHER / MER_EX_ARCHER_CAPTAIN, cost = ftol(cost*1.6). If gold DAT_00a596c4 ≥ cost: gold -= cost; release LARGE_WEAPON slot; leader ⇒ all members leave_team, else leave_team; set_nation(briber.nation); clear task/orders/targets (+0x2d7/+0x2d9/+0x2db, and other units targeting it); if rank<3 && briber.count < briber.max → join_team(briber); +0x2eb=2; narration "RTS_OTHER_NARRATOR_001". CONFIRMED (constants f32 0x9cfc20=10, 0x9cf784=1, 0x9cf77c=0.5, 0x9d7590=1.6).

#### 6c. Facilities (facility array global 0xa5d100, FUN_0043c450(id); processed every game tick: 4e5fc0 → 46d4a0 → vtbl+0x10 for each valid facility)
Facility kinds (FUN_0046d820 name→kind): BARRACK 1, SHOP 2, RECRUIT 3, STABLE 4, WEAPON 5, GRANARY 6, HOUSE 7, PUB 8, CHAPEL 9, POST 10, GUARD 11, KING 12(?), PRISON 13, MONASTERY 15, WORKSHOP 16. vtbl+0x18 → spawn-barrack sub-object (BARRACK returns this, 4682e0), vtbl+0x24 → recruit sub-object (RECRUIT returns this, 469310), vtbl+0x1c → workshop.

Common facility base: +0x04 short kind, +0x08 char[32] name, +0x29/+0x2d/+0x31 f32 rally point / base z, +0x35 f32 facing (rad), +0x39..+0x41 min vec3, +0x45..+0x4d max vec3 (spawn rect), +0x6d owner Town* (+0x26 nation, +0x52 int population, +0x56 int population max, +0x1ca int pending-unit count, +0x1ce int captured-by-player, +0x1e2 terrain).

**Spawn barrack** (kind 1, vtbl 0x9d0780; process = FUN_00467590):
| off | type | field |
|---|---|---|
| +0x79 | short | spawned leader recno |
| +0x7b | short | leader unit type (0 = none) |
| +0x7d | short | leader level |
| +0x7f | short | team id (for leader and members) |
| +0x81 / +0xc1 | char[64] | strings copied to unit +0x3d (name) / +0x7f |
| +0x101 | order rec | initial order: +0x101 action, +0x105 char prio, +0x106/+0x10a/+0x10e/+0x112 p1..p4, +0x116/+0x11a from_leader/issuer (never set), +0x11e duration ms |
| +0x122 | short | pending member groups (max 4) |
| +0x124 / +0x12c / +0x134 | short[4] each | group unit type / remaining count / level |
| +0x13c | int | follow-unit recno (members get task FOLLOW_UNIT) |
memset(+0x79, 0, 0xc3) = reset (467f90).

```c
// FUN_00467590 SpawnBarrack::process()  — at most ONE unit per tick
if (!leader_type && !groups) return;
if (leader_type) {
  if (!leader_recno) {
    if (!spawn_rect_clear()) return;              // 46da90: no unit with x∈[min.x,max.x], y∈[min.y,max.y]
    L = spawn(leader_type); L.set_rank(type.rank); L.spawned=1; L.set_world(current);
    copy names; L.set_nation(owner.nation); L.pos = (rect centre x,y, terrain z (467fd0)); L.facing = this.facing;
    L.team_id = team_id;
    if (type.rank > 2) { L.create_team(); for every unit u with u.team_id==team_id && !leader_of(u): u.join_team(L); }
    if (game_mode==2) L.+0x10c = 2;
    if (order.action) add_order(L, action, prio, p1..p4, now + duration, 0, 0);
    leader_recno = L; L.raise_to_level(leader_level); if (L.rank < 4) L.recalc(4a95a0); L.set_hp(max_hp);
    return;
  }
  if (!valid(leader_recno)) { reset(); return; }
}
if (!groups) { reset(); return; }
if (count[0] == 0) { if (groups < 2) reset(); else pop group 0; return; }
if (!spawn_rect_clear()) return;
M = spawn(type[0]); M.set_rank(type.rank); M.spawned=1; world; nation=owner.nation; M.team_id = team_id;
if (Lead = find_leader_by_team_id(team_id) /*51fe60*/) M.join_team(Lead);
pos/facing as leader; if (order.action) add_order(M, ...);
if (valid(follow_recno)) M.set_task(FOLLOW_UNIT 10, prio 5, para1 = follow_recno);
M.raise_to_level(level[0]); recalc if rank<4; full hp; game_mode 2 → +0x10c=2; count[0]--;
```

**Recruit facility** (kind 3, vtbl 0x9d07f8, ctor 468410, process = FUN_00468600):
| off | type | field |
|---|---|---|
| +0x79 / +0x7d | DynArray<10B> | recruit-list entries (list 1 / list 2) |
| +0x81 / +0x85 | int | list ids (RECRUIT.RES record index) |
| +0x89 | DynArray<10B> | order queue {short type; int team_id; int count} |
| +0x8d | f32 | SellingRatio |
| +0x91 | short | facility id of its spawn barrack |
| +0x93 | int | available recruits = ftol(owner.pop - 0.95*owner.pop_max) |
| +0x97 | char | can_recruit (default 1) |
| +0x98 | char | recruit level (default 1; RECRUIT.RES Level if >0) |
```c
// FUN_00468600 RecruitFacility::process() (each tick)
avail = ftol(owner.pop - 0.95 * owner.pop_max);          // f64 0x9d0870
B = facility(this.spawn_barrack_id)->as_spawn_barrack();
if (B.groups == 0 && queue.size > 0) {
  q = queue[0]; B.add_member_group(q.type, q.team_id, 1, this.level);       // 467ea0
  B.order = {action 7 MOVE_TO_TARGET, prio 5, p1..p3 = ftol(rally x,y,z), p4 0, duration 5000 ms};  // 4bb950 on B+0x101
  if (--q.count == 0) queue.remove(0);
}
```
→ units come out one per queue step, each walks to the rally point (5 s order), then joins its team leader if `team_id` has a live leader (51fe60), else stays team 9999 and is absorbed by the first friendly leader passing within 300 (4655a0).

Script handlers (barracks):
| handler | addr | effect |
|---|---|---|
| facility_barrack_add_leader(fac, type, team, level, name, title) | 6548f0 → 467dd0 | only if no leader pending: +0x7b/+0x7f/+0x7d, +0x79=0, strings; owner.+0x1ca++ |
| facility_barrack_add_member(fac, type, team, count, level) | 654960 → 467ea0 | if groups<4: +0x7f=team, append group; owner.+0x1ca += count |
| set_facility_follow_unit_recno(fac, recno) | 654ae0 | spawn-barrack +0x13c |
| set_can_recruit_flag(fac, f) | 654bc0 | recruit +0x97 |
| set_recruit_level(fac, lvl) | 654c20 | recruit +0x98 |
| set_barrack_captured(fac, 1) | 654c80 | owner +0x1ce=1, owner nation = player's nation, pop = pop_max, reset spawn barrack, swap flag model (468c10: English↔French flag) |
| facility_barrack_add_leader_order(fac, action_name, prio, _unused, p1..p4, dur) | 654d70 | spawn-barrack order record (+0x101 = 4bbbc0(name)) |
| barrack_set_recruit_list(fac, barrack_id, list1, list2) | 654f10 → 468790 | recruit +0x91 = barrack_id; lists from RECRUIT.RES; Population>0 → owner pop & pop_max; Level>0 → +0x98 |
| workshop_set_recruit_list(fac, list) | 654fa0 → 46b730 | workshop +0xec=list; up to 4 entries (46a6f0): +0xa4 type[4], +0xac price[4] (= type+0x161), +0xb4 short[4] (entry int), +0xbc int[4] (= ftol(entry float*1000) → build ms) |
| search_recruit_list_by_code(code) | 65b7c0 → 4cad00 | 1-based index of RECRUIT.RES record by Code (stricmp), 0 if none |

#### 6d. RECRUIT.RES (`unit\recruit.res`, loader FUN_004ca760, record parser FUN_004ca980, global RecruitRes at 0xa51278 {int count; Rec* recs; char loaded}, rec(i)=FUN_00469520)
Res archive (index of names then text records "VERSION 1.0 … END"). Record (0xb4 bytes):
| off | field | source key |
|---|---|---|
| +0x00 | short id (1-based) | |
| +0x02 | char[32] Code | `Code : X` |
| +0x23 | DynArray<10B> entries {short type (527… name→id 5270b0); f32 a; int b} | `TYPE : a : b` lines until END |
| +0x27 | f32 SellingRatio | |
| +0x2b | char[128] Location (default = Code) | `Location` |
| +0xac | int Level (-1 none) | `Level` |
| +0xb0 | int Population (-1 none) | `Population` |
Barrack records: `FRENCH_LIGHT_INFANTRY : 1 : 50` etc.; workshop: `CANNON : 2 : 0`. For workshops a*1000 = build time ms; b → +0xb4 slot. For barracks a/b meaning UNKNOWN (UI uses type+0x161 as price, not b).

#### 6e. Barracks UI (interfac\i_barr.cpp, object 0x6bc530 ctor .. 0x6c1c70)
Fields: +0x35 facility id, +0x39 customer unit recno (leader or 0), +0x3e gold snapshot (DAT_00a596c4), +0x42 int[6] current per class, +0x5a int[6] ordered, +0x72 allowance, +0x76 ordered total, +0x7a team size incl. pending, +0x7e cost total. Slot order: 0 LIGHT_INF(class1), 1 HEAVY_INF(2), 2 MACEMAN(4), 3 POLEMAN(3), 4 ARCHER(5), 5 CROSSBOW(6); names hard-coded FRENCH_*.
- open FUN_006bda00: allowance = clamp(ftol(owner.pop - 0.95*owner.pop_max), 0, 20); counts = team members by class + spawn-barrack pending groups of same team + recruit queue entries of same team (6bd4b0; loop `break`s at first other-team entry — bug).
- click +/- FUN_006bf630: add allowed if (no leader || max_troop_size(leader) > +0x7a) && allowance ≥ 1 && gold ≥ cost + price(type+0x161); then cost += price, allowance--, ordered++, size++. Minus reverses.
- confirm FUN_006bdc00: for each slot with n>0: recruit.queue.push({type, team_id = leader ? leader.team_id : 9999, n}) (468cd0); gold -= cost; owner.pop -= ordered.
What the player gets: n units of the chosen type at recruit level +0x98, spawned one per tick at the linked barrack, ordered to the rally point, joining the customer's team (or the free pool 9999). Price per unit = unit type +0x161 (int gold).

### 7. Formations

#### 7a. Team::update_formation(force, use_delta) — FUN_004fa0f0 (ECX = Team). CONFIRMED (disasm)
Callers: 5287d0/5288f0 (1,1) on add/remove; 52c490 phase 2 per leader tick (0,1); set_formation_pos (1,0); RTS move 44cfb0 (1,1) after setting anchor; APPROACH_PLACE executor 4bdf90 (1,1).
```c
L = leader; if (!valid) return;
if (L->task_id == 12 /*RTS_MOVE_TO*/) return;                     // anchor owned by RTS move
old_c = new_c = (0,0,0);
if (L->place(+4) && L->place_slot(+6) && (L->task==1||L->task==6) && L->task_p1 == L->place) {
  if (!force) return;                                             // defend/attack-place: anchor = building slot
  old_c = center(prev_anchor, prev_angle);                        // 451dc0
  anchor = place(+4)->slot_pos(+6) (4c2470); angle = place->slot_angle(+6) (4c24b0);
  new_c = center(anchor, angle);                                  // 44a950
} else {
  d2 = (L.x-anchor.x)^2 + (L.y-anchor.y)^2;
  if (d2 < LD_FM_DT /*int 160000 = 400², GAMEPLAY.RES*/) { if (!force) return; use_delta = 0; }
  else if (!force) { old_c = center(prev...); anchor = L.pos; angle = L.facing(+0x1a); new_c = center(anchor, angle); }
  // force && far: anchor NOT moved, delta stays 0
}
if (ftype == 0) return;
delta = new_c - old_c;      // 60cc90 (vec sub)
if (ftype != 1) return;
cols = n<=1?1 : n<=4?2 : n<=9?3 : n<=16?4 : n<=25?5 : (uninit);   // n = member count (char) ⇒ ceil(sqrt(n))
W = (cols-1)*100; SP = 100; ROW0 = -100;
a = (uint8)ftol(angle * 40.74366543152521);                 // f64 0x9d8640 = 128/π; 256-step LUT
c = cos256(a) (576ac0), s = sin256(a) (5768d0)             // LUT: table[k]=sin(k*π/128), k<64, table[64]=1.0
for (i = 0; i < n; ++i) {
  fwd  = ROW0 - (i / cols) * SP;                            // -100, -200, ...  (behind leader)
  side = W/2 - (i % cols) * SP;                             // C int /2 on W
  world[i] = (c*fwd - s*side, s*fwd + c*side);              // float, z=0
  local[i] = (fwd, side);
  if valid(members[i]) count group by members[i].type.class:
     {1 LIGHT_INF, 4 MACEMAN} → gA;  {2 HEAVY_INF, 3 POLEMAN} → gB;  {5 ARCHER, 6 CROSSBOW, 8 SIEGE} → gC;  {0,7 CAVALRY,9} → none
}
used[20] = {0}; k = 0;
for (j < gA) assign(k++, mode 1); for (j < gB) assign(k++, mode 3); for (j < gC) assign(k++, mode 2);
   // assign(k,mode): idx = pick(anchor.xy + world[k], local[k], delta, used, mode, use_delta, tol = SP/10 = 10)
   //   M = members[idx-1]; M.+0x412 = world[k].x; +0x416 = world[k].y; +0x41e = W; +0x422 = local[k].fwd; +0x426 = local[k].side
L.+0x41e = W; L.+0x422 = 0; L.+0x426 = 0;
```
⇒ slots are filled front-to-back, row-major (row = i/cols, col left→right from +W/2 to −W/2): melee light/mace first (closest rows), heavy/pole next, ranged/siege last. Cavalry/citizens get no slot (+0x412 keeps stale value, 0 if never set). Dead-but-undeleted members count and get slots.

center(anchor, angle) (451dc0 uses prev +0x48/+0x54, 44a950 uses +0x38/+0x44): `d = -100 - W/2; return (anchor.x + cos256(a)*d, anchor.y + sin256(a)*d)` (only if ftype==1, else unchanged zero).

#### 7b. Slot picker FUN_0045de50(Team) (CONFIRMED)
```c
int pick(vec2 slot_w, vec2 slot_local, vec2 delta, char used[], int mode, int /*unused*/, int tol) {
  float best = 1e10f; int bi = 0, keep = 0; float keep_d;
  for (j = 0; j < count; ++j) {
    if (used[j] || !valid(members[j])) continue;
    if (!class_in_mode(member, mode)) continue;   // 1:{1,4} 2:{5,6,8} 3:{3,2}
    p = member.pos.xy;
    if (dist2(p, prev_anchor.xy /*+0x48,+0x4c*/) < LD_FM_DT) p += delta.xy;   // predict rigid translation
    d = sqrt(dist2(slot_w, p));
    if (d <= best) { best = d; bi = j+1; }         // ties → later member
    if (slot_local == member.local(+0x422,+0x426)) { keep = j+1; keep_d = d; }   // exact float compare
  }
  if (keep && keep != bi && keep_d < tol + best) bi = keep;   // hysteresis 10 units
  used[bi-1] = 1; return bi;    // bi==0 would write used[-1] (unreachable: group counts match)
}
```

#### 7c. FOLLOW_FORMATION order (action 15) executor FUN_004be770 (ECX = OrderQueue; current order rec at +0x1d: +0x22 p1, +0x26 p2, +0x2a p3 "target cached", +0x32 from_leader(used as path-follow flag), +0x36 issuer=leader, +0x3a expire). HIGH/CONFIRMED
```c
if (anim in {0x40,0x41,0x42,0x3c,0x29,0x4e}) { cancel (action=0); return; }
if (expire < now) { finish (4c1480); return; }
if (510f80() || !508ab0()) return;                       // can't move now
if (p3 == 1) tgt = (float)p1, (float)p2, unit.z;
else { tgt = slot_world(unit) /*448870: leader.team.anchor + unit.+0x412*/; p1=ftol(x); p2=ftol(y); p3=1;
       leader.team.prev_anchor = anchor; prev_angle = angle; }   // snapshot for delta prediction
d2 = horizontal dist² from tgt to (mounted? mount(+0x1e2) : unit);
reach = REACH_DT (15); if (mounted || class==8 siege || unit.LOD(+0x22) > 1) reach = 30;
if (d2 <= reach²) { finish; if (has leader && spawned(+0x102)) turn_to(leader.team.angle in degrees); return; }
if (issuer invalid || issuer dead || issuer has no order queue || !issuer.has_current(action) (4bb8d0) || dist²(unit, issuer) >= 160000) from_leader = 0;
if (valid target(+0x2d7) alive && dist²(unit,target) < d2) { finish; return; }   // break formation to fight
mode = (d2 >= 2.25e6 /*1500²*/) ? 1000 : 500;
r = move_to(505dd0)(tgt, mode, from_leader != 0, issuer);  if (r != 3 && r != 0) from_leader = 0;
```
Issued by infantry AI (e.g. 534090/53b610/540070/543800): if unit has a leader and `search_for_unit(unit,"L",1000)==0` → give_order(15, prio 2, team_flag 1, dur 10000 ms) + clear target; else if no target ("T") → give_order(15, prio -2 (0xfffffffe), team_flag 1, 10000 ms). HIGH (pushes at 534141/53417f).

#### 7d. Formation-related constants
| name | value | src |
|---|---|---|
| slot spacing / row 0 offset | 100 / -100 | 4fa3c0 `mov [ebp-0x3c],0x64`, `[ebp-0x40],0xffffff9c` CONFIRMED |
| hysteresis tol | 10 (= 100/10) | 4fa6ec CONFIRMED |
| re-anchor threshold LD_FM_DT | 160000 (dist², =400) | GAMEPLAY.RES [LD_FM_DT]; read 51c680 CONFIRMED |
| arrive REACH_DT | 15 (30 for mounted/siege/LOD>1) | GAMEPLAY.RES, 4be770 CONFIRMED |
| run threshold | dist² ≥ 2.25e6 → move mode 1000 else 500 | 4be770 HIGH |
| angle→byte | ×40.74366543152521 (128/π), uint8 wrap | f64 0x9d8640 CONFIRMED |
| follow distance | 200 + 100*cols (leader), 200 no team | 6592c0 CONFIRMED |
| max members | 20 hard; max_troop_size ≤ 20 | 438f80, 448080 |

### 8. Gaps
- Task 3 RECRUIT_TROOP native routine: none dedicated (falls into class default routines); semantics of task para for it UNKNOWN.
- RECRUIT.RES entry columns (a, b) for barracks: UNKNOWN use (UI uses type+0x161 price).
- Town object (+0x6d) layout beyond fields listed; population regrowth not traced.
- Nation +0x1371/+0x14a5/+0x1385 names are HYPOTHESIS (gold / recruit pool / population).
- give_item_to_unit_troop flag semantics in 495f40 only partially read.

## 5. Player commands, RTS mode, key bindings
Owner: AITroopRE.TroopCommand.RTSCommand. Addresses are VAs; `this` = ViewCtrl unless stated. Decomp args/ECX recovered from `dis()`. Design reference: `des/strategic_1..16.txt` (in-game RTS help pages, quoted where they confirm code).

### 1. ViewCtrl object (global 0xa5b9e8) — control mode owner

| off | type | meaning | evidence | conf |
|---|---|---|---|---|
| +0x00 | int | player-controlled unit recno (`DAT_00a5b9e8`) | 0x42de70, 0x5094d0, 0x7c6240 | CONFIRMED |
| +0x08 | int | command points (`DAT_00a5b9f0`), see §6 | 0x5229a0, 0x456960 | CONFIRMED |
| +0x10 | int | current mode 0..4 (`DAT_00a5b9f8`) | 0x42e040, is_rts_mode 0x65ed70 | CONFIRMED |
| +0x14 | int | previous mode; while in RTS this is the action control scheme used for all key lookups | 0x42e040, 0x440b30 pushes `[this+0x14]` | CONFIRMED |
| +0x18 | ptr | camera-track container (0x4c bytes) loaded from `RESOURCE\rts.cm` | 0x43f7c0 | HIGH |
| +0x79 | byte | `set_mode` param2 ("forced/allowed" flag; bypasses mission gates) | 0x42e040, 0x42e1a0 | HIGH |
| +0x7d | int | RTS last-command/UI state (table §3.6) | 0x440b30 etc. | CONFIRMED |
| +0x81 | byte | mode switch pending (set by set_mode, cleared when old mode's exit is done) | 0x42e040, 0x42f700, 0x4300e0 | CONFIRMED |
| +0x82/+0x86 | int,uint | keyboard mode: last move-key code / time (double-tap <300 ms = dodge/roll anims 0x15/0x16/0x17) | 0x4343e0 | HIGH |
| +0xed | float | RTS camera distance (reset 700.0 = 0x442f0000; zoom clamp 187.5..2500) | 0x42de70, 0x447960 | CONFIRMED |
| +0xf1 | int | previous RTS state | 0x442ef0 | CONFIRMED |
| +0xf5 | int | RTS state (§2.2) | 0x43fc90, 0x442ef0, 0x4404c0 | CONFIRMED |
| +0xf9 | char[10] | selection group sizes; group 0 = working selection, 1..9 = Ctrl-stored groups | 0x418310, 0x44aad0 | CONFIRMED |
| +0x104 | char | #entries of group 0 that pre-existed the current drag box (shift-additive box) | 0x440b30 (0x441d1c), 0x4489c0 | HIGH |
| +0x105 | int[10][100] | group recnos (`+0x105 + g*400 + i*4`), max 100 ('d') per group | everywhere | CONFIRMED |
| +0x1235 | byte | drag-box active | 0x440b30 | CONFIRMED |
| +0x1236 | char | active group index (commands act on group[+0x1236]) | everywhere | CONFIRMED |
| +0x1237 | char | last commander hotkey index 1..8 (second press = center; on RTS exit switch control to it) | 0x451530, 0x4404c0 | CONFIRMED |
| +0x1238 | int | focused commander recno (0 if none) | 0x449b30, 0x451530, 0x451430 | HIGH |
| +0x123c/+0x1240 | float | drag start mouse x,y | 0x440b30, 0x4489c0 | CONFIRMED |
| +0x1248 | int | hover pick type (§3.3) | 0x44ac60 | CONFIRMED |
| +0x124c | uint | cursor-anim last frame time (ms) | 0x442ef0 | CONFIRMED |
| +0x1250 | int | hover target recno (unit or item) | 0x44ac60 | CONFIRMED |
| +0x1254 | int | cursor-anim family (0 idle) (§3.7) | 0x442ef0 | CONFIRMED |
| +0x1258 | char[8] | cursor sprite name in RTSICON (e.g. "MOVTO03") | 0x442ef0, 0x4450f0 | CONFIRMED |
| +0x1261 | ptr | RTSICON sprite res (= 0xa48f50, loaded by loader as "Loading rts_cursor_res") | 0x43fc90, 0x46fc30 (0x470113) | CONFIRMED |
| +0x1265 | handle | RTSICON image (`0x6099c0("RTSICON",1,0)`, freed in rts_deinit) | 0x43fc90, 0x4404c0 | CONFIRMED |
| +0x1269 | byte | "team over capacity" popup pending | 0x44f750, 0x440b30 head | CONFIRMED |
| +0x744 | byte | paused / menu-open flag (`FUN_004301c0`, 27 callers) | 0x4301c0 | HYPOTHESIS |

Other globals: world_s camera array 0xa48cc0, current camera idx `DAT_00a48cef` (=world_s+0x2f; 1/2 block RTS toggle; 2 also blocks set_mode); world far distance `world_s+0x14` (`_DAT_00a48cd4`) = 8000.0 in RTS, restored from config `DAT_00a58050` on exit; `DAT_00a5704d` = hovered siege-weapon recno (class 8 with +0x38c) for highlight; `DAT_00a58fa7` = campaign/scenario progress object (first int = scenario counter; RTS toggle needs ≥5, character switch needs >1) [HYPOTHESIS for meaning]; `DAT_00a500d2` game-running flag (0 → RTS input ignored).

### 2. Modes and RTS state machine

#### 2.1 Modes (`+0x10`), CONFIRMED by popup texts t_basic.txr POPUP|2..5 and key scheme indices

| mode | name | per-frame keys | enter | update | draw | exit |
|---|---|---|---|---|---|---|
| 0 | keyboard action | 0x4343e0 (scheme 0) | 0x592bd0 | 0x434f40 | 0x434f20 | 0x4343c0 |
| 1 | joypad action | 0x432d30 (scheme 1) | 0x592bd0 | 0x4338c0 | 0x4338f0 | 0x432d10 |
| 2 | mouse-driven action | 0x438b30 (keys via scheme 0) | 0x438ad0 | 0x439090 | 0x4390f0 | 0x438b00 |
| 3 | FPS (keyboard+mouse) | 0x430480 (scheme 3) | 0x430440+0x592bd0 | 0x431520 | 0x431550 | 0x430460 |
| 4 | RTS | 0x440b30 | Rts_init 0x43fc90 | Rts_process 0x442ef0 | Rts_draw 0x4450f0 | Rts_deinit 0x4404c0 |

Start of mission `ViewCtrl_reset 0x42de70`: mode = config+0x6b (`DAT_00a58023`); if 1 and no joystick (`*DAT_00a51806==0`) → 0 and config rewritten. (Local CONFIG.DAT has +0x6b = 3.)

#### 2.2 Mode switch `ViewCtrl_set_mode 0x42e040(new, flag)` (CONFIRMED)
```
if (+0x81 || cam_idx==2 || +0xcd || (mode==4 && new!=+0x14) || new==mode) return;
ViewCtrl_deinit_mode(1)                    // 0x42df50 → per-mode exit (4: Rts_deinit(1))
if (mode==4 && player alive) stop_task(player)   // FUN_00528300
DAT_00a45f3e->0x5ef400(new==4 ? 0 : 1)     // only if scene+0x6e; HYPOTHESIS: toggles player-follow/LOD feature
+0x81=1; +0x79=flag; +0x14=mode; +0x10=new
```
`ViewCtrl_process 0x42f700` each frame: if mode==4 and player dead → set_mode(+0x14). If +0x81: wait for old mode's "exit done" (modes 0-3: always 1; mode 4: `Rts_is_exit_done 0x447860` = state==4, then flush render, `0x6169d0(1)`), then +0x81=0 and call new mode's enter. Then run update of (+0x81 ? old : new) mode.

Toggle key (action 23 "Action/RTS mode toggle", default F2) in `ViewCtrl_process_key 0x42e1a0`: allowed iff pressed && (scenario≥5 || +0x79) && cam_idx∉{1,2}: mode==4 → set_mode(+0x14,0) else set_mode(4,0). Script `quit_rts_mode` 0x65a320 = set_mode(+0x14,0); `is_rts_mode` 0x65ed70 = (+0x10==4).

#### 2.3 RTS sub-state `+0xf5` (CONFIRMED, 0x43fc90/0x442ef0/0x4404c0)

| state | meaning | transition |
|---|---|---|
| 0 | entering: camera track "rts_play" (built from "rts_init" + player pos) running | track finished (`0x428aa0()==0`) → `0x429860(0)`, state 1 |
| 1 | active: input processed (`Rts_process_input` early-outs unless state==1); auto camera-distance fix while view invalid | — |
| 2 | exit requested (Rts_deinit with camera) | track idle → build "rts_deinit" track to player follow pose (+50 z), start transitions on 2 cameras, play → 3 |
| 3 | exit camera running | track finished → 4 |
| 4 | exit done (mode switch may complete) | — |
Rts_deinit with no view/track container or `param==0` → state 4 immediately.

#### 2.4 Rts_init 0x43fc90 (CONFIRMED)
1. cursor res, "POINT00", +0x1254=0; load RTSICON if needed.
2. player unit idle anim (`0x508b90`), `0x6169d0(0)`, world far = 8000.0, `0x44cf50(0)`.
3. player +0x102 (short)=1, +0x23f=0, actor subobject flag +4=1 (HYPOTHESIS: hand player to AI / show selection ring).
4. camera: copies "rts_init" keys into "rts_play" relative to player, lowering start height in 200 steps until `Rts_camera_view_valid` (min 187.5); camera 4 transition; `0x429860(2)`; play "rts_play" at now.
5. +0xf1=+0xf5=0; clear highlights (`0x451430`: unhighlight all groups, +0x1237=0, +0x1238=0); group0 size=0, active group=0, +0x1237=0; `DAT_00a5704d=0`.
6. selection := [player] + player's team members passing `Rts_is_selectable`, highlight (`Unit::0x519250(1)`), +0x1238 = player.
`Rts_resume 0x43f940` = same without camera track (used by `ViewCtrl_after_load 0x42d980` when saved mode==4), sets state 1 directly.

#### 2.5 Rts_deinit 0x4404c0(with_camera) (CONFIRMED)
Free RTSICON; far distance := config `DAT_00a58050`. If with_camera:
- if +0x1237 (last hero hotkey) names a playable hero (`Unit_is_playable_hero 0x42ff90`: JOAN_OF_ARC/JEAN_DE_METZ/LAHIRE/ALENCON) → `ViewCtrl_switch_character(+0x1237)`.
- player: +0x102=0, +0x23f=0; delete current order; if (task!=0 || para1==0) {pending(+0x2be)=current; current=TASK{0}}; every team member: delete order, current task=TASK{0} (members fall back to inheriting leader's task); actor flag=0.
- camera "rts_aim"→"rts_play" lerped from RTS cam to player (`0x447930` lerp); if distance>200 start camera 2 transition + play; state=2; `DAT_00a5704d=0`; clear highlights.

### 3. RTS input — Rts_process_input 0x440b30 (called from process_key when mode 4)

#### 3.1 Input sampling
- edge-scroll dir bits from `DAT_00a51804` (bit2→1, bit3→2, bit0→4, bit1→8) → `Camera_rts_edge_scroll 0x445990` (see docs CAMERA_SYSTEM.md).
- rotate: (wheel-button held `DAT_00a516d8` && mouse dx `DAT_00a516dc`>0) or held action 50 → 2; dx<0 or action 51 → 1. `Rts_camera_rotate 0x447740`: yaw ±frame_ms·0.15° (0.15·0.017453292 rad/ms), pitch fixed π/4 (0x3f490fdb), applied only if `Rts_camera_view_valid`.
- zoom: wheel `DAT_00a516e4`>0 or held action 48 → 1 (in); <0 or action 49 → 2. `Rts_camera_zoom 0x447960`: dist ∓ 2·frame_ms, clamp [187.5 (`f32 0xa0bbf0`), 2500], stored +0xed.
- `Rts_camera_view_valid 0x453930(CameraState)`: unproject the 4 screen corners; each ray must hit terrain and hit point must be inside map (`0x717910`). CONFIRMED.
- mouse (ilib 0xa516b0+0x66c..): `0x5945d0(rect,btn)` any click count, `0x5940f0` single click, `0x594360` double click; btn 0 left / 1 right. Main view rect = View3D +0x10..+0x1c. `Minimap_is_mouse_over 0x717650` (minimap obj 0xa4f381: visible +0xe, rect at (W-0xd3,10) size +0xa/+0xc).
- one-shot keys: action 19 → `0x453790(1)` (HP item to selection?), action 20 (no Alt) → `0x453790(0)`.

#### 3.2 Dispatch priority (first match wins; HIGH, from decomp order)
1. `+0xf5!=1` → return 0. If `+0x1269` and popup closed: result 1 → `Rts_cmd_assign(target,1)` (confirm), else "can't" voice. `+0x7d==10` (building UI open) → return 0.
2. Ctrl+'1'..'9' (`Key_match_char 0x599240(scan, mods, '1'+n, 4)`) → `Rts_store_group 0x418310(n)`: copy group0 → group n, voice RTS_NARRATOR_001.
3. Alt+'1'..'9' (flag 8) → `Rts_recall_group 0x44aad0(n)`: unhighlight all, active group = n, highlight members.
4. commander select (only if no left click on portrait panel rect 0xa48868..74, else portrait under cursor `0x6ffd90`): pressed action 25..32 → idx 1..8 (Joan 25→1, Jean 26→2, La Hire 27→3, Alençon 28→4, Orléans 29→5, Charles 31→6, Richemont 30→7, Flavy 32→8); scancode 0x39 (Space) → 9 (no hero → no-op). Portrait single click → recno, double click → recno + "dblclick". → `Rts_select_commander 0x451530(idx, recno, dbl, !shift)`.
5. left double-click in view: `Rts_dismiss_siege_crew 0x44a690` (double-click siege weapon: release all push slots +0x378/+0x37c, stop tasks, clear unit +0x384/+0x388/+0x21d, voice RTS_SIEGE_007) else `Rts_operate_item 0x44a430` (double-click own/neutral gate class 1 with +0xce==0 → event 0x20; item class 10 flags +0x25e && !+0x66 → event 0x200 via `0x4eef00`; help page 12 "double-left-click on a castle gate to raise the gate").
6. left single click (not dragging) → +0x1254=1 (POINT anim), +0x124c=now, `Rts_click_select 0x449b30(!shift)`.
7. left button down, not dragging, not over minimap → start box: +0x123c/+0x1240 = mouse, +0x1235=1, +0x7d=3.
8. left released while dragging → +0x7d=0, +0x1235=0, ack voice.
9. not dragging (or mouse not moved since press): +0x104 = group0 size; if !(minimap hovered && right click): `Rts_pick_target 0x44ac60(&+0x1248,&+0x1250,&pos)` every frame (drives cursor) and on right click in view execute §3.3; else minimap right-click → `0x717700(&pos)` → `Rts_cmd_move(pos, ctrl)`. Then edge scroll / rotate / zoom.
10. dragging & moved → `Rts_box_select 0x4489c0(!shift)`.

Modifier mask `DAT_00a51800` (prev frame `DAT_00a51802`): 0x3 Shift, 0xc Ctrl, 0x30 Alt. CONFIRMED via `Key_is_pressed_dik 0x4cc570`.

#### 3.3 Hover pick → right-click command (Rts_pick_target 0x44ac60; CONFIRMED structure)
Ray from mouse (`View3D_unproject`) against units (`0x54eec0` iter, actor+0x90 bounds: sphere/box/cylinder kinds 1/2/3) and items (`0x54f510` iter, item classes 10/1/0x27/4 or +0xa2 flag), nearest hit; else terrain `0x452830`. Requires group[active] non-empty for any command.

| type | condition (on hovered object u) | cursor | command | effect |
|---|---|---|---|---|
| 0 | terrain hit | POINT00 | `Rts_cmd_move(x,y,z, ctrl)` | RTS_MOVE_TO / attack-move |
| 1 | unit and `Unit_is_enemy 0x5094d0` (nation relation `0x4b6860`) | INFATT/HVYATT/ARCATT | `Rts_cmd_attack_unit(u)` | task 13 |
| 2 | item, `0x509590(item)`≠0 (hostile, HYPOTHESIS) and: class 1 with +0xc2>0, +0xd2==1, +0xd1≠+0xd2; or class 4 with +0xc6>0; or class 0x12; or class 10 with !+0x25e (no 0x509590 check) | *ATT00 | `Rts_cmd_attack_item(item)` | task 17 |
| 4 | item with +0xa2 and def +0x1c4 | POINT00 | `Rts_cmd_move(item pos +0x56..)` | move |
| 6 | u is siege weapon (+0x38c≠0) with a free M or F push slot, selection has a non-leader E/F/MER_LIGHT_INFANTRY | ASSTO_00 | `Rts_cmd_assign(u,0)` | task 15 |
| 7 | u friendly leader, all selected rank≤2, some selected non-leader not class 8/9 from another team | ASSIGN01 | `Rts_cmd_assign` | join u's team |
| 8 | u same nation, rank>3 (commander), selection has another rank>2 unit | FOLLOW01 | `Rts_cmd_assign` | task 10 |
| 9 | u friendly but not a same-nation leader | CASSTO00 | `Rts_cmd_assign` → "can't" | nothing |
| 10 | terrain hit on building `0x452610` (≤1 leader selected): barracks (kind 3, own nation, +0x79, +0x97==1) or workshop/resource (kind 0x10) within 512 of the selected leader (or player if none); resource kind (+0x79≠0) only for ALENCON within 1024 | BARR01 / WEAPON00 / IRON01 (+0x79==1) / WOOD01 | open barracks UI `0x6c0310(1)` / siege-workshop UI `0x6ed3e0(1)`; +0x7d=10 | interface |
| 11 | u has +0x2e9 (mount) and the single selected leader is the player, dist ≤100 | — | `Rts_cmd_ride 0x452720(u)` → event 0x200 (`0x4eef00(0x200,1,u,1,leader,1,pos,100.0,…)`), +0x9e=0, +0x7d=0xb; next frame if leader task==IDLE(7) stop it | HYPOTHESIS: mount horse |

#### 3.4 Commands → tasks (set_task FUN_00527fb0; TASK via FUN_004e4520). All CONFIRMED from disasm pushes.
Common per selected live unit `u` (group[active]): build TASK on stack, `stop_task(u)` (0x528300), if order queue has a current order: `OrderQueue_end_current 0x4bb660(u)` + `DynArray 0x584580(1)` (drop queued orders), `set_task(u,&task)`. Priority = u's current prio (+0x2a9) + 1, so always accepted by set_task (new prio > current). No give_order is used by RTS commands.

| command | fn | task id | prio | p1 | p2 | p3 | expire | notes |
|---|---|---|---|---|---|---|---|---|
| move (RMB terrain) | 0x44cfb0(x,y,z,ctrl=0) | 12 RTS_MOVE_TO | +1 | int slot x | int slot y | int slot z | now+180000 ms (0x2bf20 = 3 min, 5400 ticks) | formation §3.5; +0x7d=0xc; cursor MOVTO anim family 6 |
| attack-move (Ctrl+RMB) | 0x44cfb0(...,ctrl) | 19 (attack-move) | +1 | x | y | z | now+180000 | +0x7d=4; family 0xe. (AI side: TaskAIGenerals: fights en route, MOVE_TO_TARGET, done ≤200) |
| attack unit | 0x44eb80(t) | 13 RTS_ATTACK_UNIT | +1 | target recno | 0 | 0 | 0 | +0x7d=1; family by single selected class: 1→3 INFATT, 2-4→2 HVYATT, 5-6→4 ARCATT, else 3 |
| attack item | 0x44f1f0(item) | 17 RTS_ATTACK_ITEM | +1 | item recno | 0 | 0 | 0 | reject if `0x425d50(item)`≠0; +0x7d=2 |
| siege crew | 0x44f750(w,_) | 15 LARGE_WEAPON_PUSH | +1 | weapon recno | 1 if all 'M' slots full (use 'F') else 0 | 0 | 0 | see §3.4.1 |
| follow commander | 0x44f750 | 10 FOLLOW_UNIT | +1 | commander recno | 0 | 0 | 0 | only selected units with rank>2; +0x7d=8 |
| join team | 0x44f750 | (no task) | — | — | — | — | — | `Unit_leave_team 0x42fcb0`, `Unit_join_team 0x438f80(leader)`, kill unit's script runtime (+0x31d→+0x4a deleted, `0x5228e0`/`0x520e80`); +0x7d=7 |
| hold / resume | Shift+commander key or portrait toggle | TASK{0, p1=1} | — | — | — | — | — | §4.2 |
"To cancel [follow], order the commander to walk to any valid location" (help 13) — matches move overriding task 10.

##### 3.4.1 Rts_cmd_assign 0x44f750(target, confirmed) (CONFIRMED)
A. target is siege weapon (+0x38c≠0; slot count = type+0x1ad, slot mask bytes type+0x1b9: 1='M', 2='F'; occupant arrays weapon+0x378 (pusher recno) / +0x37c):
   free first count of busy slots; for each selected live E/F/MER_LIGHT_INFANTRY: if no free slot, evict one occupant that is not selected and whose +0x29d < now (stop task, clear its slot, unit+0x388/+0x384=0); stop when neither M nor F slot free (`Unit_push_slots_full 0x50d550(mask)` returns 1 iff every slot of mask is occupied); detach unit from its previous weapon (+0x384 weapon, +0x388 slot: clear arrays, face weapon `0x50abd0`, idle anim), leave team, stop task, drop orders, task 15 (above), `0x519250(0)` unhighlight and remove from selection. +0x7d=6, family 8 (ASSTO_), voice RTS_SIEGE_001/002 (random 0..1).
B. else (target is a unit, no push slots):
   - n_new = #selected live non-leaders with rank<3 from a different team; `any_officer` = some selected unit is leader or rank≥3.
   - if !any_officer && target is leader: cap = `Unit_max_team_size 0x448080(target)`;
     - cap < n_new + team size: if !confirmed && team non-empty → open popup t_interf CHARMOVE|1 ("General %s can only command a maximum of %d units… will remove %d existing units… proceed?", `0x4de530`), +0x1269=1, pause (`DAT_00a500d2=0`), return. If confirmed: sort members by distance to leader (qsort `0x7b2d55`), remove farthest (leave_team), then sort candidates by distance and join nearest min(n_new, cap−kept).
     - else: each candidate (not class 8/9) joins until team size ≥ cap. +0x7d=7, family 9 (ASSIGN).
   - if any_officer && target rank>3: every selected rank>2 unit → task 10 FOLLOW_UNIT(target) (target itself included if selected — FOLLOW on self). +0x7d=8, family 10.
   - nothing done → family 0xb CASSTO, "can't" voice.
`Unit_max_team_size 0x448080`: L = `0x523920()` (leadership attr, HYPOTHESIS), if unit+0x1c1: L = L·(100+bonus(0x17))/100 (`0x497e10`); size = max(0,(L−20)/5) + 9; size = max(size, current members); min(size, 20). CONFIRMED arithmetic.

#### 3.5 Formation move algorithm Rts_cmd_move 0x44cfb0(x,y,z,ctrl) (CONFIRMED)
```
if (!minimap_hover && !0x693580(x,y,z,1)) {   // destination not reachable/valid
    +0x7d=5; cursor "CMOVTO00"; family 7; "can't move" voice; return 0;
}
for u in sel: p=u.prio; stop_task(u); u.prio=p; drop orders
teams = distinct u+0x31 team ids (ascending), cnt[t] = #selected in team t; C = centroid of live selected
dir = Angle_set_from_vector(atan2(y-C.y, x-C.x))    // 0x577b90; angle stored in turns, sin/cos = 0x577b10/0x577b50 (·2π)
S=400; cols=ceil(sqrt(nteams))                      // Misc_round 0x590740(v,0,2)=ceil
half=(cols-1)*S/2
for i,t in teams:
   row = -(i/cols)*S;  lat = half - (i%cols)*S
   slot = (x + cos*row - sin*lat, y + sin*row + cos*lat, z)
   for u in sel with team t, alive, u.task != 12:
     if (no leader(u) || cnt[t] < 2):                 // individual grid
        s = (class(u)==8 ? 200 : 80); c=ceil(sqrt(cnt[t])); h=(c-1)*s/2; j=k[t]++
        r = -(j/c)*s; l = h - (j%c)*s
        p = (int(slot.x) + cos*r - sin*l, int(slot.y) + sin*r + cos*l, z)
     else:                                            // team keeps its formation
        if is_leader(u): team+0x38/+0x3c/+0x40 = slot; team+0x44 = dir·2π; Team::0x4fa0f0(1,1) (re-form)
        p = (int(slot.x) + cos*u.fx - sin*u.fy, int(slot.y) + sin*u.fx + cos*u.fy, int(slot.z)+u.fz)
            // u+0x422 fx, u+0x426 fy, u+0x41a fz = formation slot offsets
     task(ctrl ? 19 : 12, u.prio+1, int p.x, int p.y, int p.z, now+180000); stop_task; set_task
+0x7d = ctrl ? 4 : 0xc; cursor MOVTO00 family ctrl?0xe:6 (POINT00/1 if minimap)
single siege unit (class 8) with all M slots full (`0x50d550(2)==0` branch) → "RTS_SIEGE_005"/CMOVTO (can't move: unmanned)
```
Note `trunc` is `__ftol` (round toward zero). Rotation formula CONFIRMED (0x577b10 = sin via fsin at 0x7b2404; 0x577b50 = cos via fcos 0x7b24b4).

#### 3.6 `+0x7d` values (CONFIRMED writers)
0 none · 1 attack unit · 2 attack item · 3 box-drag · 4 attack-move · 5 cannot move · 6 siege crew assigned · 7 troops joined · 8 follow · 10 building interface open (cleared when `0x6c03e0`/`0x6ed4c0` report closed) · 11 ride event · 12 move.

#### 3.7 Cursor sprites (RTSICON) and animation (Rts_process 0x442ef0, Rts_draw 0x4450f0)
Idle (+0x1254==0) cursor from hover type: 0 POINT00, 1/2 INFATT00|HVYATT00|ARCATT00 (by single selected unit class, default INFATT; also highlights target `0x6fe280`/`0x6fe340`), 6 ASSTO_00, 7 ASSIGN01, 8 FOLLOW01, 10 BARR01/WEAPON00/IRON01/WOOD01. After a command the family plays frame names prefix+NN, advancing every 50 ms (`+0x124c+0x32 < now`) until last frame then back to family 0:

| family | frames | | family | frames |
|---|---|---|---|---|
| 1 | POINT00..05 | | 8 | ASSTO_00..10 |
| 2 | HVYATT00..08 | | 9 | ASSIGN00..15 |
| 3 | INFATT00..08 | | 10 | FOLLOW00..17 |
| 4 | ARCATT00..08 | | 0xb | CASSTO00..10 |
| 6, 0xe | MOVTO00..10 | | 0xf | BARR01..18 (loops while +0x1248==10) |
| 7 | CMOVTO00..10 | | 0x10/0x11/0x12 | WEAPON01..09 / IRON01..09 / WOOD01..17 (loop while type 10) |
Cursor drawn centered unless name starts "POINT" (hotspot = mouse). Drag box drawn with `0x633070`. Acknowledgement voices: string built "RTS" + `0x453da0(recno)` (unit voice prefix e.g. `_LTINF_`, `_ARCHER_`, `_LAHIRE_`…) + 3-digit index (random ranges via `0x590690(a,b)`), played `0x492c40`; "RTS_LTINF_0nn" generic when several units.

#### 3.8 Selection
- `Rts_is_selectable 0x453620(recno)`: alive; type class≠0; subclass∉{2,7}; unit+0x2f==0 (team control flag, script `set_team_control_flag 0x658e10` sets +0x2f = (flag==0)); current task≠15; +0x384==0 (not crewing); siege weapons (type+0x1ad>0) need all 'M' and 'F' slots manned (`0x656bb0(recno,"M"/"F")`). CONFIRMED.
- `Rts_click_select 0x449b30(replace)`: replace → clear all; else flatten active group into group0. Picked unit (`0x447ab0`; if object's +0x1e6 rider valid use it) must be selectable and allied with player (`0x5095e0`) or be the player; add (dedupe); if leader add team members; +0x1238 = recno if rank≥4 and not additive. CONFIRMED.
- `Rts_box_select 0x4489c0(replace)`: frustum from drag rect (6 planes, `0x60d920`); while dragging, entries beyond +0x104 pre-existing that leave the frustum are unhighlighted/removed; then adds selectable friendly units inside (cap 100). HIGH.
- `Rts_select_commander 0x451530(idx,recno,dbl,noshift)`: hero names 1 JOAN_OF_ARC 2 JEAN_DE_METZ 3 LAHIRE 4 ALENCON 5 ORLEAN 6 charlie7 7 arthur_de_richemont 8 FLAVY (`0x5270b0` type→`0x51fd50` unit). Requires alive and +0x2f==0; non-playable commanders also need position on map (`0x717910`). If noshift && (+0x1237==idx || dbl): voice "001" and, if playable, `ViewCtrl_switch_character(idx)`; camera centered on hero (pitch π/4, distance stepped −200 until view valid) — "press the commander's key twice"/double-click portrait (help 4). Else voice "002". Selection: noshift → clear, +0x1237=idx, +0x1238=hero; shift → append. Adds hero + selectable team members.
- groups: Ctrl+n stores group0 (not active group) into n; Alt+n activates n (help 6).

### 4. Action-mode (third-person) commands — no follower order keys exist
DEFAULTK has no follow/hold/attack/charge keys; followers inherit their leader's task (get_curr_task). Player-side troop interactions found:

#### 4.1 `ViewCtrl_process_key 0x42e1a0` (currently misnamed Game_quick_save) — global key handler, runs before per-mode handlers. Scheme = +0x10 (or +0x14 in RTS). CONFIRMED order:
1. guards: `DAT_00a596ab`, paused (+0x744), `0x42ced0`, +0x81, +0x7c, `DAT_00a48ce0`, cam_idx==2, menu keys `0x6f5a70` (actions 34-41).
2. action 53 Quick Save (SAVE\…-QuickSave, GameFile_save), 54 Quick Load (`0x477640`/`0x6e4360`), 46 In-Game Objective toggle (state machine `DAT_00a4d767` 0→1, 1↔2, 3→2 with timestamps).
3. Shift held + action 25..32 → hero unit → `Unit_toggle_hold 0x5121c0(1)` (help 8: "press shift + the commander's pre-assigned numeric key to toggle between holding and resuming").
4. mode≠4 && !+0xac: action 24 (switch char) / 25..28 → if (+0x79 || scenario>1) && player not in anim 0x4f..0x52 (or dead) → `ViewCtrl_switch_character(0 / 1..4)`.
5. action 23 RTS toggle (§2.2).
6. debug (`DAT_00a5ba61`): DIK 0x37 dump camera/player pos; DIK 0x26 dump camera 3.
7. if mode≠4 and player anim ∈ {0x2a,0x3c,0x40,0x4e,0x41,0x42} → +0xa8=0, skip; else per-mode key handler (§2.1 table).

#### 4.2 `Unit_toggle_hold 0x5121c0(recurse)` (CONFIRMED)
```
if (u+0x2f == 1) return;                      // not controllable
if (mode==4 || u != player):
   if (u.task.id==0 && u.task.p1(+0x2aa)!=0) { u.task = u.pending(+0x2be); u.pending=TASK{0,0,p1=1}; voice "…007" }   // resume
   else { u.pending = u.task; u.task = TASK{0,0,p1=1}; voice "…006" }                                          // hold
   voice only if rank>3
delete order queue object (+0x2a1: end current, destroy 0x415220(1), =0, 0x422f10)
if (recurse && is_leader(u)) for members: toggle_hold(0)
```
HOLD marker = current task {id 0, prio 0, p1 1}. Portrait toggle button (red/green, help 8): `CommanderPanel 0x6fd670` (panel recnos +0x125[], count +0x145) calls toggle_hold(1) on click, in RTS or for non-player commanders.

#### 4.3 `ViewCtrl_switch_character 0x432300(idx)` (CONFIRMED)
Playable list at 0x9f8b10 stride 0x104: JOAN_OF_ARC, JEAN_DE_METZ, LAHIRE, ALENCON. idx 0 = cycle to next alive/controllable (+0x2f==0, !0x432b90, +0x114≠−1, on-map or dead) hero; 1..4 = that hero. Sets player (`0x42f9c0`), `DAT_00a57fae` = its nation, UI updates. If mode≠4: new hero: drop orders, idle anim, if not on hold {pending=current; current=0}; its members' tasks = 0; old hero: unless pending is hold marker, current=pending, pending=TASK{0,0,1}. I.e. control suspends the hero's AI task and returns it to the previous hero. Script `switch_controllable_unit` 0x65a2f0 → this. Death of the player unit (non-Joan) also calls it (`Unit_on_killed 0x7c6240`).

#### 4.4 Interact (action 17) chain in action handlers (e.g. 0x4343e0): first of `0x43bef0`, `0x43bd40`, `0x43be40`, `0x43c210`, `0x43bc50` that succeeds, else `0x43bfe0` (barracks UI) / `0x43c080` (workshop UI).
- `ViewCtrl_interact_recruit 0x43c210`: `0x48dfb0(100)` on player = nearest recruitable unit within 100; it leaves its team, `join_team(player)`, stop task, drop orders, destroy its script runtime. CONFIRMED (ECX checked). (Recruit details: TeamRecruit notes.)
- `Unit_find_recruitable_unit 0x48dfb0(radius)` (CONFIRMED, decomp): only for a live player with +0x339 and a team (+0x31 ≠ 0, +0x33 team record) below `Unit_max_troop_size`; candidates: same nation (+0x2), another team (+0x31), rank +0x104 < 3, not the player (`DAT_00a5b9e8`), +0x2f == 0, no weapon attached (+0x384 == 0), type class (+2) not 0 / 8 / 9, type special +0x118 0 or 1, not task-locked (+0x2e1, `Unit_is_task_locked 0x432b90`), alive; 3-D distance ≤ radius and `Unit_relAngleToUnit` within ±90°; nearest wins. No task test: a light infantryman walking to a siege weapon (LARGE_WEAPON_PUSH, not yet attached) is recruitable — R next to him takes him back from the weapon.
- `ViewCtrl_interact_hire 0x43be40` → `Unit_hire_mercenary 0x4e8500(player)` on unit `0x46fa80(200)`: needs +0x2eb==1 (for hire), cost (ftol, MER_EX_ARCHER(_CAPTAIN) priced differently) ≤ gold `DAT_00a596c4` else one-shot "not enough gold" message; pay, release weapon slot, members leave team, switch nation `0x509250`, stop task, destroy order queue, clear targets referencing it, join buyer's team if rank<3 and room (`max_team_size`), +0x2eb=2, counters `DAT_00a591a9++`, `DAT_00a591ad+=cost`, voice RTS_OTHER_NARRATOR_001. HIGH.

### 5. Key bindings — remapkey.cpp 0x4cae70-0x4cb310 (RemapKey object 0xa48820)

Object (0x14 bytes): +0 int n_desc, +4 KeyDesc* (0x28 each), +8 KeyEntry* current, +0xc KeyEntry* defaults, +0x10 int n_entries (=55). Init `0x4cae70` reads DBF tables DEFAULTK and KEYDESC from `Resource/STD.SET` (SET = u16 count + 13-byte dir entries {name[9], u32 offset}; members are dBase III tables), then `RemapKey_load_config 0x4cc220`.

KeyEntry (0x30): +0x00 char name[32] (KEY_ACTION), +0x22 u8 joypad button (J_JOYPAD, 1-based, 0/0xff none), +0x23 u8 mouse button (FPS_MOUSE atoi: 1 LMB 2 RMB 3 MMB, 0/0xff none), +0x24 int K_KEY, +0x28 int J_KEY, +0x2c int FPS_KEY (DIK scancodes parsed "%x"; empty → 0xffff). KeyDesc (0x28): +0 int code, +4 char text[32], +0x27 char device (0 key,1 joypad,2 mouse).

Scheme argument (= mode): 0 K_KEY; 1 J_JOYPAD if set and joystick present else J_KEY; 3 FPS_KEY or FPS_MOUSE; others → no key.
- `RemapKey_is_pressed 0x4cb820(scheme,action)`: key edge `Key_is_pressed_dik 0x4cc570(dik)` (= this frame's scancode `DAT_00a51d5a`; Enter 0x1c/0x9c alias; Ctrl/Shift/Alt from modifier rising edge; 0x3a, 0x45, 0x46, 0xffff never), joypad edge `0x597ed0`, mouse click `0x594700(btn-1)`.
- `RemapKey_is_held 0x4cb6d0`: `Key_is_down_dik 0x4cc3d0` (state table `DAT_00a51700[dik]`), joypad state, mouse held `DAT_00a51da4[btn]`.
- `RemapKey_key_name 0x4cb5b0` → `RemapKey_find_desc 0x4cb9b0(device,code)` text; script `get_key_string(name)` 0x65e0a0 maps A_ATTACK 13, B_ATTACK 14, DEFEND 12, ARROW_SHOOTING 18, INTERACT/RIDE 17, CHARACTER_INTERFACE 35, DOWN_ATTACK 12, USE_HEAL_ITEM 19, COMBO_INTERFACE 36, SWITCH_CHARACTER 24, FORWARD 4, BACKWARD 5, TURN_LEFT 8, TURN_RIGHT 9, SHIFT_LEFT 6, SHIFT_RIGHT 7, SHIFT 3, SHIFT_CAMERA 2, ZOOM_IN 48, ZOOM_OUT 49, ENERGY_ATTACK 15, SWITCH_JOAN 25, SWITCH_JEAN 26, SWITCH_LA_HIRE 27, GAME_OBJECTIVE_KEY 40, RTS_MODE 23 (using scheme = current mode, so returns nothing while in RTS).
- `RemapKey_assign 0x4cba40(scheme,action,device,code)`: action 34 (Main Game Menu/Esc) not remappable; code must exist in KEYDESC; on conflict the other action gets its default back if that default is free, else the replaced old key (swap). `RemapKey_reset_scheme 0x4cb410(scheme)` copies defaults for one scheme.
- Persistence: CONFIG.DAT (0xb05 bytes): +0xb4 byte "custom keys" flag (`DAT_00a5806c`), +0xb5 55×0x30 KeyEntry image (`DAT_00a5806d`). `RemapKey_save_config 0x4cc380` sets flag + memcpy; load copies only fields +0x22..+0x2f (names come from DEFAULTK). CONFIRMED (0xb5+55·0x30 = 0xb05 = required file size).

#### 5.1 Default table (DEFAULTK, action id = 1-based row; DIK hex; KEYDESC names). Rows with no keys are section headers.

| id | action | K (keyboard) | J key / pad btn | FPS key / mouse |
|---|---|---|---|---|
| 1 | *Movement Keys* | | | |
| 2 | Center camera on main character (pad) | – | 0 / pad A(=10) | – |
| 3 | Center camera on main character | 2A Shift | – | – |
| 4 | Move forward | C8 Up | – | 11 W |
| 5 | Move back | D0 Down | – | 1F S |
| 6 | Strafe Left | – | – | 1E A |
| 7 | Strafe Right | – | – | 20 D |
| 8 | Turn left | CB Left | – | – |
| 9 | Turn right | CD Right | – | – |
| 10 | Walk | – | – | 2A Shift |
| 11 | *Control Keys* | | | |
| 12 | Defend | 12 E | 0 / pad 2 | 10 Q |
| 13 | Light attack | 10 Q | 0 / pad 4 | – / LMB |
| 14 | Heavy attack | 11 W | 0 / pad 3 | – / RMB |
| 15 | Energy attack | 1F S | – | 2E C |
| 16 | Jump | 39 Space | 0 / pad 1 | 39 Space |
| 17 | Interact | 13 R | 0 / pad 8 | 13 R |
| 18 | Range mode toggle | 2C Z | 0 / pad 9 | 2C Z |
| 19 | Use HP recovery item | 3D F3 | 0 / pad 5 | 3D F3 |
| 20 | Use EP recovery item | 3E F4 | 0 / pad 6 | 3E F4 |
| 21 | Chain | – | – | – |
| 22 | *Command Keys* | | | |
| 23 | Action/RTS mode toggle | 3C F2 | 3C F2 | 3C F2 |
| 24 | Switch character | 29 ` | 29 ` | 29 ` |
| 25 | Switch to Joan of Arc | 2 '1' | 2 | 2 |
| 26 | Switch to Jean de Metz | 3 '2' | 3 | 3 |
| 27 | Switch to La Hire | 4 '3' | 4 | 4 |
| 28 | Switch to the Duke of Alencon | 5 '4' | 5 | 5 |
| 29 | Command the Bastard of Orléans | 6 '5' | 6 | 6 |
| 30 | Command Sir Arthur de Richemont | 7 '6' | 7 | 7 |
| 31 | Command King Charles VII | 8 '7' | 8 | 8 |
| 32 | Command Guillaume de Flavy | – | – | – |
| 33 | *Menus* | | | |
| 34 | Main Game Menu (fixed) | 1 Esc | 1 | 1 |
| 35 | View Character Details | 3B F1 | 0 / pad 7 | 3B F1 |
| 36 | Combo Window | – | – | – |
| 37 | Save Game Menu | 41 F7 | 41 | 41 |
| 38 | Load Game Menu | 42 F8 | 42 | 42 |
| 39 | Options Menu | 43 F9 | 43 | 43 |
| 40 | Mission Brief | 58 F12 | 58 | 58 |
| 41 | Message Log | 32 M | 32 | 32 |
| 42 | *Onscreen Indicators* | | | |
| 43 | Close Attention Message | 1D Ctrl | 1D | 1D |
| 44 | Show Hit Point Bar | 57 F11 | 57 | 57 |
| 45 | Zoom Minimap | F Tab | F | F |
| 46 | In Game Objective | 18 O | 18 | 18 |
| 47 | *Camera Control* | | | |
| 48 | Zoom camera in | 4E NUMPAD+ | 4E | 4E |
| 49 | Zoom camera out | 4A NUMPAD- | 4A | 4A |
| 50 | Rotate Camera Clockwise | 33 ',' | 33 | 33 |
| 51 | Rotate Camera Counterclockwise | 34 '.' | 34 | 34 |
| 52 | *Miscellaneous* | | | |
| 53 | Quick Save | 3F F5 | 3F | 3F |
| 54 | Quick Load | 40 F6 | 40 | 40 |
| 55 | Capture Screen | 44 F10 | 44 | 44 |
(Exact DBF strings: `Resource/STD.SET` member DEFAULTK; also exported as `work/re-scripts/std_set/DEFAULTK.csv`.) Troop/RTS-related: 23, 24, 25-32, 48-51 (RTS camera), plus non-remappable Ctrl/Alt+1..9 groups and mouse. Hold toggle = Shift + 25..32.

Per-mode action usage (from call sites): mode 0 uses 3,4,5,8,9,12-20,48,49; mode 1: 2,12-14,16-20,48,49 (+ analog stick); mode 2: 17,19,20; mode 3: 4-7,10,12-21,48,49; RTS: 19,20,25-32,48-51 (scheme +0x14).

### 6. Command points (vestigial) — HIGH CONFIDENCE
GAMEPLAY.RES: `[MAX_COM]2000 //MAX_COMMAND_POINT`, `[COM_LIM]200 //CHANGE_COMMAND_LIMIT`, `[COM_TIME]1000 //CONSUME_COMMAND_POINT_TIME`, `[COM_CONS]1 //COMMAND_POINT_CONSUME_PER_TIME` → globals 0xa3cc64, 0xa3cbc8, 0xa3ccc4, 0xa3cbd8 (loader 0x51c680). Only MAX_COM is ever read. `ViewCtrl+8` gains the player unit's XP when not in RTS (`Unit_add_exp 0x5229a0`: `if (u==player && mode!=4) cp = min(cp+exp, MAX_COM)`), saved/loaded in profile (+0x40; 0x720ca0 / 0x41c810), cheat `add_command_point` (console 0x454560 → 0x454c80 → 0x456960) sets cp = MAX_COM. No code consumes or displays it, and COM_LIM/COM_TIME/COM_CONS have no readers (absolute refs scanned; ViewCtrl `+8` not read by any ViewCtrl method) → RTS mode is not gated or drained by command points in the shipped build. A reimplementation may ignore it (keep value for save compatibility).

### 7. Misc helpers
- `Misc_round 0x590740(f, digits, mode)`: mode 1 floor, 2 ceil, else half-away (0.5); returns float. CONFIRMED.
- `Unit_set_rank 0x5089e0(r)` (+0x104 int "ranking", script set_unit_ranking 0x6556e0): r>2 → leave team and create own team (`0x447600`) i.e. rank≥3 = commander; rank>3 = hero/general (voices, follow targets). HIGH.
- `Unit_is_enemy 0x5094d0(recno)` ECX=self: other alive (or is player) and nation≠0 and relation `0x4b6860(self nation, other nation)`. CONFIRMED.
- `Unit_hide_in_command_interface` script 0x65fb50: unit+0x30 byte.
- strategic_command_res (0xa48a18 ← "STRATEGY", loader 0x46fc30 @0x4703cf) belongs to the in-game RTS help viewer (`0x736120` loads `DES\strategic_<n>.txt`, 16 pages), not to RTS command logic. "Loading command_prompt" / "remap_key_system" in 0x46f3a0 (remap object init at 0x46f9da). HIGH.
- `Unit_on_killed 0x7c6240` (8 callers): XP to killer team, hero death voice "RTS_OTHER"+prefix+random, player death → switch character, Flavy-like (+0x8a==1) units revive as IDLE task 7. HYPOTHESIS (not fully analysed).

### Gaps
- Exact semantics of 0x5ef400 (scene toggle on RTS enter/exit), player unit +0x102 / +0x23f flags.
- `0x693580` destination validity test, `0x452830` terrain pick, `0x4eef00` event ids 0x20/0x200 meanings — not opened.
- How AI routines treat HOLD marker (task 0, p1 1) vs leader-task inheritance in get_curr_task — for TaskAI* agents.
- Mode 2 (mouse-driven) control: analysed in docs/GAMEPLAY_IMPLEMENTATION.md "Point & click" (0x438b30, 0x439090, 0x43acb0, 0x43b2d0).

---

# Part C — Pathfinding, movement, ground height, physics/collision, LOS, projectiles, limits (joan.exe)

All addresses are joan.exe VAs. "unit" = unit record (`UnitArray_get 0x41b3c0`),
"body" = physics body ("charmove" record, `FUN_006af310(idx)` with ECX = physics world `0xa570a8`).
Projectiles: Appendix A; entity containers/limits: Appendix B.

## 0. Summary (for the C++ port)

| System | Algorithm | Key functions |
|---|---|---|
| Path data | precomputed **convex-polygon navmesh** per map (`MAP\<name>.PFD`) + 257×257 (or 221×221) **height grid** (same heights as `.GRI`) | loader `00670980` |
| Path search | **A\*** over polygons (node = polygon centroid), open list = sorted singly linked list, **expansion cap = path_to's budget** (OrderExec 500 / 1000, APPROACH_PLACE up to 2000, sidestep 0x4f7bf0 / click move 0x43a580 100), partial path to best node on failure | `0066cb40` → `0066ce30` |
| Path smoothing | portal list (shared edges) → **funnel / string pulling** with agent radius, 8-portal look-ahead, lateral offset inside portal | `0066ece0` |
| Poly lookup | AABB tree (BVH, median split on longest axis) over polygon bboxes | `006745e0`/`006747a0`/`006752c0`/`00675430` |
| Hierarchical | world-level PFD (`<scen>w.pfd`, coords /32) for cross-map routes; local PFD per map | `00505dd0` |
| Ground height | navmesh polygon → height grid (triangulated bilinear, checkerboard diagonals) or polygon plane (walkways) | `00664180`, `004c7b70` |
| Character physics | force integrator in metres (46.1 units = 1 m), g = −9.81, step-up 80 units, swept shape collision vs units/items/geometry, roll-back on hit + slide | `006ad660` → `006a45a0`, `0068de40` |
| LOS | segment vs **anti-portal quads** (from `.POR` + closed gates) | `00560d80` → `00530f20` |
| Gates | toggle navmesh polygon blocked flag (+0x20) + recompute connectivity components; add/enable LOS quads | `0048d4b0`, `006af710` |
| spindex.cpp | uniform-grid rect index used ONLY by the dead editor triangulator | `00681fe0`.. |

Units: world units (map 16384 = 256 cells × 64). Times: ms (game clock `DAT_00a500ba`); 30 Hz tick = 33.3 ms.

---

## 1. Map files consumed by the path/physics code

Loader `FUN_004f2f80` (world_s.cpp), per local map object ("level", unit+0x339):

| Field | File | Loader | Confidence |
|---|---|---|---|
| level+0x54 | `MAP\<dir>\<name>.PFD` (push 0x9d8038 ".PFD", `004f3168`) | `00670980` (ECX=level+0x54, ctor `00664fd0` memset 0x38) | CONFIRMED |
| level+0x58 | `MAP\<dir>\<name>m.PFD` (0x9d81dc "m", `004f31fe`); none shipped | `00670980`; pointer stored `004f328b` | CONFIRMED (load) / HYPOTHESIS ("m" = alternative mesh) |
| level+0x5c | `.trk` (0x9d81d4) → `FUN_004eb640` (track movement, unit+0x3c9 mode) | | HIGH |
| level+0x60 | `.cm` camera tracks `FUN_00426330` | | HIGH |
| level+0x74/+0x78/+0x7c | anti-portal quads from `.POR` (0x9d81b0) via `FUN_005593e0` | §6 | CONFIRMED |
| world+0x6a | `<world>w.PFD` (world-level mesh, `FUN_00442d30`, push 0x9d8038 at `00442ddd`); `map\sample.pfd` in `004797a0` | `00670980` | CONFIRMED |
| `.GRI` | terrain grid/heights (render+ground types; see docs/formats §4.1) | `FUN_004f6eb0` | (owned by WorldFormats) |
| `.PLN` | only when `DAT_00a0b6ac`!=0 — not used by path code | | HIGH (no path xrefs) |

Mesh selection `FUN_005600c0(short sel)` (ECX=level): sel==0 → level+0x58 if non-null else +0x54; sel!=0 → +0x54 if
non-null else +0x58. Units pass `unit+0x102` (short). Shipped data has no `m.PFD` so +0x54 is always used. CONFIRMED.
Local map origin: level+0x44 / +0x48 (int world units); path code works in map-local coordinates
(`unit.pos - origin`, e.g. `004f80a8..004f80fa`). CONFIRMED.

### 1.1 `.PFD` on-disk layout — CONFIRMED (parser written against all 38 shipped files; every file consumed exactly to EOF)

File opened in **record mode** (`FUN_00588000(name,1,1)` at `006709d3`): `read_int` (`FUN_005893c0`) = raw LE i32;
`read_block` (`FUN_00588a80`) = `u16 len` prefix + `len` bytes. All ints little-endian i32.

```
i32 nModels;            nModels × { u16 len(=0x80); char path[128]; }   // Object\Models_Unique\...  (walls/castles)
i32 nPoints;            nPoints × { i32 x, y, z; }                      // map-local world units
i32 nPolys;             nPolys  × {
                            i32 type;            // 0 = ground (height from grid), 1 = on a model (walkway, plane height)
                            i32 bbox[6];         // minx,miny,minz,maxx,maxy,maxz (overwritten from points at load)
                            i32 modelIdx;        // 1-based into model table; 0 = none -> poly+0x24 = FUN_004a36c0(name) (ECX 0xa58f50)
                            i32 nVerts; i32 vert[nVerts];   // 1-based point ids, ring order
                        }
i32 nLinks;             nLinks  × { i32 polyA, edgeA, polyB, edgeB; }   // 1-based; edge e = verts[e-1]->verts[e]
i32 hasGrid;            if hasGrid:
                          i32 x0, y0, cols, rows, cellW, cellH;           // shipped: 0,0,257|221,257|221,64,64 (world maps: cell 2)
                          i16-range i32 height[rows][cols];             // read x fastest; same values as .GRI heights
                          i32 nSub; nSub × { i32 x, y, cols, rows, cellW?, cellH?; i32 h[rows][cols]; } // detail patches; none shipped
```

- Link `FUN_00664130(edgeA, polyB, edgeB)` (ECX=polyA): `polyA.neighbor[edgeA-1]=polyB`, and if edgeB!=0
  `polyB.neighbor[edgeB-1]=polyA`. CONFIRMED.
- Grid stored as plasma 2D short array (`FUN_004c56b0` ctor, `FUN_004c58a0(cols-1, rows-1, 0)` allocs
  `(cols)*(rows)*2` bytes, `FUN_004c5a20(x,y,v)` sets `data[y*cols+x]`), at pathsp+0x30; cellW/cellH at
  pathsp+0x24/+0x28. Sub-grid pointer table pathsp+0x34[cols*rows] indexed by `(y/cellH)*cols + x/cellW`. CONFIRMED.
- S1L1: 7 models, 968 points, 595 polys (364 ground / 231 walkway), 609 links, grid 257×257×64, heights −1034..1538
  (= `.GRI` decode range). Polys have 3..13 verts. World PFDs (`SnL1w.pfd`): 0 models, ≤269 points, type 0 only,
  grid cell 2, heights all 0.
- Post-load (`00671364..00671545`): for each poly with `+0x20 != 1`, sum lengths of its **linked** edges
  (`FUN_007b2344`=sqrt); if `sum < nLinkedEdges * 170 (0xaa)` → `poly+0x20 = -1` ("**narrow**" polygon). Then
  `FUN_00675660` computes connected components. CONFIRMED.

### 1.2 Runtime structures

PathSpace (0x38 bytes, ctor `00664fd0`):

| Off | Type | Meaning | Conf |
|---|---|---|---|
| +0x00 | BlkArrayB* | points, rec 0x0c, 2048 recs/block (`FUN_00637aa0(0xc,0x800)` at `00670ac8`; uti3d\blkarrb.cpp, address-stable) | CONFIRMED |
| +0x14 | BlkArrayB* | polygons, rec 0x74, 2048/block (`00670be5`) | CONFIRMED |
| +0x18 | BlkArrayB* | BVH nodes, rec 0x38, 2048/block (`006745e0`) — built lazily | CONFIRMED |
| +0x1c | DynArray* | query result list, rec 4, grow 0x1e (`0066c0ab`) | CONFIRMED |
| +0x20 | int | A\* search stamp (starts 1; first use clears every poly+0x38) | CONFIRMED |
| +0x24/+0x28 | int | height grid cell size x/y | CONFIRMED |
| +0x2c | int | weight `k` of polygon-size bonus in f (never written → 0) | HIGH |
| +0x30 | plasma* | height grid {i16 maxX, i16 maxY, i16* data} | CONFIRMED |
| +0x34 | void** | detail sub-grid per coarse cell (null in shipped data) | CONFIRMED |

Polygon (0x74 bytes):

| Off | Type | Meaning | Conf |
|---|---|---|---|
| +0x00 | int | id (1-based) | CONFIRMED |
| +0x04 | int | type 0 ground / 1 walkway on model | CONFIRMED |
| +0x08..+0x1c | int[6] | bbox min xyz, max xyz (recomputed from verts in `00663cb0`) | CONFIRMED |
| +0x20 | int | state: 0 open, **1 blocked** (closed gate; never entered, also ignored by lookups), **−1 narrow** (skipped when agent radius ≥ 200) | CONFIRMED |
| +0x24 | int | model/item-res id for walkways | HIGH |
| +0x28 | int | nVerts | CONFIRMED |
| +0x2c | int | stamp for radius-clearance flood (`006718c0`, global counter `DAT_00a57632`) | CONFIRMED |
| +0x30 | Point** | vertex ptrs | CONFIRMED |
| +0x34 | Poly** | neighbour per edge (edge i = v[i]→v[i+1 mod n]) | CONFIRMED |
| +0x38 | int | A\* visit stamp | CONFIRMED |
| +0x3c | Poly* | A\* parent | CONFIRMED |
| +0x40 | Poly** | A\* children array (cap +0x54 short) | CONFIRMED |
| +0x44 | Poly* | open/closed list link; also component-flood link | CONFIRMED |
| +0x48/+0x4c | int | centroid x,y (vertex average, `00664920`) | CONFIRMED |
| +0x50 | i16 | centroid z | CONFIRMED |
| +0x52 | i16 | child count | CONFIRMED |
| +0x57 | u8 | closed flag | CONFIRMED |
| +0x58 | int | g | CONFIRMED |
| +0x5c | int | f | CONFIRMED |
| +0x60 | int | h | CONFIRMED |
| +0x64 | i16 | traversal cost (always 1, set only by loader `00663d40`) | CONFIRMED |
| +0x66 | i16 | max cost along path (primary sort key) | CONFIRMED |
| +0x68 | int | accumulated cost | CONFIRMED |
| +0x6c | PathSpace* / map id | owner | CONFIRMED (loader `00670ca7`) |
| +0x70 | int | connected component id (−1 for blocked) | CONFIRMED (`00675660`) |

Path (embedded in unit at **+0x432** local path and **+0x473** world path; globals `0xa3cae0`, `0xa3cb28` temp):
DynArray of 0x24-byte waypoints (`FUN_005843d0(0x24,0x10,0)` = `0066e4b0`) + start waypoint at +0x1d.
Waypoint = `{float x,y,z; int ax,ay,az; int bx,by,bz}` (pos + portal edge endpoints). Storage is reversed:
`Path_get(i)` (`0066e8d0`): i==0 → start (+0x1d); 1..n → element `n+1-i`; so get(1) is the first portal after the start,
get(n) = destination (`0066e940` = element 1). `0066e8a0` count, `0066e640(start,dest)` reset+append dest,
`0066e720(p)` overwrite destination, `0066e790(a,b)` append portal waypoint at the edge midpoint,
`0066fa00(dx,dy,dz)` translate, `0066faa0(sxy,sz)` scale, `0066e5b0(src)` copy. CONFIRMED.

---

## 2. Path search — `PathSpace_find_path 0066cb40` (thiscall, ret 0x34)

Args: `(float sx,sy,sz, float tx,ty,tz, Path* out, float radius, int startPoly, int maxExpand, float lateral, float width, float unused)`.

1. `startPoly==0` → `PathSpace_find_poly 0066c020(sx,sy,sz,radius)`; if 0 → `0066c710(ftol sx,sy,sz)` nearest poly
   (BVH branch & bound, init best = 0x7fffffff); still 0 → return 0. CONFIRMED.
2. Stamp = pathsp+0x20++ (first call clears all poly+0x38). `out.reset(start, target)` (ints, `ftol`).
3. `ok = 0066ce30(startPoly, 0, start, target, stamp, out, radius, maxExpand, -1, pathsp+0x2c)`.
4. ok → `out.set_dest(target)`; always `0066ece0(out, radius, lateral, width, unused)` (smoothing).
   Returns 1 only if ok and target was integral (`fild ftol(tx)==tx && ty`), else 0. CONFIRMED (`0066cd16..0066cd6c`).

### 2.1 A\* `0066ce30` (cdecl)

- `if radius < 200.0 → radius = 0` (`0066ce30` head; f32 200). Radius>0 makes polys with +0x20==−1 impassable.
- Early out: target inside start poly (`006718c0(tx,ty,0,0)`) → return 1 (path = [dest]).
- Costs (ints): `dist(a,b) = isqrt(dx²+dy²)` (`00590410`→`005903d0`→`00590340` integer sqrt, 32-bit — no overflow guard).
  - start: `g = 0`, `h = dist(target, centroid)`.
  - child: `g = parent.g + dist(child.centroid, parent==start ? startPoint : parent.centroid)`, `h = dist(target, child.centroid)`.
  - `f = g + h − (|bboxW| + |bboxH|) * 4 * k` (`(id%3 != −1) ? 4 : 6`, always 4; `k`=pathsp+0x2c=0 ⇒ **f = g + h**).
  - `max66 = max(parent.max66, child.cost64)`, `acc68 = parent.acc68 + child.cost64`.
- Order (`0066fca0`, `00664b00`): compare `max66` first, then `f` (lower first). Open list = singly linked through +0x44,
  insertion-sorted; ties: new node goes **before** equal nodes (`cmp<=0` → insert in front).
- Neighbour filter: neighbour exists, ≠ parent, `+0x20 != 1`, and (`+0x20 != −1` or radius ≤ 0).
- Re-open: if already stamped and new (max66,f) is better (`cmp<0`): reparent (`00664eb0`, maintains children arrays),
  update g/h/f; if not closed, move in open list; if it has children, propagate new g/f recursively (`0066ddb0`).
- Loop: pop head, `maxExpand--` (100 from all game callers); goal test = target inside popped poly → build path:
  walk parent chain appending each shared portal edge (`verts[i], verts[i+1]` where `neighbor[i]==parent`) → return 1.
- Failure (open empty or budget spent): best = argmin over closed+open of `h + g/16` (`(g + (g>>31 & 15)) >> 4`);
  dest = best centroid, then for each edge of best intersect segment centroid→target (`0066df30`, exact int cross
  products) and keep the crossing with the largest fraction (`0066fc50` 64-bit fraction compare) → dest = that point
  (z linearly interpolated along the edge, `005921a0` = muldiv). Then append portals back to start; return 0.
  CONFIRMED (decomp + disasm `0066ce30`).

### 2.2 Point-in-poly with clearance `Poly_contains 006718c0(x, y, radius, ignoreBlocked)`

- Blocked poly (+0x20==1) → false unless `ignoreBlocked`.
- bbox reject, then for every edge: `cross = (x−x1)(y2−y1) − (y−y1)(x2−x1)`; `cross > 0` → outside (polys are wound so
  the interior has cross ≤ 0). CONFIRMED.
- radius>0: for **wall** edges (no neighbour, or neighbour blocked): if `distSq(point, edge) < r²` → outside, EXCEPT
  axis-aligned edges lying exactly on the local-map border (map record bounds +0x1c/+0x20/+0x28/+0x2c minus origin)
  — map borders are not walls. For **open** edges within r: recursive clearance check into the neighbour
  (`00671cb0`, stamp `DAT_00a57632`). CONFIRMED.
- `PathSpace_find_poly 0066c020(x,y,z,r)`: BVH box query `[x−1,y−1,z−17]..[x+1,y+1,z+17]`
  (16.0 @0x9dbe90 + 1.0 @0x9cf784), then candidates newest-first; accept first with `Poly_contains(x,y,r)` and
  `|ftol(Poly_height(x,y)) − z| ≤ 16`. CONFIRMED.

### 2.3 Smoothing `Path_smooth 0066ece0(radius, lateral, width, unused)`

- `lateral *= 2.5; width *= 2.5` (f32 0x9dbe94).
- For each waypoint i (from 1): portal = (a,b) relative to previous point; shrink both ends by `radius`
  (`0066f820`: only if `|b−a| > 2·radius`, else use midpoint). Funnel sweep over the next ≤8 portals
  (`local_10+8`) narrowing left/right boundaries (cross-product tests); outcome fraction `t` ∈ {0 (left corner),
  0.5 (straight through), 1 (right corner)}. If `width != 0`: `t` re-mapped with `clamp(−lateral/width + 0.5, 0, 1)`
  (`00590b00` clamp) so units keep a formation-lateral offset inside wide portals; if the portal is narrower than
  `width`, `t = clamp(−lateral/width+0.5,0,1)` directly. Waypoint = intersection of (prev → funnel point) with the
  portal edge (`0066f910`), z interpolated along the edge. HIGH (structure CONFIRMED, exact funnel semantics HYPOTHESIS).
- Cleanup: drop consecutive waypoints closer than 0.5 in x and y. CONFIRMED.

### 2.4 Connectivity — `00675660`

Flood fill over open neighbours (skipping +0x20==1) assigns `poly+0x70 = componentId` (0..), blocked → −1. Re-run after
every gate toggle (`006af710`, `006afb10`). Used for fast reachability (HYPOTHESIS: queried via `+0x70` compares in
movement/AI code). CONFIRMED (computation).

### 2.5 BVH — `PathSpace_build_bvh 006745e0 / 006747a0`

Node 0x38: `+0 parent, +4 left, +8 right, +0xc splitAxis, +0x10..+0x18 bbox min, +0x1c..+0x24 bbox max,
+0x28..+0x30 centre, +0x34 leaf Poly*`. Top-down over poly array; split axis = longest extent of poly bbox **centres**;
median via `00674fc0` (quickselect, comparators `006750b0/00675100/00675150` for x/y/z); leaf when 1 poly.
Queries: `006752c0` box-overlap collect; `00675430` nearest (min box distance `00663ad0`, leaf distance `006730d0`).
CONFIRMED.

---

## 3. How units request and follow paths

### 3.1 `Unit_move_to 00505dd0(tx,ty,tz, int maxExpand, bool followLeader, short)` (thiscall unit)

- Returns 0 nothing done / 1 path built (non-empty) / 2 empty path / 3 special (track/no level). (`return 2 − (count!=0)`).
- If `unit+0x3b8`: direct steering (no path), stores dest at +0x3cd/+0x3d1/+0x3d5. CONFIRMED.
- Follow-leader shortcut: if `followLeader` and leader (`FUN_0050ac20`) alive and within 320 units
  (`102400.0` = 320², `0x140`) → copy the leader's path with offset (`006ac050(..., unit+0x432, leader+0x432, ...)`).
  CONFIRMED.
- Track mode (`unit+0x3c9` and level+0x5c): `FUN_004eb760` / `FUN_004eb810` (class 9 / subclass 0xb) move on `.trk`. HIGH.
- Otherwise local mesh path into `unit+0x432` with **radius**:
  - default `BODY_R*2 = 64` (BODY_R `0xa3cc9c`=32, `GAMEPLAY.RES {UNIT}[BODY_R]32`, read `0051c689`),
  - class 8 (cavalry) or subclass 2 or `FUN_00430190` (player/hero mount?): `10 × shapeRadius(unitType)` where shape
    radius from unit-type +0x1c3 (1 sphere → +0x1d3; 2 box → max |+0x1d7|,|+0x1db|,|+0x1e3|,|+0x1e7|; 3 → +0x1fb);
    these units pass `unused=0.5` instead of −1. CONFIRMED (`00506681` fmul 10.0 @0x9cfc20).
  - `lateral = unit+0x426`, `width = unit+0x41e` (formation slot offsets, HYPOTHESIS), `maxExpand` from caller (100).
- Cross-map: if target is not inside the current level (`FUN_0060d5f0` false or level+0x43==0), plan on the world mesh
  (world+0x6a) with all coordinates /32 and radius `BODY_R/32`, into `unit+0x473`, scale back `(32,1)` (`0066faa0`),
  remember start at +0x490. Then find the first..last world waypoint inside the current level and replan that span
  on the local mesh (`0xa3cb28`/`0xa3cae0` temp), splicing it in front of the remaining world path. CONFIRMED (flow).
  Details (disasm 0x5062d4..0x506301, 0x506822..0x50695a): after `Path_scale(32,1)` the world path is translated by
  **(+16,+16,0)** (cell centre; the destination too, so a cross-map route ends at target+16). Start waypoint = ftol(pos/32).
  Run `[c0..c4]` = leading waypoints inside the (loaded) current level, `c4 = c0+1` if only one; the local leg goes
  `get(c0) → get(c4)` (z of the waypoints = 0); unit path = world[1..c0] + local[1..n] + world[c4+1..count−1] + dest
  (dest = local dest when c4 == count). CONFIRMED. Shipped world meshes differ: S2/S3/S5/S6/S7 cover the whole world,
  S4L1w is only 15 doorway polys straddling node borders (6 components) — a far target gives a partial route through the
  nearest doorway, the unit replans locally once in the next level. Port: `NavWorld::find_path` (src/game/multimap_nav.cpp).
- Total path length → `unit+0x40e` (sum of segment lengths `004256d0`). Waypoint index `unit+0x3e6` = 0.

### 3.2 Following — `Unit_update_movement 0052daa0` (per tick)

- Walk vs run: if `WK_DIST (0xa3ccd8 = 50) < unit+0x40e` → action 11 (run) else action 10 (walk)
  (`0052eab5..0052eae4`, `FUN_00501190(0xb|0xa,0,1)`). `HORSE_WD` 500 → `0xa3cbe0` (horses). CONFIRMED.
- Arrival radius `R = REACH_DT (0xa3cc28 = 15)` scaled (`0052ee04..0052ef6a`):
  - infantry: ×1, but ×5 if LOD tier `unit+0x22 > 1`;
  - class 8 / `FUN_00430190`: ×5 if LOD ≤ 1 else ×10;
  - last waypoint: ×2 for class 8 / `FUN_00430190` / LOD>1; task `unit+0x2a5 == 15` (LARGE_WEAPON_PUSH) → ×1.
  - Advance `+0x3e6` while `distSq(unit, wp) ≤ R²`; when it reaches count → path done (clear, wait counters reset).
  - Else steer: heading to waypoint (`00577b90`); if |Δheading| > 3.0 (degrees, HYPOTHESIS) → turn in place
    (`0050abd0`) and zero body velocity (+0x142/+0x146/+0x260). CONFIRMED.
- Blocked handling (wait counters, GAMEPLAY.RES `MAX_W_C 30`→0xa3ccdc, `INIT_W_C 5`→0xa3cd04; units = movement frames):
  - `+0x3fa` current wait length, `+0x3f6` countdown. Normal state: `+0x3f6 == +0x3fa`.
  - Stuck detector: `+0x402` counter incremented while body reports blocked (`004fd3d0()==1`), time `+0x406`;
    if stuck > 500 ms → `+0x3f6--` (enter waiting), `FUN_004360d0`.
  - While waiting: `+0x3f6--`; at 0: if `+0x3f2==0` → **sidestep** `004f7bf0` and set `+0x3f2=1`; else replan;
    `+0x3fa = (+0x3fa+1 ≥ MAX_W_C) ? INIT_W_C : +0x3fa+1`; `+0x3f6 = +0x3fa`. CONFIRMED.
- Sidestep `004f7bf0`: side = `+0x3ee` (0/1 alternate, `+0x3ee = (+0x3ee+1)&1`): angle = heading(+0x40a) ±
  (90° + rand(25)°) [cases 0/1] or ±(90°+rand(45)°) [2/3]; distance `rand(100)+20`; if walkable (`00523e50`) →
  `Unit_move_to(p, 100)`, then path from p to the old destination appended (radius BODY_R, or 3×shape radius for class 8).
  CONFIRMED.

### 3.3 Per-tick cost

One `find_path` = ≤100 polygon expansions + funnel over the path; called only on (re)plan events (orders, wait-count
expiry, sidestep, leader-follow refresh). No global per-tick budget found. HIGH.

---

## 4. Ground height, slopes

- `Poly_height 00664180(x,y)` (map-local): ground poly with grid → `gx = x/cellW, gy = y/cellH` (float), cell
  `ix=ftol(gx), iy=ftol(gy)`; detail sub-grid if present; else `Plasma_sample 004c7b70(gx, gy, diag=((ix+iy)&1)==0)`.
  Walkway poly → plane through verts 0,1,2 (Cramer, int math). CONFIRMED.
- `004c7b70` (triangulated bilinear; clamp ix∈[0,maxX−1], iy∈[0,maxY−1]; fx,fy fractional; h00=(ix,iy), h10=(ix+1,iy),
  h01=(ix,iy+1), h11):
  - `diag==0` (odd cell): `fx+fy > 1 ? h11 + (1−fx)(h01−h11) + (1−fy)(h10−h11) : h00 + fx(h10−h00) + fy(h01−h00)`
  - `diag==1` (even cell): `fy−fx > 0 ? h01 + fx(h11−h01) + (1−fy)(h00−h01) : h10 + fy(h11−h10) + (1−fx)(h00−h10)`
  CONFIRMED.
- Normal `Poly_normal 006645b0`: ground → slope from the grid via `Plasma_sample_bicubic 004c7100(gx,gy,&dx,&dy)`
  (Hermite bicubic using finite-difference derivatives `004c6cd0` d/dx, `004c6e40` d/dy, `004c6fb0` d²/dxdy; exact
  value at grid nodes); gradients /cell size → atan → two rotation matrices applied to up vector `DAT_00a18a60`;
  walkway → plane normal of verts 1,2,3. Height itself uses the planar-triangle `004c7b70`, not the bicubic. CONFIRMED.
- Physics ground query `Phys_ground 006adfd0(outN, outH, ioMap, ioPoly, meshSel, x,y,z, ..., bodyType)` and
  `006ae2a0` (known poly): `h = Poly_height(x − map.originX, y − map.originY)`; normal; then `if (N.z/|N.xy| > 0.0)`
  (`006accf0`, `fcomp 0.0 @0x9cf5c4`, `006ae0db`) normal := (0,0,1) — i.e. **every upward normal is flattened**, so
  contact forces ignore terrain slope (threshold constant is 0.0; a `if(0)` variant at `006ae0c9` is dead).
  No poly → h = 0, or −5000 if world+0xb1c. CONFIRMED.
- Poly tracking `006ae8e0`: try current poly + up to 4 neighbour walks (`00673d00`), accept if poly height ≤ body z
  (+tolerance); else global search `006aae70`; poly switch accepted when |Δh| > 30 handled as a different layer. HIGH.

## 5. Character physics (`006ad660` → `006a45a0`) and collision

Manager = global `0xa570a8` ("charmove"): +0x586 last tick time, +0x58e map table (0x34-byte records, count +0xb0a,
record +8/+0xc origin, +0x10/+0x14 float origin, +0x1c/+0x20/+0x28/+0x2c bounds), +0xb12 enabled, +0xb1c
"void floor −5000" flag. `006ad660` per game tick: dt = now − last (ms); bodies with type (+4) ∈ {2,3,4,5,0x12} →
`006a8060` (projectiles/rigid bodies, §9), others → `006a45a0`; then `006b1350` (sync to actor). CONFIRMED.

Body record (selected; state block A at +0x0c..+0x142, state block B at +0x142.. (+0x136 bytes, rolled back on hit)):

| Off | Meaning | Conf |
|---|---|---|
| +0x04 | body type (0x0b unit … 2,3,4,5,0x12 projectiles; 6/7 mounted/vehicles?; 9..0x13 special) | HIGH |
| +0x08 | actor/scene object (actor+8 = scene type, 0xb = unit; actor+0xc = owner unit) | CONFIRMED |
| +0x142 | velocity xyz (m/s) | HIGH |
| +0x1f6/+0x1fa/+0x1fe | position (metres = units/46.1) | CONFIRMED |
| +0x202/+0x206/+0x20a | position int world units | CONFIRMED |
| +0x20e / +0x212 | current poly id / map id | CONFIRMED |
| +0x216 | i16 on-ground | CONFIRMED |
| +0x218 | ground height (world units) | CONFIRMED |
| +0x21c..+0x234 | timed force #1 (knockback): vec, start, duration(float ms), end, flag — `006b11a0(vec,start,dur)` | CONFIRMED |
| +0x236..+0x24e | timed force #2 (push-aside): `006b12a0(vec,start,dur)` | CONFIRMED |
| +0x250..+0x268 | timed force #3 (contact normal saved, duration +0x260) | HIGH |
| +0x278 | idle step counter; >999 idle steps → body sleeps (skip) | CONFIRMED |
| +0x28a | material: 1/2 → friction static 1.0 / kinetic 0.9, else 0 (`006aa9e0`) | CONFIRMED |
| +0x298 | collided last step; +0x29a collided body id (unit body) | CONFIRMED |

Integrator (`006a45a0`): mass = `unit+0x243` (int) for unit bodies else `006aabb0`; gravity force `m·(−9.81)`
(`0x9dd75c`); friction factor `μ·0.5·N·dt` clamped ≤1 (μ static/kinetic); dt cap per call 20 ms (1 ms for mesh-sel≠0);
step-up threshold **80 units** (`1.7353579750096775 m` = 80/46.1, `0x9dd750`): if new ground differs by more, poly
change refused. Ground snap: if `z < ground` → `z = ground, vz = 0`; on-ground if `z ≤ ground + 0.0015 m`;
airborne flag used by AI when `z ≥ ground/46.1 + 0.5 m` (`0052daa0`). HIGH (constants CONFIRMED, exact
integration order HYPOTHESIS).

Movement collision:
- **Swept shape test** `Phys_sweep 0068de40(body, move, out t)`: AABB of shape grown by `|move| + 270` (250+20)
  queried in scene sectors (`00429090`); tests units in sectors (`0054eec0`, chain unit+0x341) within 2×reach,
  items (`0054f510`, chain item+0xb2; skips item class 8..0xc; only collidable items `004a4050` or res+0x1c2,
  within 1000 units), other objects (`0054f960`). Pair test `006a1bb0` dispatch on shape types (actor+0xb8:
  1 sphere centre +0xbc r +0xc8; 2 box min +0xcc max +0xd8; 3 cylinder centre +0xe4 r +0xf0 h +0xf4; all ×actor
  scale +0x28): sphere–sphere `006a0d80`, sphere–box `00697140` (box space), sphere–cylinder `00699a20`, …;
  relative velocity; contact normal → `DAT_00a69cd8`. When the mover is a projectile actor (actor+8 ∈ 6..9 =
  ThrowItem kinds, §10) a dead/flagged unit (unit+0x38e) is temporarily tested as a cylinder (shape 3). On hit: `+0x298=1`, `+0x29a=other`; if t==0 (already interpenetrating) the moving unit's own shape
  **shrinks by 0.005** per hit (self-resolving overlap). CONFIRMED.
- Scene-geometry test `00692c10`: sector items' collision meshes (`005d4a80` ray/mesh), excluding item classes
  6,7,8,9,0xf,0x10,0x13; eye height uses unitType+0x109/2. HIGH.
- Response: step rolled back (block B → A), velocity zeroed; next step the horizontal velocity component toward the
  other body is removed (`v −= n·(v·n)/|n|²` if `v·n>0`) → **slide around**. CONFIRMED.
- **Unit–unit push-aside** (no positional separation): if the mover is in a move action (state 10,11,14..20 or
  prev-state same) and the blocker is an idle-able unit body (not types 6,7,0xb,0xf,0x10,0x11,0x13, not being pushed),
  the blocker gets `006b12a0(perp, now, frameMs*4)` with `perp` = unit vector perpendicular to (blocker−mover)
  on the side of the mover's motion. CONFIRMED (`006a45a0` ≈ decomp offset 17000-21700).
- **Knockback transfer**: a body under force #1 (|F| = local_294) hitting a unit (subclass 0, not in states
  0x213==1, 0x18, 0x1a, 5, 6) passes the same magnitude along (other−self) for 100 ms; AI (non-player) targets with
  |F| ≥ 6000 or F.z ≥ 3000 → `FUN_00509000` (knock-down), 3000 < |F| < 6000 → action 0x23 (stagger). CONFIRMED.
- Nav-mesh walls: `00671fb0`/`00672500` clip circle movement against wall edges within radius (`00672e40` gives
  edge line or endpoint tangent; map-border edges exempt), called from `006a9570` (walking/camera step). HIGH.

## 6. Line of sight — `Level_segment_blocked 00560d80` (ECX = unit+0x339 level; ret 0x1c)

Args `(float ax,ay,az, float bx,by,bz, int eyeZ)`: `az += eyeZ`; for each quad i in `level+0x74[0..+0x78)` (0x74 B):
`00530f20`: skip if `+0x70==0`; `d = b−a`; plane `n=+0x30, D=+0x3c`; `t = −(n·a + D)/(n·d)`; hit if `0 < t < 1` and
for all 4 edges `dot(cross(edge_k, P − v_k), n) ≥ 0`. Returns 1 = **blocked**. CONFIRMED.
Quad: `v0..v3 +0x00..+0x2c` (vec3 each), `n +0x30`, `D +0x3c`, edges `+0x40,+0x4c,+0x58,+0x64` (v1−v0, v2−v1, v3−v2,
v0−v3), `+0x70` enabled. Pool grows by 8 (`00560c70`).
Sources: `.POR` "Anti-Portal" blocks (`005593e0`/`00559470`): four `(x,y,z)` lines → v0, v1, v3, v2 (2nd pair
swapped), x/y + level origin; also registered as render anti-portals (`005cb880`). Gate quads (§7).
Caller: target search `0054b480` with `a = self pos`, `b = target pos`, `eyeZ = ftol(unitType.height(+0x109) × 0.8)`
(f32 0x9d727c); blocked targets are skipped. CONFIRMED.

## 7. Walls, buildings, gates

- Walls/castles are navmesh holes (no polygons) plus walkway polygons (type 1) on top, linked by models in the PFD
  model table; physics collision via item collision meshes (`00692c10`) and items with `NeedCollision`/`CollisionType`
  (item res, parsed `004a0160` — see §10). HIGH.
- Gate `Gate_set_closed 0048d4b0(bool close)` (i_gate.cpp): closing refused if a class-8 / mount / subclass-2 unit is
  within 300 units (90000 = 300², scan of sectors ±300 around gate). Then `006af710(x,y,z, meshSel(res+0x1e7: 0,1,2→
  −1/..), close, &pathsp)`: find poly under gate (both meshes when sel −1), refuse if any body stands on it
  (`body+0x20e/0x212`), set `poly+0x20 = close` (1 blocked / 0 open), recompute components (`00675660`).
  Rotating/door gates (res+0x1c5 != 0) use `006afb10` with the gate matrix (poly set by OBB). On close: first time
  create one LOS quad per gate-poly edge pair (`00560c70`, count `gate+0xd3`, ids `gate+0xd7`), enable (+0x70=1);
  on open disable. Gate actor body `+0xb8` collision shape set to res+0x1e9 (closed) / 0 (open). CONFIRMED.
- `i_wall.cpp 00487460` is only the wall destructor (frees two child objects +0xd2/+0xda and +0xeb buffer). CONFIRMED.
- Script `set_charmove_detect_block_flag` (FUNC 391, handler `0065f670`) writes byte `DAT_00a5ba64`; the only other
  reference is a reset at `0041cd11` (campaign advance) — **flag is never read** (dead). CONFIRMED.
  `get_charmove_mode` (380) returns `DAT_00a5b9f8` (player control mode; 2 tested in `0052daa0`). CONFIRMED.

## 8. spindex.cpp (0x681ee0–0x683bb0) — editor-time only

Uniform grid of rectangles: header `{int cellSize, width, height, cols=ceil(w/cs), rows=ceil(h/cs), Cell* cells}`
(`00681fe0(w,h,cs)`; 8 bytes/cell), entries in a DynArray (`{.., x1,y1,x2,y2 at +0xc..+0x18, c0,r0,c1,r1 at
+0x1c..+0x28, inline 4-cell link storage / heap when more}`, `00683910`). API: insert rect `006821d0` → id; remove
`006822d0`; move/update `006824a0`; query rect `00682800(out list, x1,y1,x2,y2)` (dedup by first covering cell);
clamp-to-grid `00683110`. Only callers: the navmesh triangulator `00667190` ("Normal finish, %d points, %d path lines,
result in %d triangles") and `0066fb40`; `00667190` and `0066a640` have **no code or data references** (dead editor
code: path lines → triangles). Runtime uses the precomputed `.PFD`. CONFIRMED.

---

## 9. Projectiles

See **Appendix A**.

## 10. Entity limits / containers / scene objects

Merged in full in **Appendix B**. Correction: in `Gate_set_closed 0048d4b0` the per-gate `+0xd3` count / `+0xd7` ids are
**LOS anti-portal quad** indices from `Level_alloc_antiportal 00560c70` (`+0x70` = quad enabled), not path polygons;
the navmesh polygon is blocked separately via `006af710` (§7).

---

## Gaps / UNKNOWN

- Funnel `0066ece0`: exact left/right boundary update rules only paraphrased; port should implement standard SSF with
  the same radius shrink, 8-portal window, t∈{0,.5,1} and lateral/width remap.
- `006a45a0` integration order (force sum → v → p), friction application and the 20 ms cap semantics: HYPOTHESIS.
- Meaning of unit+0x41e/+0x426 (formation lateral/width) and `unused` arg (0.5 / −1).

---

## Appendix A — Projectiles

### Projectiles: arrows, bolts, explosive arrows, siege shots (joan.exe)

Scope: projectile records, THROWI tunables, ballistic solver, launch, per-tick integration, hit tests,
hit → event hand-off, explosions, kegs, siege firing. Damage numbers are CombatRE's (hand-off points only).
Units: world units; physics bodies work in metres (1 m = 46.1 units, const 0x9cf780). Times in ms
(game clock `DAT_00a500ba`, frame delta `DAT_00a500be`; 30 Hz tick ≈ 33 ms).

#### 1. Kinds, item types, body types — CONFIRMED

`Projectile_init 0x507fc0(unit, ammoItemType)` maps the ammo ITEMTYPE code (1-based index into
STD.SET ITEMTYPE) to `proj+0x0c kind`:

| ammo ITEMTYPE (code) | kind | THROWI prefix | body type (+4) | link tag (actor+0x86 +8) | example items (item.res) / shooters (unit.res SRangeWeaponId) |
|---|---|---|---|---|---|
| Normal Arrow 0x0d, Normal Bolt 0x10 | 0 | ARW | 2 | 6 | CROSSBOW_ARROW, Blot03 (player / inventory ammo) |
| Projectile Arrow 0x0e, Projectile Bolt 0x11 | 1 | PROJ | 3 | 7 | ARROW (ENGLISH_ARCHER), blot01 (FRENCH_CROSSBOW_MAN), AUTOBOW_ARROW |
| Explosive Arrow 0x0f, Explosive Bolt 0x12 | 2 | EXPL | 4 | 8 | ENGLISH_EXPLOSIVE_ARROW, HEAVY_CROSSBOW_ARROW (CROSSBOW siege) |
| — (never mapped) | 3 | STM | 5 | 9 | unused: no ammo code maps to 3 |
| Normal Stone 0x14 | 4 | STC | 5 | 9 | TREB_ROCK (TREBUCHET) |
| Cannonball 0x13 | 5 | CAN | 0x12 | 9 | CANNONBALL (CANNON, SIEGE_TOWER), FAUC_BALL, Bombard_BALL |

Evidence: switch at 0x507fc0 (0xd/0x10→0, 0xe/0x11→1, 0xf/0x12→2, 0x13→5, 0x14→4); body type is the
3rd arg of `006adbf0/006af370` (2,3,4,5,5,0x12); link tag written to `actor+0x86 +8` (6,7,8,9,9,9).
The explosive keg is NOT a projectile: it is a world item (class KEG) that explodes when hit (§8).

#### 2. GAMEPLAY.RES {THROWI} — reader `GameplayRes_load_throwi 0x5525b0` — CONFIRMED

Called once from 0x46f350. TL, DL, MXD, MND, TR are parsed with `atoi` (0x7b15cb), M, FC, AV, GL, ZE with
`atof` (0x7b2110). Globals and shipped values:

| key | ARW | PROJ | EXPL | STM | STC | CAN | use |
|---|---|---|---|---|---|---|---|
| TL int (ms) | 7000 @a39d88 | 7000 @a39db8 | 7000 @a39d7c | 8000 @a39d38 | 20000 @a39da8 | 8000 @a39dcc | lifetime (§6) |
| DL int (units) | 3000 @a39cb4 | 3000 @a39db4 | 3000 @a39d3c | 4000 @a39d08 | 20000 @a39d80 | 4000 @a39d70 | max horizontal distance from launch point (§6) |
| M float | 1.0 @a39dac | 1.0 @a39dd4 | 1.0 @a39d50 | 1.0 @a39d44 | 1.0 @a39cb8 | 1.0 @a39dbc | body mass; solver impulse multiplier |
| MXD int→float | 0 @a39d74 | 4000 @a39dc0 | 4000 @a39d0c | 200 @a39d94 | 20000 @a39d58 | 4000 @a39d00 | solver clamps aim point to this 3-D distance |
| MND int→float | 0 @a39d84 | 0 @a39da4 | 0 @a39d1c | 20 @a39d10 | 500 @a39ce4 | 500 @a39d64 | stored (proj+0x42), passed to solver, **never read** (no ref to [ebp+0x2c] in 0x496d40) |
| TR int→float | 0 @a39cc0 | 0 @a39dc4 | 150 @a39d24 | 100 @a39cf4 | 100 @a39d68 | 150 @a39d54 | radius of the hit events posted on impact (§7) |
| FC float | 200 @a39d60 | 30000 @a39cd4 | 30000 @a39cf0 | 15000 @a39dc8 | 30000 @a39cbc | 120000 @a39d28 | launch force: kind 0 always; others only when no target (§4) |
| AV float | 0 @a39d5c | 1800 @a39cd8 | 1800 @a39d18 | 750 @a39d20 | 2150 @a39cec | 3000 @a39cc8 | average speed (units/s) → initial flight-time guess |
| GL float | 0 @a39cfc | 0.5 @a39cdc | 0.5 @a39dd8 | 3 @a39ce0 | 1 @a39d78 | 1 @a39cf8 | gravity multiplier: g = GL·9.81 m/s² |
| ZE float | 20 @a39d8c | 20 @a39ce8 | 20 @a39d34 | 20 @a39d2c | 20 @a39d04 | 20 @a39d6c | only feeds the derived V0/T below |

Derived per kind (computed in the loader, disasm 0x5527a4..0x55283c; constants 2.0@9cf794, 9.81@9d402c,
1.0@9cf784, 46.1@9cf780, 1000.0@9cf778; sqrt = 0x4256d0 → 0x7b2344):
```
h  = ZE/46.1 (m);  g = GL*9.81
if GL == 0:  V0 = 0;  T = TL
else:        V0 = sqrt(2*g*h);  T = ftol((V0 + sqrt(2*g*h + V0*V0)) / GL / 9.81 * 1000)   // ms, truncation
```
Globals: ARW V0 @a39d48 / T @a39d30 (float), PROJ @a39cac / @a39d90 (int), EXPL @a39cd0 / @a39ccc,
STM @a39cb0 / @a39cc4, STC @a39dd0 / @a39db0, CAN @a39d4c / @a39d40.
Shipped: ARW V0 0, T 7000; PROJ/EXPL V0 2.0630 m/s, T 1015; STM 5.0533, 414; STC/CAN 2.9175, 717.
Only consumer: the world look-ahead sweep in 0x693750 (§5.3). STM's pair is never read (switch has no case 3).

`Projectile_load_kind_params 0x498e90` copies the kind's row into the record (+0x32..+0x52, §3).
`Ballistic_solve_by_kind 0x4945c0(out, from, to, kind)` calls the solver with (M, MXD, MND, AV, GL,
minDirZ) where minDirZ = 0 (kind 0, 3), 0.25 (kinds 1, 2, 4), 0.1 (kind 5). Used only by the aim states
0x30/0x32 in 0x7c7ab0 to pick the high/low aim animation (§9).

#### 3. Projectile record (0x6c bytes, `operator new(0x6c)` + memset ctor 0x420110) — CONFIRMED unless noted

| off | type | field | writer / evidence |
|---|---|---|---|
| +0x00 | int | own index in manager array | 0x4ea700 (`*rec = 0x417520()` after add); used by 0x4ea9c0 |
| +0x04 | int | physics body id (`006af310(id)` → body) | 0x507fc0 (006adbf0 or the item actor's existing body +0x5c) |
| +0x08 | int | ammo item instance recno (`0x49ea90(itemType)`) | 0x507fc0 |
| +0x0c | int | kind 0..5 | 0x507fc0 |
| +0x10 | int | particle handle "ARROW" trail (when `DAT_00a57ffb` effects flag) | 0x5133e0 kind 0, 0x4fcc00 kinds 1/2 |
| +0x14 | uint | launch time ms (0 = spawned, not launched) | 0x5133e0 |
| +0x18/+0x1c | float | launch x, y (world) | 0x5133e0 |
| +0x20 | float | launch z, forced 0 (distance check is horizontal) | 0x5133e0 |
| +0x24 | short | dead flag | `Projectile_kill 0x4e6fb0` |
| +0x26 | int | recno of what was hit (unit recno or item instance id) | 0x514c40, 0x4fbc90 |
| +0x2a | int | shooter unit recno | 0x507fc0 |
| +0x2e | int | shooter body id (`unit.actor+0x5c`), ignored by the hit test | 0x507fc0 |
| +0x32 | int | TL ms | 0x498e90 |
| +0x36 | int | DL units; kind 0 only: `DL = ARW_DL * Inventory_get_effect_total(0x10 RANGE_DISTANCE, shooter) / 100` | 0x507fc0 → 0x497e10 |
| +0x3a | float | M | |
| +0x3e | float | MXD | |
| +0x42 | float | MND (unused) | |
| +0x46 | float | TR = event radius | |
| +0x4a | float | FC | |
| +0x4e | float | AV | |
| +0x52 | float | GL | |
| +0x56 | uint | look-ahead window start (now − dt) | 0x693750 |
| +0x5a | uint | next look-ahead time (start + T) | 0x693750 |
| +0x5e | uint | predicted world-hit time | 0x693750 |
| +0x62 | int | predicted world-hit item id | 0x693750 |
| +0x66 | ptr | trail object (operator new 0xaf, ctor 0x687130, `0x687500(TL)`) | 0x5133e0 |
| +0x6a | short | ground explosion already done | 0x4fcc00 |

RANGE_DISTANCE aggregation for case 0x10 starts at 0 and adds `(c+100)*v/100` for each equipped/active
effect (0x497e10), so a bow without a RANGE_DISTANCE effect gives kind-0 arrows DL = 0: the arrow is removed
as soon as it moves horizontally (§6). Code fact CONFIRMED; gameplay impact UNKNOWN (no AI unit uses kind-0
ammo; LONGCBOW 75, RAPIDFBOW 25, LONGBOW 80, RECURVEBOW 50, gem Bkstar 20 carry the effect).

##### Manager `0xa48500` (DynArrayB of record pointers) — HIGH
ctor 0x4e8a50: `0x5856a0(elemSize 4, grow 0x14)`, so no cap. Add 0x5857a0 (reuses free slots).
`ProjectileMgr_tick 0x4913c0` runs every game tick from `Game_process_tick` (call at 0x4e60a9, right before
the event queue 0xa5ccd0 at 0x4e60b3). Remove `0x4ea9c0(idx)`; clear 0x4e9f10; dtor 0x4e95f0;
save 0x482ea0 / load 0x482f70 (from GameFile write 0x474ec0 / read 0x4735c0, records stored whole);
0x44cdd0 removes records whose item has `+0xaa == arg` or lies in a region (HYPOTHESIS);
0x4ead00(nation) kills by nation (HYPOTHESIS); `ProjectileMgr_nearest_incoming 0x467c30(mode, R)` returns
the nearest live projectile item within R of the unit, optionally only from hostile shooters ('E') and from
the unit's current target — used by the AI block reflex (§10).

#### 4. Firing and launch

##### 4.1 Release in the action-animation update `0x7c7ab0` (states 0x33, 0x38) — CONFIRMED
`rem = Unit_anim_time_remaining 0x5045f0()` (ms left in the current action animation). The shot is
released when the nocked display item `unit+0x1bd` is valid and `rem <= threshold`:
- default 650 (0x28a)
- unit-type +0x118 Special CANNON 4, FAUCONNEAU 5, BOMBARD 9: 1667 (0x683)
- TREBUCHET 10: 0 (end of the animation)
- AI unit whose held item `+0x1bf` has item type 0x0c (Normal Crossbow Weapon): 1300 (0x514)

Sequence: AI only: `Unit_ai_compute_shot_dir 0x50aca0` → `ProjectileMgr_spawn 0x4ea700(0xa48500, unit,
ammoType)` where ammoType = inventory ranged ammo record +2 (player) or nocked item +0x50 (AI) →
`ProjectileMgr_launch_for_shooter 0x431480(0xa48500, shooter, item, dir=unit+0x365..0x36d, offset)`
(offset = zero vector, or (5.337, 0, 7.5) for the AI crossbow case) → `0x50a480(0,0,0,0)` →
`0x49f370(unit+0x1bd)` deletes the display item.
0x431480 launches every record of that shooter with launch time 0 (`Projectile_launch 0x5133e0`).

##### 4.2 Aim direction `unit+0x365` (float3, normalized) — CONFIRMED
- AI (0x50aca0): `normalize(target.pos − self.pos)` using unit +0x0e origins (unit target `+0x2db`), or
  `item.pos − self.pos` (item target `+0x2dd`), else actor forward (`actor+0x8c` row 0) with z += 0.15
  for CANNON/FAUCONNEAU.
- Player (0x42ba40, state 0x36 → sets state 0x33): with a locked target (`ctrl+0xbb`):
  `normalize(target.pos + (0,0,60) − cameraEye)`; without one, the camera yaw/pitch forward vector.
  (Target lock 0x42c260 AIM_R/AIM_MXD/AIM_HA: see combat_re/PlayerControl.md.)

##### 4.3 `ProjectileMgr_spawn 0x4ea700` → `Projectile_init 0x507fc0(unit, ammoType)` — CONFIRMED
memset record; +0x2a shooter; +0x2e shooter body; kind; `0x498e90`; item = `0x49ea90(code)` (kinds 0-2:
the inventory ammo code from 0x497580; kinds 3-5: unit-type +0x177 SRangeWeaponId); item made visible
(vtable +0x18(2), +0x1c()); link tag 6/7/8/9 and back-pointer at `actor+0x86 +0x8/+0xc`; body:
`006adbf0(actor, M, bodyType, 0, 2, 0)` if the actor has none, else `006af370(body, M, bodyType, 0, 2, 0)`;
`actor+0x86 +0x5c = body id`.

##### 4.4 `Projectile_launch 0x5133e0(this, item, dir[3], offset[3])` — CONFIRMED (disasm-checked)
Skipped when already launched (+0x14 ≠ 0); killed when the item is invalid or the shooter dead.
- `+0x14 = now`. Spawn point P = world position of the shooter skeleton bone (item `+0x9e` attach id, or
  item-type +0xa; `0x5ba160`/`0x5cf8f0`/`0x60ef80` matrix × zero vector) + offset. CANNON (4) and
  FAUCONNEAU (5) instead add `actorRow0·68.695 (0x428963d7) + actorRow1·12 (0x41400000)` in xy and
  z += 10.122 (muzzle).
- The item is placed at P (`0x41ff50`) and gets the shooter's yaw; the actor collision object is attached
  (`0x60ee60`, `0x5efc50`).
- Launch impulse goes to body +0x21c via `Body_apply_force 0x6b11a0(force, now, durationMs)`:
  - **kind 0**: `force = dir · FC` (200), duration **100 ms**. No gravity (GL 0) → straight line.
    Mass 1 → +20 m/s over the first 100 ms → 922 units/s final speed. No ARR_ACCU scatter.
  - **kinds 1, 2** with a live unit target `shooter+0x2db`: aim point Q = target.pos with
    z += targetType.height(+0x109)·0.8. If |Q − shooter.pos| > 300 (3-D): Q.x += s1, Q.y += s2 with
    `s = Unit_arrow_scatter_offset 0x512130()` = ±Misc_random(ARR_ACCU=50) (sign from random(2):
    0→+, 1→−), or 0 for the player and for unit-type class 8 SIEGE_WEAPON. Solve (§4.5) with
    minDirZ 0.25. Then if distance > 300 and dot(normalized horizontal launch dir, normalized horizontal
    actor forward `actor+0x8c +0x58/+0x5c`) < 0.9 (> 25.8° off-facing): re-aim along the facing at the
    same distance: Q' = S + |Q − S'|·forward where S' = shooter.pos with z += height·0.8, plus a new
    scatter, then solve again.
    With an item target `+0x2dd`: Q = item pos, z += (item has no actor ? itemType+0x26 : actor shape
    +0x40); same scatter (> 300) and re-aim rules.
    With no target: `force = (dir.x, dir.y, dir.z + 0.1) · FC`.
    Duration **1 ms** in every case.
  - **kinds 3, 4, 5**: unit target: Q = pos, z += height·0.8, no scatter, minDirZ 0.1. Item target:
    z += sphere radius (+0x1f9) / box half height ((+0x211 − +0x205)/2) / cylinder half height (+0x225/2)
    by itemType CollisionType +0x1e9, else +0x26 or actor shape/4. No target: force = dir·FC.
    Duration 1 ms.
- `006af370(body, M, type, 0, 2, 1)` (re-arm body), store launch xy, +0x20 = 0; kinds 0-2 also create
  the trail (+0x66, lifetime TL); the player's shot calls `0x497490(1)` (ammo use).
- Shooters whose unit code is not AUTOBOW/AUTOBOW2 turn to the shot yaw (`0x50abd0`, stricmp ≠ 0).

##### 4.5 Ballistic solver `0x496d40(out, S, Q, M, MXD, MND, AV, GL, minDirZ)` — CONFIRMED (disasm)
```
d3 = |Q − S|; if d3 > MXD: Q = S + normalize(Q − S)·MXD; d3 = MXD
T  = d3 / AV                                // s
v(T) = ( (Q−S).x/46.1/T, (Q−S).y/46.1/T, (GL*9.81*T*T/2 + (Q−S).z/46.1)/T )   // m/s
dir  = normalize(v(T))
dh   = horizontal |Q − S|
zmin = dh >= 800 ? minDirZ : dh*1.5/800 − 1.5           // 800@9d72a8, 1.5@9d72a4, −1.5@9d72a0
T = 0.1
while dir.z < zmin: T += 0.2; dir = normalize(v(T))     // 0.2@9cfbb4; lobs the shot
out = v(T_final) * M * 1000                             // 1000 = 0x447a0000
```
Applied as a force for 1 ms on mass M, this gives exactly Δv = v(T). Example (PROJ, flat, dh = 1000):
T0 = 0.556 s → dir.z = 0.035 < 0.25 → T steps 0.3, 0.5 … 1.7 → vz = 4.17 m/s, vx = 12.8 m/s.
With AV = 0 and MXD = 0 (ARW kind 0 via 0x4945c0) T0 = 0/0 → NaN; then T = 0.3 gives v = 0 and the loop
ends → zero vector. This only affects the aim-pose choice (§9). HYPOTHESIS: relies on D3DX normalize(0) = 0.

#### 5. Per-tick physics `Phys_update_projectile 0x6a8060(body, mgr, now, dtMs)` — CONFIRMED
Called from `0x6ad660` (physics manager 0xa570a8) for body types 2,3,4,5,0x12 with dt = now − mgr+0x586,
ECX = mgr+1, 5th stack arg mgr+2 = ECX for the collision calls.

Body fields used (state block S = body+0x142, previous copy at body+0x0c, 0x136 bytes):
| off | field |
|---|---|
| +0x142 | velocity float3 (m/s) |
| +0x14e | float3 (angular, unused by projectiles) |
| +0x1e6 | orientation quaternion float4 |
| +0x1f6 | position float3 (m); previous at +0xc0 |
| +0x216 | short "below terrain / grounded" |
| +0x21c | force float3; +0x228 start ms; +0x22c duration ms (float, counted down); +0x230 end; +0x234 short "force applied" |
| +0x250/+0x260/+0x268 | second "drive to velocity" term (cleared by 0x6b11a0; character physics only) |
| +0x27a | mass |
| +0x298 | short "collided, frozen": function returns 0 at entry when set |
| +0x29a | hit body owner |

Algorithm:
1. Gravity multiplier and substep cap by body type: 2 → ARW_GL, 2 substeps; 3 → PROJ_GL; 4 → EXPL_GL;
   5 → STC_GL (also used for STM); 0x12 → CAN_GL; 5 substeps for 3/4/5/0x12. Types 3/4/5/0x12 return 1
   at once when +0x216 is set (grounded body stops).
2. `if dt > cap: h = min(dt*0.001/cap, 1.0) s, steps = cap; else h = 0.001 s, steps = dt` (1-ms steps).
3. The force starts on the substep matching +0x228 (delay counter `local_10`: 0 when start == last physics
   time `DAT_00a5762e`; steps−1 when start == now; else (start − last − 1)/(h·1000)).
4. Each substep (the last fractional step scales h by the remainder):
   `a = (force active && duration > 0.00015) ? force/mass : 0`; when duration < h·1000 the step is shortened
   to the remaining duration and the rest is carried over; duration −= h·1000; +0x234 = 1.
   `a.z += GL·(−9.81)`. Linear drag −0.3·v only for non-projectile types (types < 2 or > 5, ≠ 0x12).
   `v' = v + a·h;  p' = p + 0.5·(v + v')·h` (trapezoid; damping term local_28 is always 0).
5. Sweep: `disp = (p' − p_prev)·46.1`; if |disp| > 0:
   - `hitBody = 0x68de40(mgr+2, body, p'·46.1, disp, &tU)` (bodies: units, items, third list; PathCollide §9).
     For link tags 6-9 the candidate unit's collider is temporarily forced to cylinder (type 3) for the test.
     Rejected when hitBody == shooter body (+0x2e), or when it is a unit (tag 0xb), the shooter is alive and:
     the target is not hostile (`Unit_is_hostile_to 0x5094d0`: nation relation 0x4b6860, target not dead),
     or the hit body type is 9, or the target state +0x215 ∈ {0x40,0x41,0x42,0x3c,0x29,0x4e} (down/dying
     states). Rejecting clears +0x298 (keeps flying through allies and corpses).
   - `hitItem = Projectile_world_sweep 0x693750(body, p_prev·46.1, disp, &tW)` (§5.3).
   - If hitBody && (!hitItem || tU < tW) && `0x514c40(hitBody)` → return 0 (the body hit wins when closer).
   - Else if hitItem → `0x4fbc90(hitItem)`, return 0. On early return the new state is not stored.
6. Terrain: world = p'·46.1; map object via `0x429090` on world `0xa45f3e`; cell = (x − lvl+0x44)/cell+0x24,
   (y − lvl+0x48)/cell+0x28, parity (cx+cy)&1 → `h = 0x4c7b70(fx, fy, parity even)`. +0x216 = 1, then 0
   when `h <= p'.z`. Off-map (no map object) stays 1, i.e. counted as grounded.
7. Store v', p', orientation; return 1.

No air drag, no terminal velocity, no wind on projectiles. CONFIRMED.

##### 5.3 World-object sweep `0x693750` — HIGH CONFIDENCE (structure; narrowphase internals unread)
Candidates: scene objects of the cells overlapping the segment (`world+0x6e`, 0x5ef610/0x60dfd0/0x5effa0,
next = obj[8]), obj[2] == 1 (item), not the projectile's own actor, item type +0x50 ∉ {8..12} (carried
weapons), item class +0x4e ∉ {7 POTION, 8 GOLD, 9 ENHANCER, 0xf MISSION, 0x13 CROSS}, itemType+0x1e3 == 0.
- Primitive colliders (obj[0x24] type byte == 7): transformed into object space, tested by shape obj[0x2e]:
  1 sphere 0x6a0f90, 2 box 0x698970 (params obj+0xcc), 3 cylinder 0x699ec0 (obj+0xe4). A hit is returned
  only when proj+0x26 ≠ 0 and `now + dt·(t−1) < proj+0x5e` (CONFIRMED disasm 0x693dba..0x693e34). For a
  fresh projectile (+0x26 == 0) primitive colliders never stop it. Meaning HYPOTHESIS.
- Mesh colliders (type ≠ 7) are tested only through a **look-ahead** ray, refreshed at most every T ms:
  when no prediction is cached or it expired, and now ≥ +0x5a and the previous velocity ≠ 0:
  `start = now − dt; +0x56 = start; +0x5a = start + T; end = p + T·(vx, vy, vz − V0)·46.1/1000`
  (T, V0 from §2: ARW 7000 ms / 0, PROJ/EXPL 1015 / 2.063, STC/CAN 717 / 2.9175). Ray vs triangles
  (0x60e3f0, 0x5d4a80); a triangle counts when its material (`… +0x6c` table, stride 0x54, +0x38) ≠ 1;
  nearest t kept (t ≤ 0.001 → immediate hit, t = 0). `+0x5e = start + ftol(T·t)`, `+0x62 = item id`.
  The hit fires on the tick where `+0x5e` falls in (now − dt, now], with t = 1 − (now − +0x5e)/dt.
  Arrows (GL 0) fly straight, so one 7-s ray is exact for them; ballistic shots use a straight
  approximation that is re-cast every T ms.

#### 6. Lifetime and removal — CONFIRMED
`ProjectileMgr_tick 0x4913c0`, for each launched (+0x14 ≠ 0), not-dead record:
- item invalid or body missing → remove.
- horizontal distance from (item pos, z = 0) to (+0x18, +0x1c, 0) > DL → remove.
- `+0x26 == 0 && (body+0x298 || +0x14 + TL < now)` → remove (stopped on a rejected hit, or timed out).
- otherwise `Projectile_update 0x4fcc00`:
  - kinds 1/2 in flight with force applied: item yaw/pitch follow velocity (`atan2`, `asin(−dir.z)`);
    grounded or frozen → trail stop 0x687850; create/move the "ARROW" particle.
  - **kind 2 on ground** (+0x216, once, +0x6a = 1): events `0x2000` to units-in-radius and items-in-radius
    with **radius 150.0 (0x43160000, hard-coded, not TR)**, source = shooter; particle
    EXPLOSIVEHIT1/2/3 = Misc_random(3).
  - **kinds 3/4 on ground**: event `0x40` to units-in-radius, radius 150.0; particle CANNONHL.
  - **kind 5 on ground**: nothing (disasm 0x4fd158..0x4fd165 checks only 3 and 4); removed silently.
  - grounded, or +0x26 ≠ 0, or frozen → free trail, delete item (0x49f370), kill.
Kinds 0/1 hitting terrain just disappear: no sticking, no ricochet. **ARW_ATH (0xa3cc40) and ARW_TIME
(0xa3cc68) are read by 0x51c680 and never referenced anywhere else** (literal-address scan of the whole
disasm) → arrows never stick in bodies. CONFIRMED.

#### 7. Hit handlers → event queue (damage hand-off) — CONFIRMED (push order from disasm)

`Event_post 0x4eef00(queue 0xa5ccd0, type, tgtKind, tgtId, srcKind, srcId, flag, pos[3], radius, b, t0, t1, b2, p15, p16)`
allocates a 0x43-byte event (`Event_init 0x47e200`): +0 type, +4 tgtKind (1 unit, 2 item, 4 …), +8 short
tgtId (0 / ≤0 = area), +0xa srcKind (1 unit, 2 item), +0xe short srcId, +0x12 byte, +0x13 pos, +0x1f float
radius, +0x23 byte, +0x26 deliver time, +0x2a time, +0x2e byte, +0x10 short p16. Types are unit-type
TriggerType bits: 2 ARROW, 0x40 STONE, 0x2000 EXPLOSIVE_ARROW, 0x4000 EXPLOSIVE_KEG.
Projectile posts always use srcKind 1, srcId = shooter, flag 1, b 0, t0 = t1 = now, b2 1, radius = TR.

`Projectile_on_body_hit 0x514c40(hitBody)` (ECX = projectile):
- unit (link 0xb/0xc): +0x26 = unit recno. **Height gate**: continue only if projectile z <
  unit.z + unitType.height(+0x109), else nothing more happens and the manager removes it. If unit+0x1e6 ≠ 0
  (HYPOTHESIS: rider/mount) +0x26 is redirected to it.
  - kinds 0/1: `(2, unit, +0x26, pos = unit origin, R = TR = 0)`.
  - kind 2: particle EXPLOSIVEHIT1-3 at the projectile actor; `(0x2000, unit, 0 area)`,
    `(0x2000, item, 0 area)`, `(0x2000, item, +0x26)` (item kind with a unit id, a code quirk), R = 150.
  - kinds 3-5: particle CANNONHL; `(0x40, unit, 0 area)`, `(0x40, item, 0 area)`, R = TR (STC 100,
    CAN 150). No direct event: the struck unit is only hit through the area test.
- item (link 1): +0x26 = item +4. Kinds 0/1: `(2, item, item+6, pos = item+0x56)`. Kind 2: `(0x2000, item, item+6)`,
  item area, unit area. Kinds 3-5: particle CANNONHH when itemType+0x1c ∈ {1 GATE, 2 OBSTACLE}, else
  CANNONHM; `(0x40, item, item+6)`, unit area, item area.
- then kill (0x4e6fb0). Returns 1.
`Projectile_on_world_hit 0x4fbc90(itemId)`: same as the item branch (+0x26 = itemId), then kill.

Event processing `EventQueue_process 0x4ef090` (tick, after projectiles), events with deliver time ≤ now:
- tgtKind 2, id ≤ 0: every item where `0x41c5e0(pos, (int)R)` → item vtable +0x48(event); id > 0: that item.
- tgtKind 1, id > 0: the unit, when `unitType.TriggerType(+0x11c) & type`; → `Unit_on_event 0x75d540`.
- tgtKind 1, id ≤ 0 (area): every unit except the source unit, filtered by TriggerType, then
  - non-explosive types: `|unit.pos − pos|² ≤ R²` (sphere);
  - 0x2000/0x4000/0x40: unitType CollisionType +0x1c3 == 2 (Box3d, e.g. CANNON): AABB of the box in unit
    space vs pos ± R (0x60d6b0); otherwise cylinder: `dx² + dy² ≤ R²` and `|dz| ≤ R`.
  No distance falloff here: every unit inside gets the same event.

Hand-off in `Unit_on_event 0x75d540` (CombatRE owns the numbers): for type 2/0x2000 with an inventory,
`ARROW_DEFLECT_OFF` (effect id 10, 0x4a45f0) chance = `Inventory_get_effect_total(10)`; when
`Misc_random(100) < chance` the hit is ignored. Then `Unit_take_ranged_hit 0x5051e0(src, type, ignoreDown)`:
returns 0 when the target is in state 0x2a or dead, is the source, is in 0x1e-0x21/0x53, or (if
!ignoreDown) is down (0x40-0x42, 0x4e, 0x29), or has +0x1dd > 0, or the source is non-hostile.
**Shield block**: target state 0x2b/0x2d and `|bearing(src)| ≤ BLOCKANG (70°)` (0x505960) → "DEFEND"
sound/anim, particle GUARDARR, state 0x2d, damage 0. Otherwise the bearing bonus is 0 (< 46°), 20
(< 136°) or 40 (back) → `0x509e90(src, type, bonus)` = ranged damage with FALL_S/FALL_E/DIST_P/SIE_UDAM
(CombatRE). Explosions call it with ignoreDown = 1.
Reaction: 0x40/0x2000 on a unit with +0x1e2 == 0 → `0x509000(0)` (knockdown); otherwise hit anim by
bearing (0x23 front, 0x27 left, 0x28 right, 0x26 back); ARROW/STONE damage also does
`fallGauge(+0x1cb) −= ARR_FALL (25)` (0x4fea80). Units whose code is not AUTOBOW get a knockback force
via 0x6b11a0 for 100 ms: horizontal (cos, sin)(bearing)·3000, z 500 (0x43fa0000), ×3.0 in state 0x4e (×2.5 on
the arrow path), ×0.7 in state 0x29. HIGH CONFIDENCE.

#### 8. Explosive kegs — HIGH CONFIDENCE
`Keg_on_event 0x48fa50` (KEG item event handler, vtable +0x48, ECX = item): reacts to 0x2000 from a unit
(srcKind 1) and to 0x4000 from an item (srcKind 2, chain reaction). With damage stage `item+0x52 ≠ 0` it
reads the action byte `itemType+0x96[stage]`: 0 → stage−1 (vtable +0x18), play stage anim
(`0x5abe30`, anim `itemType+0x87[stage]`), post `(0x4000, item area)` and `(0x4000, unit area)` from the keg
(srcKind 2, srcId = keg, pos = item+0x56, **R = 200.0 (0x43480000)**); 1 → same without the anim;
2 → stage−1 and anim only. For 0x4000 it also spawns the effects `itemType+0x17a/+0x17e` (5000 ms).
The 0x4000 unit event lands in 0x75d540 (srcKind 2, keg valid, 0x505650 damage) → knockdown 0x509000
or state 0x4e.

#### 9. Aim states and siege AI hooks
- 0x7c7ab0 state 0x30 (kind 1 params) / 0x32 (kind 0) run `0x4945c0` toward the target for the aim pose:
  at the end of the animation, state 0x36 when dir.z < 0.1, else 0x39. HIGH CONFIDENCE.
- AI script class ids `UnitAI_script_class_id 0x546520`: human_cannon → 8 (`0x533170`), human_trebuchet
  → 0xe (`0x533920`), autobow → 9; task bodies (0x543800/0x543fd0/…) belong to the AI task notes.
  Both run "WEAKEST" target selection, then the task switch; firing itself is §4.1. CONFIRMED (dispatch only).
- `des/cannon.txt`, `des/trebuchet.txt` (with battering_ram/crossbow/bombard/fauconneau .txt and
  siege_help.txt) are plain help text shown by the siege workshop menu `0x73a320` (objmenuw.cpp).
  No gameplay data. CONFIRMED.

#### 10. AI block reflex (BLOCKPER) — CONFIRMED
`Ai_block_chance 0x512070` = ftol(BLOCKPER(50) × {EASY_A 0.5, NORM_A 1.0, DIFF_A 1.5, VDIFF_A 2.0}
[difficulty DAT_00a57fc2]) → 25/50/75/100. AI task bodies (0x538c90 and 15 siblings): when
`0x467c30('E', R)` finds an incoming enemy projectile, the unit is not already blocking (0x510f80) and
`Misc_random(100) ≤ chance`, push face-task 10 (0x653000, 2500) and defend task 4 for 2000 ms. The block
is then resolved by BLOCKANG in §7.

#### Reimplementation sketch (kinds 1-5, unit target)
```
Q = tgt.pos + (0,0,tgtType.height*0.8) [+ (±rnd(ARR_ACCU), ±rnd(ARR_ACCU), 0) if kind<=2 && |Q-S|>300 && !player && cls!=8]
v = solve(S, Q, MXD, AV, GL, minDirZ)          // §4.5
body.v = v (applied in the first 1-ms substep); each tick: dt = now-last; integrate (§5) with g = GL*9.81
hit order per substep: bodies (cylinder vs units) < world look-ahead < terrain
remove when: dist_h > DL, now > launch+TL (no hit yet), grounded, hit
```

#### Gaps
- Narrowphase shapes (0x6a1bb0, 0x6a0f90, 0x698970, 0x699ec0, 0x5d4a80) are PathCollide's; not re-read.
- `unit+0x1e6` redirect meaning (rider/mount) HYPOTHESIS; body type 9 skip meaning UNKNOWN.
- Damage values (0x509e90, 0x505650, 0x509000) are CombatRE's.
- The primitive-collider `+0x26` gate (§5.3) looks unintended; runtime effect not verified.

---

## Appendix B — Entity limits / containers

### joan.exe — entity containers, limits, scene sectors, item collision properties

Scope: containers/caps for units, items, projectiles, particles/effects, sounds; DynArray/DynArrayB/BlkArrayB;
level/scene-sector partition and how units/items are registered; item-res properties (EnlargeSceneSector,
NeedCollision, CollisionType, notBlock, HasPath, …); actor ("scene object") type ids.
All offsets are byte offsets. Coordinates: x,y = ground plane, z = height (CONFIRMED by terrain z = corner heights, 0x4f6eb0).

#### 0. Hard caps — summary

- **No hard entity caps with error strings exist.** strings.tsv contains no "too many/exceed/overflow/limit/max"
  messages for game entities (only libpng/jpeg/zlib/CRT and script names like `set_game_time_limit`). CONFIRMED (grep).
- Every game container is a growable heap array (DynArray/DynArrayB/BlkArrayB or hand-rolled realloc). CONFIRMED.
- Implicit caps (HIGH CONFIDENCE): unit and item record numbers are stored as `short` (unit+0x00, item+0x04, unit
  target recno +0x2d7, hand items +0x1bd/+0x1bf, UnitArray cursors +0x40..), so >32767 live slots would break.
  Sound3D source count is a `short` (+0x24) grown by 32 (0x4907e0); real cap = OpenAL `alGenSources` failure (returns NULL).
- Time-slicing (not a cap): UnitArray tick `0x51e280` processes LOD tier 0–1 units every tick, tiers 2/≥3 in
  round-robin batches (cursors UnitArray+0x40/+0x42/+0x44 short, batch sizes +0x58/+0x5a/+0x5c..+0x62 short
  recomputed each tick as `max(n*1.0/elapsed, K)` with K = 50.0 (0x9cfc18) for +0x58/+0x5a, 1.0 for +0x5c,
  10.0 (0x9cfc20) for +0x5e/+0x60/+0x62; disasm 0x51e6b9..0x51ee68). HYPOTHESIS on the exact meaning of
  n/elapsed (cpuusage timer 0xa485a8 `0x45e750/0x45e7f0`); AI agents own LOD semantics.

#### 1. DynArray (ilib\dynarr.cpp) — CONFIRMED

Ctor `0x5843d0(int recSize, int growBy, int initCap)`; `initCap==0` → `growBy`. Body = `Mem_add(recSize*cap)`.

| off | type | field |
|---|---|---|
| +0x00 | int | capacity (records allocated) |
| +0x04 | int | growBy (block) |
| +0x08 | int | cur recno (1-based cursor; set by add/find) |
| +0x0c | int | size (last recno) — `DynArray_size 0x417ad0` |
| +0x10 | int | recSize |
| +0x14 | int | sort key offset (`0x585250`) , init −1 |
| +0x18 | u8 | sort key type 1=int 2=short 3=char 4=char* strcmp 5=inline str (`0x585280`) |
| +0x19 | void* | body (packed, unaligned field) |
| size 0x1d | | `File_write(this,0x1d)` in save `0x585100` |

- add `0x584620(src)`: `size++; cur=size; if cap<size → realloc(cap+growBy)`; copy recSize bytes (or zero 1st byte if src NULL).
- insert-before-cur `0x584790`, insert-at `0x5848d0(recno,src)` (memmove up).
- delete `0x584a10(recno)` (recno<0 → cur): memmove down, `size--`; **shrinks** when `size < cap − 2*growBy` → realloc(cap−growBy).
- zap `0x584580(shrink)`: size=cur=0, optionally realloc to growBy. clean-with-free `0x584f70`.
- get address = `body + (recno-1)*recSize` (`0x4b9c30`, generic); sort `0x5850b0` (qsort), linear find `0x5846f0/0x584c80/0x584d60`.

#### 2. DynArrayB (ilib\dynarrb.cpp) — slot-stable array with free list — CONFIRMED

Ctor `0x5856a0(recSize, growBy, userParam, initCap)` → DynArray ctor + fields:

| off | type | field |
|---|---|---|
| +0x1d | int[2]* | free-recno stack (8-byte entries, first dword = recno) |
| +0x21 | int | free-stack capacity (grows by DynArray.growBy, `0x5858c0`) |
| +0x25 | int | free-stack count |
| +0x29 | int | userParam (ctor arg 3; always 0 in game ctors) |
| +0x2d | u8 | 0 |

- add `0x5857a0(src)`: if free count>0 → **reuse most recently deleted recno (LIFO)**, else `size++`. Grows like DynArray. Copies recSize bytes.
- delete `0x5858c0(recno)`: push recno on free stack, **memset record to 0** (size is not decreased). So a deleted
  pointer slot reads NULL → `is_deleted(recno) = get_ptr(recno)==0` (`0x428b70`, `0x425d50`, `0x561320`).
- `DynArrayB_get_ptr 0x41b3f0(recno)`: `recno<1||recno>size ? 0 : *(void**)(body+(recno-1)*recSize)` — i.e. the
  arrays below store **pointers** (recSize 4). Iteration idiom everywhere: `for r=size..1 if get_ptr(r)`.
- clean `0x585d20` (zap + free count=0); save `0x5859d0`/`0x585d50` (writes 0/1 presence + object), load `0x585a60`.
- recno→compacted index `0x585cb0`.

#### 3. BlkArrayB (uti3d\blkarrb.cpp) — chunked, address-stable (used by path meshes) — CONFIRMED

Note: `0x637aa0` is **not** DynArrayB; it is `BlkArrayB(recSize, blockRecs)` (base ctor `0x637520`).

| off | field |
|---|---|
| +0x00 | block pointer table; +0x04 table capacity (grows nblocks+15) |
| +0x08 | recSize; +0x0c records per block |
| +0x10 | internal record count; +0x14 block count |
| +0x18 | free list (int*), +0x1c free count, +0x20 free cap |
| +0x24 | id→internal index table (int*), +0x28 cap, +0x2c count (B-layer) |

alloc internal `0x637700` (free-list first), address `0x637920(i)= blocks[(i-1)/per][(i-1)%per]`, add `0x637be0`, add-n `0x637cb0`, reset `0x637bb0`, dtor `0x637b00`.

#### 4. Global containers (static ctors `0x569xxx–0x56cxxx`, names from load strings in `0x470d40`/`0x46f3a0`)

| global | name (load string) | kind | recSize, growBy (disasm push) | element object | add / getter |
|---|---|---|---|---|---|
| 0xa47c08 | unit_array | DynArrayB | 4, 0x14 (`0x51dbf7`) | Unit 0x4b4, ctor 0x4155e0 | add 0x51dfc0, get 0x41b3c0, del 0x51e1f0 |
| 0xa460a0 | item_array (world items) | DynArrayB | 4, 0x14 (`0x49e8ea`) | Item, size by class (§6) | add 0x49ea90, get 0x425d20, deleted? 0x425d50 |
| 0xa48500 | throw_item_array (projectiles) | DynArrayB | 4, 0x14 (`0x4e8a67`) | ThrowItem 0x6c | add 0x4ea700 (Projectiles agent owns) |
| 0xa570a8 | physis_sys (physics bodies) | DynArrayB | 4, 5 (`0x6ad0e9`) | body created by 0x6adbf0 | get 0x6af310 |
| 0xa57078 | animation_particle_effect_array | DynArrayB | 4, 0x14 (`0x41b487`) | effect 0xa3 (terrain fx none/mud/snow) | add 0x41baf0/0x41b640, get 0x41bd60, deleted? 0x41bd90 |
| 0xa527a8 | particle_emitter_res | — | — | — | load 0x4c19a0 |
| 0xa45f08 | world_array | DynArrayB | 4, 0x1e (`0x560fa7`) | World 0x92, ctor 0x41ea20 | add 0x5611b0, get 0x420240, set current 0x561350 |
| 0xa484d0 | place_array | DynArrayB | 4, 0x1e | Place 0x1f6 | get 0x456c70 |
| 0xa5d100 | facility_array | DynArrayB | 4, 0x1e | polymorphic 0x140/0x8e/0x99/0x2f14/0x79 (`0x480ce0`) | get 0x43c450 |
| 0xa52940 | territory_array | DynArray | 4, 0x1e | Territory 0xe0 | get 0x420ff0 |
| 0xa527c8 | water_array | DynArrayB | 4, 0x14 | Water 0x34 | get 0x469d10 |
| 0xa48aa8 | water_wave_array | DynArrayB | 4, 0x14 | 0x18b/0x2d8 | load 0x56ed40 |
| 0xa57f80 | nation_array | DynArrayB | 4, 0x14 | Nation 0x1545 | get 0x47b600 |
| 0xa3ce90 | glowing_light_array | DynArrayB | 4, 0x14 | 0x55 | |
| 0xa506e0 | modify_light_array | DynArrayB | 4, 0x14 | 0x24 | |
| 0xa50530 | modify_item_array | DynArrayB | 4, 0x14 | | get 0x4853e0 |
| 0xa5ccd0 | trigger_system | DynArray | 4, 0x14 | Trigger 0x43 | get 0x4eef00 |
| 0xa48f08 | talk_res | DynArrayB | 0x17 inline, 0x64 | | |
| 0xa50020 | news_array | DynArray | 0x52 inline, 0x64 | | |
| 0xa55a88 | message array | DynArrayB | 4, 5 | 0x5b | |
| 0xa53c28 | msg log | DynArrayB | 4, 5 | 0x1f | |
| 0xa47ca8 | profile_array | DynArrayB | 4, 0x14 | 0x48 | |
| 0xa549e8 | game_file_array | DynArray | 4, 0xa | | |
| 0xa58cb8 | animation_res | DynArray | 0x130 inline, 5 | | get 0x4174f0 |
| 0xa50460 | unit_script_res | DynArrayB | 0x10c inline, 0x1e | | |
| 0xa56a68 | bone_name_res | DynArray | 0x38 inline, 0x1e | | |
| 0xa50298 | weather effects ("Thunder", rain.txt) | DynArray | 4, 0xa | | get 0x41bf10 |
| 0xa58f50 | item_res | custom | rec 0x2bf (`0x49fbb0`) | ItemRes | get 0x425ce0, by name 0x4a36c0 |
| 0xa3cae0, 0xa3cb28 | temp waypoint lists (unit path) | DynArray | 0x24, 0x10 (`0x66e4b0`) | | users 0x505dd0; each Unit also owns two (0x4155e0) |

Unit array specifics (CONFIRMED unless noted):
- UnitArray object = DynArrayB + `+0x2e short` (selected/followed unit recno; cleared when that unit deleted, `0x51e1f0`),
  `+0x30 u8[8]`, `+0x38 u8[8]` zeroed (ctor `0x51dbe0`), `+0x40/+0x42/+0x44` round-robin cursors, `+0x58..+0x62` batch sizes (§0).
- `UnitArray_add 0x51dfc0(unitTypeId)`: `new Unit(0x4b4)`; if `DAT_00a58f95` (campaign carry-over flag, set 0x52fee5/0x720f6e,
  cleared 0x70bcf8) and the current profile holds a stored unit for this type (`0x4ca290`, `0x4c91c0`) the stored hero is
  re-added instead; else `Unit_init 0x4f8550(type)`, `DynArrayB add`, `unit+0x00 = (short)recno`. Returns recno. HIGH.
- delete `0x51e1f0(recno)`: virtual dtor (`0x471530(1)`) then DynArrayB delete (slot reused LIFO).
- clear-all `0x51df00` iterates size..1.
- Find by unit-type `0x51fd50(type)` (unit+0x3b), by name `0x51fdd0(name)` (unit+0x3d strcmp), by +0x31 `0x51fe60`.
- Tick `0x51e280` only processes units with `unit+0x335 == DAT_00a45f3e` (current World).

#### 5. World → Level → SceneSector partition

##### World (0x92 bytes, ctor 0x41ea20; WorldArray 0xa45f08 +0x32 current recno, +0x36 current World* = `DAT_00a45f3e`) — HIGH
| off | field | evidence |
|---|---|---|
| +0x02 | short recno | 0x5611b0 |
| +0x6e | Scene* (en3d) — all levels' sectors live in this one scene | 0x42b31e |
| +0x72 | int level count | 0x547bf0 |
| +0x76 | int level capacity (grow to count+n+16) | 0x547bf0 |
| +0x7a | Level** | 0x547bf0 |
| +0x7e | level AABB tree, `2n−1` nodes × 0x38, built lazily | 0x44a260 / 0x548880 |
| +0x82 | Level** query result buffer | 0x429090 |

- `World_add_levels 0x547bf0(n)`: `new Level(0xac)` ctor 0x41b5c0, `level+0 = World*`.
- `World_query_levels 0x429090(Box3d*, int* count)` → Level** (tree walk `0x54ad90`).
- `World_locate 0x54a2c0(Level** lvl, SceneSector** sec, D3DVECTOR* pos)` (ecx = World): point box; fast path if
  current `sec->exact` contains pos → 1; else query levels at pos, try current level first, then others; first sector
  whose **exact box (sector+0x20)** contains pos wins → returns 2 and writes lvl/sec; none → `*lvl = first hit level`, returns 0.
  Containment test `0x60d5f0` is inclusive (`min<=p<=max` on x,y,z). CONFIRMED (disasm 0x54a2f1, 0x54a3e9).

##### Level ("map piece" = unit+0x339 / item+0xaa / place+0x3a; 0xac bytes) — HIGH
| off | field |
|---|---|
| +0x00 | World* |
| +0x08 | Box3d (6 floats) — used by World_locate for "is pos still in current level" (0x54a345) |
| +0x43 | u8 load state: 0 unloaded, 1 loaded, 2 marked-to-load, 3 keep (0x547630) |
| +0x44/+0x48 | int origin x,y (all map coords are relative) |
| +0x4c/+0x4e | short, read first by map header 0x42c6b0 |
| +0x50 | int (≥1) terrain grid param (0x4f6eb0) |
| +0x54, +0x58 | path meshes (PathSpace; loaded by 0x670980) |
| +0x60 | camera-related object (0x44c887) |
| +0x64 | int scene-sector count |
| +0x68 | int capacity (grows +0x100) |
| +0x6c | SceneSector** |
| +0x70 | char[n][0x21] sector names |
| +0x80/+0x84 | texture-name count / char[n][0x20] (terrain .GRI) |
| +0x88 | short*[] per-column terrain texture index grid |
| +0x8c/+0x90 | unit list tail/head (link unit+0x33d prev, +0x341 next) |
| +0x94/+0x98 | item list tail/head (link item+0xae prev, +0xb2 next) |
| +0x9c/+0xa0 | place list tail/head (link place+0x3e prev, +0x42 next) |

List insert at head (newest): `Level_add_unit 0x437770(unit)` (ecx = new level, unlinks from old), `Level_add_item 0x43b190`,
`Level_add_place 0x42c140`; unlink-only `0x54f110` (item), `0x54f580` (place); clear `0x54f290`, `0x54f700`;
heads `0x54eec0` (+0x90 units), `0x54f510` (+0x98 items), `0x54f890` (+0x9c places); next `0x54ef40`(unit+0x341), `0x54f530`(item+0xb2). CONFIRMED.

Level streaming ("World activate sectors"; `World_draw 0x44c800`): box = camera pos ± 8000.0 (0x9cf7a4) inner and ± 16000.0
(0x9cf7a0) outer, all three axes (CONFIRMED disasm 0x44c908..0x44c9a1). `0x5479a0(inner,outer,1)` returns 1 if any
inner-box level has state 0 or any loaded level is outside the outer box. `0x547630`: inner levels 0→2, outer levels 1→3;
levels still 1 (outside outer) → unload `0x42ac40`; 2 → state 0 then load `0x4329e0` (→ `Level_load 0x4f2f80`, reads
MAP\*.res/.PFD/m*.PFD/.trk/.cm/_STG/.GRI/.PLN); 3 → 1. Optional `free_world_video_memory.sys` file sets a threshold
(0xa0bbf8) for VRAM-pressure unload `0x4457e0`. CONFIRMED.

##### Scene sector creation — CONFIRMED
- Map text keyword `CreateSceneSector` (parsed in `0x42c6b0`, which first reads two shorts into level+0x4c/+0x4e) →
  `Level_parse_scene_sector 0x551440(file, create=1)`: tokens `name` (last char stripped, ≤0x40), then
  `minX, maxX, minY, maxY, minZ, maxZ` (each `label value`, ints; X/Y += level origin), then 6 ignored fields.
  `Level_new_sector 0x42b270` → `Scene_create_sector 0x5ea460` (scene = level->world->+0x6e).
  - `sector+0x08` render/cull box = exact box with X,Y expanded by **128.0** (0x9d7c50), Z unchanged (`SceneSector_set_box 0x5ef880`, also clears +0x38/+0x3c/+0x40, +0xde=0, +0xdf=1).
  - `sector+0x20` exact (membership) box (6 floats) copied raw.
  - name stored in level+0x70 (`0x41baa0`, strncpy 0x20); lookup by name `Level_find_sector 0x54e6a0`.
- Scene (en3d): `+0x24` sector count, `+0x28` capacity (grows count+8), `+0x2c` SceneSector**, `+0xc9` parallel array,
  `+0x30` sector tree root (invalidated by `0x5eb030`, rebuilt lazily = `Scene::build_sector_struct_recur`),
  `+0xcd/+0xd1` translucent-actor sort buffer (grows +0x80). Box query `Scene_query_sectors 0x5ef610(Box3d*, int* n)`.
- SceneSector (0xe4 bytes, ctor 0x5ef800 memset): `+0x00 Scene*`, `+0x08 Box3d render`, `+0x20 Box3d exact`,
  `+0x38/+0x3c/+0x40` owned plane/geometry objects, `+0x5a` 0x10×6 sub-array, `+0xba..+0xc6` children, `+0xca` actor list
  first (oldest), `+0xce` actor list last (newest), `+0xd2/+0xda` morph list, `+0xde/+0xdf` flags.
- Terrain patches (`Level_load_terrain 0x4f6eb0`, file grid cells 0x54 bytes, 64 world units per cell):
  for each cell with flag byte[0]≠0: allocate one 0x66-byte TerrainPatch from global pool (`0x54e930(n)`: `DAT_00a3ce38`
  ptr array, `DAT_00a3ce30` count, `DAT_00a3ce34` capacity grows ×4; get `0x54eb20`), box
  `x0 = origin.x + i*64`, `y0 = origin.y + j*64`, `x1 = x0+64`, `y1 = y0+64`, `z = min/max(4 corner short heights at cell+7) ∓ 1000.0` (0x9cf778),
  `patch+0x20/+0x22 = i,j`, `patch+0x24 = SceneSector` = cell short +9 ? namedSector[cell+9 − 1] : sector named
  **"Default scene sector"** (`0x54e6a0("Default scene sector")`, 0x4f70e2). So every terrain cell belongs to exactly one scene sector; the
  map must declare a sector called "Default scene sector". CONFIRMED (disasm 0x4f75a8..0x4f79b0).

##### Actor ↔ sector registration — CONFIRMED
- en3d Actor (0x10c, ctor 0x5aab20: memset, +0x28 scale=1.0, +0x10a=1): `+0x08 int type`, `+0x0c owner*`, `+0x18 SceneSector*`,
  `+0x1c Scene*`, `+0x20 prev(newer→older is +0x24)`, `+0x24 next`, `+0x5c physics body`, `+0x8c node/matrix`,
  `+0x90 collision mesh (char +4 kind; 7 = skip)`, `+0xa8 view depth (sort key)`, `+0xb8..+0xf4 collision shape (§7)`, `+0xfe/+0x102/+0x106 angles`.
- `SceneSector_add_actor 0x5efc50(actor)` (ecx = sector): unlink from old sector, link as newest (`+0xce`), set actor+0x18/+0x1c (`0x5acf10`).
  `SceneSector_remove_actor 0x5efdb0`, clear `0x5eff10`, first `0x5effa0`(+0xca, walk +0x20), last `0x5efff0`(+0xce, walk +0x24).
- Units: `Unit_update_location 0x4fee60` (called from `Actor_set_pos 0x5ac310` for type 0xb): `World_locate(&unit+0x339,&unit+0x345,&unit+0x0e)`;
  if sector changed: remove actors +0x2f5 (body), +0x319, +0x325, hand items (+0x1bd/+0x1bf → item actors +0x86/+0x8e), lights +0x32d/+0x331 (`0x6891d0`);
  if level changed: `Level_add_unit`, hand items re-registered to level; then if level loaded (+0x43≠0) and unit inactive (+0x2ed==0)
  → activate `0x442820`; if level unloaded and unit active → deactivate `0x41fcc0`; else add all actors to the new sector. Then mounted/attached
  sub-units (unit-type +0x1ad count, +0x1b1 offsets, +0x1b5 angles) are carried along. Unit fields: +0x335 World*, +0x339 Level*, +0x33d/+0x341 links, +0x345 SceneSector*.
  Gameplay effect (CONFIRMED): `Unit_process 0x52c490` phase 1 (`Unit_updateAction`, turn `FUN_004f2190`, `Unit_process_movement`)
  runs only `if (unit+0x2f5 != 0)`, and `Unit_deactivate` → `0x4f07f0` destroys +0x2f5: units in streamed-out levels keep
  thinking/issuing orders (phases 0/2/3; `Unit_process` only checks +0x335 == current world) but neither animate, turn nor
  move, and melee/ranged hit tests (`0x5048b0`/`0x5051e0`) return 0 against inactive units. Level unload `0x449780` also
  frees the level's PathSpace (`0x6ad450` clears bodies' map/poly), so `Unit_path_to` (level+0x43 == 0) plans world-mesh only.
- Items: `Item_update_location 0x4311d0` (from Actor_set_pos types 1/10/6–9): same scheme with item+0xa6 World*, +0xaa Level*, +0xb6 SceneSector*,
  actors +0x86 (main), +0x8e (second model), emitter actors from anim-particle-effect slots +0x96/+0x98; active flag +0x82; activate vtbl+0x1c, deactivate vtbl+0x20.
- `Item_activate 0x4227c0` (vtbl+0x1c): create actor (vtbl+0x10 `0x42f210`), collision shape (vtbl+0x7c `0x41e850`), locate sector, add actors+emitters to sector,
  create physics body if NeedCollision (§6).

#### 6. Item resource record (ItemRes, 0x2bf bytes; `item\ITEM.RES`, parser `ItemRes_parse 0x4a0160`, loader `0x49fbb0`) — CONFIRMED offsets

Positional header (in file order): `+0x00 short class` (ITEMCLAS 1-based), `+0x02 short type` (ITEMTYPE 1-based), `+0x04 short recno`,
`+0x10 char* name`, `+0x06 short model_res` (dir `ITEM\<subdir>\`), `+0x08 short 2nd model (MODEL\, 'none'→0)`, `+0x0a short attach bone`
(table of 10 bone names at 0x9f748c stride 0x38, `0x423220`), optional `Description` → `+0x20 char*` (`\n` escapes), optional
`StructionType` → `+0x1c short` (TOWER 1, WALL 2, GATE 3, WOODGATE 4, WOODDOOR 5, TRUNK 6), `+0x14 char*` icon/basename,
class-specific block (below), then 5 positional shorts `+0x24..+0x2c`, optional `Price +0x2e int`, `+0x32 int`.
Class-specific: type 0x25 (MISC obj) `+0x62 n`, `+0x87`; class 6 WEAPON `+0x62 n` model names `+0x77[n]`, `+0x42 int`, colour `+0x46` (BLUE0/GREEN1/YELLOW2);
class 1 GATE `+0x5e int`, **`notBlock` +0x3e int**, `NeedKey +0x66 u8`, `+0x62 n` anim entries (`+0x67 int[n]`, model `+0x77[n]`, anim `+0x87[n]`, kind `+0x97 u8[n]`: 0 AnimName,1 ModelName,2 Model+Anim);
class 2/3 or type 4: same list + AddModelName(3)/ModelName2(4); class 10 BOX / 18 KEG: NeedKey + list; class 7 POTION `+0x23d int`.

Keyword properties (free order loop):

| key | off | type | meaning / consumer |
|---|---|---|---|
| Damage | +0x4e | int | copied to item+0x72 (`Item_init 0x4178e0`) |
| IncDamage | +0x52 | int | |
| DamageType | +0x4a | int | via 0x4a45f0 |
| MinLevel / MaxLevel | +0x36 / +0x3a | int | |
| Defense | +0x56 | int | → item+0x76 |
| Duration | +0x5a | int | → item+0x7a |
| BlurVertexCtr | +0xc3 short, +0xc5 char[0x20], floats +0xe6+i*12 | | weapon trail |
| HasEmitter | names +0x122+i*4 ('NONE' skipped), offset vec +0x132+i*12, float +0x162+i*4, u8 +0x182+i, i<4; resolved ids +0x172+i*4 (HYPOTHESIS: resolved by 0x49fdf0) | | item activation spawns first 2 (`0x42f210`, anim-particle-effect array) |
| **NeedCollision** | **+0x1c2** | short | ≠0 → `Item_activate` creates physics body `physis_sys.create 0x6adbf0(actor, mass, p3, 0, 2, 0)`: class 4 WALL mass **1.0e7** (0x4b189680), p3=0x14; else mass **60.0** (0x42700000), p3=0 (disasm 0x422977..0x4229d0). Also used by projectile sweep `0x68de40` (items with body and NeedCollision or 0x4a4050 within |dx|,|dy|≤1000). |
| Pickable | +0x1c4 | u8 | read after ItemRes_get at 0x44b41c/0x44c228 (0x44ac60), 0x49dc55, 0x4f4d32 (Level_load_objects) |
| HasBoundingBox | +0x1c5 short, floats +0x1c7..+0x1db (min xyz, max xyz) | | read after ItemRes_get at 0x478b6e, 0x50b050, 0x50b1f6; gate 0x48d4b0 uses it to choose physics path |
| CanTransparent | +0x1df | bool | 0x4531b2 in 0x452d20 (HYPOTHESIS: camera see-through fade) |
| ModifyCamera | +0x1e0 | bool | 0x42456b in camera code 0x424010 |
| **EnlargeSceneSector** | **+0x1e1** | bool | Level object load `0x4f3df0` (only when load mode param==1, type≠6 grass, class≠12 WAVE): for every sector from `Scene_query_sectors(point)` whose **exact** box contains the object's (x,y,z), grow the **render** box: minX=min(minX, x−0.6R), minY=min(minY, y−0.6R), maxX=max(maxX, x+0.6R), maxY=max(maxY, y+0.6R) where `R = max(3 int size fields from the placement record)`, `0.6R = (R/2 truncated toward 0)*1.2` (0x9d8290), Z untouched; then `Scene_invalidate_tree 0x5eb030`. Purpose: big objects straddling a sector are not culled. CONFIRMED (disasm 0x4f4a71..0x4f4d20). |
| HasAntiPortal | +0x1e2 | bool | occluder |
| SkipIntersectWithItem | +0x1e3 | bool | physics sweep `0x693750`: items (actor type 1, collision mesh kind≠7, type∉{8..12}, class∉{7,8,9,15,19}) are ray/mesh-tested only if this is 0 |
| **HasPath** | **+0x1e4** | u8 | item actor's collision mesh is a walkable/pickable surface: mouse ground pick `0x452830` accepts ≠0; height/ray queries `0x445990` (World) and `0x427d90` (camera) accept ==2 only. HIGH |
| AntiPortalPercentage | +0x1e5 | short | |
| ItemUsage | +0x1e7 | u8 | BOTH 0, AI 1, MAIN 2 (selects physics variant in gate code 0x48d4b0) |
| ShowArmoryInterface | +0x1e8 | u8 | |
| **CollisionType** | **+0x1e9** | int | `None`0 / `Sphere`1 / `Box3d`2 / `Cylinder`3 (`0x4a4540`); values are ints converted to float. Sphere +0x1ed cx,+0x1f1 cy,+0x1f5 cz,+0x1f9 radius; Box +0x1fd minX,+0x201 minY,+0x205 minZ,+0x209 maxX,+0x20d maxY,+0x211 maxZ; Cylinder +0x215 cx,+0x219 cy,+0x21d cz,+0x221 radius,+0x225 height. Consumer §7; aim-height uses radius / (maxZ−minZ)/2 / height/2 (`0x5146a8`). |
| TerrainSound | +0x229 int sound id, +0x22d int | | |
| Sound / Sound2 / Sound3 | +0x231 / +0x235 / +0x239 | int | sound ids (0x4d12f0) |
| RequiredLevel | +0x241 | int | |
| EffectiveRequirementType | +0x245 n, +0x249 ptr (16-byte recs: type, v1, v2, BELOW0/OVER1) | | |
| AdditionalEffectType | +0x24d n, +0x251 ptr (12-byte recs) | | |
| UsableUnit | +0x255 short | | |
| CombineItem / ResultItem | +0x257 / +0x259 short | | |
| UniqueItem / CanDrop / CanSell / OpenWithInteract | +0x25b / +0x25c (def 1) / +0x25d (def 1) / +0x25e | u8 | |
| MessageName | +0x18 char* | | |
| DetailDescriptionFilename / Nation | +0x25f / +0x27f | char[0x20] | |
| HasLight / HasLight2d | +0x186.. / +0x1b1.. | | light params |

**notBlock** (+0x3e, GATE only): `Gate_set_closed 0x48d4b0(close)` — when closing and notBlock==0, scan scene sectors in a box
gate.pos ± 300 (200+100) and refuse (return 0) if any unit actor (type 0xb) whose unit-type class==8, or `0x430190`, or subclass(+0x118)==2
is within 300 units in XY (dist² ≤ 90000.0). notBlock≠0 skips that check. Closing then marks the gate's path polygons blocked (+0xd3 count, +0xd7 polygon ptrs, poly+0x70=0)
and on opening re-creates them. HIGH.

Item class ids (STD.SET ITEMCLAS, 1-based, CONFIRMED from data): 1 GATE, 2 OBSTACLE, 3 HOUSE, 4 WALL, 5 TERRAIN, 6 WEAPON, 7 POTION, 8 GOLD,
9 ENHANCER, 10 BOX, 11 LADDER, 12 WAVE, 13 SWITCH, 14 PATH, 15 MISSION, 16 ARMOR, 17 MISC, 18 KEG, 19 CROSS, 20 PTERRAIN.
ITEMTYPE (1-based): 1 Gate,2 Obstacle,3 House,4 Wall,5 Terrain,6 Grass terrain,7 Permanent terrain,8 L-hand,9 R-hand,10 2-hand,11 Range,12 Crossbow,
13 Arrow,14 Projectile arrow,15 Explosive arrow,16 Bolt,17 Projectile bolt,18 Explosive bolt,19 Cannonball,20 Stone,21–25 potions,26 Gold,27 Amulet,
28 Gemstone,29 Ring,30 Box,31 Ladder,32 Switch,33 Path,34 Mission item,35 Mission letter,36 Armor,37 Misc object,38 Keg,39 Cross.

World item object by class (`Item_create 0x49ea90`, operator_new sizes CONFIRMED): GATE 0xdb (ctor 0x48c250), OBSTACLE 0xce (0x434380),
HOUSE 0xce (0x48ea70), BOX 0xcd (0x489ea0), KEG 0xc2 (0x48f740), WALL 0xef (0x432210), SWITCH 0x107 (0x434040), PATH 0xce (0x4352b0),
MISC/MISSION/CROSS/others 0xc2 base (0x416a50, vtable 0x9d3d78; MISSION/CROSS also copy name to +0x0c). LADDER gets item+0x7e=0x200.
Base Item fields: `+0x00 vtbl`, `+0x04 short recno`, `+0x0a short item_res`, `+0x4e short class`, `+0x50 short type`, `+0x52 int anim/model idx`,
`+0x56/+0x5a/+0x5e pos`, `+0x62/+0x66/+0x6a angles`, `+0x6e scale (1.0)`, `+0x72 Damage`, `+0x76 Defense`, `+0x7a Duration`, `+0x82 active`, `+0x83 reentry`,
`+0x86 Actor*`, `+0x8e 2nd Actor*`, `+0x96 short[2] emitter ids`, `+0x9e`, `+0xa6 World*`, `+0xaa Level*`, `+0xae/+0xb2 level links`, `+0xb6 SceneSector*`.

#### 7. Actor collision shape (filled by `Item_setup_collision 0x41e850`, vtbl+0x7c) — CONFIRMED
`actor+0xb8` shape: 0 none, 1 sphere (`+0xbc,+0xc0,+0xc4` center, `+0xc8` radius), 2 box (`+0xcc..+0xe0` min xyz,max xyz), 3 cylinder
(`+0xe4,+0xe8,+0xec` center, `+0xf0` radius, `+0xf4` height). Default = sphere from model bound (model+0x34..+0x40); for ITEMTYPE 13–18
(arrows/bolts) sphere at origin radius **10.0**. ItemRes CollisionType≠0 overrides. Physics radius from shape `0x6adbf0`: sphere r, box max(|min|,|max|) per axis…
(Projectiles/physics agent owns the solver).

#### 8. Actor ("scene object") type ids at actor+0x08 — CONFIRMED writes, owner at actor+0x0c

| id | owner | set at | notes |
|---|---|---|---|
| 1 | world Item | 0x42f3c8 (Item_create_actor) | `Actor_set_pos 0x5ac310` → item pos +0x56, `Item_update_location` |
| 10 | Item with ITEMTYPE 6 (grass terrain) | 0x42f3b6, 0x492fa3, 0x4f1bd6 | same handling as 1 |
| 6 | ThrowItem kind 0 (arrow/bolt ARW) | 0x5080ee | owner+8 = item_array recno of the thrown item |
| 7 | ThrowItem kind 1 (projectile PROJ) | 0x5081fa | |
| 8 | ThrowItem kind 2 (explosive EXPL) | 0x508306 | |
| 9 | ThrowItem kinds 3,4 (stone),5 (cannon) | 0x50841a/0x50852e/0x508642 | kinds from ITEMTYPE: 13/16→0, 14/17→1, 15/18→2, 0x13→5, 0x14→4 (`0x507fc0`) |
| 0xb | Unit | 0x50cdad, 0x5115f6, 0x52badb | owner pos +0x0e; `Unit_update_location` |
| 2 | particle-system / effect actors | 0x41aac1 (anim particle effect), 0x68142c (particle sys), 0x68b563/… (weather rain) | |
| 0x10 | unit attached sub-model actor (unit+0x321; unit-type +0x1d9≠0 and +0x202=='W') | 0x502605, 0x50358d | HYPOTHESIS: siege/vehicle part |
| 0x27 | slash trail effect ("SLASH_02") | 0x687e9e | |
| 0 | default (memset) | 0x5aab20 | |

Render grouping `Actor_render_group 0x5aed50`: {2,0x27,0x10}→3, 0xb→1, else 2 (`Scene_render` passes). Translucent pass sorts the
sector's actors by `actor+0xa8` descending (`qsort` cmp `0x5f0780`, "sector sort actor"), buffer scene+0xd1 grown by 128.
Switch on the type in `Actor_set_pos 0x5ac310`: cases 1,10 / 6–9 / 0xb (CONFIRMED). The id space is shared with en3d (script compiler node
types at 0x642xxx are a different struct; ignore).

#### 9. Path mesh containers (uti3d\pathsp.cpp) — HIGH
`PathSpace_load 0x670980` (level+0x54/+0x58; callers 0x4f2f80, 0x442d30, 0x4797a0): vertices `BlkArrayB(0x0c, 0x800)` (3 ints x,y,z),
polygons `BlkArrayB(0x74, 0x800)` at +0x14 (poly +0x00 id, +0x04 type 0/1, +0x08..+0x1c 6 ints, +0x24 item_res via name `0x4a36c0`, +0x6c owner PathSpace).
Builders 0x665480/0x665b20/0x666160/0x666780/0x666db0/0x667190/0x66a640 use BlkArrayB(…,0x800 or 0x80) and DynArray(4/0x10/8, 0x800) temporaries.
No caps; block size 2048 records. Pathfinding semantics: other agents.

#### Gaps
- Exact purpose of HasPath==1 vs 2 beyond "pick vs. height-ray" (0x445990/0x427d90 not fully decoded). HYPOTHESIS.
- Actor type 0x10 semantics (unit+0x321 sub-model) — HYPOTHESIS.
- Physics body create args (p3/p5) and solver: Projectiles/physics agent.
- facility_array polymorphic class ids and sizes not mapped to facility kinds.

---

# Part D — Trigger system, end conditions, mission transition (joan.exe)

All addresses are VA (image base 0x400000). Time = game ms `DAT_00a500ba` (`now`), frame delta ms `DAT_00a500be` (`dt`, also `0xa500b6`); 30 Hz tick → 1 tick ≈ 33.3 ms. Real-time ms clock used by UI = `DAT_00a500b2`.
Globals used below: `Game` = `0xa58f90` (game.cpp object), `Scenario` = `Game+0x17` = `0xa58fa7`, `Battle` = `0xa51294` (battle.cpp loader), `Sys` = `0xa50088`, `TriggerSys` = `0xa5ccd0`, unit array `0xa47c08`, world items `0xa460a0`, modify-item array `0xa50530` (its DynArray at `0xa50534`), player main character recno `DAT_00a5b9e8` (set_main_character), sound-effect system `0xa5cf98`.

---------------------------------------------------------------------------------------------------
## 1. Per-tick order inside `Game_process_tick 0x4e5fc0` (ECX = Sys) — CONFIRMED (disasm `dis(0x4e5fc0)`)

```
0x4e5fcc  if finish(0xa59298)==0 && Battle.loaded (FUN_0041c7e0: Battle+0 > 9):
              Game+0x740 (0xa596d0, total play time ms) += dt;  Scenario+0x1da (0xa59181, scenario passed time) += dt
0x4e6009  if Sys+0x44 (game speed) != 0.0 && Sys+0x151 == 0:
   if Battle.loaded && Sys+0x3a(cur mode, 0xa500c2) != 5:            -- world simulation, fixed order:
      0xa47ba8 GameClock      FUN_00476bb0   (clock minutes += dt*24/60/1000 = 0.0004/ms; 1440 → new day FUN_004953d0)
      0xa506e0 modify_light   FUN_004b0780
      0xa50530 modify_item    FUN_004afb20
      0xa47c08 unit_array     FUN_0051e280
      0xa3ce90 glowing_light  FUN_00487960
      0xa527c8 water          FUN_00547030
      0xa48aa8 water_wave     FUN_00431f20
      0xa45d30 dynamic_grass  FUN_00464740
      0xa48500 throw_item     FUN_004913c0   (projectiles; creates ARROW/STONE/EXPLOSIVE triggers on hit)
      0xa5ccd0 TriggerSys     FUN_004ef090   ("trigger process")
      0xa484d0 place_array    FUN_004c55d0
      0xa5d100 facility_array FUN_0046d4a0
      0xa52940 territory      FUN_004e4bb0
      0xa58f90 Game           FUN_00471620   (scenario script + END CONDITIONS + step switch)
      0xa51298 (Battle+4)     FUN_0063c650   (iterates script-thread array → FUN_0063b5e0; HYPOTHESIS: running script threads)
   if Sys+0x3a == 9 (in-game): UI/world-view ticks: 0xa4d618 objective menu FUN_006fd8b0, 0xa5d380 cut-scene FUN_006ca660,
      0xa48a38 FUN_00761630, 0xa57030 FUN_006d3590, 0xa53cb0 game-end menu FUN_00721270, 0xa53ff8 game-over box FUN_006b7f20,
      0xa40470 win message FUN_006e7cf0, … cameras 0xa50298 (FUN_0068a170/0068c2a0/0068a010), 0xa57078 anim-particles FUN_0041bc60
   switch Sys+0x3a: 9 → 0xa48cc0 FUN_005634e0, 0xa570a8 FUN_006ad660 ; 0xe → 0xa46030 FUN_006d6370 ; 0xf → 0xa52968 FUN_006b5900
0x4e6347  mode 9 && !Sys+0x151 → more UI (0xa411d0 FUN_00751750 …) ; mode 3 → menu UIs
0x4e6413  0xa5cec8 FUN_006e4a20, 0xa57c98 FUN_006d1980
0x4e6435  if !Sys+0x151: 0xa5cf98 FUN_00470f80 ("trigger process sound": frees SE channels 1..12 whose voice state==4/stopped) ; Sys FUN_004e6fe0 (music selector)
```
- Triggers are processed AFTER units/projectiles of the same tick and BEFORE place/territory/end conditions. End conditions are evaluated after territory update. CONFIRMED.
- Profiler names "trigger process" (0x9fbf06) / "trigger process sound" (0x9fbf38) are entries of a 0x32-byte-stride name table with no code xref (profiler compiled out). The two functions they correspond to are `FUN_004ef090` and `FUN_00470f80` (HYPOTHESIS by order/name).

---------------------------------------------------------------------------------------------------
## 2. Trigger system (`TriggerSys` @ `0xa5ccd0`)

### 2.1 Container — CONFIRMED
- DynArray of `Trigger*` (element size 4; ctor `FUN_004eed50` → `DynArray_init(4, 0x14, 0)` 0x5843d0; global ctor wrapper `FUN_00569d70`). 1-based indices; deleted slot = NULL (`FUN_004f0290` is_deleted).
- `"Loading trigger_system"` in `Game::init FUN_00470d40` calls `FUN_004eedf0` = **empty** (nothing loaded at init). Not called when `Game+0x17 (scenario no) == 6` (whole array-load block is skipped for scenario 6, see §4).
- Deinit `FUN_004eee10` (from `Game::deinit FUN_00471010`): delete every trigger, then clear array.
- `TriggerSys::add` **`FUN_004eef00`** (16 stack args, `ret 0x40`): `new(0x43)`, memset 0 (`FUN_004eff60`), `Trigger::init FUN_0047e200`, `DynArray_add`, returns new index (`FUN_00417520` = last index).
- `TriggerSys::get(idx)` `FUN_00485f20`; `TriggerSys::del(idx)` `FUN_004ef020` (delete obj, `DynArray_del` slot).
- Save `FUN_00485e70` (writes byte this+0x1d, count, then each record raw 0x43 bytes via `FUN_00486090`); load `FUN_00485f50` (clear, read count, `new(0x43)` + raw `File_read(0x43)` `FUN_004860d0`). Called from gamefil2.cpp (0x4760ca / 0x4747ca). NOTE: `+0x3f` (object pointer) is saved raw → stale after load (original bug, HIGH).

### 2.2 Trigger record (0x43 bytes, packed) — offsets CONFIRMED from `FUN_0047e200`, meanings HIGH unless noted
| off | type | name | meaning |
|---|---|---|---|
| +0x00 | u32 | flags | one TRIGGER_* bit (table §2.3) |
| +0x04 | i32 | kind | victim class: 1 = units, 2 = modify-items (doors/walls/towers/kegs/anim props), 4 = sound emitter |
| +0x08 | i16 | target | kind1: unit recno; kind2: modify-item recno; kind4: sound-res id. Invalid/0 → **area** search |
| +0x0a | i32 | src_type | 1 = unit, 2 = world item (0xa460a0), 3/other = none |
| +0x0e | i16 | src | source recno (attacker unit / item) |
| +0x10 | i16 | attack_id | combo/attack-res id (`0xa5d0f8` res, `FUN_00416f20`); used for hit damage/knockback; ≠0 bypasses the 80 ms early-hit gate |
| +0x12 | i8 | priority | 1 or 2 (melee=2 first swing); sort key — see bug note |
| +0x13 | vec3 f32 | pos | world x,y,z |
| +0x1f | f32 | radius | |
| +0x23 | u8 | follow | each tick re-centre pos on source (`FUN_00468a80`: unit→`FUN_004fedb0` centre, item→`FUN_00499930`) |
| +0x24 | i16 | param | FOG: new fog distance; GATE (from .stg): extra short (unused by process) |
| +0x26 | u32 | t_active | game ms; trigger idle until `now >= t_active` |
| +0x2a | u32 | t_expire | game ms; 0 = never expires |
| +0x2e | u8 | pending | keep-alive until first activation (expiry blocked while set; cleared at first activation) |
| +0x2f..+0x3e | — | UNKNOWN (never written by init; zeroed) |
| +0x3f | ptr | obj | only for flags 4/8/0x80: scene mesh object (`FUN_0054e6a0(name)`, world-scene object list by name) |

Expiry test `FUN_0043a530`: `!pending && t_expire != 0 && now >= t_expire` → delete. Typical runtime triggers use `t_active = t_expire = now + delay, pending=1` → fire **exactly once** on the first tick with `now >= t_active`, deleted on the next tick. CONFIRMED.

### 2.3 Trigger types (`FUN_00494dc0` name→bit, CONFIRMED) and who creates them
| bit | name | creators (call site → args) |
|---|---|---|
| 0x1 | TRIGGER_ATTACK | `FUN_00501190` (unit set-action; melee swings) e.g. 0x5029d1: `add(1, kind 1|2, tgt 0, src_type 1, src=unit, prio 2/1, pos=strike point, r = combo[+0x30]+reach, follow 1, t_active=t_expire=now+combo[+0x28], pending 1, obj 0, attack_id=combo id)` — two triggers per swing: kind 1 (units) and kind 2 (items) |
| 0x2 | TRIGGER_ARROW_ATTACK | projectile hit `FUN_00514c40` (0x514df4: `add(2, 1, tgt=hit unit (proj+0x26), 1, shooter, 1, proj pos, r=proj+0x46, 0, now, now, 1, 0, 0)`), `FUN_004fbc90` |
| 0x4 | TRIGGER_FADEOUT | .stg `CreateTrigger` only |
| 0x8 | TRIGGER_FADEIN | .stg only |
| 0x10 | TRIGGER_THROW | `FUN_00501190` (grab/throw, tgt = unit+0x2d7 current target) |
| 0x20 | TRIGGER_GATE | script `add_gate_trigger(item, delay)` `FUN_0065c530`: `add(0x20, 2, tgt=item.modify_recno(+6), 1, src=player 0xa5b9e8, 1, item pos, r 1.0, 0, now+delay, now+delay, 1, 0, 0)`; also `FUN_0043b2d0`, `FUN_0044a430`, `FUN_004a71f0`, .stg |
| 0x40 | TRIGGER_STONE_ATTACK | catapult/trebuchet stones `FUN_004fbc90`/`FUN_004fcc00`/`FUN_00514c40`; script `break_wall(item, unit)` `FUN_00650e70`: `add(0x40, 2, item, 1, unit, 1, item pos, r 1000.0, 0, now, now, 1, 0, 0)` |
| 0x80 | TRIGGER_FOG | .stg only |
| 0x100 | TRIGGER_PLAY_ANIMATION | .stg only (115 in shipped maps) |
| 0x200 | TRIGGER_INTERACT | player interact `FUN_00452720` (r 100.0), `FUN_0043bc50` (r 200.0), `FUN_0043bd40`(r 100), `FUN_0044a430`, `FUN_00501190`; lever items `FUN_00479270` (unused: all shipped objects have `TriggerType:0`) |
| 0x400 | TRIGGER_DOWN_ATTACK | `FUN_00501190` (attack on downed unit) |
| 0x800 | TRIGGER_PLAY_SOUND | `FUN_0050db80` (unit positional sounds; kind 4, src_type 1 = unit), .stg |
| 0x1000 | TRIGGER_PLAY_AMB_SOUND | .stg only (22 in shipped maps) |
| 0x2000 | TRIGGER_EXPLOSIVE_ARROW_ATTACK | projectiles `FUN_004fbc90`/`FUN_004fcc00`/`FUN_00514c40` |
| 0x4000 | TRIGGER_EXPLOSIVE_KEG_ATTACK | keg explosion `FUN_0048fa50`: `add(0x4000, 2|1, 0, src_type 2, keg item, prio 1, keg pos, r 200.0, 0, now, now, 1, 0, 0)` ×4 pairs |

Unit-type side (unitres.cpp `FUN_00525080`): `TriggerType :` list (terminated by `TRIGGER_END`) ORs bits into unit-type `+0x11c` = **susceptibility mask** (only ATTACK, DOWN_ATTACK, ARROW, STONE, EXPL_ARROW, EXPL_KEG, THROW, INTERACT accepted). Unit-type `+0x1c3` bounding shape (1 sphere/cyl params +0x1c7.., 2 box params +0x1d7.. (6 floats), 3 +0x1ef..). CONFIRMED.

### 2.4 Map-file triggers (`.stg` `CreateTrigger { … }`) — CONFIRMED
Parser `FUN_00450ea0` (dispatched by `FUN_0044e5f0` from world_s.cpp loader `FUN_004f2f80` with create=1; `FUN_00439ff0` calls with create=0 = parse only). Field names are ignored, values positional:
`trigger_type:<NAME>; x; y; z; radius;` then type-specific: FADEIN/FADEOUT → `<mesh name>`; GATE/FOG → `<short>`; PLAY_ANIMATION → `target_item:<item name>`; PLAY_SOUND/PLAY_AMB_SOUND → `code:<sound code>`. x,y get loader offsets `this+0x44 / +0x48` added (int). Created:
- FADEOUT/FADEIN/FOG: `add(bit, 1, 0, 1, 0, 1, pos, r, 0, t_active 1, t_expire 0, 0, obj=FUN_0054e6a0(name), 0)`; FOG then `+0x24 = value`.
- GATE: same with obj 0, `+0x24 = value`.
- PLAY_ANIMATION: only if item found (`FUN_0049f420(name)`) and it has a modify-item (`+6 != 0`): `add(0x100, 2, item.modify_recno, 1, 0, 1, pos, r, 0, 1, 0, 0, 0, 0)`.
- PLAY_SOUND/AMB: `add(bit, 4, FUN_004d12f0(code), 3, 0, 1, pos, r, 0, 0, 0, 0, 0, 0)`.
All map triggers are permanent (t_expire 0). Shipped data contain only PLAY_ANIMATION (115) and PLAY_AMB_SOUND (22).

### 2.5 Process — `TriggerSys::process FUN_004ef090` — CONFIRMED (disasm 0x4ef090–0x4efc3f)
```
if count: qsort(data, count, 4, cmp 0x45bdf0)   // BUG: cmp compares byte +0x12 of the *element address*
                                                 //   (i.e. a byte of a neighbouring pointer), not of the Trigger.
                                                 //   Result order is effectively arbitrary. Reimpl: keep insertion order.
for idx = count .. 1:                            // reverse order; deletion during loop is safe
  t = get(idx); if null continue
  if t.flags & 4/8/0x80 (FADEOUT/FADEIN/FOG):    // player-proximity triggers, never expire, ignore t_active
      P = player unit (0xa5b9e8) if valid else continue
      if |P.pos(+0xe) - t.pos|^2 <= r^2:  FADEOUT: t.obj->+0xdf = 0 ; FADEIN: t.obj->+0xdf = 1 ; FOG: fog_dist(0xa48cdc,float) = (float)t.param
      continue
  if expired(t): del(idx); continue
  follow(t)                                     // +0x23
  if t.t_active > now: continue
  t.pending = 0
  switch t.kind:
  case 2 (modify items):
     if t.target <= 0: for item = items.count..1 (0xa50534):
          if src_gate(t) fails: continue
          if item footprint test FUN_0041c5e0(item, t.pos, (int)t.radius): item->vtbl[0x48](idx)
     else if item valid: if src_gate(t): item->vtbl[0x48](idx)          // no position test
  case 1 (units):
     if target unit valid:                       // single victim, no position test
        if !(t.flags & type(target).mask) continue
        if src_gate(t) && throw_gate(t): Unit::on_trigger(target, idx)  FUN_0075d540
     else for u = units.count..1:
        skip null; skip u == t.src when src_type==1
        if !(t.flags & type(u).mask) continue
        if !src_gate(t) || !throw_gate(t) continue
        c = unit centre FUN_004fedb0
        if flags & (0x2000|0x4000|0x40):
           if type(u).shape(+0x1c3)==2: AABB(t.pos±r) transformed into u's local frame (inverse actor matrix,
                                         or translation(u.pos)*rot(u+0x1a) when no actor) overlaps unit box (+0x1d7) [FUN_0060d6b0]
           else: (t.x-c.x)^2+(t.y-c.y)^2 <= r^2 && |t.z-c.z| <= r      // vertical cylinder
        else: |c - t.pos|^2 <= r^2                                      // sphere
        if u.state in 0x40..0x42 (downed, FUN_00508fb0):
           skip if (src_type==1 && src.state==0x46) || t.attack_id != 0
        rc = Unit::on_trigger(u, idx); if rc == 2: break               // trigger consumed (THROW, DOWN_ATTACK…)
  case 4 (sound):
     if src_type==1 && src unit invalid: del; continue
     if src_type==2 && src item invalid: del; continue
     SE_play_trigger(0xa5cf98, idx) FUN_004c0680
src_gate(t): only when (flags & (ATTACK|DOWN_ATTACK)) && src_type==1:
     source must be valid AND in an attack state (FUN_00510f80: state ∈ {8,0x19,0x1c..0x22,0x46,0x49,0x53})
     AND NOT (state==0x49 && now - actor(+0x2f5)->+0x4c < 80 ms && attack_id==0)    // hit cancelled if attacker interrupted
throw_gate(t): if (flags & THROW) && src_type==1: source valid and state == 0x3a
```
Timing: 80 ms ≈ 2.4 ticks. CONFIRMED (`cmp edx,0x50 / jae`).

### 2.6 Actions (receivers) — HIGH
- `Unit::on_trigger FUN_0075d540(idx)` (ECX = victim). Early out 0 if `FUN_00432b90()` (HYPOTHESIS: unit in cut-scene/scripted) unless INTERACT, or victim dead (`FUN_0042d690`). Returns 1 = processed, 2 = consumed (stop area scan), 0 = ignored. Per flag:
  - ATTACK(1): requires src_type 1; damage `FUN_005048b0(src)` (0 → miss). Sets victim/attacker mutual target `+0x2d7` (unless victim busy in states 0x1c,0x49,0x30,0x32,0x36,0x33,0x39,0x38), hit reaction by relative angle `FUN_00505960`: |a|≤45 → action 0x23, 45<a<135 → 0x28, -135<a<-45 → 0x27, else 0x26 (also 50 % `FUN_00509000` for victims in states 0x1e..0x21,0x53); knockback vector from combo res (`attack_id`, defaults 3000/500, res +0x4c/+0x50), `+0x371 = 1`.
  - ARROW(2)/STONE(0x40)/EXPL_ARROW(0x2000): block chance `FUN_00497e10(10,…)` vs `Misc_random(100)` when victim `+0x1c1`; damage `FUN_005051e0(src, flags, 0/1)`; same reactions; knockback 3000 horizontal, 500 vertical.
  - KEG(0x4000, src_type 2): damage `FUN_00505650(item)` → action 0x4e (or fall `FUN_00509000`).
  - THROW(0x10): victim grabbed by thrower (action 0x3b, faces thrower, offset 60 units) → returns 2.
  - DOWN_ATTACK(0x400): attacker state 0x22 and victim state 0x40/0x41/0x42/0x29 → action 0x29, returns 2.
  - INTERACT(0x200): victim+0x2e3 = interacting unit.
  - Combat numbers belong to CombatRE; listed here only for dispatch.
- Modify-item `vtbl[0x48](idx)` implementations (all return 1): `FUN_00433670` animated prop (PLAY_ANIMATION: plays item anim `FUN_005abe30` when any unit scene-node within r), `FUN_0048caf0` **gate** (i_gate.cpp; GATE 0x20 toggles: `+0xd1==0 → open FUN_0048d2b0(1)`, `==1 → close(0)`; INTERACT toggles if `+0x7e` and unit allowed `FUN_00428600`; ATTACK damages only for LAHIRE or ram subclass 0xb), `FUN_0048a1f0` generic destructible, `FUN_0048ef50` keg-only destructible, `FUN_00493680` wall-like destructible, `FUN_0054ffd0` tower ("towerfire.txt" below 50 %), `FUN_00479270` lever (INTERACT → spawns trigger of type item+0x7e on item `+0x103`).
- Sound `FUN_004c0680` (ECX 0xa5cf98): needs listener `DAT_00a57ef8` (+5 pos); if `t_expire==0` and already playing at pos (`FUN_004d00e0`) → skip; if `r != 0` and `|listener - pos| > r` → skip; else allocate voice (`FUN_0045f450`, fallback `FUN_00493340`), volume = master·res+0x69·0.01, bind voice → trigger idx (`FUN_004d06b0`) and pos.

---------------------------------------------------------------------------------------------------
## 3. End conditions / objectives (end_con.cpp 0x467260–0x467390, game.cpp)

### 3.1 Storage — CONFIRMED
`Game+0x309` (0xa59299) i16 count (max **0x28 = 40**); records at `Game+0x30b` (0xa5929b), stride **0x1a**; script index n is 1-based → rec = `0xa5929b + (n-1)*0x1a`.
| rec off | Game off (i=0) | type | field (script func) |
|---|---|---|---|
| +0x00 | +0x30b | i8 | type: 0 WIN (main), 1 LOSE, 2 SIDE quest, 3 IMPERATIVE (`set_end_condition_type` 0x65a390) |
| +0x01 | +0x30c | char* | description (heap; `set_end_condition_des` 0x65a3c0 → `EndCon::set_des FUN_00467300`; `_des_2` 0x65a400 = same + SE "OBJECTIVE_UPDATE" + objective-UI refresh `FUN_007076c0`) |
| +0x05 | +0x310 | char* | territory link name (`set_end_condition_territory_link` 0x65a460 → `FUN_00467390`; UI only) |
| +0x09 | +0x314 | i16 | finish state 0 NORMAL, 1 WIN, 2 LOSE (`set/get_end_condition_finish` 0x65a4a0/0x65a670) |
| +0x0b | +0x316 | i16 | para1 (0x65a6a0) — not read by engine evaluation; read only by objective/debrief UI (0x70545b, 0x706211) |
| +0x0d | +0x318 | i16 | para2 (0x65a6e0) — no engine reader found |
| +0x0f | +0x31a | i16 | enabled (default 1 on add; 0x65a720/0x65a7e0) |
| +0x11 | +0x31c | i32 | gold award (0x65a810) |
| +0x15 | +0x320 | i32 | experience award (0x65a840) |
| +0x19 | +0x324 | u8 | awarded flag |
- `add_end_condition` 0x65a350 → `Game::add_end_con FUN_00471e20`: memset rec, enabled=1, count++, objective-UI add `FUN_007078c0(n-1)`, returns n (0 if full). `del_end_condition` → `FUN_00471ef0` removes the LAST record only. `EndCon::free FUN_00467260` frees +1/+5.
- `set_end_condition_finish(n, "WIN"|"LOSE"|"NORMAL")` acts only on change: WIN → 1 + SE "OBJECTIVE_FINISH" (`FUN_004ceef0` on 0xa5cf98) + UI `FUN_00707960`; LOSE → 2 + UI `FUN_00707b20`; NORMAL → 0 + UI `FUN_00707400`.
- `set_end_condition_enabled(n, v)`: on 0→nonzero plays SE "OBJECTIVE_ADD"; v==0 → UI hide `FUN_007072c0`, else UI show `FUN_00707400`.
- Objective sounds/UI live in objective menu `0xa4d618` (FUN_0070xxxx) and SE system 0xa5cf98; "objective_finish N" / "add_command_point" are **developer console** commands (`FUN_00454560`/`FUN_00454c80`, cheat console `FUN_0045bf30`); `objective_finish` = `FUN_00455c00`: rec[N].state = 1 and if all records are 1 → finish = 1 (bypasses win delay/message). HIGH.

### 3.2 Game state fields — CONFIRMED
| addr | Game off | meaning |
|---|---|---|
| 0xa59298 | +0x308 | finish: 0 running, 1 won, 2 lost, 3 "continue to next scenario" (set by end menu 0x720f75) — `set_game_finish("WIN"/"LOSE")` 0x6594e0 sets 1/2 directly; `is_game_finish` = finish != 0 |
| 0xa596c8 | +0x738 | win delay ms (`set_delay_before_win` FUNC 402 → 0x65fba0); S1L1 uses 15000 (= 450 ticks) |
| 0xa596cc | +0x73c | win-pending start time (game ms), 0 = none |
| 0xa596d0 | +0x740 | total play time ms (profile +0x3c) |
| 0xa596d4 | +0x744 | cut-scene active (`FUN_004725f0` set by cut-scene sys 0xa5d380; `FUN_004301c0` get) — blocks win and count timers |
| 0xa596c4 | — | player gold (gold awards added here) |
| Game+0x2b4 | | i32[4][5] ms countdowns, decremented when scenario mode != TRAINING — UNKNOWN purpose |
| Game+0x304 | | i32 ms countdown used in TRAINING mode — UNKNOWN purpose |

### 3.3 Per-tick `Game::process FUN_00471620` — CONFIRMED (disasm)
```
mode = Scenario::mode() FUN_004cd510        // 0 NORMAL,1 INTERACT,2 TRAINING,3 BATTLE (set_scenario_mode)
if mode==2: G.t304 = max(0, G.t304-dt) else for i<4,j<5: G.t2b4[i][j] = max(0, … - dt)
if Sys.mode != 5 && finish != 2:            // scenario script runs every tick (stops once lost)
    script = Scenario.no==0 ? Scenario.inline.script (Game+0x1b1) : step[cur-1].script(+0x186)
    if script: script->vtbl[0xc](0)
if Scenario.no==0 || Scenario.cur == Scenario.req:
    Scenario::process FUN_004cd5d0:
       if finish==0 && Battle.loaded:
          if step.time_limit(+0x1a6) > 0: step.time_left(+0x1aa) -= dt; if time_left < 1: finish = 2   // time-limit LOSE
          if DAT_00a500d2 && !cutscene: count_time1/2/3 (Scenario+0x1ca/+0x1ce/+0x1d2) += dt
    if finish==0 && count != 0:
       allwin = true
       for r: if r.des && (r.type==0||r.type==2) && r.state==1 && !r.awarded:
                 r.awarded=1; gold += r.gold
                 if r.exp: for each unit u: if u.+0x104 > 3 && is_hero(u) [FUN_0042ff90: type code ∈ JOAN_OF_ARC, JEAN_DE_METZ, LAHIRE, ALENCON]:
                              Unit::add_exp FUN_005229a0(u, r.exp, 1, 0)
       for r: if r.enabled && r.des && r.type==0 && r.state != 1: allwin=false; break
       for r: if r.des && r.enabled && r.type==1 && r.state==2: finish = 2; allwin=false   // LOSE immediately
       if allwin: if winStart==0: winStart = now    else winStart = 0
    if finish==0 && winStart && (uint)(now-winStart) >= winDelay && !cutscene:
       finish = 1
       for r: if r.enabled && r.type==3: r.state = 1
       if Scenario.no != 5 && count: for r: if r.enabled && r.des && r.type==2 && r.state==0: r.state = 2   // unfinished side quests failed
       if !winMsg(0xa40470).active: winMsg.show FUN_006e7900
else:                                        // change_scenario_step requested
    Battle::start_scenario FUN_0041cc80(Scenario.no, Scenario.req, 0); Battle::load_world FUN_0041c810(); Sys.req_mode(+0x3c) = 9
```
Notes: only records with a description count; disabled records are ignored; with zero type-0 records the win timer starts as soon as `count>0`. Win delay compare is unsigned `>=` (`jb` skip). Main-character death → LOSE is **script convention**, not engine: e.g. `S1L1_finish.sce`: `if is_unit_dead(joan) { set_end_condition_finish(3,"LOSE"); call_gameover_msg(text) }`. There is no engine path that shows the game-over UI when finish becomes 2 (time limit included): the UI comes from script `call_gameover_msg` (FUNC 4 → `FUN_0065ebd0`) — HIGH.
Strings WinScenario/LoseScenario/…/WinCookingContest are in **dead code** `FUN_004a9d40` (0 callers; leftover music-cue table from another Enlight game). CONFIRMED (functions.tsv callers=0).

---------------------------------------------------------------------------------------------------
## 4. Scenario / campaign flow

### 4.1 Scenario object (`0xa58fa7` = Game+0x17) — CONFIRMED (scenario.cpp)
| off | type | meaning |
|---|---|---|
| +0x00 | i32 | scenario no (campaign chapter 1..8; 0 = stand-alone map/editor) |
| +0x04 | i32 | current step (1-based, 0xa58fab) |
| +0x08 | i32 | requested step (0xa58faf; `change_scenario_step` 0x659830) |
| +0x0c | i32 | step count (`StepNo`) |
| +0x10 | ptr | step array (0xa58fb7), stride **0x1b2** |
| +0x14 | step | inline step record used when no==0 (time limit 0xa59161/0xa59165, stage 0xa59155) |
| +0x1ca/+0x1ce/+0x1d2 | i32 | scenario count timers 1..3 (ms) |
| +0x1da | i32 | scenario passed time (ms) |
| +0x277/+0x27b | i16*/i16 | unit-type list (cleared via `FUN_00527690`) ; +0x27d/+0x281 item list ; +0x283/+0x287 scenario generals (ptr[], `add_scenario_general`) |
Step record (0x1b2): +0x000 WorldFileName[0x41], +0x041 UnitFileName, +0x082 ScriptFileName, +0x0c3 DescriptionFileName, +0x104 ObjectiveFileName, +0x145 ObjectiveMapFileName (each char[0x41]); +0x186 script object*; +0x18a..+0x196 0; +0x19a i32 scenario stage (`set/get_scenario_stage`); +0x19e/+0x1a2 heap ptrs; +0x1a6 i32 time limit ms (−1 none; `set_game_time_limit` sets limit and left); +0x1aa i32 time left; +0x1ae i32 scenario mode.
- File `SCENARIO\scenario_%02d.SCR` (`FUN_004ccfb0`): tokens `code <id> StepNo <n>` then per step 6 `Name : value` pairs. Shipped: 01 S1L1, 02 S3L1, 03 S2L1, 04 S4L1, 05 S5L1, 06 S9L1 (re-uses scenario5 world S5L1w), 07 S7L1, 08 S6L1; all StepNo 1.
- `Scenario::set(no) FUN_004ccf40`: if `no==0 || no != cur`: deinit `FUN_004cc850`, cur=no, load .SCR, +0x1ae=0, `FUN_004b3160(no)`.

### 4.2 `Battle::start_scenario FUN_0041cc80(no, step, keep)` (ECX 0xa51294) — CONFIRMED order
1. if Scenario.no==0: `0xa53c28 FUN_004b31c0`; `0xa48ae8 FUN_006e5e00`; `DAT_00a48c5e=0`.
2. `Battle::deinit FUN_0041c780(keep)`: if `Scenario.no != 5 || !keep` → `FUN_005624c0` (world deinit); `Game::deinit FUN_00471010(keep)`; Battle.progress = 0.
   - `Game::deinit(keep)`: UI resets…; if `!(no==5 && keep)`: deinit all arrays, triggers (`FUN_004eee10`), 40× `EndCon::free`, count=0. If `no==5 && keep` (5→6): keep world/arrays/triggers; drop every enabled end-con with state≠0 and type≠3 (`FUN_00471f90`), free Game+0x29a list, zero unit fields +0x1b0..+0x1bb. Then `Scenario::deinit`, finish=0.
3. `Scenario::set(no)`; Scenario.cur = Scenario.req = step; finish = 0; Battle.progress = 0.
4. `Game::init FUN_00470d40` (arrays incl. "Loading trigger_system"; end-con count=0) — whole block skipped when Scenario.no == 6.
5. Compile `SCRIPT\SCENARIO\<ScriptFileName>.SCE` → new script object (`FUN_0063e450(0x70)`/`FUN_0063f190`, `FUN_00649280` compile) stored in step+0x186. Missing file → `Scenario::set(0)`.
6. Compile `<DescriptionFileName>.SCE` (e.g. S1L1_des.sce: defines all end conditions), run once (`vtbl[0xc](0)`), delete.
7. Battle.progress = 1.

`Battle::load_world FUN_0041c810`: (scenario ≠ 6) load world/units of the step (`FUN_0044e8c0(1, step, step, 0)`; loading bar 5 %→15 %), profile `FUN_004ca290()`: restore Game lists (`FUN_004728f0`, `FUN_00472c40`), gold = profile+0x38, play time = profile+0x3c; step script `vtbl[0x24]()`, `FUN_0063ed40`, `vtbl[0xc]` (script init pass); camera/cull boxes ±8000/±16000 around player; Battle.progress = 10 (loaded); `DAT_00a5b9f0` = profile+0x40; scenario start real time `0xa5916d` = now; game speed 1.0; `DAT_00a58f96`=1.

### 4.3 Screen modes (Sys+0x3a current, +0x3c requested, +0x3b previous) — `Sys_set_screen_mode 0x499540` exit/enter tables, per-frame in `FUN_0052f930`
| mode | per-frame | enter | meaning |
|---|---|---|---|
| 1 | 0x47b7c0 → req 3 | — | boot → menu |
| 2 | — | 0x4797a0 (sample.pfd test scene) | 3D test view |
| 3 | 0x47b8d0 (main menu UI) | — | main menu |
| 9 | 0x562f40 | — | in-game |
| 0xb | 0x75fa50: `Battle::load_world`; req 9 | 0x75f580 | direct load (no screen) |
| 0xc | 0x75f180 loading-screen state machine (0xa501f0): stage 5 → kind(+0x8f) 0 `Battle::load_world`, 1 `load_game FUN_0041ea60(+0x90)`; kind 2 waits 5000 ms; stage 8 → req 9 | 0x755e30 (picks SL_ backdrop per scenario no) | scenario loading |
| 0xd | 0x755d80: `Battle::start_scenario(no+1, 1, keep=1)`; req 0xc | 0x755840 | **advance to next campaign scenario** |
| 0xe | 0x6d6130/0x6d6370 intro movies (MOVIE\ENLIGHT.WMV…, 4000 ms gap) → req 1 | 0x6d61d0 | intro |
| 0xf | 0x417410 | 0x6b56b0 | UNKNOWN UI |
`Game_process_tick` gates: world sim needs Battle.loaded and mode≠5; UI ticks need mode 9.

### 4.4 Win → next scenario (ordered call flow) — HIGH (each step CONFIRMED in code; button identities HYPOTHESIS)
1. `Game::process 0x471620`: win delay elapsed → finish=1 → `WinMsg::show FUN_006e7900` (0xa40470): +0xd32=1, loads "COMMELE" etc., title = "Yow win!" if no==0 else scenario name `FUN_004dd9c0(no)`, SE `FUN_004e4520(7,0x7f,1…)`, stops player/units actions (`FUN_00508b90(0,1)`), clears unit orders.
2. Win message animation `FUN_006e7cf0` (every 50 ms real). Input `FUN_006e7880` (from Sys input `FUN_0052f930`): on click → `GameEndMenu::open FUN_007226a0(2)` (0xa53cb0; +1 = win mode, sets `DAT_00a591c7=1`, pauses `DAT_00a500d2=0`) and `WinMsg::close FUN_006e7b20`.
3. Game-end menu input `FUN_00720ca0` (buttons at menu+0x54..+0x7c): page buttons → `FUN_00721a60(n)` (win mode: +0x54 → page 7 = mission debrief `FUN_0072c740`/"debrief_1..3"); +0x74 RETRY (not when no==0): `start_scenario(no, cur_step, 0)` + req 0xc (scenario 6 retry instead loads save `<text 6>.SAV` via loading-screen kind 1 `FUN_007563f0`); +0x7c CONTINUE: if no==8 → ending `FUN_006d19e0(0)`; else profile = `FUN_004ca290()`: `FUN_0051ff00` (store heroes), `Scenario::store_generals FUN_004cdbc0`, `FUN_004727b0`/`FUN_00472b00` (Game lists +0x71c/+0x720 … → profile +0x28/+0x2c …), profile+0x38 = gold, +0x3c = play time, +0x40 = `DAT_00a5b9f0`, `Profile::save FUN_004c8810`; `DAT_00a58f95=1`; finish = **3**; `Sys_set_screen_mode(0xd)` (immediate).
4. Mode 0xd per-frame `FUN_00755d80`: `Battle::start_scenario(no+1, 1, 1)` → req 0xc.
5. Mode 0xc loading screen `FUN_0075f180` → `Battle::load_world` → req 9.
- Debug key 0x74 in `FUN_0052f930`: `start_scenario(no+1,1,0)` + req 0xc (skip chapter cheat).
- Lose: script → `call_gameover_msg` → `FUN_0065ebd0` → game-over box `0xa53ff8` (`FUN_006b8af0` open, text `FUN_006b8260`). Input `FUN_006b7d50`: RETRY (no≠0): `start_scenario(no, cur_step, 0)` + req 0xc; LOAD: `FUN_0074da40(1,0)`; third button: `FUN_006e4c00(0)` (HYPOTHESIS: quit to main menu).
- Load game `FUN_0041ea60`: if loaded finish==1, re-shows win message.

### 4.5 Autosave — CONFIRMED
- `GameFile_make_name 0x477770(buf, auto)`: auto=0 → first free `SAVE\<name>%04d.SAV` (0..9999); auto=1 → `_AUTO000.SAV`.. `_AUTO014.SAV` (15 slots, first unused, else overwrite oldest by FILETIME at entry+0x108).
- Its only auto=1 caller `FUN_004779e0` (Game_autosave) has **no callers and no pointer references** in the image → dead code. Callers of make_name: `FUN_004779e0` (dead) and save-game UI `FUN_0074f230` (ECX 0xa549e8, auto=0).
- Effective "autosave" = scenario script `save_game(name, desc)` (FUNC 315 → `FUN_0065bed0`: `SAVE\<name>.SAV`, entry+0x19a = scenario no, +0x19e = flag, `GameFile_save`); shipped scripts: only `S1L1_tactic.sce:930 save_game("Mission1-AutoSave", "Mission 1 Auto Save")`.

---------------------------------------------------------------------------------------------------
## 5. Other recovered facts used above
- Game clock `0xa47ba8` `FUN_00476bb0`: if game speed `0xa500cc` != 0: `+0xc` (f32 minute of day) += dt·24/60/1000; ≥ 1440 → `+0xc = +0x1c` (day start minute) and `Clock::new_day FUN_004953d0` (+8 day count, +0x10 day of month ≤30, +0x44 weekday = day%7, month/year rollovers via `FUN_00495810`/`FUN_004957e0`). 1 game day = 3600 s real. CONFIRMED (constants 24.0 @0x9d3d74, 60.0 @0x9cf9cc, 1000.0 @0x9cf778).
- Territory tick `FUN_004e4bb0` (0xa52940): (a) player-in-territory test over each territory's sub-areas (count +0x27, shape byte +0x48+i: 0 AABB x∈[+0x4c,+0x6c] y∈[+0x5c,+0x7c]; 1 circle centre (+0x4c,+0x5c) r=+0xcc; 2 convex quad (x_k,y_k) = (+0x4c,+0x5c),(+0x6c,+0x7c),(+0x8c,+0x9c),(+0xac,+0xbc), inside when all 4 edge cross products < 0; ints), enter/leave messages by territory type +0x26 (0..5) throttled 4000 ms (this+0x21), player unit +8 = current territory; (b) round-robin ONE territory per tick (cursor this+0x1d): for owner `+0x38 != -1` and `+0x45==0`, flag +0x46 = some non-owner, non-dead unit with LOD tier (+0x22) < 4, class ∉ {0,8,9}, subclass ∉ {2 horse,7 animal}, not cut-scene is inside; rising edge stores now in +0xdc. HIGH.
- Sound-effect channel cleanup `FUN_00470f80` (0xa5cf98): channels 1..12 (`FUN_004cf690`), free when voice valid, not locked (+0x2d), and state==4.
