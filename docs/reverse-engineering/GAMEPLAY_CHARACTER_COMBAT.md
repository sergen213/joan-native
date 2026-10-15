# Gameplay: characters, controller and combat (joan.exe)

Reverse-engineered behaviour of the player characters and units: object layout, controller and input, melee, ranged, siege, XP and levels, regeneration, RNG, and the per-tick update order. It is written as the spec for the C++ reimplementation.

**Sources.** Ghidra export `work/ghidra/export/joan_decomp.c` and the objdump listing `work/re-scripts/joan.asm`; the asm was checked wherever a value, call argument or width matters. Data files: `Resource/GAMEPLAY.RES`, `std_set/*.csv` (animation/move tables, LEVEL.csv, DEFAULTK.csv), `Resource/animscr.res`, `unit/unit.res`, and `work/re-scripts/manual.txt`. Function names are in `tools/ghidra/names_combat.tsv` (≈310 entries). Scratch helper: `work/combat_re/decomp.py`.

**Confidence labels.**
- **CONFIRMED**: read directly in code or asm.
- **HIGH**: strong evidence, but not every path was traced.
- **HYPOTHESIS**: inferred.
- **UNKNOWN**: not determined.

Decompiler output is not verified source.

**Conventions.**
- `U+0xNN` is a byte offset in the Unit object; the decompiler's `short*` indexing has been converted to bytes.
- `ftol` is MSVC `__ftol` (0x7b26cc), which truncates toward zero.
- `now` = `DAT_00a500ba` (game time in ms); `dt` = `DAT_00a500be` (game ms elapsed in the last main-loop iteration).
- Gameplay timers are in **milliseconds**. A tick is 33.3 ms (30 Hz), so `ticks = ms / 33.33`.
- World units: physics works in metres, with **46.1 units = 1 m** (constant 0x9cf780; Part D). Gravity is 9.81·scale. z is up; x/y are the ground plane (U+0x0e x, U+0x12 y, U+0x16 z).
- The player hero recno is `DAT_00a5b9e8` (= `g_PlayerCtrl+0`). The unit array is at `0xa47c08` (`UnitArray_get` 0x41b3c0, 1-based recno).

## Contents
1. Timing model and per-tick update order
2. RNG
3. GAMEPLAY.RES constants (master table)
4. Part A: unit object, data records, regen, XP and levels
5. Part B: input and player controller
6. Part C: melee combat
7. Part D: ranged and siege
8. Original-game quirks and bugs a faithful port must reproduce (or consciously fix)
9. Gaps and open questions

---------------------------------------------------------------------------------------------------
## 1. Timing model and per-tick update order

### 1.1 Clocks (Sys object 0xa50088; `Sys_main_loop` 0x499960) — CONFIRMED (asm 0x499cae..0x499cdb)

Each main-loop iteration wakes when `MsgWaitForMultipleObjectsEx` times out on the FrameTimer deadline. FrameTimer is 30/30 Hz for draw and process; it is 500/500 in slow mode (Sys+0x18).

| global | Sys off | meaning |
|---|---|---|
| `0xa500b6` | +0x2e | `real_dt` = ms since the previous loop iteration, clamped to 1000. Forced to **33** while video-capture mode `DAT_00a3a30c` is on (toggled by a hotkey in 0x52f930; frames are dumped by 0x4d7f80). |
| `0xa500be` | +0x36 | `dt = ftol((float)(u64)real_dt * Sys.speed(+0x44))` (fild qword; fmul; call ftol). |
| `0xa500ba` | +0x32 | `now += dt`, every loop iteration. Gameplay timestamps compare against this. |
| `0xa500c8` | +0x40 | frame factor = real_dt/1000·30, capped at 5.0 (consumers not traced here) |

The order inside one iteration is:
1. `Sys_disp_frame` (if draw is due).
2. **`Sys_process_frame_input` 0x52f930.** This pops **one** input event and runs the hotkeys, the UI and **`PlayerCtrl_process` 0x42e1a0** (via 0x562f40). It runs only on drawn frames.
3. The mouse deltas are latched.
4. `real_dt`, `dt` and `now` are updated.
5. **`Game_process_tick` 0x4e5fc0**, if process is due (30 Hz) and the app is active and not minimised.

Consequences for a port:
- **Gameplay is not fixed-dt.** Timers written as `now - ts >= X` are exact in game ms. Timers written as `x -= dt` (for example invulnerability `U+0x1dd`, the spell timers in `Game+0x2b4`, and the time of day) subtract only the *last iteration's* elapsed time. HIGH: when draw and process both run at 30 Hz the two are ≈equal (33 ms).
- To make a port deterministic, run fixed `dt = 33` ms with `now += 33` per tick. This is exactly what the original does in video-capture mode (`0xa3a30c`).
- **The player controller runs per drawn frame, not per logic tick.** It consumes one queued input event per frame, so a key-press edge is visible for one frame. Horse turning `TURN_ANG` = 6° is applied **per frame** (Part B).

### 1.2 `Game_process_tick` 0x4e5fc0 — call order (CONFIRMED order; `this` from `mov ecx,imm` in asm)

The block runs if `Sys.speed != 0`, the suspend flag `Sys+0x151 == 0`, the world state `*(0xa51294) > 9`, and the screen is not 5.

| # | function | this | subsystem | conf |
|---|---|---|---|---|
| 1 | 0x476bb0 `TimeOfDay_process` | 0xa47ba8 | minute += dt·0.0004 (1 game minute = 2.5 s); wraps at 1440 | HIGH |
| 2 | 0x4b0780 | 0xa506e0 | ambient/wind-like objects; consumes RNG 0x590660 (0x4afd60) | HYPOTHESIS |
| 3 | 0x4afb20 | 0xa50530 | polymorphic world-object array: vcall +0x44 (deleted?) else vcall +0xc process | HIGH |
| 4 | **0x51e280 `UnitArray_process`** | 0xa47c08 | **all units (§1.3)** | CONFIRMED |
| 5 | 0x487960 | 0xa3ce90 | round-robin array; distance 1000/2500 fade (ambient sound emitters?) | HYPOTHESIS |
| 6 | 0x547030 | 0xa527c8 | empty stub | CONFIRMED |
| 7 | 0x431f20 | 0xa48aa8 | objects in a box around the player unit (proximity show/hide) | HYPOTHESIS |
| 8 | 0x464740 | 0xa45d30 | dynamic grass/foliage (`dyngrass.cpp`); **consumes the global RNG** | HIGH |
| 9 | 0x4913c0 | 0xa48500 | **projectile (ThrowItem) manager**: lifetime/distance kill (Part D §3.6) | CONFIRMED |
| 10 | **0x4ef090 `Trigger_process`** | 0xa5ccd0 | **attack/event triggers: melee hits and splash are delivered here** (Part C §3.2, Part D §6) | CONFIRMED |
| 11 | 0x4c55d0 | 0xa484d0 | per-location objects (town garrison list, 60 s counter) | HYPOTHESIS |
| 12 | 0x46d4a0 | 0xa5d100 | polymorphic array vcall +0x10 | UNKNOWN |
| 13 | 0x4e4bb0 | 0xa52940 | map trigger areas vs the player unit position | HYPOTHESIS |
| 14 | 0x471620 | 0xa58f90 (Game) | game timers `Game+0x2b4[4][5]`, `+0x304` (−= dt), mission script | HIGH |
| 15 | 0x63c650 | 0xa51298 | dialog/script entities | HYPOTHESIS |

After these:
- **Screen 9 (in-game) only:** UI and HUD processors (0x6fd8b0, 0x6ca660, 0x761630, …, conditional 0x6f7d40/0x6e67b0/0x6c8c00/0x4af590/0x46e8d0), particle systems 0x68a170 ×2 (`dt_real/1000` seconds) and 0x68c2a0, and 0x4aede0→0x41bc60.
- **Screen 9:** 0x5634e0 (camera manager) and **0x6ad660 → 0x6a8060, the physics/projectile integrator** (Part D §3.4). Screens 0x0e/0x0f run other updates.
- **Always, if not suspended:** the screen-9 HUD block (0x6bfe00 …), 0x6e4a20, 0x6d1980, 0x470f80, and **0x4e6fe0 (battle-music radius BATTLE_R)**.

### 1.3 Unit scheduler `UnitArray_process` 0x51e280 — CONFIRMED (asm 0x51e280..0x51f78c)

The scheduler does nothing if `DAT_00a06bf4 == 0` or the cut-scene flag `0xa596ab` is set.

**Special mode** (`Game 0x4301c0` true; HYPOTHESIS: cinematic/dialog) iterates recno N..1:
- If `U+0x2e` is set: `U+0x22 = 1`, then `Unit_process(0..3)`.
- Otherwise, unless `U+0x2e1`: `Unit_stop` 0x508b90(0,1), then `Unit_process(0)` and `(1)`.

**Normal mode:**

*Pass 0*, over all units N..1 with `U+0x335 == world`:
- `U+0x22 = Unit_calc_lod_tier` 0x508880. The tier is by squared 2-D distance (x,y) to the player unit, or to the camera in RTS mode (`0xa5b9f8 == 4`):
  - player unit → 1;
  - d² ≤ 4e6 (2000 units) → 1;
  - ≤ 1.6e7 (4000) → 2;
  - ≤ 6.4e7 (8000) → 3;
  - else → 4. Units with no player unit get 4.
- If the unit is "alive or busy", call `Unit_process(0)` and `Unit_process(1)` only when the tier is ≤ 1. Alive or busy means any of:
  - `U+0x102 == 0`, or
  - HP `U+0x135 > 0`, or
  - lives `U+0x120 != 0`, or
  - retreat `U+0x114 == -2`, or
  - its actor is still animating a timed effect.
- Otherwise the unit is dead and finished: notify its leader (0x4216d0 → 0x5288f0) and delete it (`UnitArray_del_unit` 0x51e1f0).

*Passes 1–12* each run a round-robin over the unit array. Each pass keeps a cursor (`this+0x40..0x56`, i16) and a per-pass budget (`this+0x58..0x6e`, i16). A unit is processed when it matches the pass filter. A pass stops when the cursor wraps or when `processed > budget`. After each pass, `budget = ftol(count·1.0f / elapsed_ms)` if that is greater than 50, else **50** (constants 1.0f @0x9cf784, 50.0f @0x9cfc18; profiler 0x45e750/0x45e7f0 on 0xa485a8).

| pass | cursor/budget | filter (tier = U+0x22, rank = U+0x104) | call |
|---|---|---|---|
| 1 | +0x40/+0x58 | tier == 2 | phase 0, 1 |
| 2 | +0x42/+0x5a | tier ≥ 3 | phase 0, 1 |
| 3 | +0x44/+0x5c | tier ≤ 1 and rank ≥ 4 (general/king) | phase 2 |
| 4 | +0x46/+0x5e | tier ≤ 1 and rank < 4 | phase 2 |
| 5 | +0x48/+0x60 | tier == 2 | phase 2 |
| 6 | +0x4a/+0x62 | tier == 3 | phase 2 |
| 7 | +0x4c/+0x64 | tier ≥ 4 | phase 2 |
| 8 | +0x4e/+0x66 | tier ≤ 1 and rank ≥ 4 | phase 3 |
| 9 | +0x50/+0x68 | tier ≤ 1 | phase 3 (**generals get phase 3 twice per tick**) |
| 10 | +0x52/+0x6a | tier == 2 | phase 3 |
| 11 | +0x54/+0x6c | tier == 3 | phase 3 |
| 12 | +0x56/+0x6e | tier ≥ 4 | phase 3 |

So every unit gets phases 0–3 each tick unless a tier holds more than about 50 units *and* the measured time budget is exceeded, in which case the remainder waits for the next tick. That second case depends on CPU timing and is therefore **non-deterministic** (HIGH). Recommendation for a port: process every unit, all phases, in recno order N..1 for pass 0 and ascending from the saved cursor for passes 1–12. This matches the original whenever budgets are not hit.

### 1.4 `Unit_process(phase)` 0x52c490 — CONFIRMED (structure); details in Parts A–D

The call returns at once unless `U+0x335 == world` (0xa45f3e). It also returns early in phases 2 and 3 when the UI pause flag `0xa40470+0xd32` is set (0x6e7b50).

**Phase 0** (state and combat bookkeeping):
- `U+0x2e3 = U+0x2e5 = 0`; 0x49da30 (leader/formation helper, UNKNOWN).
- Player unit only: `Player_critCheck` 0x416680 (critical-attack sequence trigger, Part C §4.4).
- Effect handles `U+0x32d/+0x331` updated (0x687a60); `Unit_clear_climb_idle` 0x518b90 (CLB_IDLE).
- If byte `U+0x2e1`, action ≠ 7 and animgrp present: `Unit_stop`, then set action IDLE (1), or 7 when byte `U+0x2e2`, via `Unit_set_action` 0x501190.
- Battering-ram track sound.
- If `U+0x2a5 == 0xf` and the unit at `U+0x2aa` is valid: collision type `actor+0xb8` is toggled by distance to that unit (≥200 or that unit's extent + 20). Meaning UNKNOWN; HYPOTHESIS: an attached or carried unit.
- If `U+0x2a5 == 0xb` and `U+0x2b2 > 0`: `U+0x2b2 -= dt` (countdown, UNKNOWN).

**Phase 1** (movement, timers, regen):
- 0x5174e0 (horse rider/mount sync); `Unit_follow_attach_yaw` 0x454b20.
- Level-arrow timeout (LEVELARR).
- If the unit has an actor:
  - **`Unit_updateAction` 0x7c7ab0**: animation-end state transitions, firing, get-up.
  - Then, if not attached (0x4f2190) or (not player-controlled and not cinematic): **`Unit_process_movement` 0x52daa0** → `Unit_move` 0x4f0b50.
- Light/effect sync; collision-type restore.
- **Regen**: EP, fall gauge, HP (Part A §3). Then `U+0x1dd -= dt` (invulnerability).
- AI auto-upgrade timer (UPG_TIME). `U+0x2a = now`.

**Phase 2** (environment and animation):
- 0x4655a0 (grass interaction, leaders).
- For team leaders (0x44a660): 0x4fa0f0(0,1).
- If `U+0x2a1` (attached object): 0x4baf80.
- Animation-LOD flag on the actor via 0x6ad980(actor+0x5c, 1|0), chosen by tier (`U+0x22`) and current action. Tier 1 can enable; tiers 2–4 disable it unless the unit is in a moving or attacking action (list in 0x52c490). HIGH.

**Phase 3** (AI), `Unit_process_ai` 0x49d810:
- Player-controlled hero (`U+0x00 == player && U+0x102 == 0`) or `U+0x1e6`: 0x4fd630.
- Otherwise the AI "think" runs when `now > U+0x29d + rec.ScriptDuration(+0x121)`. `U+0x29d` is first initialised to `now + Misc_random(500)`; this is the RNG-staggered AI start. It then runs 0x528440 and the unit's attached script/troop logic.

---------------------------------------------------------------------------------------------------
## 2. RNG — CONFIRMED

**Single global generator** `Misc` object at **0xa45fa8**, seed at **0xa46022** (`+0x7a`). It is a Borland-style LCG:
```c
uint32_t seed;                                  // 0xa46022
int rand15(void)      { seed = seed*0x015A4E35u + 1u; return (seed >> 16) & 0x7fff; }   // 0x5905a0
int random(int n)     { seed = seed*0x015A4E35u + 1u; return (int)((unsigned)(n * ((seed>>16)&0x7fff)) >> 15); } // 0x590540, [0,n)
int random_range(int lo,int hi){ return lo + ftol((float)rand15()/32767.0f*(float)(hi-lo) + 0.5); } // 0x590690, [lo,hi]; 32767.0f@0x9d8c14, 0.5 double@0x9d3e18
double rand_unit_excl(){ return rand15()*3.0517578e-05; }      // 0x590630  (/32768) [0,1)
double rand_unit_incl(){ return rand15()*3.051851e-05;  }      // 0x590660  (/32767) [0,1]
float  random_float(float a,float b){ return a + (b-a)*(rand15()/32767.0); } // 0x5906f0
uint32_t rand_raw()   { advance; return seed; }                // 0x5905f0
```
`random(n)` multiplies in 32-bit signed `int` (`imul`). It overflows for `n > 65536`, which no call site uses.

**Seeding.** `Misc_randomize` 0x5904e0 sets `seed = time(NULL)` (CRT 0x7b33bf). It is called once from WinMain (0x420f71). A second `Misc` at 0xa50490 is seeded the same way but **never used for random numbers**: every one of the 377 RNG call sites loads `ecx=0xa45fa8`. The seed getter 0x5904c0 has **no callers**, and no other code references 0xa46022, so the **seed is not saved in savegames** (HIGH: no save path references 0xa45fa8; see also the WorldFormats save docs).

**Consumers are interleaved.** One stream is shared by gameplay and by cosmetic systems: grass 0x464740, wind 0x4afd60, particles, idle-animation picks (IDLE2_CH/IDLE3_CH 0x429ec0, IDLE_CC 0x7c7ab0, STOP_CC 0x4533e0), and the AI think stagger. A bit-exact replay therefore needs every consumer reproduced in the same order. Gameplay-relevant uses:

| use | call | where |
|---|---|---|
| item drop roll at spawn | `random(100) <= DROP%` | Unit_init 0x4f8550 (Part A §2) |
| AI combo length | `random_range(0,3)` × 'A' | Unit_init 0x4f8550 |
| AI think stagger | `random(500)` ms | 0x49d810 |
| FATAL_KILL item | `random(100) < bonus` | 0x509950 |
| down-attack defence ignore | `random_range(50,100)` % | 0x5048b0 (inert while DEF_RAT=0) |
| AI block | `random(100)` vs BLOCKPER×diff | 0x512070 |
| knockdown lying time | `500 + random(2500)` ms | 0x501190 |
| arrow spread | ±`random(ARR_ACCU)` | 0x512130 |
| drop-item table pick | weighted | 0x7c6240 / DROPITEM.RES |
| critical sequence | random anim record until COMBOAVAIL=='Y' | 0x462970 |
| AI upgrade | random combo levels | 0x4a95a0 |

For deterministic simulation: seed explicitly, and keep cosmetic consumers on the same stream if replay compatibility with the original is required. Otherwise split them onto a separate generator. Splitting is a deliberate deviation.

---------------------------------------------------------------------------------------------------
## 3. GAMEPLAY.RES constants — master table (CONFIRMED)

`Resource/GAMEPLAY.RES` is a packed text file. A section index (`ECON`, `THROWI`, `UNIT`, `CHARMOVE`, `WEATHER`, `GAME`) is followed by `{SECTION}` blocks of `[KEY]value  // original #define`.

Loaders:
- `GameplayRes_load_unit_consts` 0x51c680: {UNIT}, {CRITICAL}, {DANGER}, {MINIMAP}, {EXP}, {R_ATT}, {C_BOUNS}, {KILL}.
- `GameplayRes_load_game_consts` 0x472ee0: {MUSIC}, {GAME}.
- `GameplayRes_load_charmove` 0x454090: {CHARMOVE}.
- 0x5525b0: {THROWI} (Part D §1).

Lookup: `GameplayRes_select_section` 0x47cbf0, then `GameplayRes_find_key` 0x47c180. The value is parsed at `line+9` with atoi (0x7b15cb) or atof (0x7b2110).

**Integer keys are parsed with atoi**, which truncates fractional values. The type column gives the store width (CONFIRMED from `mov ds:X,ax` / `eax` / `fstp`). "Used by" lists the non-loader functions that reference the global in the asm. **unused** = loaded but never read. **never loaded** = present in the file but the exe never asks for it.

Notable effects:
- **DEX_DAM 1.5 → 1** and **DEF_RAT 0.3 → 0**. DEF_RAT = 0 removes the DEF stat from every damage formula.
- **EASY_B..VDIFF_B are never read.** The loader reads the `_A` keys twice into two globals: guard chance 0x511f80 and BLOCKPER 0x512070.
- The comments' original defaults differ from the shipped values (for example INVC_DUR 3000 → 6000, MAX_W_C 40 → 30).

| section | key | file value | global | type | effective value | used by | original `#define` comment |
|---|---|---|---|---|---|---|---|
| UNIT | BODY_R | 32 | 0xa3cc9c | i32 | 32 | 0x4f7bf0 0x505dd0 | #define UNIT_BODY_RADIUS 32 |
| UNIT | MAX_W_C | 30 | 0xa3ccdc | i32 | 30 | 0x52daa0 | #define MAX_WAIT_COUNT  40// wait for 40 frame when the path |
| UNIT | INIT_W_C | 5 | 0xa3cd04 | i32 | 5 | 0x4c9930 0x4f8550 0x52daa0 | #define INIT_WAIT_COUNT  15 |
| UNIT | WK_DIST | 50 | 0xa3ccd8 | i32 | 50 | 0x52daa0 | unit walks if the path distance is less than 500 , otherwise |
| UNIT | HORSE_WD | 500 | 0xa3cbe0 | i32 | 500 | **unused** | horse walk distance  500 |
| UNIT | LD_FM_DT | 160000 | 0xa3cc74 | i32 | 160000 | 0x45de50 0x4be770 0x4fa0f0 | 40000 |
| UNIT | REACH_DT | 15 | 0xa3cc28 | i32 | 15 | 0x4bd420 0x4bdf90 0x4be770 0x4bed40 0x4bfa10 0x4bfd70 0x52daa0 0x535020 0x545ea0 | define how close to a pos that the unit stop  10 |
| UNIT | FELL_DUR | 2500 | 0xa3cc88 | i32 | 2500 | 0x501190 | stand up from knock down after 2.5 sec  2500 |
| UNIT | INVC_DUR | 6000 | 0xa3cbe4 | i32 | 6000 | 0x501190 | duration for getting rid of enemy attack  3000 |
| UNIT | STR_HP | 0 | 0xa3cc1c | i16 | 0 | 0x456070 0x4561d0 0x4949f0 0x50d640 | strength to max hit point ratio  3 |
| UNIT | DEF_HP | 0 | 0xa3ccba | i16 | 0 | 0x456330 0x4949f0 0x50d640 | defense to max hit point ratio  2 |
| UNIT | DEF_FALL | 0.2 | 0xa3ccd0 | f32 | 0.2 | 0x52c490 | DEFENSE_TO_TOLERANCE_FALLING_RATIO  0.2 |
| UNIT | DEF_HP_R | 0.1 | 0xa3cbf0 | f32 | 0.1 | 0x52c490 | DEFENSE_TO_HIT_POINT_RECOVERY   0.1f |
| UNIT | RECOV_HP | 10000 | 0xa3cc04 | i32 | 10000 | 0x52c490 | RECOVER_HP_TIME       10000 |
| UNIT | EGATK_MP | 40 | 0xa3cd28 | i32 | 40 | 0x437910 0x43a7d0 0x43acb0 0x4bca60 0x652c10 | #define ENERGY_ATTACK_MP 12 |
| UNIT | EGATK_HP | 20 | 0xa3cd1c | i32 | 20 | 0x437910 0x4bca60 0x652c10 | #define ENERGY_ATTACK_HP 12 |
| UNIT | HATK_MP | 6 | 0xa3cbd0 | i32 | 6 | **unused** | #define HEAVY_ATTACK_MP  6 |
| UNIT | RECOV_MP | 1 | 0xa3cd20 | i32 | 1 | 0x52c490 | #define RECOVER_MP   1 |
| UNIT | RECOV_T | 10000 | 0xa3ccac | i32 | 10000 | 0x52c490 | #define RECOVER_MP_TIME  10000 |
| UNIT | ARW_ATH | 10 | 0xa3cc40 | i32 | 10 | **unused** | #define MAX_ARROWS_ATTACHED    10 |
| UNIT | ARW_TIME | 30000 | 0xa3cc68 | i32 | 30000 | **unused** | #define ARROW_ATTACHED_TIME    30000 |
| UNIT | CLB_IDLE | 20 | 0xa3cce8 | i32 | 20 | 0x500ba0 0x518b90 | #define DEFAULT_AFTER_CLIMBING_IDLE_TIME 1000 |
| UNIT | MAX_COM | 2000 | 0xa3cc64 | i32 | 2000 | 0x456960 0x5229a0 | #define MAX_COMMAND_POINT 2000 |
| UNIT | COM_LIM | 200 | 0xa3cbc8 | i32 | 200 | **unused** | #define CHANGE_COMMAND_LIMIT 200 |
| UNIT | COM_TIME | 1000 | 0xa3ccc4 | i32 | 1000 | **unused** | #define CONSUME_COMMAND_POINT_TIME 10000 |
| UNIT | COM_CONS | 1 | 0xa3cbd8 | i32 | 1 | **unused** | #define COMMAND_POINT_CONSUME_PER_TIME 10 |
| UNIT | CRI_DIFF | 10 | 0xa3cc78 | i32 | 10 | 0x416680 | #define SHOW_CRITICAL_DIFF 10 |
| UNIT | CRITEXP | 3.0 | 0xa3cbd4 | f32 | 3.0 | 0x7c6240 | #define CRITICAL_KILL_EXP_GAIN 3.0 |
| UNIT | LEVELARR | 5000 | 0xa3cc58 | i32 | 5000 | 0x52c490 | #define LEVEL_ARROW_DURATION 5000 |
| UNIT | LEVSTEP | 300 | 0xa3cc14 | i32 | 300 | 0x704260 | #define LEVEL_ARROW_STEP_DURATION 300 |
| UNIT | AI_MOVE | 1.2 | 0xa3cc70 | f32 | 1.2 | 0x4f0b50 | #defien AI_MOVE_RATIO 1.3f |
| UNIT | MINDEXS | 30 | 0xa3cd00 | i32 | 30 | 0x4f0b50 | #define MIN_DEXTERITY_AFFECT_SPEED 30 |
| UNIT | MAXDEXS | 200 | 0xa3cbdc | i32 | 200 | 0x4f0b50 | #define MAX_DEXTERITY_AFFECT_SPEED 30 |
| UNIT | MOR_NOR | 50 | 0xa3cc8c | i32 | 50 | 0x49d5f0 0x509950 0x509e90 | #define MORALE_NORMAL  70 |
| UNIT | MOR_CH_U | 1.0 | 0xa3cc80 | f32 | 1.0 | 0x64f320 | #define MORALE_CHANGE_TO_ALL_UNIT  0.2f |
| UNIT | MOR_CH_T | 0.3 | 0xa3cd08 | f32 | 0.3 | 0x64f320 0x7c6240 | #define MORALE_CHANGE_TO_TROOP   0.3f |
| UNIT | WP_DAMG | 0 | 0xa3cd14 | i32 | 0 | 0x5104f0 | #define STANDARD_WEAPON_DAMAGE   10 |
| UNIT | AMR_DEF | 1 | 0xa3cc00 | i32 | 1 | 0x49d5f0 0x509950 0x509e90 | #define ARMOR_LEVEL_DEFENSE    10 |
| UNIT | AMREXDEF | 0.5 | 0xa3cd0c | f32 | 0.5 | 0x509e90 | #define ARMOR_3_DEFEND_EXPLOSIVE_RATIO 0.5 |
| UNIT | SIE_UDAM | 1.5 | 0xa3cbec | f32 | 1.5 | 0x509e90 | #define SIEGE_WEAPON_UNIT_DAMAGE_RATIO 1.5 |
| UNIT | F_ATTACK | 130 | 0xa3ccf4 | i32 | 130 | 0x49d5f0 0x509950 0x509e90 | #define FRIENDLY_ATTACK_BONUS   130 |
| UNIT | F_DEFEND | 70 | 0xa3cc2c | i32 | 70 | 0x49d5f0 0x509950 0x509e90 | #define FRIENDLY_DEFEND_BONUS   70 |
| UNIT | IDLE2_CH | 0.2 | 0xa3cc94 | f32 | 0.2 | 0x429ec0 | #define DEFAULT_IDLE_2_CHANCE   0.2f |
| UNIT | IDLE3_CH | 0.4 | 0xa3cc30 | f32 | 0.4 | 0x429ec0 | #define DEFAULT_IDLE_3_CHANCE   0.4f |
| UNIT | IDLE_CC | 0.01 | 0xa3ccf0 | f32 | 0.01 | 0x7c7ab0 | #define  DEFAULT_IDLE_CHANGE_CHANCE  0.05f |
| UNIT | STOP_CC | 0.3 | 0xa3cce4 | f32 | 0.3 | 0x4533e0 | #define DEFAULT_STOP_CHANCE    0.3f |
| UNIT | ARR_ACCU | 50 | 0xa3cca4 | i32 | 50 | 0x512130 | #define ARROW_ACCURACY_DISTANCE   50 |
| UNIT | BLOCKPER | 50 | 0xa3cc90 | i32 | 50 | 0x512070 | #define BLOCK_PERCENTAGE   50 |
| UNIT | BLOCKANG | 70 | 0xa3ccb0 | i32 | 70 | 0x5048b0 0x5051e0 | #define BLOCK_ANGLE   70 |
| UNIT | ARR_FALL | 25 | 0xa3cc08 | i32 | 25 | 0x75d540 | #define ARROW_FALLING   14 |
| UNIT | T_FALLF | 6000 | 0xa3cc84 | i32 | 6000 | 0x75d540 | #define TOTAL_FALLING_FORCE  6000 |
| UNIT | V_FALLF | 3000 | 0xa3ccc0 | i32 | 3000 | 0x75d540 | #define VERTICAL_FALLING_FORCE  3000 |
| UNIT | F_FALLR | 250 | 0xa3cc34 | i32 | 250 | 0x75d540 | #define FORCE_TO_FALLING_RATIO  250 |
| UNIT | LINF_DI | 10 | 0xa3ccd4 | i16 | 10 | 0x4f8550 | #define LIGHT_INF_DROP_ITEM_PERCENTAGE  10 |
| UNIT | LCAP_DI | 100 | 0xa3cc1a | i16 | 100 | 0x4f8550 | #define LIGHT_CAP_DROP_ITEM_PERCENTAGE  20 |
| UNIT | HINF_DI | 12 | 0xa3cc5c | i16 | 12 | 0x4f8550 | #define HEAVY_INF_DROP_ITEM_PERCENTAGE  30 |
| UNIT | HCAP_DI | 100 | 0xa3cbe8 | i16 | 100 | 0x4f8550 | #define HEAVY_CAP_DROP_ITEM_PERCENTAGE  50 |
| UNIT | ARC_DI | 8 | 0xa3cd18 | i16 | 8 | 0x4f8550 | #define ARCHER_DROP_ITEM_PERCENTAGE  30 |
| UNIT | ACAP_DI | 100 | 0xa3ccca | i16 | 100 | 0x4f8550 | #define ARCHER_CAP_DROP_ITEM_PERCENTAGE  50 |
| UNIT | CRO_DI | 8 | 0xa3cc7c | i16 | 8 | 0x4f8550 | #define CROSSBOW_MAN_DROP_ITEM_PERCENTAGE  30 |
| UNIT | CCAP_DI | 100 | 0xa3cbcc | i16 | 100 | 0x4f8550 | #define CROSSBOW_MAN_CAP_DROP_ITEM_PERCENTAGE  50 |
| UNIT | POLE_DI | 10 | 0xa3ccb8 | i16 | 10 | 0x4f8550 | #define POLEMAN_DROP_ITEM_PERCENTAGE  30 |
| UNIT | PCAP_DI | 100 | 0xa3cca0 | i16 | 100 | 0x4f8550 | #define POLEMAN_CAP_DROP_ITEM_PERCENTAGE  50 |
| UNIT | MACE_DI | 8 | 0xa3cca2 | i16 | 8 | 0x4f8550 | #define MACEMAN_DROP_ITEM_PERCENTAGE  30 |
| UNIT | MCAP_DI | 100 | 0xa3cc38 | i16 | 100 | 0x4f8550 | #define MACEMAN_CAP_DROP_ITEM_PERCENTAGE  50 |
| UNIT | PEFF_R | 7 | 0xa3ccc8 | i16 | 7 | 0x501190 | #define PARTICLE_EFFECT_RATIO   10 |
| UNIT | HP_PER | 4 | 0xa3cc7e | i16 | 4 | 0x50d640 | #define HP_PER_POINT     20 |
| UNIT | EP_PER | 3 | 0xa3cc4c | i16 | 3 | 0x50d640 | #define EP_PER_POINT     5 |
| UNIT | STR_PER | 1 | 0xa3cd1a | i16 | 1 | 0x50d640 | #define STRENGTH_PER_POINT   1 |
| UNIT | DEF_PER | 1 | 0xa3cc1e | i16 | 1 | 0x50d640 | #define DEFENCE_PER_POINT   1 |
| UNIT | DEX_PER | 1 | 0xa3cc18 | i16 | 1 | 0x50d640 | #define DEXTERITY_PER_POINT  1 |
| UNIT | LDS_PER | 1 | 0xa3cc10 | i16 | 1 | 0x50d640 | #define LEADERSHIP_PER_POINT  1 |
| UNIT | STR_DAM | 3.0 | 0xa3cce2 | i16 | 3 | 0x5104f0 | #define STRENGTH_DAMAGE_RATIO         2 |
| UNIT | DEX_DAM | 1.5 | 0xa3cca8 | i16 | **1** (atoi) | 0x510bf0 | #define DEXTERITY_DAMAGE_RATIO         2 |
| UNIT | DEF_RAT | 0.3 | 0xa3cce0 | i16 | **0** (atoi) | 0x49d5f0 0x509950 0x509e90 | #define DEFENSE_RATIO         0.3 |
| UNIT | UPG_TIME | 600000 | 0xa3cc24 | i32 | 600000 | 0x52c490 | time before ai use upgrade points |
| UNIT | BRI_COMM | 5 | 0xa3cd10 | i32 | 5 | 0x4e8500 0x6d23b0 | DEFAULT_GOLD_PER_BRIBE_PER_COMMON_UNITS |
| UNIT | BRI_CAPT | 10 | 0xa3cc54 | i32 | 10 | 0x4e8500 0x6d23b0 | DEFAULT_GOLD_PER_BRIBE_PER_CAPTAIN_UNITS |
| CRITICAL | FADE_IN1 | 200 | — | — | — | **never loaded** | Fade in duration 1  200 |
| CRITICAL | FADE_IN2 | 200 | — | — | — | **never loaded** | Fade in duration 2  200 |
| CRITICAL | FADE_DUR | 600 | — | — | — | **never loaded** | Fade in duration  600 |
| CRITICAL | FADE_OUT | 350 | — | — | — | **never loaded** | Fade out duration  350 |
| CRITICAL | GLOW_IN1 | 200 | — | — | — | **never loaded** | Glow in duration 1  200 |
| CRITICAL | GLOW_IN2 | 200 | — | — | — | **never loaded** | Glow in duration 2  200 |
| CRITICAL | GLOW_IN3 | 200 | — | — | — | **never loaded** | Glow in duration 3  200 |
| DANGER | INDANGER | 0.2 | 0xa3cd24 | f32 | 0.2 | 0x4fd980 0x6f9b60 | Define hit point level persentage of "in danger" 0-1 |
| MINIMAP | VISION_R | 2000 | 0xa3cc44 | i32 | 2000 | 0x4f8550 | The range that enemy unit can be shown on minimap |
| EXP | MEM_EXP | 0.4 | 0xa3ccbc | f32 | 0.4 | 0x5229a0 | Experience to team member percentage (from team leader) |
| EXP | LD_EXP | 0.4 | 0xa3ccec | f32 | 0.4 | 0x5229a0 | Experience to team leader percentage (from team member) |
| R_ATT | FALL_S | 800 | 0xa3cc0c | i32 | 800 | 0x509e90 | Distance for arrows at which the damage starts to fall |
| R_ATT | FALL_E | 2400 | 0xa3cc6c | i32 | 2400 | 0x509e90 | Distance for arrows at which the damage is zero |
| R_ATT | DIST_P | 0.5 | 0xa3cc20 | f32 | 0.5 | 0x509e90 | Max penalty on damage over distance |
| C_BOUNS | C_BONUSL | 50 | 0xa3cccc | i32 | 50 | 0x7c6240 |  |
| C_BOUNS | E_BONUSL | 100 | 0xa3ccb4 | i32 | 100 | 0x7c6240 |  |
| C_BOUNS | M_BONUSL | 3 | 0xa3cc3c | i32 | 3 | 0x7c6240 |  |
| KILL | EP_NOR | 6 | 0xa3cc60 | i32 | 6 | 0x7c6240 |  |
| KILL | EP_CAP | 15 | 0xa3cc48 | i32 | 15 | 0x7c6240 |  |
| KILL | EP_GEN | 30 | 0xa3cc50 | i32 | 30 | 0x7c6240 |  |
| CHARMOVE | AIM_R | 25.0 | 0xa3373c | f32 | 25.0 | 0x42c260 | #define AIMING_RANGE  50.0f |
| CHARMOVE | AIM_MXD | 2500.0 | 0xa33758 | f32 | 2500.0 | 0x42c260 0x42c830 | #define AIMING_MAX_DISTANCE 1500.0f |
| CHARMOVE | AIM_HA | 60.0 | 0xa33754 | f32 | 60.0 | 0x42bcd0 0x42c260 0x42c830 0x42cb20 | #define AIMING_HALF_ANGLE 60.0f |
| CHARMOVE | TURN_ANG | 6.0 | 0xa33740 | f32 | 6.0 | 0x43dab0 | #define HORSE_TURN_MAX_ANGLE  5.0f |
| CHARMOVE | JUMPMAXD | 100 | 0xa3374c | i32 | 100 | 0x430480 0x432d30 0x4343e0 0x437b60 | #define JUMPING_BUTTON_MAX_DURATION 300 |
| CHARMOVE | JUMPMIND | 100 | 0xa33748 | i32 | 100 | **unused** | #define JUMPING_BUTTON_MIN_DURATION 100 |
| CHARMOVE | CHAINPRO | 200 | 0xa33744 | i32 | 200 | 0x438820 | #define CHAIN_ATTACK_TIME_PROPORTION 200 |
| CHARMOVE | CRI_TIME | 10000 | 0xa33750 | i32 | 10000 | 0x45fae0 | #define CRITICAL_TIMEOUT 10000//---- weather effect constant |
| GAME | EASY_D | 0.7 | 0xa391ac | f32 | 0.7 | 0x5104f0 0x510bf0 |  |
| GAME | NORM_D | 1.0 | 0xa3918c | f32 | 1.0 | 0x5104f0 0x510bf0 |  |
| GAME | DIFF_D | 1.5 | 0xa391b0 | f32 | 1.5 | 0x5104f0 0x510bf0 |  |
| GAME | VDIFF_D | 2.0 | 0xa39194 | f32 | 2.0 | 0x5104f0 0x510bf0 |  |
| GAME | EASY_A | 0.5 | 0xa391a4 + 0xa39184 | f32 | 0.5 | 0x512070 0x511f80 |  |
| GAME | NORM_A | 1.0 | 0xa39188 + 0xa3919c | f32 | 1.0 | 0x512070 0x511f80 |  |
| GAME | DIFF_A | 1.5 | 0xa391a0 + 0xa391a8 | f32 | 1.5 | 0x512070 0x511f80 |  |
| GAME | VDIFF_A | 2.0 | 0xa39198 + 0xa39190 | f32 | 2.0 | 0x512070 0x511f80 |  |
| GAME | EASY_B | 0.5 | — | — | — | **never loaded** |  |
| GAME | NORM_B | 1.0 | — | — | — | **never loaded** |  |
| GAME | DIFF_B | 1.5 | — | — | — | **never loaded** |  |
| GAME | VDIFF_B | 2.0 | — | — | — | **never loaded** |  |

THROWI (projectile physics) keys: Part D §1. Economy, weather and cloud keys are out of scope.

## 4. Part A — Unit object, data records, regen, XP and levels

Conventions: offsets are BYTE offsets from the Unit `this` (ECX). Decompiler `short*` indexing (`p[0x8c]`) has been converted (×2).
"rec" = unit-class record returned by `FUN_0042d0a0(unit+0x3b)` (UnitRes, loaded by 0x525080 from `unit/unit.res`).
Timers are ms; ticks = ms/33.33. `now` = `DAT_00a500ba` (game time ms), `dt` = `DAT_00a500be`.
`ftol` = MSVC `__ftol` (0x7b26cc) = truncation toward zero.
Player hero recno = `DAT_00a5b9e8` (compared against unit+0x00).

### 1. Unit object (size ≥ 0x432; plain struct, no vtable)

| off | type | meaning | evidence | conf |
|---|---|---|---|---|
| +0x000 | short | unit recno (own id in UnitArray 0xa47c08); compared with player recno `DAT_00a5b9e8` | 0x5229a0 `*this==DAT_00a5b9e8`; 0x522c40; 0x4fc220 `param==*this` | CONFIRMED |
| +0x002 | short | nation recno (side). `FUN_0047b600(nation)+0x14` = nation name ("English"/"French"); set by 0x509250 (script set_unit_nation); 0 = none | 0x509250, 0x655750, 0x7c6240 | CONFIRMED |
| +0x00a | int | current animgrp record index (into unit's anim group, `FUN_004168d0(idx)`) | written 0x4ff9f0 (`mov [edx+0xa],param1` @0x4ffade); read 0x7c6240 `FUN_004168d0(killer+0xa)` | CONFIRMED |
| +0x00e | float[3] | world position x,y,z | getter 0x4fedb0, setter 0x4fedf0 (→actor 0x6aee40 if actor else store + 0x4fee60) | CONFIRMED |
| +0x01a | float | yaw / facing in RADIANS | getter 0x4ff850, setter 0x4ff870; script set_unit_dir 0x655590 passes deg*0.017453292 | CONFIRMED |
| +0x01e | float | yaw in DEGREES (mirror of +0x1a) | setter 0x50abd0, getter 0x50ac00; 0x454b20 sets `rad/0.017453292` | CONFIRMED |
| +0x022 | int | LOD tier (1..4; phase-2 branches on it) | 0x52c490 phase 2 | HIGH |
| +0x02a | int | timestamp of last phase-1 process (`=now`) | 0x52c490 end of phase 1 | CONFIRMED |
| +0x031 | short | team id (troop); members of same troop share it | 0x657fd0 set_unit_team; 0x7c6240 morale loop | CONFIRMED |
| +0x033 | int | ptr team object if this unit is the team LEADER (else 0). team+2 = char member count, team+3 = short[] member recnos | 0x44a660 `is_leader = +0x33!=0`; 0x5229a0, 0x522c40 loops | CONFIRMED |
| +0x037 | short | leader recno (for members) | 0x4216d0 get_leader (returns self if leader) | CONFIRMED |
| +0x03b | short | unitres id (rec index for FUN_0042d0a0) | 0x4f8550 first store; 0x65c2e0 set_unit_id | CONFIRMED |
| +0x03d | char[0x28+] | display Name (strcpy from rec+0xb4) | 0x4f8550; getter 0x4fe4f0 returns this+0x3d | CONFIRMED |
| +0x07f | char[0x20+] | ShortName (rec+0xde) | 0x4f8550 | CONFIRMED |
| +0x0c1 | char[0x20+] | Title (rec+0x129) | 0x4f8550 | CONFIRMED |
| +0x102 | short | 1 = currently player-controlled unit (switch_controllable_unit) | writers 0x42f9c0 (called from 0x432300), 0x41bf40; 0x501190 checks `player && +0x102==0` | HIGH |
| +0x104 | int | RANK (rec Ranking enum: 0 INFANTRY,1 ARCHER,2 CAVALRY,3 CAPTAIN,4 GENERAL,5 KING). Init 0 in Unit_init, then set to rec+0x149 by creator via 0x5089e0 | 0x5089e0, callers 0x467590/0x558a60 pass rec+0x149; nation leader forced 4 (0x4b5630) | CONFIRMED |
| +0x108 | short | KingdomId (rec+0x14b) | 0x4f8550 | CONFIRMED |
| +0x10a | short | RaceId (rec+0x14d) | 0x4f8550 | CONFIRMED |
| +0x10c | int | training mode flag (set_unit_training_mode). On set: saves lives to +0x110, lives=1, full HP/EP; on clear restores lives | 0x510030 | CONFIRMED |
| +0x110 | int | saved lives during training | 0x510030 | CONFIRMED |
| +0x114 | int | retreat state (script set/get_unit_retreat). -1 disables HP regen; 1 → on death: survive with HP 1 & run-away (0x7c6240), then set to -1; -2 special | 0x656210/0x656260, 0x52c490, 0x7c6240 | CONFIRMED (semantics HIGH) |
| +0x118 | int | HP-regen timestamp | 0x52c490 | CONFIRMED |
| +0x11c | int | Life (rec+0x159) copy #1 (max lives?) | 0x4fe640 | CONFIRMED store / HYPOTHESIS meaning |
| +0x120 | int | lives remaining (rec "Life"). HP setter: when HP hits 0 and lives≠0 → lives--, HP=maxHP. is_dead = HP<1 && lives==0 | 0x4fe670, 0x4fe510, 0x42d0e0 | CONFIRMED |
| +0x124 | short | LEVEL (1..30) | 0x4f8550 (rec+0x199), 0x522c40, script get_unit_level 0x65c1a0 | CONFIRMED |
| +0x126 | int | EXPERIENCE (cumulative) | 0x5229a0 `+0x126 += xp`; 0x522c40 compare | CONFIRMED |
| +0x12a | short | SKILL points (level table SKI_POINT; spent on NEW_COMBO / skill upgrades) | 0x522c40, 0x50d640 case 8, 0x4949f0 | CONFIRMED |
| +0x12c | short | ATTRIBUTE points (level table ATT_POINT; spent on STR/DEX/DEF/LEAD/HP/EP) | 0x522c40, 0x50d640 | CONFIRMED |
| +0x12e | short | rec+0x19b (never written by loader → 0) | 0x4f8550, setter 0x4fe860 | UNKNOWN |
| +0x130 | byte | level-up arrow visible flag (cleared LEVELARR=5000 ms after +0x131) | 0x522c40, 0x52c490 | CONFIRMED |
| +0x131 | int | level-up timestamp | 0x522c40 | CONFIRMED |
| +0x135 | int | current HP (setter 0x4fe510 clamps [0, maxHP()]) | 0x4fe510 | CONFIRMED |
| +0x139 | int | HP mirror (set to maxHP at init/level-up, to HP on life-revive) — probably HP-bar trail | 0x4f8550, 0x522c40, 0x4fe510 | HYPOTHESIS |
| +0x13d | int | base max HP (getter 0x523320 adds item effect 1 MAX_HIT_POINT). Setter 0x45b720: raising maxHP also heals by the delta | 0x523320, 0x45b720 | CONFIRMED |
| +0x141 | int | RegenHp (rec+0x1a5) — NOT used by regen code found | 0x4f8550, setter 0x4fea20 | CONFIRMED store / UNKNOWN use |
| +0x145 | int | current EP (setter 0x4fe5d0 clamps [0, maxEP()]) | 0x4fe5d0 | CONFIRMED |
| +0x149 | int | base max EP (getter 0x523700 adds item effect 2). Setter 0x45bba0 | 0x523700 | CONFIRMED |
| +0x14d | int | RegenEp (rec+0x1a7) — not used by regen | 0x4f8550, setter 0x4fea50 | CONFIRMED store |
| +0x151 | int | EP-regen timestamp | 0x52c490 | CONFIRMED |
| +0x155 | short | base STR (getter 0x5237e0 + item 3; setter 0x4fe6e0) | | CONFIRMED |
| +0x157 | short | base DEX (getter 0x523860 + item 4; setter 0x4fe710) | | CONFIRMED |
| +0x159 | short | base DEF (getter 0x5238c0 + item 5; setter 0x4fe740) | | CONFIRMED |
| +0x15b | short | base LEADERSHIP (getter 0x523920 + item 6; setter 0x4fe770) | | CONFIRMED |
| +0x15d | short | EArmorId (rec+0x16d) | 0x4f8550 | CONFIRMED |
| +0x15f | short | armor item id (rec+0x16f EArmorCode; script get/set_unit_armor_id) | 0x4f8550, 0x6562b0/0x656300 | CONFIRMED |
| +0x161 | short | Loyalty (rec+0x19d); setter 0x4fe890 | 0x4f8550 | CONFIRMED |
| +0x163 | short | MORALE 0..100 (rec+0x19f); setter 0x4fe8c0 (see §5) | 0x4f8550, 0x4fe8c0, 0x7c6240 | CONFIRMED |
| +0x165 | short | Reputation (rec+0x1a1); setter 0x4fe9c0 | | CONFIRMED |
| +0x167 | int | unit mode: 1 ATTACK, 2 ACTIVE, 3 IDLE (script set_unit_mode); rec Behavior 1..4 copied (all units have Behavior 0 → not written by init) | 0x4f8550 switch, 0x6515d0 | CONFIRMED |
| +0x16b | int | target-selection behaviour: 1 NEAREST (init), 2 WEAKEST, 3 IMPORTANT, 4 NEAREST_ARCHER, 5 NEAREST_HIGH_PRIORITY | 0x4f8550, 0x651690 | CONFIRMED |
| +0x16f | int | ArcherySkill (rec+0x183) | setter 0x4fe7a0 | CONFIRMED |
| +0x173 | int | HorseRiding (rec+0x185) | 0x4fe7d0 | CONFIRMED |
| +0x177 | int | SwimmingSkill (rec+0x187) | 0x4fe800 | CONFIRMED |
| +0x17b | int | ElephantRiding (rec+0x189) | 0x4feb80 | CONFIRMED |
| +0x17f | int | WideAnimal (rec+0x18b) | 0x4febb0 | CONFIRMED |
| +0x183 | int | Preaching (rec+0x18d) | 0x4febe0 | CONFIRMED |
| +0x187 | int | PowerJump (rec+0x18f) | 0x4fec10 | CONFIRMED |
| +0x18b | int | Disguise (rec+0x191) | 0x4fec40 | CONFIRMED |
| +0x18f | int | Persuade (rec+0x193) | 0x4fec70 | CONFIRMED |
| +0x193 | int | LockPicking (rec+0x195) | 0x4feca0 | CONFIRMED |
| +0x197 | int | Mountain (rec+0x197) | 0x4fecd0 | CONFIRMED |
| +0x1aa | ptr | upgrade list (DynArray of 8-byte entries {short upitemId; short unlocked; short applied; short comboLevel}), created by 0x5313d0 (`FUN_005843d0(8,0x1e,0)`) | Unit_init asm 0x4f9111 `add ecx,0x1aa` → 0x531360/0x5313a0/0x531520 | CONFIRMED |
| +0x1ae | short | medal count (+1 per normal kill, +10 per kill of rank≥3) — script get_unit_medal_number | 0x7c6240 @0x7c6d66, 0x65c0d0 | CONFIRMED |
| +0x1b0 | short | kill count | 0x7c6240, 0x65c130 (killed = +0x1b0 + +0x1b2) | CONFIRMED |
| +0x1b2 | short | officer kill count (victim rank ≥3) | 0x7c6240 | CONFIRMED |
| +0x1b4 | byte | "critical kill" flag on killer → XP ×CRITEXP | 0x7c6240 @0x7c6e43 | CONFIRMED |
| +0x1b5/+0x1b7 | short | zeroed in init | 0x4f8550 | UNKNOWN |
| +0x1b9 | short | (on leader) troop members lost | 0x7c6240 victim's leader ++ | CONFIRMED |
| +0x1bb | short | (on leader) kills by troop | 0x7c6240 killer's leader ++ | CONFIRMED |
| +0x1bd/+0x1bf | short | two attached world-item recnos (faded out on death) | setter 0x4fe6a0; 0x7c6240 | HYPOTHESIS |
| +0x1c1 | ptr | inventory (0x1e bytes, see §1b) | 0x4f8550 | CONFIRMED |
| +0x1c5 | short | drop item id (rolled at init from DropItemList, script set_unit_drop_item_id) | 0x4f8550, 0x653210 | CONFIRMED |
| +0x1c7 | int | drop item level/value (item res +0x36) | 0x653210 | CONFIRMED |
| +0x1cb | short | FALL (knock-down) gauge 0..100, init = rec "Falling" (=20 for all units). Reaching 0 → knock-down (riding: action 0x4e H_DROP; else 0x509000) | setter 0x4fea80; 0x52c490 regen | CONFIRMED |
| +0x1cd | int | fall-gauge regen timestamp | 0x52c490 | CONFIRMED |
| +0x1d1 | int | knock-down remaining ms (on expiry gauge reset to 100) | 0x7c7ab0 | HIGH |
| +0x1d5 | short | saved in hero snapshot | 0x462610 | UNKNOWN |
| +0x1dd | int | countdown ms (−dt each phase 1); set 0x7fffffff on retreat-death | 0x52c490, 0x7c6240 | UNKNOWN purpose |
| +0x1e2 | int | recno of horse being ridden (get_unit_riding_recno / get_unit_horse_recno) | 0x655990, 0x655b20, 0x4fc220 | CONFIRMED |
| +0x1e6 | int | getter 0x4f2170 | | UNKNOWN |
| +0x1f5 | int | recno of unit this one is attached to (yaw copied from it each tick) | 0x454b20, getter 0x4f2f60 | HIGH |
| +0x1f9 | int | recno of unit attached to this one (horse→rider); getter 0x508f80 | 0x7c6240, 0x454b20 | HIGH |
| +0x20f | int | climb-idle timestamp (cleared after CLB_IDLE) | 0x518b90 | HIGH |
| +0x213 | short | animation POSITION (0 STAND, 1 JUMP) | 0x5124b0 anim lookup, setter 0x500d10 | CONFIRMED |
| +0x215 | short | current ACTION (= animgrp MOVEMENT enum, §1c). Init 1 IDLE; set via 0x501190(action,…) | 0x4f8550, 0x501190, 0x508fb0 | CONFIRMED |
| +0x217 | short | movement of anim currently played | 0x5124b0 | CONFIRMED |
| +0x219 | short | previous movement | 0x5124b0 | CONFIRMED |
| +0x21b | short | ITEM_USING (weapon anim set: 0 DEFAULT,1 1SWORD(init),2 2SWORD,3 2HAMMER,4 2SPEAR,5 2STAFF,6 2FLAIL,7 BOW,8 CROSSBOW,9 SHIELD) | 0x5124b0 cmp rec[4]; enum 0x416910 | CONFIRMED |
| +0x21d | short | anim SPECIAL key (init 0) | 0x5124b0 cmp rec[5] | CONFIRMED |
| +0x227..+0x235 | bytes | AI combo buffer: +0x22d.. = 'A' × n, +0x234 = n = RNG lo_hi(0,3) (0x590690), +0x227/+0x22d init 0xff | 0x4f8550 | HYPOTHESIS |
| +0x299 | int | script duration of rec ScriptFile (script+0x22) | 0x4f8550 | HIGH |
| +0x2d7 | short | target unit recno (script set_unit_target_unit_recno) | 0x6558d0 | CONFIRMED |
| +0x2df | short | guard/block chance base: 30 (UnitClass ARCHER/CROSSBOW), 50 other, 90 (rank 4/5); +5/+10 on hits, ×2 in 0x509000; getter 0x511f80 = ftol(v × difficulty mult 0xa39184/0xa3919c/0xa391a8/0xa39190 by config 0xa57fc2) | 0x511e70, 0x511f80 | HIGH |
| +0x2e3 | short | interact unit recno | 0x655bd0 | CONFIRMED |
| +0x2eb | short | bribe state (2 = bribed) | 0x6561b0 | CONFIRMED |
| +0x2f1 | ptr | animgrp table object (+0x2c count, +0x30 records 0x99 each) | 0x5124b0 | CONFIRMED |
| +0x2f5 | ptr | actor/model; actor+0x6c/0x70/0x74 = velocity vector (set 0x505d90), actor+0xb8 collision type | 0x505d90, 0x52daa0 | HIGH |
| +0x31d | ptr | HP-bar object (+0x52 hide flag) | 0x652de0 | CONFIRMED |
| +0x335 | ptr | world ptr (unit processed only if == DAT_00a45f3e) | 0x4fed00, 0x52c490 | CONFIRMED |
| +0x378/+0x37c | ptr | attach-point arrays (rec AttachPointCount ×4 bytes) | 0x4f8550 | CONFIRMED |
| +0x3bc | int | AI auto-upgrade timestamp (set at level-up; after UPG_TIME → 0x4a95a0) | 0x522c40, 0x52c490 | CONFIRMED |
| +0x3c1 | int | vision range (init VISION_R=2000; set_unit_vision_range) | 0x4f8550, 0x65f980 | CONFIRMED |
| +0x3f6/+0x3fa | int | = INIT_W_C (5) | 0x4f8550 | UNKNOWN |

Other init-only fields of 0x4f8550 (zeroed, purpose not traced): +0x23f, +0x2e7, +0x32d/+0x331 (effect handles), +0x351, +0x355, +0x356, +0x365..+0x36d (vec3 = DAT_00a69cd8..), +0x380=-1, +0x38c=1, +0x3c0=1, +0x3e5=1, +0x3e6, +0x3ea.

Gold: NOT per unit. Party gold = global `DAT_00a596c4` (script get_unit_gold 0x65c200 / add_gold 0x65c220, clamp 99,999,999 in 0x4566f2). CONFIRMED.

#### 1b. Inventory object (unit+0x1c1, 0x1e bytes, ctor 0x495950 memset 0)
| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | short | weapon mode: 0 none, 1 melee (from PWeaponId), 2 ranged-only (PRangeWeaponId when no melee) | 0x4f8550, 0x4974f0 |
| +0x02 | ptr | melee slot 0 item instance (primary weapon) | 0x4970c0(1,0,…) |
| +0x06 | ptr | melee slot 1 (secondary/shield) | 0x4970c0(1,1,…) |
| +0x0a | ptr | ranged weapon instance (item class 0xb/0xc) | 0x497270, getter 0x497560 |
| +0x0e | ptr | ammo instance (SRangeWeaponId); ammo count = inst+2 (script get_unit_arrow_quantity) | 0x4973a0, 0x497580, 0x65c3a0 |
| +0x12/+0x16/+0x1a | ptr | bag objects (0x3e bytes; 0x457290 (1,4,1)/(1,6,8)/(1,6,4)); +0x16/+0x1a forced for rank>2 (0x495b40) | 0x4959b0, 0x495b40 |
Slot getter 0x4974f0(slot): mode 0→0; mode 2→+0x0a; mode 1→ptr at +0x02+slot*4. CONFIRMED.
Item instance (0x12 bytes): +0 short itemId, +2 short quantity (ammo count = itemres+0x2a at init), +4 short level/param (itemres+0x36), +6/+0xa/+0xe int. CONFIRMED (0x4970c0, 0x4973a0).

#### 1c. Action / MOVEMENT enum (+0x215, +0x217; parser 0x415820) — CONFIRMED
0 DEFAULT,1 IDLE,2 IDLE2,3 IDLE3,4 IDLE_BAT,5 IDLEUP,6 IDLEDN,7 IDLE_T,8 JATTBI,9 TIPTOE,10 WALK,11 RUN,12 T_LEFT,13 T_RIGHT,14 MOVE_L,15 MOVE_R,16 MOVE_B,17 MOVE_FL,18 MOVE_FR,19 MOVE_BL,20 MOVE_BR,21 JUMP_B,22 JUMP_R,23 JUMP_L,24 DOWN,25 JATTBD,26 UP,27 CHAIN,28 ATTACK,29 JATTACKB,30 ENGATK,31 ENGATK2,32 ENGATK3,33 ENGATK4,34 DOWNATK,35 ATTACKED,36 ATTKED2,37 ATTKEDR,38 BATTKED,39 LATTKED,40 RATTKED,41 DNATKED,42 DIE,43 GUARD1,44 GUARD2,45 GUARDR,46 BACK,47 CLIMB,48 SHTLOAD,49 SHTLOADC,50 SHTLOAD2,51 SHTOUT,52 SHTOUT2,53 SHTOUTC,54 SHTIDLE,55 SHTIDLEC,56 SHTOUTH,57 SHTIDLEH,58 THROW,59 THROWN,60 STANDUP,61 DIZZY,62 STOP,63 STOP_B,64 KNOCKDN,65 KNOCKDN2,66 KNOCKDN3,67 H_IDLE,68 H_WALK,69 H_BACK,70 H_RUN,71 H_STOP_B,72 H_STOP,73 H_ATTACK,74 H_HITED,75 H_BHITED,76 H_LHITED,77 H_RHITED,78 H_DROP,79 H_MNT_L,80 H_MNT_R,81 H_DMNT_L,82 H_DMNT_R,83 RUSH,84 PUSH,85 PUSH2,86 PUSH_L,87 PUSH_R,88 BATTER_C,89 C_IDLE_A,90 C_IDLE_B,91 C_DN_AB,92 C_DN_AS,93 C_DN_BA,94 C_DN_CA,95 C_DN_CB,96 C_DN_SC,97 C_UP_AB,98 C_UP_AC,99 C_UP_BA,100 C_UP_BC,101 C_UP_CS,102 C_UP_SA,103 C_DROP,104 CA_IDLE,105 CA_RUN,106 CA_WALK.
`is_knocked_down` 0x508fb0 = action in [64,66]. POSITION enum (0x4157c0): STAND 0, JUMP 1.

#### 1d. Animgrp record (0x99 bytes, `FUN_004168d0(i)` = base+(i-1)*0x99; loader 0x414a30 `animgrp.cpp`, columns of std_set/<UNIT>.csv)
| off | type | column |
|---|---|---|
| +0x00 | int | POSITION enum |
| +0x04 | int | MOVEMENT enum |
| +0x08 | char[6] | COMBO string (A, AA, AB, …) |
| +0x0e | char | COMBOAVAIL (Y/N) |
| +0x0f | char | CHAINAVAIL |
| +0x10 | int | ITEM_USING enum |
| +0x14 | int | SPECIAL enum (0x416aa0) |
| +0x18 | int | ENERGY_PT (EP cost) |
| +0x1c | int | DAMAGE (percent multiplier used by melee damage 0x5104f0: `…*rec[+0x1c]/100`) |
| +0x20/+0x24/+0x28 | int | BLEND_TIME / START_TIME / END_TIME (ms) |
| +0x2c | char[0x20] | UPPERFILE; +0x6e anim handle |
| +0x4d | char[0x20] | UPPERNAME |
| +0x72 | int | SCRIPT id (0x41a0e0) — hit script: `FUN_00416f20(id)+0x4c` knockback dist (default 3000), `+0x50` lift (default 500) (0x7c6240) |
| +0x76/+0x7a | int/char[8] | SOUND id / name |
| +0x83 | int | SOUND_TIME |
| +0x87/+0x8b | int/char[8] | SOUND2 id / name |
| +0x94 | int | SOUNDTIME2 |
| +0x98 | byte | SHOW_CRIT (critical icon) |
All CONFIRMED from 0x414a30 field widths/order.

### 2. Unit-class record (UnitRes, loader 0x525080 `d:\ww\unitres.cpp`, file `unit/unit.res` "Key : value" text)
Loader reads key token then value token in fixed order (CONFIRMED by token sequence vs. file text).

| off | type | unit.res key | parse | copied to unit |
|---|---|---|---|---|
| +0x002 | short | UnitClass: CITIZEN0 LIGHT_INFANTRY1 HEAVY_INFANTRY2 POLEMAN3 MACEMAN4 ARCHER5 CROSSBOW_MAN6 CAVALRY7 SIEGE_WEAPON8 CART9 (default 1) | 0x527220 | (drop-chance class) |
| +0x004 | char[0x20] | Code | strncpy | |
| +0x025 | char[0x20] | Model | | +0x84 model handle, +0x90 anim id (Animation) |
| +0x046 | char[0x20] | HighPolygonModel; +0x70 anim, +0x6c handle | | |
| +0x068 | int | ShadowModel handle | | |
| +0x074 | int | ModelLevel (n extra ModelName/AnimationName pairs → +0x88+i*4 handle, +0x94+i*4 anim) | | |
| +0x09c..+0x0ac | float | WalkSpeed, RunSpeed, ShiftLeftSpeed, ShiftRightSpeed, ShiftBackSpeed | atof | |
| +0x0b4 | char[0x28] | Name | | +0x3d |
| +0x0de | char[0x20] | ShortName | | +0x7f |
| +0x100 | char[8] | CFileName | | |
| +0x109 | short | Height | | |
| +0x10b | short | BodyShape FAT0 NORMAL1 SHORT2 | | |
| +0x10d | char[8] | Birthday | | |
| +0x117 | char | Sex ('M'/'F') | | |
| +0x118 | short | Special: NORMAL0 MAIN1 HORSE2 ELEPHANT3 CANNON4 FAUCONNEAU5 CROSSBOW6 ANIMAL7 SEIGE_TOWER8 BOMBARD9 TREBUCHET10 BATTERING_RAM11 AUTOBOW12 CART13 SPECIAL14 | | |
| +0x11c | uint | TriggerType flags: ATTACK 1, ARROW 2, THROW 0x10, STONE 0x40, INTERACT 0x200, DOWN_ATTACK 0x400, EXPLOSIVE_ARROW 0x2000, EXPLOSIVE_KEG 0x4000 | | |
| +0x120 | byte | is playable hero (Code ∈ {JOAN_OF_ARC, JEAN_DE_METZ, LAHIRE, ALENCON}, 0x430050) | | gates AI auto-upgrade |
| +0x121 | int | ScriptDuration | | |
| +0x125 | int | ScriptFile id (NONE→0) | | +0x299 = script+0x22 |
| +0x129 | char[0x20] | Title | | +0xc1 |
| +0x149 | short | Ranking: INFANTRY0 ARCHER1 CAVALRY2 CAPTAIN3 GENERAL4 KING5 (0x527130) | | +0x104 via 0x5089e0 |
| +0x14b | short | KingdomId | | +0x108 |
| +0x14d | short | RaceId (0x4ca400) | | +0x10a |
| +0x151 | int | HP | | +0x13d (and HP) |
| +0x155 | int | EP | | +0x149 (and EP) |
| +0x159 | int | Life (extra lives) | | +0x11c, +0x120 |
| +0x15d | int | Experience = XP value of killing this unit (per victim level) | | — |
| +0x161 | int | Gold | | — |
| +0x165 | int | Resource (only if UnitClass==8) | | — |
| +0x169 | int | UpgradeList index (0x531df0 into UPGRADE table) | | |
| +0x16d | short | EArmorId (atoi) | | +0x15d |
| +0x16f | short | EArmorCode item id (optional key) | | +0x15f |
| +0x171 | short | PWeaponId item | | inventory slot 0 |
| +0x173 | short | SWeaponId item | | inventory slot 1 |
| +0x175 | short | PRangeWeaponId item | | inventory +0x0a |
| +0x177 | short | SRangeWeaponId item (ammo) | | inventory +0x0e |
| +0x179 | short | DropItemList (0x463ed0) | | rolled → +0x1c5/+0x1c7 |
| +0x17b..+0x181 | short | Strength, Dexterity, Defense, Leadership | | +0x155..+0x15b |
| +0x183..+0x197 | short | ArcherySkill, HorseRiding, SwimmingSkill, ElephantRiding, WideAnimal, Preaching, PowerJump, Disguise, Persuade, LockPicking, Mountain | | +0x16f..+0x197 (ints) |
| +0x199 | short | Level | | +0x124 (XP = LEVEL[level-1].EXPERIENCE if level≥2 else 0 → +0x126) |
| +0x19b | short | (not parsed, 0) | | +0x12e |
| +0x19d | short | Loyalty | | +0x161 |
| +0x19f | short | Morale | | +0x163 |
| +0x1a1 | short | Reputation | | +0x165 |
| +0x1a3 | short | Behavior (1..4 → +0x167) | | |
| +0x1a5 | short | RegenHp | | +0x141 |
| +0x1a7 | short | RegenEp | | +0x14d |
| +0x1a9 | short | Falling | | +0x1cb |
| +0x1ab | short | Dizzy | | — |
| +0x1c3 | int | CollisionType None0 Sphere1 Box3d2 Cylinder3 (0x527d20) | | actor+0xb8 |
| +0x1c7..+0x1d3 | float | Sphere params (4) | atoi→float | |
| +0x1d7..+0x1eb | float | Box3dInfX/Y/Z, Box3dSupX/Y/Z | | |
| +0x1ef..+0x1ff | float | CylinderCenterX/Y/Z, CylinderRadius, CylinderHeight | | |
| +0x1ad | int | AttachPointCount; +0x1b1 float[3]×n, +0x1b5 float×n, +0x1b9 byte×n (M1/F2/B3), +0x1bd short×n item | | |
units.tsv columns map 1:1 to these keys (UnitClass, Ranking, HP, EP, Strength…Level, WalkSpeed, RunSpeed, Gold, Experience, UpgradeList, DropItemList, PWeaponId, PRangeWeaponId, EArmorId, RegenHp, RegenEp, Morale, Special).

#### Drop-item roll at init (0x4f8550), RNG = `FUN_00590540(100)` ∈[0,100)
If rec+0x179≠0: rank≥4 → always; rank 3 → `r <= {LIGHT:LCAP_DI, HEAVY:HCAP_DI, POLE:PCAP_DI, MACE:MCAP_DI, ARCHER:ACAP_DI, CROSSBOW:CCAP_DI}` (all 100); rank<3 → `r <= {LINF_DI 10, HINF_DI 12, POLE_DI 10, MACE_DI 8, ARC_DI 8, CRO_DI 8}` (P = (X+1)/100). Other classes never drop. On success 0x464100/0x464030 fill +0x1c5/+0x1c7. CONFIRMED.

### 3. Regeneration (Unit_process 0x52c490, phase 1; asm 0x52ccd2..0x52cf3c) — CONFIRMED
EP (RECOV_T=10000 ms=300 ticks, RECOV_MP=1):
```
if HP==0 || EP >= maxEP():  ts151 = now
elif now - ts151 >= RECOV_T (unsigned):  ts151 = now; if !DAT_00a5d384: setEP(EP + RECOV_MP)   // clamp [0,maxEP()]
```
Fall gauge (+0x1cb), every 1000 ms while gauge < 100:
```
ts1cd = now
if action in {64,65,66 (KNOCKDN*), 41 (DNATKED)}: g = ftol((float)g + ((float)DEF()*DEF_FALL + 1.0f)*10.0f)   // = g + 2*DEF + 10 with DEF_FALL 0.2
else:                                             g = ftol((float)(g+1) + 10.0f*DEF_FALL)          // = g + 3
setFall(g)  // 0x4fea80 clamps 0..100
```
(constants 1.0f@0x9cf784, 10.0f@0x9cfc20, DEF_FALL float@0xa3ccd0; DEF() = 0x5238c0 incl. items)
HP (RECOV_HP=10000 ms, DEF_HP_R=0.1):
```
if HP==0 || HP >= maxHP() || retreat(+0x114) == -1:  ts118 = now
elif now - ts118 >= RECOV_HP: ts118 = now; if !DAT_00a5d384:
    inc = (float)DEF()*DEF_HP_R;  if !(inc > 1.0f) inc = 1.0f        // fcomp; test ah,0x41
    setHP(ftol((float)HP + inc))                                     // 0x4fe510 clamp [0,maxHP()]
```
Effective: every 10 s (300 ticks) HP = trunc(HP + max(DEF()*0.1f, 1.0f)) — fractional part lost each time (DEF 25 → +2, DEF 9 → +1). `DAT_00a5d384` = global pause/cutscene flag (cleared 0x471010) — HYPOTHESIS on meaning.
Rec RegenHp/RegenEp (+0x141/+0x14d) are NOT used here.
Other phase-1 timers: +0x130 arrow flag cleared when `now-+0x131 > LEVELARR(5000)`; +0x1dd -= dt; if +0x3bc≠0 and `now-+0x3bc >= UPG_TIME(600000)` and !rec.isHero → 0x4a95a0 auto-upgrade, +0x3bc=0.

### 4. XP

#### Kill reward — Unit_die 0x7c6240 (this = victim, param = killer recno) — CONFIRMED (asm 0x7c6d02..0x7c6f70)
EP bonus to killer: `setEP(EP + (victim.rank<3 ? EP_NOR 6 : victim.rank==3 ? EP_CAP 15 : EP_GEN 30))`.
Counters: killer +0x1ae += (rank<3 ? 1 : 10); +0x1b0 += 1; +0x1b2 += (rank≥3); leaders' +0x1bb/+0x1b9.
```
m = 1.0f
if killer.byte[0x1b4]:             m = CRITEXP (3.0)
if killer.level > victim.level:    m *= 1.0f / (float)(1 << (killer.level - victim.level))
if killer.recno == player && DAT_00a5b9f8 != 4:
    t = (int)(short)DAT_00a5ba62 / C_BONUSL(50)   // a5ba62 = combo hit counter; int division
    t = min(t, M_BONUSL 3)
    m *= (float)E_BONUSL(100) * 0.01f * t + 1.0f
v = (float)rec(victim).Experience * victim.level * m
xp = ftol(v > 10.0f ? v : 10.0f)
killer.EXPUP(xp, share=1, quiet=0)        // 0x5229a0
```
Morale (same function): `k = W1(victim unitres, 0x527dd0) + W2(killer unitres, 0x527e60)`; W1: rank 0-2→1, 3→2, 4/5→3, else 0; W2: rank 0-2→1, 3→2, 4→(Code JOAN_OF_ARC ? 4 : 3), 5/other→0. `d = max((float)k*MOR_CH_T(0.3), 1.0f)` (fcomp; `test ah,1`); every unit with killer's nation (+0x02≠0) & killer's team id (+0x31≠0): `setMorale(ftol((float)morale + d))`; every unit with victim's nation & victim's team: `setMorale(ftol((float)morale - d))`. Victim's nation obj (0x509400): `0x4b5520(-maxHP() * W1(victim))`. CONFIRMED (asm 0x7c6f75..0x7c7147).

#### EXPUP 0x5229a0(xp, share, quiet) — CONFIRMED (asm)
```
if FUN_004cd510(&0xa58fa7)==2 or rec.UnitClass==8 (siege): return
if share:
   if is_leader (+0x33):  s = ftol((float)xp * MEM_EXP 0.4)
        for each member recno in team: if exists && !dead: member.EXPUP(ftol((float)s / (float)team.count), 0, quiet)
   else: L = leader; if L && !L.dead: L.EXPUP(ftol((float)xp * LD_EXP 0.4), 0, quiet)
XP(+0x126) += xp                         // shares are EXTRA, not deducted
if player unit: 0x6fe520(stats, xp); if DAT_00a5b9f8!=4: DAT_00a5b9f0 = min(DAT_00a5b9f0 + xp, MAX_COM 2000)  // command points
LEVELUP(quiet)                           // 0x522c40
if player unit: effect "EXPUP" at pos
```
team.count = char at team+2 (whether it includes the leader: UNKNOWN).

#### LEVELUP 0x522c40(quiet) — CONFIRMED
LEVEL table (std_set/LEVEL.csv, loader 0x4a4ca0 `levelres.cpp`, 7-byte rec {char lvl; int EXPERIENCE; char ATT_POINT; char SKI_POINT}, lookup 0x4a4e60):
```
if dead: return 0
c = LEVEL[level]; if !c or !LEVEL[level+1] or XP < c.EXP: return 0
do { unlockUpgrades(level) /*0x523240*/; level++; if player: 0x703680 (HUD);
     arrow=1; ts131=now; attrPts(+0x12c) += c.ATT; skillPts(+0x12a) += c.SKI; c = LEVEL[level]
} while (c && XP >= c.EXP)
ts3bc = now                                // AI upgrade timer
announce (non-player general of player side with kills>0)
setHP(maxHP()); +0x139 = maxHP(); setEP(maxEP())   // full heal on level-up
if !quiet && model: "flash.txt" effect; if player: "LEVELUP" effect
if leader: each alive member: 0x523190(level)       // members dragged up to leader level
```
Threshold semantics: row L EXPERIENCE = total XP needed to leave level L. Max level effectively 30 (LEVEL[31] missing; at 30 a jump to 31 is possible only via loop when XP ≥ 4,065,260 — edge case).

| L | EXP | ATT | SKI | | L | EXP | ATT | SKI |
|---|---|---|---|---|---|---|---|---|
|1|1000|5|1| |16|573300|9|2|
|2|3200|5|1| |17|676600|9|2|
|3|5800|5|1| |18|784870|9|2|
|4|10400|6|1| |19|910440|9|2|
|5|17800|6|1| |20|1108880|10|2|
|6|28500|6|1| |21|1347470|10|2|
|7|45600|6|1| |22|1536160|10|2|
|8|73000|7|1| |23|1735850|10|2|
|9|109600|7|1| |24|1961480|11|2|
|10|161070|7|1| |25|2306660|11|2|
|11|209470|7|1| |26|2583550|11|2|
|12|272260|8|1| |27|2893510|11|2|
|13|326830|8|1| |28|3240780|12|2|
|14|392170|8|1| |29|3629740|12|2|
|15|485870|8|1| |30|4065260|12|2|

0x523190(newLevel) set-level: if alive and level<newLevel: `EXPUP(LEVEL[newLevel-1].EXP - XP, 0, 1)` then 0x4a95a0. Script `unit_level_up level [unit]` (0x454560 → 0x454c80 → 0x455cc0): clamp level 1..30, target = player or named unit → 0x523190. Script `set_unit_level` (0x65c250): 0x523190 then HP=maxHP. `add_unit_experience` (0x65c9e0): EXPUP(xp,0,0).

#### Upgrades / stat growth
Unlock: per-unit list = UPGRADE.csv (code→DBF e.g. JOAN→JOANUP.csv rows {LEVEL, CODE}), loaded 0x530d70 (4-byte entries {short level, short upitemId}). `0x523240(L)`: for each row with level==L → 0x5313d0(upitemId): find entry in unit list (+0x1aa) and `unlocked++`, or append {id, unlocked=1, applied=0, comboLevel=1}. Init: unlock(0) then mark all those applied (0x531520 → applied++ if applied<unlocked) — level-0 abilities known for free.
UPITEM.csv (100-byte rec, 0x531790): type (0x531ba0: STRENGTH1 DEXTERITY2 DEFENSE3 CHARISMA4 LEADERSHIP5 HIT_POINT6 ENERGY_POINT7 NEW_COMBO9 JUMP_ATTACK_B10 DOWN_ATTACK11 RUSH_ATTACK12 ENERGY_ATTACK13 RANGE_ATTACK14 HORSE_ATTACK_A15 HORSE_ATTACK_B16), +0x46 PARA1..3 (7-char each), +0x5b short COST, +0x5d REQUIRE. "Skill" types (8..16, 0x531ed0) are paid from +0x12a, others from +0x12c.
Apply 0x4949f0(entry): if points ≥ COST and applied<unlocked: points -= COST; applied++; STR: STR+=P1, maxHP += P1*STR_HP(0); DEX: +P1; DEF: DEF+=P1, maxHP += P1*DEF_HP(0); LEAD: +P1; HIT_POINT: maxHP += P1 (heals delta), HP=maxHP; ENERGY_POINT (7) and skill types: only marked applied (no stat code here). CONFIRMED.
Player manual allocation 0x50d640(type, undo, entryIdx) (1 point each, undo reverses): 1 STR += STR_PER(1) & maxHP += STR_PER*STR_HP; 2 DEX += DEX_PER(1); 3 DEF += DEF_PER(1) & maxHP += DEF_PER*DEF_HP; 5 LEAD += LDS_PER(1); 6 maxHP += HP_PER(4) (heals delta); 7 maxEP += EP_PER(3) (0x45bba0); 8 combo: entry.comboLevel++, skillPts--. Type 4 (CHARISMA) unsupported. CONFIRMED (widths: all short globals `movsx WORD`).
AI auto-upgrade 0x4a95a0 (unitlvl.cpp; after UPG_TIME, non-hero only, or via 0x523190): walk upgrade list from last to first, apply every affordable unlocked-not-applied entry (skill entries require REQUIRE combo already applied or empty); then spend remaining skill points: repeatedly pick a random NEW_COMBO entry with comboLevel<10 (`FUN_00590540(n)`), comboLevel++, skillPts--. CONFIRMED (decomp), combo-level effect on damage: see MeleeCombat (UNKNOWN here).
JOANUP unlocks: L0 DOWN_ATTACK, HORSE_ATTACK_A, combos A AA AAA AAAA AAAB AAB AB B, RANGE_ATTACK; L1 BB; L3 RUSH_ATTACK; L6 BBB; L8 JUMP_ATTACK_B; L10 BBBB; L12 HORSE_ATTACK_B; L14 AAAAA; L17 AAAAB; L19 ENERGY_ATTACK_UP; L22 BBBBB; L24 AAAAAB.

### 5. Morale setter 0x4fe8c0(v) — CONFIRMED (asm 0x4fe8cc..)
```
if v < morale && team(+0x31)!=0 && (L = leader-or-self) && L.inventory:
    if aggr(MORALE_DROP 0x13) == 0: return            // default 100; any MORALE_DROP item → 0 → immune
    base = aggr(MORALE_BASE 0x12)                     // max over items, default 0
    if base < morale: return                          // NOTE: drop ignored whenever morale > base (incl. base 0)
    if v < base: morale = base; return
morale = clamp(v, 0, 100)
```
aggr = 0x497e10 called on THIS unit's inventory (ecx=this+0x1c1) with leader recno. As coded, team members/leaders with an inventory (all units get one in Unit_init) never lose morale via this setter unless morale ≤ MORALE_BASE. Report as code fact; gameplay consequence not runtime-verified.

### 6. Snapshot pair (no callers found; dead code?)
0x462610 Unit→record (0x140 bytes) and 0x466320 record→Unit: rec+0 unitres id, +2 ↔ unit+0x102, +4..+0x3e (0x3b bytes) ↔ unit+0x16f..+0x1a9 (skill ints archery..mountain + bytes up to +0x1a9), +0x3f name, +0xc3 title, +0x104 rank, +0x108/+0x10a kingdom/race, +0x10c level, +0x10e (+0x12e), +0x110 HP, +0x114 maxHP, +0x118 RegenHp, +0x11c RegenEp, +0x120 STR, +0x122 DEX, +0x124 XP, +0x128 loyalty, +0x12a morale, +0x12c reputation, +0x12e mode, +0x13a fall gauge, +0x13c (+0x1d5). Restore bug: DEX and KingdomId are self-assigned (not restored). CONFIRMED (no xrefs in asm to 0x408c47 thunk).

## 5. Part B — Input and player controller

Confidence: CONFIRMED = read in decomp and/or asm; HIGH = strong evidence, small gap; HYPOTHESIS; UNKNOWN.
Times are game-ms (`DAT_00a500ba`), ticks = ms/33.33. Offsets are byte offsets.

---------------------------------------------------------------------------------------------------
### 1. Input layer (DirectInput 8)

#### 1.1 Input object `g_Input` @ 0xa516b0 (CONFIRMED: `mov ecx,0xa516b0` before 0x5959c0/0x594f90 thunks)

| off | type | meaning | evidence | conf |
|---|---|---|---|---|
| +0x00 | u8 | initialised | 0x592380 | CONFIRMED |
| +0x02 | u8 | input suspended (poll returns 0) | 0x594f90 | CONFIRMED |
| +0x03 | u8 | "reset modifier state" request → 0x598ac0 | 0x594f90 | CONFIRMED |
| +0x08 | IDirectInput8* | | 0x592380 | CONFIRMED |
| +0x0c | IDirectInputDevice8* mouse | buffered (DIPROP_BUFFERSIZE=200), DIPROP_AXISMODE=REL(1), coop level 5 (NONEXCLUSIVE\|FOREGROUND) | 0x592380 | CONFIRMED |
| +0x10 | IDirectInputDevice8* keyboard | buffered (DIPROP_BUFFERSIZE=16), coop level 6 (NONEXCLUSIVE\|BACKGROUND) | 0x592380 | CONFIRMED |
| +0x14/+0x18/+0x1c | int | cursor x / y / wheel-z (clamped to +0x636..+0x64a rect) | 0x594f90, 0x593f60 | CONFIRMED |
| +0x20/+0x24/+0x28 | int | LMB(1)/RMB(2)/MMB(4) down flags | 0x594f90 | CONFIRMED |
| +0x2c/+0x30/+0x34 | int | last raw mouse dx/dy/dz (0xa516dc/e0/e4) | 0x594f90 | CONFIRMED |
| +0x44/+0x48/+0x4c | int | ACCUMULATED mouse dx/dy/dz since last latch (0xa516f4/f8/fc) – used for yaw/pitch | 0x594f90, 0x431790 | CONFIRMED |
| +0x50 | u8[256] | DIK key-held table; value = `data & 0x80` (0x80 held, 0 up) → 0xa51700 | 0x598bd0 | CONFIRMED |
| +0x150 | u16 | modifier/lock bits: 1 LShift,2 RShift,4 LCtrl,8 RCtrl,0x10 LAlt,0x20 RAlt,0x40 NumLock,0x80 CapsLock,0x100 ScrollLock,0x200 Insert (0xa51800) | 0x598bd0, 0x598ac0 | CONFIRMED |
| +0x152 | u16 | previous-frame modifier bits (0xa51802) | 0x594f90, 0x4cc570 | CONFIRMED |
| +0x154 | u16 | arrow bits 1 Left,2 Right,4 Up,8 Down | 0x598bd0 | CONFIRMED |
| +0x156 + j*0x39 | joystick state (4 pads, 0x31e+j*0xc6 device recs) | 0x592380, 0x598730 | CONFIRMED |
| +0x66a | u16 | modifiers of current event | 0x5932e0 | CONFIRMED |
| +0x66c | u8 | current event is a mouse click | 0x5932e0 | CONFIRMED |
| +0x66e | int | current event type | 0x5932e0 | CONFIRMED |
| +0x672+b*0x1c | struct | per-button click tracking: x,y,…,[+0x10] click count (double-click if Δt < DAT_00a1590c and |Δx|,|Δy| < 5 px), [+0x14] time | 0x5932e0 | CONFIRMED |
| +0x682 / +0x69e | int | LMB / RMB click count of current event (read by 0x594700) | 0x594700 | CONFIRMED |
| +0x6aa | int | **scan code of current key-down event** (0xa51d5a), 0 if none | 0x5932e0 | CONFIRMED |
| +0x6ae | int | translated char of current key-down event (0xa51d5e), via 0x599240 | 0x5932e0 | CONFIRMED |
| +0x6f5/+0x6f6/+0x6f7 | u8 | LMB/RMB/MMB held (0xa51da5..) | 0x5932e0 | CONFIRMED |
| +0x70d + i*0x51 | event[20] | ring buffer of events; head +0xd61, tail +0xd65 | 0x592d00/0x592dc0/0x592f00 | CONFIRMED |

Event types (ring entry +0): 0 LMB down, 1 RMB down, 2 key down, 3 ?, 4 LMB up, 5 RMB up, 6 key up, 7 ? (CONFIRMED 0x594f90/0x5932e0). Entry: +4 DI timestamp, +8 modifiers, +0xa x, +0xe y, +0x12 scan code.

#### 1.2 Per-frame flow (CONFIRMED unless marked)
1. `Input_poll` 0x594f90 (looped by 0x5959c0 until no data): reads both DI buffers (`GetDeviceData`), merges mouse/keyboard records in timestamp order; mouse axis records update accumulators; button/key records are pushed into the ring (`0x592d00`, key: 0x598bd0 → 0x592dc0 down / 0x592f00 up). Ctrl/Shift/Alt/Caps/Num/Scroll/Insert only update +0x150 (not queued).
2. Per drawn frame `0x52f930` calls `Input_get_event` 0x5932e0 **once** → pops ONE event into the "current event" fields (+0x6aa/+0x6ae/+0x66c...). If the ring is empty the current-event fields are cleared. Then UI handlers, then 0x562f40 → `PlayerCtrl_process` 0x42e1a0 (`mov ecx,0xa5b9e8` @0x562fb0). So **the player controller runs once per rendered frame**, not per 30 Hz logic tick; it uses DAT_00a500be (game ms elapsed this loop) and DAT_00a500b6 (real ms) [CONFIRMED: Sys_main_loop calls 0x52f930 only on drawn frames, §1.1].
3. Consequence: a key "press" edge is visible to gameplay for exactly one frame; with several queued events only one is consumed per frame (CONFIRMED 0x5932e0 pops one).

#### 1.3 Key-query primitives
| fn | semantics | conf |
|---|---|---|
| 0x4cc3d0 `Key_held(dik)` | 0→0; 0x3a(Caps),0x45,0x46→0; 0x1c/0x9c Enter → keystate[0x1c]\|\|[0x9c]; 0x1d/0x9d → mods&0xC; 0x2a/0x36 → mods&3; 0x38/0xb8 → mods&0x30; 0xffff→0; else `(u8)keystate[dik]` (=0x80 when held) | CONFIRMED |
| 0x4cc570 `Key_pressed(dik)` | current event scan code == dik (Enter: 0x1c or 0x9c); modifiers: (prevMods&m)==0 && (mods&m)!=0 | CONFIRMED |
| 0x597ed0 `Joy_button(i)` | pad button i held (via mapping table) | CONFIRMED |
| 0x594700 `Mouse_clicked(b)` | b=0 LMB: click count of current LMB-down event; 1 RMB; 2 either | CONFIRMED |
| 0x4cb6d0 `Action_held(mode,action)` | see 1.4 | CONFIRMED |
| 0x4cb820 `Action_pressed(mode,action)` | same, edge versions (joystick edge = held now && !held prev) | CONFIRMED |

#### 1.4 Key binding (remap) table — remapkey.cpp, object @ 0xa48820 (CONFIRMED `mov ecx,0xa48820` before 0x4cb6d0/0x4cb820 thunks)
Built by 0x4cae70 from `STD.SET` tables **DEFAULTK** and **KEYDESC** (extracted: `work/re-scripts/std_set/DEFAULTK.csv`, `KEYDESC.csv`). Entry = 0x30 bytes, index = action id (1-based record #):

| entry off | from column | used by mode | meaning |
|---|---|---|---|
| +0x00 | KEY_ACTION (0x20 chars) | | display name |
| +0x22 | J_JOYPAD (hex, byte) | 1 joystick | pad button+1 (0/0xff none) |
| +0x23 | FPS_MOUSE (atoi, byte) | 3 kbd+mouse | mouse button 1 L,2 R,3 M (0/0xff none) |
| +0x24 | K_KEY (hex) | 0 keyboard-only | DIK code (0xffff none) |
| +0x28 | J_KEY (hex) | 1 joystick (fallback key) | DIK |
| +0x2c | FPS_KEY (hex) | 3 kbd+mouse | DIK |
Mode 3 lookup: key (+0x2c) OR mouse button (+0x23); mode 1: pad button (only if a pad exists) else +0x28; mode 0: +0x24. Mode 2 → always 0 (CONFIRMED 0x4cb6d0/0x4cb820).
User overrides: if config byte `0xa5806c` (=Config 0xa57fb8+0xb4) ≠0, 0x4cc220 copies per-action bytes/ints from Config at 0xa5806d+(i)*0x30 (fields +0x22..+0x2c same layout) → Config block +0xb5..+0xb04 = 0x35 entries × 0x30 (CONFIRMED 0x4cc220).
Name lookup for UI: 0x4cb5b0/0x4cb9b0 (KEYDESC device 0 key,1 pad,2 mouse).

##### Action ids & default bindings (CONFIRMED from DEFAULTK.csv + 0x65e0a0 script names + controller usage)
| id | DEFAULTK row | script name (0x65e0a0) | kbd-only (K_KEY) | kbd+mouse (FPS) | pad |
|---|---|---|---|---|---|
| 2 | Center camera | SHIFT_CAMERA | – | – | btn 10 |
| 3 | Center camera / shift modifier | SHIFT | LShift | – | – |
| 4 | Move forward | FORWARD | Up | W | – |
| 5 | Move back | BACKWARD | Down | S | – |
| 6 | Strafe left | SHIFT_LEFT | – | A | – |
| 7 | Strafe right | SHIFT_RIGHT | – | D | – |
| 8 | Turn left | TURN_LEFT | Left | – | – |
| 9 | Turn right | TURN_RIGHT | Right | – | – |
| 10 | Walk | – | – | LShift | – |
| 12 | Defend (= Down-attack key) | DEFEND / DOWN_ATTACK | E | Q | btn2 |
| 13 | Light attack (A) | A_ATTACK | Q | LMB | btn4 |
| 14 | Heavy attack (B) | B_ATTACK | W | RMB | btn3 |
| 15 | Energy attack | ENERGY_ATTACK | S | C | – |
| 16 | Jump | – | Space | Space | btn1 |
| 17 | Interact | INTERACT | R | R | btn8 |
| 18 | Range mode toggle | ARROW_SHOOTING | Z | Z | btn9 |
| 19 | Use HP recovery item | USE_HEAL_ITEM | F3 | F3 | btn5 |
| 20 | Use EP recovery item | – | F4 (ignored if Alt held) | F4 | btn6 |
| 21 | Chain | – | (unbound) | (unbound) | – |
| 23 | Action/RTS toggle | RTS_MODE | F2 | F2 | |
| 24 | Switch character (cycle) | SWITCH_CHARACTER | ` | ` | |
| 25..28 | Switch to Joan / Jean / La Hire / Alençon | SWITCH_JOAN/JEAN/LA_HIRE | 1..4 | 1..4 | |
| 29..32 | Command Bastard/Richemont/Charles VII/Flavy | – | 5..7, – | | |
| 34 | Main menu | – | Esc | | |
| 35 | Character details | CHARACTER_INTERFACE | F1 | | |
| 36 | Combo window | COMBO_INTERFACE | (unbound) | | |
| 37/38/39 | Save/Load/Options | | F7/F8/F9 | | |
| 40 | Mission brief | GAME_OBJECTIVE_KEY | F12 | | |
| 41 | Message log | | M | | |
| 43/44/45/46 | Close msg / HP bar / zoom minimap / objective | | LCtrl / F11 / Tab / O | | |
| 48/49 | Zoom in/out | ZOOM_IN/OUT | Num+/Num- (+ mouse wheel in KBM: dz>0 in, <0 out) | | |
| 50/51 | Rotate camera CW/CCW | | , / . | | |
| 53/54/55 | Quick save/load/capture | | F5/F6/F10 | | |
(ids 1,11,22,33,42,47,52 are menu section headers.)

#### 1.5 Mouse sensitivity & options (CONFIRMED 0x7467e5.., 0x431790, 0x42bcd0)
* Config float `0xa58027` sens ∈[-1,1] = (slider−10)/10, default 0 → copied to controller +0xe1 by 0x432250 (clamped ±1).
* On-foot yaw: `deg = ftol(accDX * ((sens+1)*0.7+0.2) * 0.2)` per frame (×0.4 instead of 0.2 when riding horse +0xc4). Default 0.18°/mickey.
* Range-mode aim: `rad += -accDX * 0.017453292 * ((sens+1)*0.7+0.2)*0.08` (pitch with dy, sign by invert).
* Invert mouse `0xa5802b` (byte, 0 default). Horse riding mode `0xa5802c` (0 = mode A, 1 = mode B). Horse camera rotation angle `0xa5802e` float default 90.0. Input method `0xa58023` (int): 0 keyboard-only, 1 joystick, 3 keyboard+mouse (default 3); if 1 but no pad → forced 0 (0x42de70).

---------------------------------------------------------------------------------------------------
### 2. Player controller object `g_PlayerCtrl` @ 0xa5b9e8 (CONFIRMED `mov ecx,0xa5b9e8`)

| off | type | meaning | evidence | conf |
|---|---|---|---|---|
| +0x00 | int | current player unit recno (DAT_00a5b9e8) | everywhere | CONFIRMED |
| +0x10 | int | control mode: 0 kbd-only→0x4343e0, 1 joystick→0x432d30, 2 mouse point&click→0x438b30, 3 kbd+mouse→0x430480, 4 RTS→0x440b30 | 0x42e1a0 switch | CONFIRMED |
| +0x14 | int | mode before RTS | 0x42e040 | CONFIRMED |
| +0x74 | u8 | camera-follow disabled | 0x4352f0/0x431790 | HIGH |
| +0x75 | float | saved facing deg (center camera) | 0x436190 | CONFIRMED |
| +0x79 | u8 | free-switch allowed flag | 0x42e1a0 | HIGH |
| +0x81 | u8 | mode change pending | 0x42e040 | CONFIRMED |
| +0x82 | int | last direction action pressed (4..7) for double-tap | 0x430480 | CONFIRMED |
| +0x86 | int | time of that press (ms) | 0x430480 | CONFIRMED |
| +0x8e/+0x8f/+0x90 | u8 | A / B / A+B button phase: 0 idle,1 down,2 released,3 re-pressed | 0x4367c0 | CONFIRMED |
| +0x92/+0x93/+0x94 | u8 | "fresh press may queue" flags | 0x4367c0 | CONFIRMED |
| +0x96 | u8 | next combo hit queued | 0x4367c0 | CONFIRMED |
| +0x97 | int | time current attack started (ms) | 0x4367c0 | CONFIRMED |
| +0x9b | i16 | queued letter 1 A, 2 B, 3 C, 4 cancel | 0x4367c0 | CONFIRMED |
| +0x9d | i8 | chain counter | 0x438820 | CONFIRMED |
| +0x9e | int | jump-key hold time (ms) | 0x430480 | CONFIRMED |
| +0xa6 | i16 | air state 0 / 1 rising / 2 descending-landing | 0x4352f0 | CONFIRMED |
| +0xa8 | int | RUN start time (0 = not running) | 0x437ed0 | CONFIRMED |
| +0xac | u8 | range (aim) mode active | 0x42b3c0 | CONFIRMED |
| +0xad/+0xb1/+0xb5 | float | range base yaw rad / deg / base pitch deg | 0x42b3c0 | CONFIRMED |
| +0xbb | int | range-mode locked target recno | 0x42ba40 | CONFIRMED |
| +0xc0 | int | time range mode entered | 0x42b3c0 | CONFIRMED |
| +0xc4 | u8 | riding horse (horse controller active) | 0x430480/0x431790 | CONFIRMED |
| +0xc5 | float | horse facing rad | 0x43dab0 | CONFIRMED |
| +0xc9 | float | horse heading deg (last gear change) | 0x43d7a0 | CONFIRMED |
| +0xcd | i16 | operating vehicle/siege (→0x42d270) | 0x430480 | HIGH |
| +0xe1 | float | mouse sensitivity [-1,1] | 0x432250/0x4322d0 | CONFIRMED |
| +0xed | float | 700.0 init (camera dist?) | 0x42de70 | HYPOTHESIS |
| +0x126a | float | last movement rel-angle | 0x4352f0 | CONFIRMED |

#### 2.1 Unit fields used by the controller (byte offsets, unit = UnitArray @0xa47c08)
| off | type | meaning | conf |
|---|---|---|---|
| +0x0e/+0x12/+0x16 | float | world x,y,z | CONFIRMED (0x4f0b50, 0x437b60) |
| +0x1e | float | facing, degrees [0,360) (get 0x50ac00 / set 0x50abd0) | CONFIRMED |
| +0x102 | i16 | AI-controlled/leader-on-path flag (speed ×AI_MOVE) | HIGH |
| +0x120 | int | (with HP<1 → dead, 0x42d0e0) | CONFIRMED |
| +0x135/+0x145 | int | HP / EP | CONFIRMED |
| +0x151 | int | time of last energy attack | CONFIRMED |
| +0x1e2 | int | mounted horse recno (0x430190) | CONFIRMED |
| +0x1fd | int | climbing object (0x42d690 → CLIMB) | HIGH |
| +0x20f | int | time current state began | CONFIRMED (0x500ba0) |
| +0x213 | i16 | posture: 0 STAND, 1 JUMP/airborne (move-list POSITION column) | HIGH |
| +0x215 | i16 | current action/anim state (enum §4) | CONFIRMED |
| +0x217 | i16 | upper-body state (used when +0x357 / range mode) | CONFIRMED |
| +0x227..+0x22c | char[6] | current combo string | CONFIRMED |
| +0x233 | i8 | combo length (-1 = reset pending, cleared 250 ms after +0x23f) | CONFIRMED |
| +0x235 | i16 | next hit requested | CONFIRMED |
| +0x237/+0x23b | uint | combo input window [open, close) ms after attack start | CONFIRMED use; source = move START/END_TIME HYPOTHESIS |
| +0x23f | int | combo lock timestamp (≠0 blocks attacks) | CONFIRMED |
| +0x243 | int | gravity scale (ftol(animrec+0x48), set 0x504010) | CONFIRMED |
| +0x2f1 | ptr | move list (STD.SET per-hero table, e.g. JOAN.csv); record via 0x4168d0: [0] POSITION, [1] MOVEMENT id, +8 COMBO char[6], +0xe COMBOAVAIL 'Y', [4] ITEM_USING, [5] SPECIAL, +0x18 ENERGY_PT (EP cost), +0x72 anim rec | CONFIRMED (fields used) |
| +0x2f5 | ptr | model; +0x5c physics body; +0x60/64/68 launch velocity; +0x6c/70/74 ground velocity | CONFIRMED |
| +0x351 | int | take-off time | CONFIRMED (0x505cc0) |
| +0x359/+0x35d/+0x361 | float | jump velocity x,y,z | CONFIRMED |
| +0x365 | float[3] | arrow aim dir | CONFIRMED |

---------------------------------------------------------------------------------------------------
### 3. Controller state machine

#### 3.1 Dispatcher `PlayerCtrl_process` 0x42e1a0 (misnamed Game_quick_save) — per frame
Early outs: cut-scene/dialog/menus (0xa596ab, 0x4301c0, 0x42ced0, +0x81, +0x7c, 0xa48ce0, 0x42d160, 0x4303d0, 0x4303f0), game state 0xa48cef==2 → return 0. Then (`m` = +0x10, or +0x14 when in RTS):
1. F5 (0x35) quick-save, F6 (0x36) quick-load, O (0x2e) objective. 
2. Shift held + 1..8 (0x19..0x20): issue order to that commander (0x51fd50 → 0x5121c0) (CONFIRMED).
3. If m≠4 and +0xac==0: ` (0x18) → `0x432300(0)` cycle; 1..4 (0x19..0x1c) → `0x432300(1..4)` (Joan, Jean, La Hire, Alençon; name table 0x9f8b10 stride 0x104). Allowed if (+0x79 or mission DAT_00a58fa7>1) and ((not on ladder 0x42d690 and state ∉{0x4f..0x52 horse mount/dismount}) or current unit dead). (CONFIRMED asm push 0..4)
4. F2 (0x17): toggle RTS (0x42e040(4) / back to +0x14) if mission ≥5 or +0x79, and 0xa48cef∉{1,2}.
5. Skip if current unit in state 0x2a DIE,0x3c STANDUP,0x40 KNOCKDN,0x4e?,0x41,0x42 (KNOCKDN2/3) → +0xa8=0, return (CONFIRMED).
6. switch m → mode controller.

#### 3.2 Keyboard+mouse controller 0x430480 (mode 3) — order of evaluation (CONFIRMED)
Inputs sampled: A=pressed(13), B=pressed(14), zoom (wheel/48/49), Z=pressed(18), Chain=pressed(21), held W(4)/A(6)/S(5)/D(7), C=pressed(15), DefendHeld(12), Jump held(16).
* **Jump timer**: if Jump held: `+0x9e += DAT_00a500be`; fire when `+0x9e >= JUMPMAXD(100)` (hold value = +0x9e, reset 0). If released and +0x9e>0: fire with hold = +0x9e. JUMPMIND (0xa33748) is loaded but **never read** (CONFIRMED: only writer 0x454090).
* moveCode = 0x431680(W,S,A,D): W+A 10, W+D 0xb, W 0x25, S+A 0xc, S+D 0xd, A 0x1c, D 0x22, S 0x14, none 0 (A+D cancel). relAngle = 0x431570: W 90, W+A 135, W+D 45, A 180, D 360, S 270, S+A 225, S+D 315, none −1 (degrees, CCW, 90 = camera forward). magnitude = 3 if any WASD else 0.
* **Double tap**: if a key event with a char occurred this frame (0xa51d5e≠0): +0x82 = first of {4,5,6,7} pressed this frame else 0; +0x86 = now. Double-tap action `dt` = +0x82 if +0x82≠0 && now−+0x86 < **300 ms** && that action pressed again now (CONFIRMED asm cmp 0x12c).
* F3 → 0x43b490(0) HP item; F4 (no Alt) → 0x43b990(0) EP item (always, even in other sub-modes).
* if range mode (+0xac) → 0x42a810 (exit on Z, fire on A, mouse aim) + strafe/back movement, return 1.
* else if riding (+0xc4) → Camera_follow_update 0x43f000, zoom, horse controller 0x43c5c0 (§3.6).
* else if +0xcd (vehicle) → 0x42d270.
* else, on foot (each step returns 1 if it consumed input):
  1. Interact R: try 0x43bef0, 0x43bd40, 0x43be40, 0x43c210, 0x43bc50 (interact handlers: doors/NPC/items…), else 0x43bfe0, 0x43c080.
  2. if DAT_00a500d2==0 (world input disabled) return 0.
  3. Z → enter range mode 0x42b3c0 (§3.7).
  4. 0x436190(0) clear camera-center flag; zoom 0x438720.
  5. **Down attack** 0x4363a0(DefendHeld) → DOWNATK 0x22.
  6. **Block** 0x436250(DefendHeld) → GUARD1 0x2b; release → 0x508b90 (back to idle).
  7. **Energy attack** 0x437910(C pressed || (A && B pressed same frame)).
  8. **Attacks / rush** 0x4367c0(A, B, dt==4).
  9. if state ∉ [0xe..0x14] (not strafing/back-stepping, 0x5091d0): Interact-mount 0x43c7d0(R) (horse), 0x42d300(R) (ladder/climb), **jump** 0x437b60(fire, magnitude, relAngle, hold).
  10. evade: dt==5 → JUMP_B 0x15; dt==7 → JUMP_R 0x16; dt==6 → JUMP_L 0x17 — each only if canAct (0x500ba0), posture≠1, state∉{5 IDLEUP,6 IDLEDN}, move list exists and contains that action (0x416dc0). Return 1 even if refused.
  11. **Chain** 0x438820(Chain pressed).
  12. Movement: if A xor D held → 0x438110(W,S,A,D) (strafe states); A&D → return 1; S only → 0x438110(0,S,0,0) → MOVE_B 0x10; nothing → 0x436190(0) and if in strafe state → 0x508b90 (stop).
  13. if no A/D/S: `Face_move_dir` 0x4352f0(relAngle, magnitude, animRemaining).
  14. `Mouse_yaw` 0x431790(relAngle) (§3.4).
  15. if no A/D/S: 0x437ed0(moveCode, WalkHeld(10)) → RUN/WALK.

`canAct` = 0x500ba0: `now >= unit+0x20f + CLB_IDLE(20 ms)` AND state ∈ {−1,0..6,9..0x14,0x2b..0x2d,0x36,0x37,0x39,0x43..0x46,0x54..0x57,0x59,0x5a} (idles, walk/run/turn/moves, guards, shoot-idles, horse idle/walk/back/run, push, C_IDLE) (CONFIRMED).

#### 3.3 Keyboard-only controller 0x4343e0 (mode 0) — differences (CONFIRMED)
* Movement keys are Up(4), Down(5), Left(8), Right(9); relAngle = 0x431570(Up,Down,Left,Right) → **camera-relative**: Left/Right make the hero turn to face camera-left/right and run (no strafing). moveCode = 0x25 if Up/Left/Right, else 0x14 if Down.
* Shift (action 3) held + direction → 0x436190(1) (camera center) and 0x438110(...) = strafe/back-step states; Shift alone recentres camera.
* Defend = pressed OR held; Down-attack/Block use it. Energy = S pressed only (no A+B).
* Double tap: Up-Up → rush (no Shift needed); Shift+Down-Down → JUMP_B, Shift+Right-Right → JUMP_R (dt 7), Shift+Left-Left → JUMP_L (dt 6), same 300 ms window.
* Jump call guarded by `held(Up)+held(Down)+held(Left)+held(Right) == 1` on the raw held values (asm 0x434a7b..0x434a87). Held values are 0x80 per key → sum is 0 or a multiple of 0x80, never 1 ⇒ **with default arrow-key bindings jump never fires in keyboard-only mode** [CONFIRMED arithmetic; INFERENCE: original bug]. The port keeps it (`process_keyboard`).
* Range mode: no movement allowed (manual), aim with keys.
* Order: Defend/down/block → attacks (before energy) → energy → R mount/climb → jump → evades → movement.

#### 3.4 Facing / turning
* **KBM yaw** 0x431790 (CONFIRMED asm): allowed if (canAct or state ∈{0x49 H_ATTACK,0x1c ATTACK,0x1d JATTACKB}) and posture≠1 and state∉{0x4f..0x52}. `d = ftol(accDX * k)` (k §1.5); during ATTACK (0x1c) d clamped to [−45,45] per frame. `facing = ftol(facing − d)` wrapped to [0,360], model yaw set (0x4ff870). If +0x74==0 and camera 0: camera yaw −= d°. Pitch: `pitch += ftol(accDY*k)*±1°` (invert), clamp [−20°, +35°] (−0.34906584..0.61086524 rad).
* **Face movement dir** 0x4352f0 (both modes): skip if guarding (0x2b..0x2d) or horse mount (0x4f..0x52). worldDir = (camYawDeg + relAngle − 90) mod 360 via 0x42fe30 (`ftol` then `%360`). Sets unit facing = worldDir. Camera (index 0, if +0x74==0) auto-rotates toward it: `camYaw += DAT_00a500b6 * 3.4906585e-05 * off` rad, off = relAngle−90 (relAngle<180) or 270−relAngle; i.e. 0.002°/ms per degree of offset (KO: Left → 90° offset → 0.18°/ms = 180°/s).
* In KBM relAngle is 90 or −1 here (strafe/back don't call it) ⇒ facing = camera yaw while running forward.
* Airborne (+0x213==1): landing detection — when body vz (+0x14a) ≤0 and +0xa6==1 and canAct: clear combo, action 6 (IDLEDN, landing), +0xa6=2. Air steering when +0xa6==2: facing rotates toward input dir by ≤30° per call, horizontal body velocity (+0x142/+0x146) rotated keeping magnitude.
* ATTACK (0x1c) with input: re-aim limited to ±45° steps if angle within 10..350 and not too far from target (50).

#### 3.5 Locomotion states (0x437ed0 / 0x438110 / 0x4350f0) (CONFIRMED)
* 0x4350f0 picks the forward state: modes ≠1: mounted→0x46 H_RUN, ladder→0x2f CLIMB, else 0xb RUN. Joystick: stick level 0 → 1 IDLE (or C_IDLE 0x5a/0x59 when on a cart/carry…), 1–2 → 10 WALK / 0x44 H_WALK, 3 → 0xb RUN / 0x46 H_RUN.
* Walk key (10) held: RUN→WALK(10), H_RUN→H_WALK(0x44).
* request = Unit_set_action 0x501190(state,0,1) if canAct and posture≠1 and not guarding; RUN start stamps +0xa8. No move key & state ∈{9,10,0xb,0x44,0x45,0x46} → stop (0x508b90(0,1); range mode 0x5124b0(1)).
* Strafe set 0x438110 (fwd,back,left,right): back→0x10 MOVE_B, left→0xe MOVE_L, right→0xf MOVE_R, fwd+left→0x11 MOVE_FL, fwd+right→0x12 MOVE_FR, back+left→0x13 MOVE_BL, back+right→0x14 MOVE_BR, fwd only→0x437ed0(0xc,walk=1). In range mode uses upper/lower split 0x5124b0.
* Joystick stick level (0x433930): m = max over axes of `|axis − centre|*16/(max−min)`; level 0 if m<2, 1 if m=2, 2 if 3..4, 3 if ≥5.

#### 3.6 Horse controller 0x43c5c0 / 0x43dab0 / 0x43d7a0 / 0x43ca90 (CONFIRMED unless marked)
* F3/F4/R handled first; R while horse idle (0x43c920: rider state 0x43 H_IDLE, horse state 1..3) → dismount 0x4fc220(0).
* Attacks 0x43ca90: A → H_ATTACK 0x49 'A' (sound 0x41); B → 0x49 'B' only if move "B" is COMBOAVAIL or skill HORSE_ATTACK_B (type 0x10) learned (0x50d3d0), EP cost = ENERGY_PT×EP_CONSUME%/100. Combos AAA/BBB wrap to 1 hit. Same window logic as §3.8.
* Turning 0x43dab0: per frame (NOT dt-scaled) heading moves toward desired by at most **TURN_ANG = 6.0°** (0xa33740). Mode A (0xa5802c==0, KBM): desired = horseFacing + relAngle − 90 (≤180) / −270 (>180) → A/D steer relative to horse; S (270) ignored; camera follows by ≤ TURN_ANG°/frame (0x590b00 clamp). Mode B (==1): desired = camera yaw + relAngle − 90 (mouse steers); standing horse may pivot ±15° about +0xc9. Other input modes: like on foot (rate 0.002°/ms·offset), clamp ±TURN_ANG.
* Gears 0x43d7a0: forward→ 0x470430(1) walk / 0x466d50 run (H_RUN), back (moveCode 0x14/0xc/0xd or <0)→0x470430(0) H_BACK; no input → 0x4533e0 stop.
* Horse speeds (0x4f0b50): H_WALK 3.0, H_BACK −3.0, H_RUN 5.0 speed units.

#### 3.7 Range mode (CONFIRMED)
* Enter 0x42b3c0 on Z: map not peaceful (0x4cd510()≠1), canAct, posture≠1, alive. Sets IDLE, +0xac=1, +0xc0=+0x97=now, base yaw = current facing, camera 1 (first person) transition, SHTLOAD 0x30 if ranged weapon has ammo (0x497580()+2 ≠0), initial target via 0x42c260.
* Target acquisition 0x42c260: valid only if (mode 3 or |camYaw−baseYaw| ≤ AIM_HA 60°) and |camPitch−basePitch| ≤ AIM_HA. For every enemy unit (0x5094d0), alive, class+0x118 ∉{2,7}: target point = pos + (0,0,classRec.height(+0x109)*0.5); v = point − camPos; `perp = sqrt(|v|² − (v·f)²/|f|²)`; accept if `perp ≤ |v|/100 + AIM_R(25)` and `|v| < best` (best init AIM_MXD 2500). Returns nearest.
* Fire 0x42ba40: A pressed while state 0x36 SHTIDLE, with ranged weapon + ammo: dir = normalize(target.pos + (0,0,60) − camPos) or camera forward; unit+0x365 = dir; action 0x33 SHTOUT.
* Exit 0x42b6e0: Z pressed (modes 0/3); joystick: when key released (hold-to-aim).
* Aim 0x42bcd0: yaw/pitch from mouse (k·0.08) or keys 1°/2°/3° per frame by level; yaw limited around base (±AIM_HA in non-KBM).

#### 3.8 Attack input 0x4367c0 (CONFIRMED)
Button phase machine for A(+0x8e), B(+0x8f), A&B(+0x90): pressed: 0→1, 2→3; released: 1/3→2 (also →2 if combo length>0); fresh flags +0x92.. set on release.
* combo-reset: if unit+0x233==−1 and now−unit+0x23f > 250 ms → clear lock and length.
* **Rush** (dt==Forward double-tap, fires on the double tap alone, no A needed): needs +0x23f==0, canAct, posture≠1, move 0x53 RUSH in list and 0x500d40(0x53) (COMBOAVAIL Y or skill RUSH_ATTACK type 0xc learned, and EP > rec.+0x18), cost = rec.ENERGY_PT × EP_CONSUME(item type 0x15, base 100)/100 (int); EP ≥ cost → EP−=cost, action 0x53.
* **A** pressed, +0x23f==0:
  * canAct, on ground, dt≠4 → action ATTACK 0x1c with letter 'A' (0x501190(0x1c,0x41,1)), sound 0x41, +0x97=now. If dt==4 (rush gate above failed) → fallback rush: move 0x53 exists and rec.+0x18 ≤ EP (no learned check, no EP_CONSUME scaling) → EP−=cost, RUSH 0x53.
  * canAct, airborne → if no combo yet, state ∉{0x1a UP,0x18 DOWN}, now−takeoff > 50 ms → ATTACK 'D' (jump attack A).
  * busy (attacking): if combo length>0, phase==3 (re-pressed), fresh, and `unit+0x237 ≤ now−+0x97 < unit+0x23b` → queue A (+0x9b=1,+0x96=1,unit+0x235=1). Also allowed if chain count +0x9d>1.
* **B** pressed: ground idle → find move ATTACK "B" (0x416e30(0x1c,"B")); cost (scaled) ≤ EP → EP−=cost, sound 0x42, ATTACK 'B'. Airborne → JATTACKB 0x1d with "E" if COMBOAVAIL or skill JUMP_ATTACK_B (type 10) learned; cost likewise. Busy → queue B (+0x9b=2) with same window.
* Queued hit fires when `now−+0x97 ≥ unit+0x23b` and +0x96: find move record matching (posture,state,weapon) whose combo string == current string + letter exactly (≤6 chars), learned (COMBOAVAIL Y or 0x50d170). Fire ATTACK with letter (B deducts its EP cost; C = 0x43). Not found → reset phases.
* Window expired without queue, canAct, length 0 → reset phases.
* **HATK_MP (0xa3cbd0) is loaded but never read** (CONFIRMED: sole xref is writer 0x51c974). Heavy-attack EP cost is per-move ENERGY_PT (e.g. Joan B = 5).

#### 3.9 Energy attack 0x437910 (CONFIRMED asm)
```
costEP = ftol( (float)EP_CONSUME * ((float)(maxEP*EGATK_MP)/100.0f) / 100.0f )  // EP_CONSUME = 0x497e10(0x15) base 100 (%, ×item bonuses)
costHP = ftol( (float)(maxHP*EGATK_HP) / 100.0f )
```
(maxEP 0x523700, maxHP 0x523320; EGATK_MP=40, EGATK_HP=20 → 40 % EP, 20 % HP). Peaceful map (0x4cd510()==1) → swallow key. Conditions: trigger, canAct, posture≠1, unit+0x23f==0, EP ≥ costEP, HP > costHP, and (0x500d40(0x1e): COMBOAVAIL/learned and not 0x432b90, or skill ENERGY_ATTACK type 0xd learned 0x50d4b0). Effects: action ENGATK 0x1e, sound 0x45, EP=EP−costEP, HP=HP−costHP, unit+0x151=now, +0xa8=0, +0x96=0.

#### 3.10 Block / down attack (CONFIRMED)
* Block 0x436250: Defend held, canAct, posture≠1 → GUARD1 0x2b (if not already 0x2b/0x2d), sound 0x43. Released while 0x2b/0x2d → 0x508b90(0,1).
* Down attack 0x4363a0: Defend held, canAct, not guarding/0x3a, posture≠1, move 0x22 DOWNATK exists with anim rec angles a1=rec+0x40,a2=rec+0x44 (sorted). Any enemy unit in state 0x40/0x41/0x42 (knocked down) or 0x29 (DNATKED), +0x1e1==0, with relative bearing (0x505960) in [a1,a2] (±360 wrap) and distance² ≤ 3600 (60 units) → if 0x500d40(0x22) (COMBOAVAIL / learned / skill DOWN_ATTACK 0xb) → DOWNATK 0x22. Down attack has priority over Block.

#### 3.11 Chain 0x438820 (CONFIRMED)
Chain key pressed and 0x504010() finds a CHAIN record for current state: if `animRemaining_ms ≤ (CHAINPRO(200)/1000) * animLength_ms / chainCount(+0x9d)` and record+0xf ≠ ' ' → action CHAIN 0x1b, +0x9d++, +0x96=1, +0x97=now. (animRemaining = 0x5045f0, animLength = 0x504760.)

#### 3.12 Jump 0x437b60 (CONFIRMED asm)
Conditions: fire, canAct, posture≠1, state∉{5,6}.
```
dirByte = ftol(worldDir_deg * 0.7111111f)          // 256/360; worldDir = (camYaw°+relAngle−90) mod 360
base = 1400                                         // 0x578
if terrain normal n under model exists and dot(n.xy, (cos,sin)) < 0:  base = ftol(1400 * n.z)   // uphill
vx = cos(dirByte) * (float)(base*magnitude);  vy = sin(dirByte) * (float)(base*magnitude)
vz = min(hold, JUMPMAXD) * 5000.0f / JUMPMAXD + 5000.0f      // 5000..10000
0x505cc0(vx,vy,vz): unit+0x351=now (if vz≠0); world+0xb16 = (float)unit+0x243; model+0x60..68 = v
action UP 0x1a, sound 0x44, +0xa8=0
```
magnitude = 3 when any direction held in KBM/KO (stick level 0..3 in joystick). sin/cos from 256-entry quarter table 0xa640ec (0x5768d0 sin, 0x576ac0 cos). If not fired but trigger set: +0x9e=0.
Air control: while state UP and animRemaining > 180 ms, 0x4352f0 rewrites body horizontal velocity = dir × magnitude×1400 (CONFIRMED).
Physics (0x6a45a0): gravity accel = −9.81 × g, g = (float)unit+0x243 (anim rec +0x48, per-move) or world default 1.0; dt factor 0.001 (per-ms → s), substep limit 20 (CONFIRMED constants 0x9dd75c=−9.81, 0x3a83126f=0.001, 0x41a00000=20). Exact g values UNKNOWN (data-driven).

#### 3.13 Items, switching (CONFIRMED)
* F3 0x43b490: among inventory items of kind 0x15 or 0x19 pick the one whose amount (+0x4e) is closest to (maxHP − HP) (ties → lowest index), apply 0x50bcd0/0x50b4f0, decrement stack, effect "spiritual.txt" 5000 ms.
* F4 0x43b990: kind 0x17, closest to (maxEP − EP), apply 0x50ba00.
* Switch commander 0x432300(n): 0 = next of the 4 heroes (JOAN_OF_ARC… table 0x9f8b10), 1..4 = specific.

#### 3.14 Skill gating (upgrade type ids, 0x531ba0 CONFIRMED)
1 STRENGTH, 2 DEXTERITY, 3 DEFENSE, 4 CHARISMA, 5 LEADERSHIP, 6 HIT_POINT, 7 ENERGY_POINT, 9 NEW_COMBO, 10 JUMP_ATTACK_B, 0xb DOWN_ATTACK, 0xc RUSH_ATTACK, 0xd ENERGY_ATTACK, 0xe RANGE_ATTACK, 0xf HORSE_ATTACK_A, 0x10 HORSE_ATTACK_B. Joan's JOANUP.csv levels (0-based): RUSH 3, JUMP_ATTACK_B 8, HORSE_ATTACK_B 12, ENERGY_ATTACK_UP 19 → manual levels 4/–/13/20 (HIGH). Learned check 0x50d3d0(type) = entry with cur==max.

---------------------------------------------------------------------------------------------------
### 4. Action/state enum (unit+0x215) — 0x415820 (CONFIRMED)
0 DEFAULT,1 IDLE,2 IDLE2,3 IDLE3,4 IDLE_BAT,5 IDLEUP,6 IDLEDN,7 IDLE_T,8 JATTBI,9 TIPTOE,10 WALK,11 RUN,12 T_LEFT,13 T_RIGHT,14 MOVE_L,15 MOVE_R,16 MOVE_B,17 MOVE_FL,18 MOVE_FR,19 MOVE_BL,20 MOVE_BR,21 JUMP_B,22 JUMP_R,23 JUMP_L,24 DOWN,25 JATTBD,26 UP,27 CHAIN,28 ATTACK,29 JATTACKB,30 ENGATK,31 ENGATK2,32 ENGATK3,33 ENGATK4,34 DOWNATK,35 ATTACKED,36 ATTKED2,37 ATTKEDR,38 BATTKED,39 LATTKED,40 RATTKED,41 DNATKED,42 DIE,43 GUARD1,44 GUARD2,45 GUARDR,46 BACK,47 CLIMB,48 SHTLOAD,49 SHTLOADC,50 SHTLOAD2,51 SHTOUT,52 SHTOUT2,53 SHTOUTC,54 SHTIDLE,55 SHTIDLEC,56 SHTOUTH,57 SHTIDLEH,58 THROW,59 THROWN,60 STANDUP,61 DIZZY,62 STOP,63 STOP_B,64 KNOCKDN,65 KNOCKDN2,66 KNOCKDN3,67 H_IDLE,68 H_WALK,69 H_BACK,70 H_RUN,71 H_STOP_B,72 H_STOP,73 H_ATTACK,74 H_HITED?,75 H_BHITED,76 H_LHITED,77 H_RHITED,78 H_DROP,79 H_MNT_L,80 H_MNT_R,81 H_DMNT_L,82 H_DMNT_R,83 RUSH,84 PUSH,85 PUSH2,86 PUSH_L,87 PUSH_R,88 BATTER_C,89 C_IDLE_A,90 C_IDLE_B,91 C_DN_AB,…,103 C_DROP,106 CA_WALK. (39/48/57/58/74/81 filled from string order, HIGH.)

---------------------------------------------------------------------------------------------------
### 5. Movement integration (Unit_move 0x4f0b50, called from 0x52daa0) (CONFIRMED unless marked)
```
S = per-state base speed (float):
   TIPTOE 9 → 1.0; WALK 10 → classRec+0x9c; RUN 0xb → +0xa0; MOVE_L 0xe, FL/FR/BL/BR → +0xa4;
   MOVE_R 0xf → +0xa8; MOVE_B 0x10 → +0xac; H_WALK 3.0; H_BACK −3.0; H_RUN 5.0; JUMP_B/R/L → 0 (lateral dir only)
   (classRec = 0x42d0a0(unit+0x3b), stride 0x203; mapping to units.tsv WalkSpeed/RunSpeed = HYPOTHESIS)
if state ∉ {MOVE_L, MOVE_R, MOVE_B}:   // dexterity bonus (rider's DEX when mounted)
   if DEX > MINDEXS(30): S *= (min(DEX, MAXDEXS 200) − 30)/(200 − 30) + 1.0      // up to ×2
if unit+0x102: S *= AI_MOVE (1.2)
S *= 1.3                                              // 0x9d51e8
if unit pointer missing (off-world LOD): LOD tier(+0x22) ≥3 → ×1.8; tier 2 and S<20 → ×1.4
if following a path waypoint (+0x102, 0x66e8a0): S = min(S, dist/46.1*1000/dt)   // don't overshoot
vel = (cos(dir)*S, sin(dir)*S, 0) → model+0x6c (0x505d90); dir from facing byte (strafes rotate dir via 0x576240)
body.move(vel, start=now, duration):  0x6b10a0(model+0x6c, now, dur)
   dur = dt (DAT_00a500be) normally; LOD tier ≤1 with +0x2a: min(now−unit+0x2a, dt);
   LOD tier ≥2: dur = max(dt, dt*dist/S*0.8) clamped 300 (units on map) / 1000 ms
```
* Speed unit: the overshoot clamp implies **1 speed unit = 46.1 world units per second** (displacement per frame = S·46.1·dt/1000) — HYPOTHESIS (physics body integration 0x6a45a0 not fully read). E.g. RUN 5 → 5·1.3·46.1 ≈ 300 wu/s.
* Physics body created lazily: 0x6adbf0(model, radius 60.0f (0x42700000), unit+0x102, 0, 2, 1) on world 0xa570a8 → model+0x5c (CONFIRMED). BODY_R (32, 0xa3cc9c) is used by unit-vs-unit separation 0x505dd0 / 0x4f7bf0, not by the player controller (CONFIRMED xrefs).
* Facing is stored in degrees (unit+0x1e), the movement direction as an 8-bit angle (256 = 360°).

---------------------------------------------------------------------------------------------------
### 6. Constants
| name | addr | value | used | conf |
|---|---|---|---|---|
| JUMPMAXD | 0xa3374c | 100 ms | jump hold cap/auto-fire | CONFIRMED |
| JUMPMIND | 0xa33748 | 100 | never read | CONFIRMED |
| CHAINPRO | 0xa33744 | 200 (‰ of anim) | chain window | CONFIRMED |
| TURN_ANG | 0xa33740 | 6.0°/frame | horse turn clamp | CONFIRMED |
| AIM_R | 0xa3373c | 25.0 | aim ray tolerance | CONFIRMED |
| AIM_MXD | 0xa33758 | 2500.0 | aim max dist | CONFIRMED |
| AIM_HA | 0xa33754 | 60.0° | aim half-angle | CONFIRMED |
| EGATK_MP/HP | 0xa3cd28/0xa3cd1c | 40/20 % | energy attack cost | CONFIRMED |
| HATK_MP | 0xa3cbd0 | 6 | never read | CONFIRMED |
| CLB_IDLE | 0xa3cce8 | 20 ms | canAct delay | CONFIRMED |
| MINDEXS/MAXDEXS/AI_MOVE | 0xa3cd00/0xa3cbdc/0xa3cc70 | 30/200/1.2 | speed | CONFIRMED |
| double-tap window | imm 0x12c | 300 ms | evade/rush | CONFIRMED |
| combo reset | imm 0xfa | 250 ms | +0x233==−1 | CONFIRMED |
| jump h-speed | imm 0x578 | 1400 × magnitude | jump | CONFIRMED |
| jump v-speed | 0x9cfc34 | 5000.0 (+5000·hold/100) | jump | CONFIRMED |
| gravity | 0x9dd75c | −9.81 × unit+0x243 | physics | CONFIRMED |
| down-attack range² | 3600.0 | 60 units | 0x4363a0 | CONFIRMED |
| air-attack delay | imm 0x32 | 50 ms after take-off | 0x4367c0 | CONFIRMED |
| speed scale | 0x9d51e8 / 0x9cf780 | 1.3 / 46.1 | Unit_move | CONFIRMED |
| cam auto-rotate | 3.4906585e-05 rad | per ms per degree offset | 0x4352f0 | CONFIRMED |
| pitch clamp | 0.61086524 / −0.34906584 rad | +35° / −20° | 0x431790 | CONFIRMED |

---------------------------------------------------------------------------------------------------

## 6. Part C — Melee combat

Scope: melee attack/action states, combos, hit detection, damage, block, knockdown, death, energy/heavy costs.
Conventions: `U+0xNNN` = byte offset in Unit object (0x4f8550 init). A = attacker, D = defender (the unit receiving the hit, `this`).
"now" = `DAT_00a500ba` game time ms; "dt" = `DAT_00a500be` ms elapsed this tick. 30 Hz logic ⇒ 1 tick ≈ 33.33 ms.
`PC` = player-controller object at `0xa5b9e8` (dword +0 = id of the player-controlled unit; +0x9d char = chain level, etc.).
RNG names per context: `RNG(n)`=FUN_00590540 → [0,n); `RNGR(lo,hi)`=FUN_00590690; `RNGF()`=FUN_00590660 (float [0,1], /32767).
Confidence labels: CONFIRMED (code+asm read) / HIGH / HYPOTHESIS / UNKNOWN.

---------------------------------------------------------------------------------------------------
### 1. Action/animation state enum (`U+0x215`, short)

`U+0x215` is the current action id and is the same enum as the animation MOVEMENT name table parsed by
`AnimName_to_id` 0x415820 (CONFIRMED: SetAction 0x501190 stores param_1 into +0x215; anim records are matched by
`rec[1]==U+0x215`, 0x504010). `U+0x219` = previous action (CONFIRMED 0x501e9b).

| id | name | id | name | id | name | id | name |
|---|---|---|---|---|---|---|---|
|0x00|DEFAULT|0x1b|CHAIN|0x36|SHTIDLE|0x51|H_DMNT_L (HIGH, by elimination)|
|0x01|IDLE|0x1c|ATTACK (all ground/jump A/B combos)|0x37|SHTIDLEC|0x52|H_DMNT_R|
|0x02|IDLE2|0x1d|JATTACKB (jump heavy)|0x38|SHTOUTH|0x53|RUSH|
|0x03|IDLE3|0x1e|ENGATK|0x39|SHTIDLEH (HIGH)|0x54|PUSH|
|0x04|IDLE_BAT|0x1f|ENGATK2|0x3a|THROW|0x55|PUSH2|
|0x05|IDLEUP|0x20|ENGATK3|0x3b|THROWN|0x56|PUSH_L|
|0x06|IDLEDN|0x21|ENGATK4|0x3c|STANDUP|0x57|PUSH_R|
|0x07|IDLE_T|0x22|DOWNATK|0x3d|DIZZY|0x58|BATTER_C|
|0x08|JATTBI (jump-heavy idle/landing-idle)|0x23|ATTACKED (front hit)|0x3e|STOP|0x59..0x67|C_* (siege crew)|
|0x09|TIPTOE|0x24|ATTKED2 (alt front hit)|0x3f|STOP_B|0x68|CA_IDLE|
|0x0a|WALK|0x25|ATTKEDR|0x40|KNOCKDN|0x69|CA_RUN (HIGH)|
|0x0b|RUN|0x26|BATTKED (hit from back)|0x41|KNOCKDN2|0x6a|CA_WALK|
|0x0c/0d|T_LEFT/T_RIGHT|0x27|LATTKED (hit from left)|0x42|KNOCKDN3| | |
|0x0e..14|MOVE_L/R/B/FL/FR/BL/BR|0x28|RATTKED (hit from right)|0x43..48|H_IDLE,H_WALK,H_BACK,H_RUN,H_STOP_B,H_STOP| | |
|0x15..17|JUMP_B/JUMP_R/JUMP_L (evade jumps)|0x29|DNATKED (hit while down)|0x49|H_ATTACK| | |
|0x18|DOWN (landing)|0x2a|DIE|0x4a..4d|H_HITED,H_BHITED,H_LHITED,H_RHITED| | |
|0x19|JATTBD (jump-heavy impact)|0x2b|GUARD1 (blocking)|0x4e|H_DROP (thrown off horse)| | |
|0x1a|UP (jump)|0x2c|GUARD2 (guard release)|0x4f/0x50|H_MNT_L/H_MNT_R| | |
| | |0x2d|GUARDR (block reaction)|0x30..0x35|SHTLOAD,SHTLOADC,SHTLOAD2,SHTOUT,SHTOUT2,SHTOUTC| | |

All ids CONFIRMED from 0x415820 strcmp chain except the three marked HIGH (their `iVar1=` assignment is split by
the decompiler; values follow the sequence).

Groups used by code (all CONFIRMED):
* "attack" states (`IsAttacking` 0x510f80): 0x08,0x19,0x1c,0x1d,0x1e,0x1f,0x20,0x21,0x22,0x46,0x49,0x53.
* "knocked down": 0x40,0x41,0x42 (`IsKnockedDown` 0x508fb0 = 0x40..0x42); plus 0x4e,0x29 in most checks.
* "can start new action" `CanAct` 0x500ba0: now ≥ `U+0x20f` + CLB_IDLE(20 ms) AND state ∈ {−1,0..6,9..0x14,
  0x2b..0x2d,0x36,0x37,0x39,0x43..0x46,0x54..0x57,0x59,0x5a}. (Guard states allow attacking out of block.)
* `U+0x213` short = posture: 0 = STAND, 1 = JUMP (anim POSITION column; CONFIRMED 0x504010 `rec[0]==U+0x213`,
  JUMP rows use it; 4367c0 chooses jump attacks when ==1).

#### 1.1 Per-unit animation table record (0x99 bytes, loader 0x414a30 "animgrp.cpp")
Source: `std_set/<UNIT>.csv` (e.g. JOAN.csv) columns → record. `U+0xa` int = index of current record
(FUN_004168d0(idx) on the unit's table `U+0x2f1`). CONFIRMED offsets:

| off | type | column | use |
|---|---|---|---|
|+0x00|int|POSITION (STAND=0/JUMP=1)|match `U+0x213`|
|+0x04|int|MOVEMENT (action id)|match `U+0x215`|
|+0x08|char[6]|COMBO string (A/B/C/D/E…)|combo matching|
|+0x0e|char|COMBOAVAIL 'Y'/'N'|'Y' = usable without learning (`AnimRec_isFree` 0x416ed0)|
|+0x0f|char|CHAINAVAIL ('1' or ' ')|' ' ⇒ CHAIN not allowed from this anim (0x438820)|
|+0x10|int|ITEM_USING (weapon type)|match `U+0x21b`|
|+0x14|int|SPECIAL|match `U+0x21d`|
|+0x18|int|ENERGY_PT|EP cost of starting this action|
|+0x1c|int|DAMAGE|attack-power multiplier (see §3)|
|+0x20|int|BLEND_TIME| |
|+0x24|int|START_TIME → copied to `U+0x237`|combo input window start (ms since attack start)|
|+0x28|int|END_TIME → `U+0x23b`|combo input window end / time queued follow-up fires|
|+0x6e|ptr|anim handle| |
|+0x72|int(short used)|SCRIPT → animscr record id (table 0xa5d0f8)|hit timing/range/arc/force|
|+0x98|bool|SHOW_CRIT|eligible for critical sequence (HYPOTHESIS; list builder not traced)|

#### 1.2 Attack script record (`Resource/animscr.res`, parser `AnimScript_parse` 0x419320; getter 0x416f20 on 0xa5d0f8)
| off | field | default | melee use |
|---|---|---|---|
|+0x28|StartTime (ms)| |trigger fire time = action start + StartTime (CONFIRMED 0x502aa6)|
|+0x2c|EndTime| |not used by melee (trigger end = start) (CONFIRMED)|
|+0x30|Range| |hit radius (+ weapon reach)|
|+0x34|StartForceTime| |not used in melee path (UNKNOWN use)|
|+0x38/+0x3c|Start/EndVerticalAngle| |not used by melee (CONFIRMED: only +0x40/+0x44 read in 0x5048b0)|
|+0x40/+0x44|Start/EndHorizontalAngle| |hit arc relative to attacker facing|
|+0x48|GravityLevel (float)|3.0|`U+0x243 = ftol()`|
|+0x4c|AttackForce|3000|horizontal knockback + balance loss|
|+0x50|VerticalAttackForce|500|vertical knockback + balance loss|
|+0x75|EmitterFlag|..|hit/swing particle emitters|

---------------------------------------------------------------------------------------------------
### 2. Combos

#### 2.1 Combo bookkeeping fields (CONFIRMED)
| field | type | meaning |
|---|---|---|
|U+0x227..0x22c|char[6]|current combo string (chars appended by SetAction: `combo[idx]=param_2; if(param_2) idx++` 0x501f0c)|
|U+0x233|signed char|combo index (# chars). 0 = none, −1 (0xFF) = combo finished → lockout|
|U+0x235|short|"follow-up queued" flag|
|U+0x237 / U+0x23b|int|input window [START_TIME, END_TIME) of current anim (ms since attack start)|
|U+0x23f|int|time the combo/attack finished (lockout start); 0 = free|
|PC+0x8e/+0x8f/+0x90|char|edge state of A / B / A+B button: 0 idle,1 pressed,2 released,3 re-pressed|
|PC+0x92/+0x93/+0x94|char|"accept one queued input" flags|
|PC+0x96|char|follow-up queued|
|PC+0x97|int|time current attack started|
|PC+0x9b|short|queued key 1=A,2=B,3=C,4=none|
|PC+0x9d|char|CHAIN level (damage multiplier, §3)|

`ClearCombo` 0x50d0d0: zero combo string, idx=0 unless idx==−1, U+0x235=0. SetAction calls it for every action except
ATTACK(0x1c)/H_ATTACK(0x49) (CONFIRMED, call at 0x5018e2 guarded by `p1!=0x1c&&p1!=0x49`), so any non-attack action (incl. CHAIN) restarts
the combo string.

#### 2.2 Player attack input `Player_attackInput(a,b,rush)` 0x4367c0 (CONFIRMED)
1. If `idx<1`: set accept flags 0x92..0x95 = 1. Update edge states.
2. If `idx==−1 && now−U+0x23f > 250` → `U+0x23f=0, idx=0` (**250 ms post-combo lockout**). All new attacks require `U+0x23f==0`.
3. Rush (param_3 = double-tap forward): if CanAct && posture≠JUMP && RUSH anim exists && `0x500d40(0x53)` (learned:
   anim rec ENERGY_PT < EP and (COMBOAVAIL or RUSH_ATTACK skill)): cost = `rec.ENERGY_PT*EP_CONSUME/100` (int; heroes);
   if EP ≥ cost: EP−=cost; SetAction(RUSH).
4. A pressed, `U+0x23f==0`:
   * attacking (¬CanAct): if `idx≥1 && Aedge==3 && accept` (or `chainLevel>1 && Aedge==3 && accept`): accept=0; if
     `U+0x237 ≤ now−PC+0x97 < U+0x23b` → queue 'A' (0x9b=1, 0x96=1, U+0x235=1).
   * jumping: if combo empty and state∉{UP,DOWN} and `now−U+0x351>50` → SetAction(ATTACK,'D') (jump attack, EP 0).
   * idle ground, no rush: HUD/crit key 'A' (0x462810(0x41,1)), SetAction(ATTACK,'A'), PC+0x97=now.
   * idle ground with rush flag: RUSH if `rec.ENERGY_PT ≤ EP` (**no EP_CONSUME scaling on this path** — CONFIRMED).
5. B pressed: same queue logic with 'B'; jumping → JATTACKB 'E' (needs JUMP_ATTACK_B skill(10) or COMBOAVAIL; cost
   ENERGY_PT·EP_CONSUME/100; PC+0x97=now); ground → ATTACK 'B' if EP ≥ cost (cost deducted, crit key 'B').
6. Execute queued follow-up when `now−PC+0x97 ≥ U+0x23b` and 0x96==1: find anim rec with same posture/state/weapon/special
   whose COMBO == current string + queued char, exact length idx+1, and (COMBOAVAIL=='Y' or learned NEW_COMBO 0x50d170).
   State CHAIN(0x1b) is always accepted. Not found → reset all edge flags (combo ends). Found: 'A'→SetAction(ATTACK,'A');
   'B'→ cost check `ENERGY_PT·EP_CONSUME/100 ≤ EP` then SetAction(ATTACK,'B') + EP−=cost (if EP short nothing fires);
   'C'→SetAction(ATTACK,'C'). PC+0x97=now.

So: **next hit input window = [START_TIME, END_TIME) ms after the previous attack started (START_TIME=0 for all attack
rows); the queued attack starts exactly at END_TIME** (first tick ≥). Rows with END_TIME 0 are finishers (no follow-up).

Attack end (UpdateAction 0x7c7ab0, remaining anim time `0x5045f0`==0): ATTACK/JATTACKB/DOWNATK/CHAIN/RUSH/ENGATK* set
`idx=0xFF, U+0x23f=now` then return to idle (0x508b90) → 250 ms lockout.

#### 2.3 CHAIN (`Player_chain` 0x438820, called from player input 0x430480 on input action #0x15)
Condition: `remaining ≤ (CHAINPRO/1000.0 · animLength) / chainLevel` (CHAINPRO=200 → last 20 % of the current anim,
10 % at level 2, …) AND current anim rec CHAINAVAIL ≠ ' '. Then SetAction(CHAIN,0), `PC+0x9d++`, PC+0x96=1, PC+0x97=now.
CHAIN anim (Joan: END_TIME 600) does not create a hit trigger. Reset: SetAction on the player unit with any action other
than ATTACK/H_ATTACK/CHAIN sets PC+0x9d=1 (`0x503fb0`, call at 0x501915). (CONFIRMED)

#### 2.4 Combo set (Joan, std_set/JOAN.csv × animscr.res); same layout for every hero table
Learning: UPITEM.csv types NEW_COMBO(9) PARA1=combo string; hero `JOANUP.csv` LEVEL column = level−1 needed
(rush 3→manual L4, horse heavy 12→L13, energy 19→L20: HIGH).

Hit time column = StartTime of the script (ms after action start), ticks = ms/33.33. Balance effect computed with §5.

| Pos | Action (id) | Combo | AVAIL | CHAIN | EP | DAMAGE | END_TIME ms (next-input deadline / next start) | SCRIPT | hit @ms (ticks) | Range | H-arc ° | Force H/V | Balance effect |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| STAND | ATTACK (0x1c) | A | Y | 1 | 0 | 2 | 317 | JOAN_ATK_A | 267 (8.0) | 74 | [-50,30] | 3000/0 | -12 |
| STAND | ATTACK (0x1c) | B | Y | 1 | 5 | 6 | 683 | JOAN_ATK_B | 600 (18.0) | 74 | [-105,95] | 4500/3000 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | AA | Y | 1 | 0 | 3 | 333 | JOAN_ATK_AA | 300 (9.0) | 59 | [-110,100] | 3000/0 | -12 |
| STAND | ATTACK (0x1c) | AB | Y | 1 | 5 | 6 | 0 | JOAN_ATK_AB | 483 (14.5) | 64 | [-100,110] | 8000/2000 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | BB | N | 1 | 5 | 7 | 867 | JOAN_ATK_BB | 633 (19.0) | 84 | [-30,40] | 2500/4000 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | AAA | Y | 1 | 0 | 4 | 417 | JOAN_ATK_AAA | 383 (11.5) | 89 | [-40,180] | 3000/1000 | -16 |
| STAND | ATTACK (0x1c) | AAB | Y | 1 | 5 | 7 | 0 | JOAN_ATK_AAB | 467 (14.0) | 114 | [-10,180] | 8000/2000 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | BBB | N | 1 | 5 | 7 | 667 | JOAN_ATK_BBB | 683 (20.5) | 89 | [-101,90] | 6500/2800 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | AAAA | Y | 1 | 0 | 4 | 417 | JOAN_ATK_AAAA | 400 (12.0) | 69 | [-100,80] | 3000/1000 | -16 |
| STAND | ATTACK (0x1c) | AAAB | Y | 1 | 5 | 7 | 0 | JOAN_ATK_AAAB | 325 (9.8) | 59 | [-45,45] | 4000/2000 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | BBBB | N | 1 | 5 | 10 | 1000 | JOAN_ATK_BBBB | 417 (12.5) | 80 | [-60,60] | 6000/2800 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | AAAAA | N | 1 | 0 | 5 | 433 | JOAN_ATK_AAAAA | 383 (11.5) | 94 | [-45,70] | 3000/2500 | -22 |
| STAND | ATTACK (0x1c) | AAAAB | N | 1 | 5 | 7 | 0 | JOAN_ATK_AAAAB | 500 (15.0) | 64 | [-180,180] | 8000/2000 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | BBBBB | N | 1 | 5 | 14 | 0 | JOAN_ATK_BBBBB | 833 (25.0) | 144 | [-90,90] | 7000/4000 | KNOCKDOWN (bal=0) |
| STAND | ATTACK (0x1c) | AAAAAB | N | 1 | 5 | 10 | 0 | JOAN_ATK_AAAAAB | 541 (16.2) | 74 | [-140,30] | 10000/2000 | KNOCKDOWN (bal=0) |
| STAND | RUSH (0x53) | A | N | - | 5 | 3 | 0 | JOAN_RUSH_ATK | 333 (10.0) | 94 | [-90,160] | 8000/1000 | KNOCKDOWN (bal=0) |
| STAND | DOWNATK (0x22) | - | Y | - | 0 | 8 | 0 | JOAN_DOWN_ATK | 400 (12.0) | 64 (no weapon reach) | [-40,40] | 0/0 | (no force applied) |
| STAND | ENGATK (0x1e) | - | N | - | 10 | 8 | 500 | JOAN_ATK_ENERGY_A | 1150 (34.5) | 120 | [-180,180] | 5000/2000 | KNOCKDOWN (bal=0) |
| STAND | ENGATK2 (0x1f) | - | N | - | 10 | 8 | 500 | JOAN_ATK_ENERGY_B | 233 (7.0) | 80 | [-180,180] | 5000/2000 | KNOCKDOWN (bal=0) |
| STAND | ENGATK3 (0x20) | - | N | - | 10 | 8 | 500 | JOAN_ATK_ENERGY_C | 233 (7.0) | 80 | [-180,180] | 5000/2000 | KNOCKDOWN (bal=0) |
| STAND | JATTBD (0x19) | - | Y | - | 0 | 8 | 700 | JOAN_ATK_JUMP_B_END | 10 (0.3) | 70 | [-180,180] | 3000/3000 | KNOCKDOWN (bal=0) |
| JUMP | ATTACK (0x1c) | D | Y | - | 0 | 5 | 500 | JOAN_ATK_JUMP | 200 (6.0) | 74 | [-45,45] | 4000/0 | -16 |
| JUMP | JATTACKB (0x1d) | E | N | - | 5 | - | 500 | JOAN_ATK_JUMP_B | 100 (3.0) | 0 | [-20,20] | 0/0 | -0 |
| JUMP | JATTBI (0x8) | - | Y | - | 0 | - | 500 | JOAN_ATK_JUMP_ENERGY_IDLE | 10 (0.3) | 0 | ? | 0/0 | -0 |
| STAND | H_RUN (0x46) | B | Y | - | 6 | 1 | 500 | horse_rush_atk | 0 (0.0) | 77 | [-30,30] | 8000/1500 | KNOCKDOWN (bal=0) |
| STAND | H_WALK (0x44) | B | Y | - | 6 | 1 | 500 | horse_rush_atk | 0 (0.0) | 77 | [-30,30] | 8000/1500 | KNOCKDOWN (bal=0) |
| STAND | H_ATTACK (0x49) | A | Y | - | 0 | 5 | 500 | joan_hor_atk_a | 267 (8.0) | 55 | [-80,40] | 5000/900 | -23 |
| STAND | H_ATTACK (0x49) | AA | Y | - | 0 | 5 | 617 | joan_hor_atk_b | 333 (10.0) | 55 | [-50,150] | 5000/900 | -23 |
| STAND | H_ATTACK (0x49) | AAA | Y | - | 0 | 5 | 467 | joan_hor_atk_a | 267 (8.0) | 55 | [-80,40] | 5000/900 | -23 |
| STAND | H_ATTACK (0x49) | B / BB / BBB | N | - | 3 | 7 | 533 | joan_hor_eng_atk | 167 (5.0) | 70 | [-180,180] | 8000/1000 | KNOCKDOWN (bal=0) |

Data from tables (CONFIRMED as data); semantics of each column as in §1.1. JATTACKB lands → DOWN(0x18) is remapped to
JATTBD (0x19) by SetAction when previous state was JATTACKB/JATTBI; IDLE after JATTACKB → JATTBI (CONFIRMED 0x502040..0x50208a).
ENGATK chain: at anim end ENGATK→ENGATK2→ENGATK3→ENGATK4 if next anim exists and (COMBOAVAIL=='Y' or ENERGY_ATTACK skill
progress > 0, 0x50d4b0) — each stage SetAction creates its own hit trigger (CONFIRMED 0x7c7ab0 cases 0x1e..0x20).
Horse lowercase script names presumably resolve case-insensitively (HYPOTHESIS; lookup 0x41a0e0 not read).

#### 2.5 Skill (combo) level multiplier `AttackSkillMult` 0x510750 (CONFIRMED, float)
`lvl = SkillLevel(type, str)` 0x50d3d0: scans unit's learned-upgrade list (records [0]=UPITEM id,[1],[2],[3]=level);
returns `[3]` of the last record whose UPITEM type==`type`, whose PARA1 string == `str` (str==0 ⇒ any) and `[1]==[2]`
(HYPOTHESIS: "fully learned"); else 0.
`m = (lvl + 10.0f) / 11.0f` (level 0 ⇒ 0.909, 1 ⇒ 1.0, 10 ⇒ 1.818). Then for heroes (`U+0x1c1`≠0) × `(bonus+100)*0.01f`:

| A state | skill type, str | item bonus |
|---|---|---|
|ATTACK 0x1c|9 NEW_COMBO, full combo string U+0x227|last char=='A' → 0xb A_ATTACK_DAMAGE else 0xc B_ATTACK_DAMAGE|
|DOWNATK 0x22|0xb DOWN_ATTACK, any|0xf DOWN_ATTACK_DAMAGE|
|JATTACKB 0x1d / JATTBI 8 / JATTBD 0x19|10 JUMP_ATTACK_B, any|0xd JUMP_ATTACK_B_DAMAGE|
|RUSH 0x53|0xc RUSH_ATTACK, any|0xe RUSH_ATTACK_DAMAGE|
|ENGATK 0x1e..0x21|0xd ENERGY_ATTACK, any|none|
|H_ATTACK 0x49|last char 'A' → 0xf HORSE_ATTACK_A else 0x10 HORSE_ATTACK_B, str = combo string|0xb / 0xc|
|anything else (e.g. H_RUN)|—|m = 1.0|

Note: UPITEM PARA1 for HORSE_ATTACK_A is "0"/"Horse" ≠ "A"/"AA"… ⇒ horse attacks always get lvl 0 ⇒ ×0.909 (HIGH, data-dependent).
Jump 'D' attack is ATTACK with string "D" → NEW_COMBO lookup fails ⇒ ×0.909 (HIGH).

---------------------------------------------------------------------------------------------------
### 3. Hit detection

#### 3.1 Trigger creation (SetAction 0x501190, CONFIRMED asm 0x502a8b..0x502bb1)
On SetAction(p) with p ∈ {0x1c,0x1d,0x08,0x19,0x1e,0x1f,0x20,0x21,0x49,0x53,0x46,0x44}:
* `reach = max(weaponItem(slot U+0x1bd).+0x32, weaponItem(slot U+0x1bf).+0x32, 0)` (itemres +0x32 = weapon range).
* `S = animscr[ animRec(U+0xa).SCRIPT ]`
* Spawn two TRIGGER_ATTACK messages via `Trigger_create` 0x4eef00 (manager 0xa5ccd0):
  `type=1, targetClass=1 (units) and =2 (objects), senderClass=1, sender=A.id, pos = A.pos (U+0xe) at action start,
  radius = (float)(S.Range + reach), start = end = now + S.StartTime, p16 = 0`.
  If A has no actor (`U+0x2f5`==0, off-screen/LOD): radius = reach+50, start=end=now, plus an INTERACT(0x200) r=100.
* DOWNATK (0x22): one trigger type 0x400 (TRIGGER_DOWN_ATTACK), targets units, radius = S.Range only (reach not added;
  the reach code assigns both slots to the same local — bug), start=end=now+S.StartTime.
* H_ATTACK while the mount (`U+0x1e2` unit) is in RUN (0xb): two extra triggers using the H_RUN anim's script
  (radius Range+reach, start now+StartTime) with p6=2 and **p16 = H_RUN script id** (force override, below).
* THROW (0x3a): TRIGGER_THROW(0x10) aimed at `U+0x2d7` target.

#### 3.2 Trigger dispatch (`Trigger_process` 0x4ef090, per tick)
* Expired = (inactive `+0x2e==0`) && end≠0 && now ≥ end → removed (0x43a530). When `now ≥ start`: set inactive and deliver
  once ⇒ **each melee trigger fires exactly once, on the first tick with now ≥ start** (CONFIRMED).
* Unit targets: every unit except the sender whose class TriggerType mask (`class+0x11c`) has bit `type`.
  For attack/down-attack from a unit: sender must still be `IsAttacking` (0x510f80) and, if sender is H_ATTACK,
  `now − actor.animStart(+0x4c) > 79 ms` unless p16≠0. Sphere test `|D.pos − trig.pos|² ≤ radius²` (3-D, float).
  Knocked-down targets (0x40..0x42) are skipped only for horse-charge triggers (sender H_RUN or p16≠0).
  Handler = `Unit_onMessage` 0x75d540; return value 2 stops the scan (down attack, throw) ⇒ **normal melee hits every
  valid unit in range (AoE); down attack hits only the first accepted unit**.

#### 3.3 Per-target validation `Melee_tryHit(attackerId)` 0x5048b0 (this = D; CONFIRMED asm)
Returns damage (float) or 0. In order:
1. D.state==DIE or D dead (`HP<1 && lives(U+0x120)==0`, 0x42d0e0) → 0. A==D → 0.
2. D.state ∈ {ENGATK1-4, RUSH, H_MNT_L/R, H_DMNT_L/R} → 0 (super-armor while doing these).
3. D knocked (0x40-0x42,0x4e,0x29) and A.state≠DOWNATK: D needs physics body (`actor+0x5c`≠0) else 0; if `A.idx≠0` and
   A's last combo char == 'A' → 0 (light hits never juggle downed units; B/jump/rush/energy do).
4. A.state==DOWNATK and D not knocked (0x40-0x42,0x4e,0x29) → 0.
5. `D.U+0x1dd` (invulnerability ms) > 0 and A.state≠DOWNATK → 0.
6. A.state==DOWNATK and `D.U+0x1e1` (already down-attacked) → 0.
7. `A.U+0x2ed`==0 → 0 (attacker not instantiated in the active scene; HIGH).
8. `reach` = slot1 weapon reach if present else slot0 (bug: no max), max(…,0). Not hostile (`Diplomacy_isHostile`
   0x4b6860 on 0xa57f80 (D.side U+0x2, A.side)) → 0 (**no friendly fire**).
9. `|D.pos − A.pos|²` (3-D, current positions) > `(R+reach)²` where R = animscr(A current anim).Range → 0.
10. `ang = RelAngle(A→D)` (0x505960/0x505a00: heading to target minus own facing, degrees, normalised to [−180,180];
    HYPOTHESIS: +ve = right). lo=min(H0,H1), hi=max(H0,H1); if ang∉[lo,hi]: ang≥0 → ang−=360, reject if ang<lo;
    ang<0 → ang+=360, reject if ang>hi.
11. `ang2 = RelAngle(D→A)` (attacker as seen from defender's facing).
    **Block**: if D.state ∈ {GUARD1 0x2b, GUARDR 0x2d} and `−BLOCKANG ≤ ang2 ≤ BLOCKANG` (inclusive, BLOCKANG=70):
    play "DEFEND" sound script, effect "GUARD" at D.pos, then if D has GUARDR anim: if D.state≠GUARDR → SetAction(GUARDR)
    else only restart when GUARDR anim finished; return 0. **No EP/HP cost for blocking** (CONFIRMED: no EP write in path).
12. Hit: optional emitter/sound from A's script; `defIgnore` = DOWNATK ? `RNGR(50,100)` :
    `|ftol(ang2)| ≤ 45 → 0; ≤ 135 → 20; else 40` (percent of D's DEF stat ignored; front/side/back).
    `dmg = Melee_applyDamage(D, A.id, defIgnore)` 0x509950. If A is the player unit → HUD target bar 0x6fe280.
    If dmg≠0 and `D.U+0x102`≠0 → `D.U+0x2df += 10` (threat, HYPOTHESIS).

BODY_R (0xa3cc9c) is NOT used in melee (only collision 0x505dd0/0x4f7bf0); REACH_DT (0xa3cc28) is AI approach distance
(0x52daa0, 0x535020, 0x4bd420…), not hit detection (CONFIRMED by xrefs).

---------------------------------------------------------------------------------------------------
### 4. Damage pipeline (all float32; `ftol` = truncate toward zero)

#### 4.1 Attack power `Unit_meleeAttackPower` 0x5104f0 (this = A; CONFIRMED asm 0x510500..0x5106c0)
```
DMG    = (float) animRec(A.U+0xa).DAMAGE              // current action's DAMAGE column
wpn    = 0
if hero (A.U+0x1c1):
    for slot in 0,1: if item = inv.slot(slot):          // 0x4974f0
        wpn += (float)( it.base(+0x4e) + (inst.level - it.minLevel(+0x36)) * it.perLevel(+0x52) )   // 0x4a41c0
    wpn = (float)(Bonus(7 WEAPON_DAMAGE)+100) * 0.01f * wpn
t      = (wpn - (float)WP_DAMG) + 100.0f               // WP_DAMG = 0
atk    = ((float)(STR(A) * STR_DAM) + t) * DMG / 100.0f // STR getter 0x5237e0 incl. item STRENGTH; STR_DAM=3 (atoi)
if hero: atk = (float)(Bonus(8 WEAPON_DAMAGE_MULTIPLIER)+100) * 0.01f * atk
if Diplomacy_isHostileToPlayer(A) (0x5094d0: player unit valid, player.side≠0, hostile(A.side,player.side)):
    atk *= {EASY_D 0.7, NORM_D 1.0, DIFF_D 1.5, VDIFF_D 2.0}[difficulty byte 0xa57fc2]
```
Item effects (8 = multiplier) aggregated by `Inv_bonus` 0x497e10 (below). Non-heroes: wpn=0, no item terms.

#### 4.2 `Melee_applyDamage(D=this, attackerId, defIgnore%)` 0x509950 (CONFIRMED asm)
```
A = UnitArray_get(attackerId)
if hero(A) and RNG(100) < Bonus(0x14 FATAL_KILL):  dmg = (float)D.HP;  goto FRIENDLY      // skips everything below
P   = Unit_meleeAttackPower(A)                                     // 4.1
mA  = (float)(A.morale(U+0x163) + 100 - MOR_NOR) / 100.0f          // MOR_NOR=50
atk = AttackSkillMult(A) * mA * P                                  // 2.5
arm = (float)(D.armorLevel(U+0x15d) * AMR_DEF)                     // AMR_DEF=1; armor level from class EArmorId / armour items type 0x10
def = (float)(D.morale + 100 - MOR_NOR) * ( (float)DEF(D) * (100.0f-(float)defIgnore) * (float)DEF_RAT / 100.0f + arm ) / 100.0f
        // DEF getter 0x5238c0; DEF_RAT parsed by atoi("0.3") = 0 ⇒ DEF stat and defIgnore have NO effect with shipped data
pen = max(0, (atk - def) / 2.0f)
dmg = DamageTakenMult(D) * (atk / 2.0f + pen)                      // 0x510e80: hero: (100-Bonus9)*(100-Bonus0x16)*0.0001 else 1.0
if A is the player unit (A.id == PC.id):
    if A.critFlag(U+0x1b4): dmg = dmg + dmg                        // critical ×2
    dmg = (float)PC.chainLevel(+0x9d) * dmg                        // CHAIN multiplier (1,2,3…)
FRIENDLY:  (only if player unit valid)
    if gameType(0xa58fa7)==8:                                     // scenario no 8 (scenario_08.SCR = S6L1, last chapter)
        if A.side != player.side: dmg *= F_ATTACK/250.0f           // 130/250
        if D.side != player.side: dmg *= F_DEFEND/100.0f
    if A.side == player.side: dmg *= F_ATTACK/100.0f               // 1.30
    if D.side == player.side: dmg *= F_DEFEND/100.0f               // 0.70
leader = A.leader (0x4216d0); if leader && leader != A:
    dmg *= (float)(min(leader.U+0x15b raw LEAD, 100) + 100) / 100.0f
D.SetHP( ftol((float)D.HP - dmg) )                                 // 0x4fe510, clamps
if A is player: if crit: A.U+0x1b5++ (crit count); if crit && D.HP==0 && D.lives==0: "CRITKILL" text, A.U+0x1b7++
return dmg
```
Notes (CONFIRMED unless stated): no randomness except FATAL_KILL roll (and down-attack defIgnore, inert). No explicit
minimum damage, but HP is stored as `ftol((float)HP − dmg)` (truncation toward zero) ⇒ **any dmg > 0 removes at least
1 HP** while HP > 0 (e.g. 60 − 0.4 = 59.6 → 59). FATAL_KILL damage still gets friendly/leader multipliers
(player-side D with F_DEFEND = 0.7 survives a "fatal" hit). `*_A` difficulty values are NOT used for damage (only
BLOCKPER, §5.1). Damage is computed at trigger time from A's *current* anim record (+0xa), so an attack trigger that
fires after A has already started the next combo step uses the new step's DAMAGE/Range (edge case, CONFIRMED by data flow).

`D.SetHP(v)` 0x4fe510: `HP = clamp(v, 0, MaxHP())`; if HP==0 and `lives(U+0x120)>0`: lives−−, HP=MaxHP, U+0x139=HP.

Sibling damage functions (same defence block; see Part D): ranged `0x509e90` (adds FALL_S/FALL_E/DIST_P distance
falloff, AMREXDEF×0.5 for armorLevel==3 vs stone/explosive, SIE_UDAM×1.5 for stone; uses 0x510f10 =(100−Bonus0x16)*0.01
only; skill mult 0x510e40 RANGE_ATTACK) and item/keg `0x49d5f0` (atk = item instance +0x72, no morale, 0x510f10).

#### 4.3 Item-bonus aggregation `Inv_bonus(type, recno)` 0x497e10 (CONFIRMED; integer math, C `/`)
Start c = 100 for 0x13 MORALE_DROP and 0x15 EP_CONSUME, else 0. Sources: hero inventory (type 1) slots 0,1 and item at
inv+0xa, then enhancer list inv+0x12 (only highest-level of duplicate ids, applicable to recno via 0x4a42f0). Value v per
item = `base + (level−minLevel)*perLevel` (0x4a4200). Combine:
* 1-6, 0x14: `c += v` · 7,8,0xa,0xb-0x11,0x17: `c += (c+100)*v/100` · 9,0x16: `c += (100−c)*v/100` ·
  0x12: `c = max(c,v)` · 0x13: any v≠0 ⇒ return 0 · 0x15: `c = c*v/100`.

#### 4.4 Critical attacks (player only)
* Trigger (`Player_critCheck` 0x416680, Unit_process phase 0, player unit, game mode `0xa5b9f8`≠4):
  if `U+0x139 − HP > CRI_DIFF(10)` → start sequence (0x462970) and `U+0x139 = HP` (CONFIRMED).
* Sequence (HUD obj 0xa4f820): random anim record from a list, re-rolled until COMBOAVAIL=='Y'; its COMBO string
  becomes the required key sequence (+0x1f, len +0x25 = number of A/B chars), start time (double) +0x27, active +0x18.
  Expires after CRI_TIME (10000 ms) (0x45fae0) (CONFIRMED).
* Each attack input calls `Crit_key(key,reset)` 0x462810 with 'A'(0x41)/'B'(0x42)/'C'(0x43)/'E'(0x45 energy): first
  clears `player.U+0x1b4=0`; if `reset` progress=0; matching key advances, wrong key resets to 0; when complete:
  "CRITATT", `player.U+0x1b4 = 1` ⇒ **that attack (all its hits) deals ×2**, until the next attack input clears it.
  Fresh ground A/B starts pass reset=1 (sequence restarts), queued follow-ups pass reset=0 (CONFIRMED).
* Crit kills: XP × CRITEXP (3.0) in Unit_die (§6).

#### 4.5 Combo-hit counter (HUD, XP only)
`DAT_00a5ba62` short ++ on every successful melee hit by the player unit (0x75d6c6) and down attack; HUD shows it once ≥2
(0x7036f0); HUD (0x702740) resets it to 0 when **>1999 ms** pass without increase (manual says 1 s; COM_TIME=1000 is parsed
but unused — CONFIRMED by xrefs). Used only for the XP bonus in §6. COM_LIM/MAX_COM/COM_CONS: not referenced in melee code
(MAX_COM used by 0x5229a0/0x456960, Part A).

---------------------------------------------------------------------------------------------------
### 5. Blocking, hit reactions, knockback, knockdown

#### 5.1 Blocking
* Player: guard key → GUARD1 (Part B). GUARD1/GUARDR stay until released (0x7c7ab0 returns early); GUARD2 → idle at
  anim end. Guard states are in CanAct ⇒ can attack straight out of guard.
* Blocks melee and arrows (same ±BLOCKANG test in 0x5051e0, effect "GUARDARR") from the front 140° arc. Down attacks
  cannot be blocked only if D is knocked (guard impossible then). No EP cost, no damage, no chip.
* AI: `Block_chance` 0x512070 = `ftol(BLOCKPER × diffA)` with EASY_A 0.5 / NORM_A 1.0 / DIFF_A 1.5 / VDIFF_A 2.0
  ⇒ 25/50/75/100. Hero AI scripts (0x538c90…0x53f260): when threatened and not attacking, `if RNG(100) ≤ chance`:
  AI cmd TURN(10) toward threat then BLOCK(4) for 2000 (HIGH: arg = duration ms). Effective probability (chance+1)/100.
* `notBlock` (itemres 0x4a0160, item category 1 with NeedKey) is a world-object (door/chest) pathing flag, not combat (HIGH).

#### 5.2 Hit reaction (`Unit_onMessage` case TRIGGER_ATTACK, 0x75d540) after dmg>0
* Retarget: D.target(U+0x2d7)=A if D's target invalid and D not attacking/shooting; A.target=D if A's invalid/dead.
* Player attacker: combo counter++ (§4.5).
* If `D.U+0x2e1`==0 (HYPOTHESIS: scripted/no-react flag):
  * A.state ∈ ENGATK1-4/RUSH: `RNG(2)==1` → Knockdown (0x509000), no flinch anim.
  * A.state ∈ ATTACK/H_ATTACK: directional flinch by `a = RelAngle(D→A)`:
    `−45≤a≤45` ATTACKED(0x23) · `45<a<135` RATTKED(0x28) · `−135≤a<−45` LATTKED(0x27) · else BATTKED(0x26).
    Non-player D (not DNATKED, class Special ∉ {AUTOBOW 0xc, CART 0xd}) turns to face A: `SetFacing(D.facingDeg − a)`.
  * other attack states (JATTBD, JATTACKB, H_RUN…): no flinch, only force.
* SetAction remaps (0x501190): ATTACKED while in ATTACKED → ATTKED2 (if anim), in ATTKED2 → ATTACKED, else random
  `RNG(2)`: 0→ATTKED2, 1→ATTACKED (if ATTKED2 exists). Any hit/knock action while knocked (0x40-0x42,0x4e) or DNATKED
  becomes DNATKED(0x29). While in a flinch (0x23-0x28) any non-hit action is refused until the flinch anim ends
  (`remaining>0`) except class SIEGE_WEAPON ⇒ hit-stun = flinch anim length (UNKNOWN ms, model data). Flinch end → idle.

#### 5.3 Balance / knockback (CONFIRMED)
`U+0x1cb` short "balance" 0..100, initial = unit class "Falling" (class+0x1a9; Joan 20), set to 100 on stand-up.
```
H = 3000, V = 500
if A has anim table: S = A.anim.SCRIPT; if msg.p16 != 0: S = p16 (horse charge); if S: H = S.AttackForce, V = S.VerticalAttackForce
if (H + V < T_FALLF(6000) && V < V_FALLF(3000)):  SetBalance(balance - (H+V)/F_FALLR)   // int division, F_FALLR=250
else:                                            SetBalance(0)
```
`SetBalance` 0x4fea80: clamp 0..100; if 0 → mounted (`U+0x1e2`)? H_DROP(0x4e) + dismount : Knockdown().
Arrow hits: `SetBalance(balance − ARR_FALL(25))` (Part D).
Regen (Unit_process 0x52c490 @0x52cd4a): every ≥1000 ms while balance<100: knocked (0x40-0x42,0x29):
`bal = ftol(bal + (DEF·DEF_FALL + 1.0)·10.0)`; else `bal = ftol(bal + 1 + 10.0·DEF_FALL)` (= +3). DEF = DEF getter.
Impulse (if D alive, has physics body, class ≠ "AUTOBOW"): horizontal dir = away from A (`yaw·180/π − RelAngle(D→self)`,
or ±90° sideways for H_RUN/horse-charge), velocity = (cos·H, sin·H, V) scaled ×2.0 if D in H_DROP, ×0.7 if DNATKED
(applied via 0x6b11a0 with now, 100). D.U+0x371 = 1 (was-hit flag, HYPOTHESIS).

#### 5.4 Knockdown / get-up / invulnerability
`Knockdown` 0x509000 (CONFIRMED): only if not already knocked, class UnitClass ∉ {8 SIEGE_WEAPON, 9 CART}, has anim
table, not mounted, `U+0x1e6`==0, `U+0x1fd`==0. n = 1 (+1 if KNOCKDN2 anim, +1 if KNOCKDN3 anim); `RNG(n)`: 0→KNOCKDN,
1→KNOCKDN2, 2→KNOCKDN3. `U+0x2df <<= 1`.
SetAction(KNOCKDN*/H_DROP): `U+0x1d1 = FELL_DUR/5 + RNG(FELL_DUR)` = **500 + [0,2500) ms**; if `U+0x104 ≥ 4`
(hero/general category): `U+0x1dd = max(U+0x1dd, INVC_DUR)` = **6000 ms invulnerability** (decremented by dt each tick in
Unit_process 0x52c490 phase 0; blocks all melee except down attacks, and item damage).
Get-up (UpdateAction 0x7c7ab0):
* AI/other units: after the knock anim finishes, `U+0x1d1 −= dt` each tick; when <0 → SetBalance(100); if dead → state
  DIE; else STANDUP(0x3c), `U+0x1e1=0`. Lying time = knock-anim length + 500..2999 ms.
* Player-controlled unit: timer decremented but ignored — stands up as soon as the knock anim ends.
* STANDUP end → idle. While knocked, SetAction refuses other actions for AI-controlled units (`U+0x102`≠0, not PC) while `U+0x1d1>0` (CONFIRMED, 0x501190 knocked-state block).
Down attack success (case 0x400): `D.U+0x1e1=1`, `D.U+0x1d1=0` (gets up right after), SetAction(DNATKED), player combo
counter++, return 2.

---------------------------------------------------------------------------------------------------
### 6. Death
* Death = `HP<1 && lives==0` (0x42d0e0). Checked after every hit in 0x75d540; then `Unit_die(killerId)` 0x7c6240
  (killer-less variant 0x64f320 for environment/keg).
* Unit_die (CONFIRMED unless noted): drops item (0x50afe0, below); releases crew/mount slots; killer EP gain
  `EP += EP_NOR(6)` if victim category `U+0x104`<3, `EP_CAP(15)` if ==3, else `EP_GEN(30)`; killer counters
  `U+0x1ae += (cat<3 ? 1 : 10)`, `U+0x1b0++`, `U+0x1b2++` if cat>2; leader counters `+0x1bb/+0x1b9 ++`.
* XP to killer (`Unit_addXP` 0x5229a0(ftol(xp),1,0)):
  `m = crit ? CRITEXP(3.0) : 1.0; if killer.level(U+0x124) > victim.level: m *= 1/2^(kLvl−vLvl);`
  `if killer is player && mode≠4: m *= 1 + E_BONUSL(100)*0.01f*min(comboCount / C_BONUSL(50), M_BONUSL(3));`
  `xp = class.Experience(+0x15d) * victim.level * m; if !(xp > 10.0) xp = 10.0` (min 10).
* Morale (killer case): `M = rank(victim class) + rank'(killer class)` with rank: Ranking 0-2→1, 3→2, 4-5→3
  (0x527dd0; killer variant 0x527e60 gives Joan 4). For every unit u: if u in killer's side & troop (`U+0x31`≠0 equal):
  `u.morale = ftol(u.morale + max(1.0, M*MOR_CH_T))`; elif u in victim's side & troop: `ftol(u.morale − max(1.0, M*MOR_CH_T))`.
  Killer-less (0x64f320): victim's nation only (+0x02 ≠ 0), same troop (`U+0x31`≠0 equal) `−= M*MOR_CH_T`, every other unit
  of that nation (other troops and troop-less units) `−= M*MOR_CH_U(1.0)`; `M = W1(victim)` only, no min 1 (CONFIRMED asm 0x64fa78..0x64fb64).
  0x64f320 is reached only from the item-hit message 0x4000 (0x75e9fb) and its own crew/driver recursion; it has no killer
  rewards, no Unit_stop on the crew, and pushes the corpse 3000/500 at angle 0.
  `SetMorale` 0x4fe8c0: when lowering a troop member (`U+0x31`≠0) whose leader has an inventory: if this unit's
  `Inv_bonus(0x13,leader)`==0 → no change (MORALE_DROP immunity); `base=Inv_bonus(0x12)`; if base < current → **no change**;
  else morale = base. Otherwise clamp 0..100. (CONFIRMED asm; with no items this means hero-led troops never lose morale.)
* Death impulse: killer's script force (default 3000/500), ×3.0 if H_DROP; SetAction(DIE) unless THROWN(0x3b).
  Non-player corpses fade (actor+0x30 = −0.1f per…, +0x38 = now). Units with `U+0x114`==1 (mission-critical hero):
  `U+0x1dd=0x7fffffff`, HP=1, mission event 0x4e4520(7, …) (defeat) instead of dying.
* Item drop (0x50afe0): if `U+0x1c5` (pre-rolled drop item id) ≠ 0 (and mode ≠ 2) spawn item at death position with level
  `U+0x1c7`. The id is chosen at spawn from the map placement "Treasure"/"TreasureLevel" or a DROPITEM.RES list via
  `DropList_roll` 0x464030: `r = RNG(totalWeight)+1`; walk entries from last to first `r -= weight`; first r<1 wins
  (weights = per-entry values, e.g. CAPTAIN1 fd_bread_s 20…; total 0 ⇒ no drop). "unique_random_list" re-rolled at death.

---------------------------------------------------------------------------------------------------
### 7. Energy attack, heavy-attack cost
* `Player_energyAttack(go)` 0x437910 (CONFIRMED asm), also AI 0x4bca60 (same costs):
  `epCost = ftol( (float)Inv_bonus(0x15 EP_CONSUME) * ((float)MaxEP*EGATK_MP/100.0f) / 100.0f )` (no items ⇒ 40 % MaxEP)
  `hpCost = ftol( (float)MaxHP*EGATK_HP/100.0f )` (20 % MaxHP).
  Requires go, CanAct, posture≠JUMP, `U+0x23f==0`, `EP ≥ epCost`, `HP > hpCost`, and ENGATK anim learned
  (0x500d40(0x1e): anim exists, `U+0x2e1`==0, COMBOAVAIL or AI) or ENERGY_ATTACK skill progress (0x50d4b0).
  Then SetAction(ENGATK), PC+0xa8=0, crit key 'E', `EP −= epCost`, `HP −= hpCost` (via SetHP, cannot kill: HP>cost),
  `U+0x151 = now` (last EP use), PC+0x96=0. Multiplayer client mode (0x4cd510()==1) just returns go.
* Effect: ordinary TRIGGER_ATTACK per stage (Joan stage 1: radius 120+reach, full 360° arc, hits at 1150 ms; stages 2/3:
  radius 80+reach at 233 ms), damage via §4 with DAMAGE=8 and ENERGY_ATTACK skill mult; force 5000/2000 ⇒ guaranteed
  knockdown; plus 50 % extra knockdown roll; attacker immune to melee/item damage during ENGATK*.
  The ENERGY_PT column (10) is NOT charged for ENGATK (cost is EGATK_MP path only) (CONFIRMED: 0x437910 has no rec+0x18 read).
* Heavy (B) attacks: cost = anim rec ENERGY_PT (+0x18) × EP_CONSUME/100 (int) per B step, jump-B, rush, horse-B (3).
  **HATK_MP (0xa3cbd0, 6) is parsed but never read** (CONFIRMED: only xref is the store at 0x51c974).
* Auto-attack order mode (0x43a7d0/0x43acb0, PC+0xdd: 1 normal, 2 combo, 3 energy): entering mode 2 subtracts raw
  `EGATK_MP` (40) EP if EP ≥ 40 (HIGH; Part B / AI).

---------------------------------------------------------------------------------------------------
### 8. Unit offsets used by melee code
| off | type | meaning | evidence | conf |
|---|---|---|---|---|
|+0x000|short|unit id|0x509abd cmp with PC id|CONFIRMED|
|+0x002|short|side/kingdom (diplomacy)|0x4b6860 args in 0x5048b0|CONFIRMED|
|+0x00a|int|current anim-record index|0x5104f0, 0x504010|CONFIRMED|
|+0x00e|vec3|position|0x505960 (+0xe), trigger pos|CONFIRMED|
|+0x031|short|troop id|0x7c7034, 0x4fe8e2|HIGH|
|+0x102|short|AI-controlled flag (0 for player unit)|0x7c7ab0 branch, 0x500d40|HYPOTHESIS|
|+0x104|int|rank category (0-2 soldier,3 captain,≥4 general/hero)|0x5020e2, 0x7c6240|HIGH|
|+0x114|int|mission-critical (1 ⇒ defeat instead of death)|0x7c6240|HIGH|
|+0x120|int|extra lives|0x4fe510, 0x42d0e0|CONFIRMED|
|+0x124|short|level|0x7c6e57, class Level|CONFIRMED|
|+0x135|int|HP|0x509c36|CONFIRMED|
|+0x139|int|HP snapshot for crit trigger|0x416680|CONFIRMED|
|+0x145|int|EP|0x437a94|CONFIRMED|
|+0x151|int|time of last EP use|0x437ac6|HIGH|
|+0x15b|short|LEAD raw|0x509bf9|CONFIRMED|
|+0x15d|short|armour level (class EArmorId; armour items type 0x10 raise it)|0x5099f7, 0x6d8ac0|CONFIRMED|
|+0x163|short|morale 0..100|0x5099c4, 0x4fe8c0|CONFIRMED|
|+0x1b4|char|critical-attack flag (player)|0x509ac8, 0x462810|CONFIRMED|
|+0x1b5/+0x1b7|short|crit attacks / crit kills counters|0x509c6c, 0x509cc3|CONFIRMED|
|+0x1bd/+0x1bf|short|equipped weapon item instances (main/second)|0x504b50|CONFIRMED|
|+0x1c1|ptr|inventory (heroes; 0 for most units)|0x5104f0|CONFIRMED|
|+0x1c5/+0x1c7|short/int|drop item id / level|0x50afe0, 0x558a60|CONFIRMED|
|+0x1cb|short|balance 0..100|0x4fea80|CONFIRMED|
|+0x1cd|int|last balance-regen time|0x52cd78|CONFIRMED|
|+0x1d1|int|knockdown remaining ms|0x5020d9, 0x7c7ab0|CONFIRMED|
|+0x1dd|int|invulnerability ms|0x502105, 0x52c490|CONFIRMED|
|+0x1e1|char|already down-attacked|0x5048b0, 0x75d540|CONFIRMED|
|+0x1e2|int|mount (horse) unit / mounted|0x430190, 0x4fea80|HIGH|
|+0x1fd|int|busy-in-structure (msgs ignored)|0x42d690|HYPOTHESIS|
|+0x213|short|posture 0 stand / 1 jump|0x504010|CONFIRMED|
|+0x215/+0x219|short|action / previous action|0x501e9b|CONFIRMED|
|+0x227..+0x22c, +0x233, +0x235, +0x237, +0x23b, +0x23f|—|combo fields (§2.1)|0x501190, 0x4367c0, 0x504010|CONFIRMED|
|+0x2d7|short|current target unit|0x75d540|HIGH|
|+0x2df|short|threat/aggro accumulator (+10 per hit, ×2 on knockdown)|0x504ff7, 0x50906a|HYPOTHESIS|
|+0x2e1|char|scripted/no-react (ignores messages except interact)|0x432b90, 0x75d5xx|HYPOTHESIS|
|+0x2ed|char|instantiated in active scene|0x442842, 0x41fce2|HIGH|
|+0x2f1|ptr|unit anim table (0x99-byte records)|0x504b2e|CONFIRMED|
|+0x2f5|ptr|actor/model (+0x4c anim start time, +0x5c physics body)|0x5045f0, 0x5048b0|CONFIRMED|
|+0x371|short|was-hit flag|0x75d540|HYPOTHESIS|
|PC+0x9d|char|chain level|0x509d40, 0x438820, 0x503fb0|CONFIRMED|

### 9. Constants used (values from GAMEPLAY.RES; parse per doc §3 master table)
| name | global | value | used by |
|---|---|---|---|
|STR_DAM|0xa3cce2 short|3|0x5104f0|
|WP_DAMG|0xa3cd14|0|0x5104f0|
|DEF_RAT|0xa3cce0 short|**0** (atoi "0.3")|0x509950 ⇒ DEF stat & defIgnore inert|
|AMR_DEF|0xa3cc00|1|0x509950|
|MOR_NOR|0xa3cc8c|50|0x509950|
|F_ATTACK / F_DEFEND|0xa3ccf4 / 0xa3cc2c|130 / 70|0x509950|
|EASY_D..VDIFF_D|0xa391ac,0xa3918c,0xa391b0,0xa39194|0.7,1.0,1.5,2.0|0x5104f0|
|EASY_A..VDIFF_A|0xa391a4,0xa39188,0xa391a0,0xa39198|0.5,1.0,1.5,2.0|0x512070 only|
|BLOCKPER / BLOCKANG|0xa3cc90 / 0xa3ccb0|50 / 70|0x512070 / 0x5048b0, 0x5051e0|
|CHAINPRO|0xa33744|200|0x438820|
|CRI_DIFF / CRI_TIME / CRITEXP|0xa3cc78 / 0xa33750 / 0xa3cbd4|10 / 10000 / 3.0|0x416680 / 0x45fae0 / 0x7c6240|
|T_FALLF / V_FALLF / F_FALLR|0xa3cc84 / 0xa3ccc0 / 0xa3cc34|6000 / 3000 / 250|0x75d540|
|ARR_FALL|0xa3cc08|25|0x75d540 (arrows)|
|DEF_FALL|0xa3ccd0|0.2|0x52c490 balance regen|
|FELL_DUR / INVC_DUR|0xa3cc88 / 0xa3cbe4|2500 / 6000|0x501190|
|EGATK_MP / EGATK_HP|0xa3cd28 / 0xa3cd1c|40 / 20 (%)|0x437910, 0x4bca60, 0x652c10; 0x43a7d0 raw|
|HATK_MP|0xa3cbd0|6|**unused**|
|COM_TIME, COM_LIM, COM_CONS|0xa3ccc4,0xa3cbc8,0xa3cbd8|1000,200,1|**unused**|
|MOR_CH_T / MOR_CH_U|0xa3cd08 / 0xa3cc80|0.3 / 1.0|0x7c6240, 0x64f320|
|EP_NOR/EP_CAP/EP_GEN|0xa3cc60/0xa3cc48/0xa3cc50|6/15/30|0x7c6240|
|C_BONUSL/M_BONUSL/E_BONUSL|0xa3cccc/0xa3cc3c/0xa3ccb4|50/3/100|0x7c6240|
|CLB_IDLE|0xa3cce8|20|0x500ba0|
|hard-coded|—|250 ms combo lockout (0x4367c0), 50 ms jump-attack delay, 79 ms H_ATTACK trigger guard (0x4ef090), 1999 ms combo-counter reset (0x702740), 1000 ms balance regen period, angle buckets 45/135 → 0/20/40 %|

### 11. Open items
* Flinch/knockdown/stand-up animation durations (model anim data) — UNKNOWN, not in code.
* Sign convention of RelAngle (+ = right) — HYPOTHESIS from LATTKED/RATTKED mapping.
* Semantics of learned-skill record fields [1],[2] — HYPOTHESIS ([2]==[1] "fully learned").
* Input action #0x15 (CHAIN key) name — see Part B.
* gameType 0xa58fa7==8: RESOLVED — 0xa58fa7 is the Scenario object's scenario no (GAMEPLAY_AI_TROOPS.md §4.1), so 8 = the last campaign chapter (S6L1).

## 7. Part D — Ranged and siege

Confidence: CONFIRMED = read in code (asm checked where a value matters) · HIGH = strong code evidence, minor gaps · HYPOTHESIS · UNKNOWN.
Units: game units ("u"); physics works in metres with **46.1 u = 1 m** (constant `0x9cf780`=46.1, `0x42386666`). Gameplay time ms; 1 tick = 33.33 ms.
World axis: physics/aim use **z = up** (gravity applied to 3rd component) — CONFIRMED 0x496d40, 0x6a8060.

---------------------------------------------------------------------------------------------------
### 1. THROWI constants (loader 0x5525b0) — CONFIRMED (asm 0x552768..)

Ints via atoi (`0x7b15cb`), floats via atof (`0x7b2110`). Order in file for STM/STC/CAN is TL,DL,M,MXD,MND,**TR,FC**,AV,GL,ZE (loader follows it).

| key | ARW | PROJ | EXPL | STM | STC | CAN | type | meaning / use |
|---|---|---|---|---|---|---|---|---|
| TL | 7000 @a39d88 | 7000 @a39db8 | 7000 @a39d7c | 8000 @a39d38 | 20000 @a39da8 | 8000 @a39dcc | int ms | lifetime (manager 0x4913c0) |
| DL | 3000 @a39cb4 | 3000 @a39db4 | 3000 @a39d3c | 4000 @a39d08 | 20000 @a39d80 | 4000 @a39d70 | int u | max horizontal distance from launch point |
| M | 1.0 @a39dac | 1.0 @a39dd4 | 1.0 @a39d50 | 1.0 @a39d44 | 1.0 @a39cb8 | 1.0 @a39dbc | float | rigid-body mass |
| MXD | 0 @a39d74 | 4000 @a39dc0 | 4000 @a39d0c | 200 @a39d94 | 20000 @a39d58 | 4000 @a39d00 | int→float | aim solver clamp distance (u) |
| MND | 0 @a39d84 | 0 @a39da4 | 0 @a39d1c | 20 @a39d10 | 500 @a39ce4 | 500 @a39d64 | int→float | copied to proj+0x42, passed to solver but **never read** by 0x496d40 |
| FC | 200 @a39d60 | 30000 @a39cd4 | 30000 @a39cf0 | 15000 @a39dc8 | 30000 @a39cbc | 120000 @a39d28 | float | launch force scale (used when no target / for ARW) |
| TR | 0 @a39cc0 | 0 @a39dc4 | 150 @a39d24 | 100 @a39cf4 | 100 @a39d68 | 150 @a39d54 | int→float | **impact (splash) radius** put in hit events |
| AV | 0 @a39d5c | 1800 @a39cd8 | 1800 @a39d18 | 750 @a39d20 | 2150 @a39cec | 3000 @a39cc8 | float u/s | average speed → flight time t=d/AV in solver |
| GL | 0 @a39cfc | 0.5 @a39cdc | 0.5 @a39dd8 | 3 @a39ce0 | 1 @a39d78 | 1 @a39cf8 | float | gravity multiplier (g = GL·9.81 m/s²) |
| ZE | 20 @a39d8c | 20 @a39ce8 | 20 @a39d34 | 20 @a39d2c | 20 @a39d04 | 20 @a39d6c | float u | height used only for V0/FT below |
| V0 (derived) | 0 @a39d48 | 2.063 @a39cac | 2.063 @a39cd0 | 5.053 @a39cb0 | 2.9175 @a39dd0 | 2.9175 @a39d4c | float m/s | |
| FT (derived) | 7000 (=TL) @a39d30 | 1015 (int) @a39d90 | 1015 @a39ccc | 414 @a39cc4 | 717 @a39db0 | 717 @a39d40 | ms | |

Derived (CONFIRMED asm 0x552776-0x55283c; constants 2.0 `9cf794`, 9.81 `9d402c`, 1.0 `9cf784`, 46.1 `9cf780`, 1000 `9cf778`):
```
if GL == 0: V0 = 0; FT = (float)TL
else: g = GL*9.81; h = ZE*1.0/46.1
      V0 = sqrt(2*g*h)                       (float32)
      FT = ftol((V0 + sqrt(2*g*h + V0*V0)) / GL / 9.81 * 1000)   (stored as float, PROJ as int)
```
V0/FT are used only by the structure look-ahead sweep 0x693750 (§3.4). Numeric FT values HIGH (float rounding ±1 ms).

Projectile type ids (proj+0xc) and physics body type (CONFIRMED 0x507fc0, 0x6a8060):

| proj type | name | item types that create it (ITEMTYPE.csv, 1-based) | body type | gravity used by integrator |
|---|---|---|---|---|
| 0 | ARW | 0xd Normal Arrow, 0x10 Normal Bolt | 2 | ARW_GL (0) |
| 1 | PROJ | 0xe Projectile Arrow, 0x11 Projectile Bolt | 3 | PROJ_GL |
| 2 | EXPL | 0xf Explosive Arrow, 0x12 Explosive Bolt | 4 | EXPL_GL |
| 3 | STM | none — no code path sets type 3 (UNUSED) | 5 | STC_GL |
| 4 | STC | 0x14 Normal Stone (trebuchet) | 5 | **STC_GL** |
| 5 | CAN | 0x13 Cannonball | 0x12 | CAN_GL |

Other item types used: 0xb Normal Range Weapon (bow), 0xc Normal Crossbow Weapon (inventory ranged slot accepts only 0xb/0xc, 0x497270).

---------------------------------------------------------------------------------------------------
### 2. Projectile ("ThrowItem") struct — 0x6c bytes (new in 0x4ea700, ctor 0x420110 = memset 0)

| off | type | meaning | evidence | conf |
|---|---|---|---|---|
| +0x00 | int | pool record id (removal key for 0x4ea9c0) | 0x4ea700 `*rec = 0x417520()`, 0x4913c0 | HIGH |
| +0x04 | int | rigid-body handle (0x6af310 lookup) | 0x507fc0, 0x5133e0 | CONFIRMED |
| +0x08 | int | scene/map-object id of projectile model (0x425d20) | 0x507fc0 | CONFIRMED |
| +0x0c | int | projectile type 0..5 (§1) | 0x507fc0 | CONFIRMED |
| +0x10 | int | looping "ARROW" sound handle (if sound on `a57ffb`) | 0x5133e0, 0x4fcc00 | CONFIRMED |
| +0x14 | uint | launch time ms (0 = not launched; launch only if 0) | 0x5133e0, 0x431480 | CONFIRMED |
| +0x18,+0x1c,+0x20 | float | launch position x,y; **z overwritten with 0** | 0x5133e0 | CONFIRMED |
| +0x24 | short | destroyed flag (set by 0x4e6fb0) | 0x4e6fb0, 0x4913c0 | CONFIRMED |
| +0x26 | int | id of thing hit (unit id or map-object id), 0 = nothing | 0x514c40, 0x4fbc90 | CONFIRMED |
| +0x2a | int | shooter unit id | 0x507fc0 | CONFIRMED |
| +0x2e | int | shooter actor body id (actor+0x5c) — self-hit filter | 0x507fc0, 0x6a8060 | CONFIRMED |
| +0x32 | int | TL | 0x498e90 | CONFIRMED |
| +0x36 | int | DL (ARW: ×RANGE_DISTANCE/100, see §4.3) | 0x498e90, 0x507fc0 | CONFIRMED |
| +0x3a | float | M | 0x498e90 | CONFIRMED |
| +0x3e | float | MXD | 0x498e90 | CONFIRMED |
| +0x42 | float | MND | 0x498e90 | CONFIRMED |
| +0x46 | float | TR (splash radius) | 0x498e90, 0x514c40 | CONFIRMED |
| +0x4a | float | FC | 0x498e90 | CONFIRMED |
| +0x4e | float | AV | 0x498e90 | CONFIRMED |
| +0x52 | float | GL | 0x498e90 | CONFIRMED |
| +0x56/+0x5a | uint | structure-sweep window start/end ms | 0x693750 | HIGH |
| +0x5e/+0x62 | uint/int | predicted structure-hit time / object id | 0x693750 | HIGH |
| +0x66 | ptr | trail object (0xaf bytes, ctor 0x687130, lifetime=TL via 0x687500) | 0x5133e0 | CONFIRMED |
| +0x6a | short | already-exploded flag | 0x4fcc00 | CONFIRMED |

Rigid body (physics engine object, `0x6af310(handle)`), relevant fields:

| off | meaning | evidence | conf |
|---|---|---|---|
| +0x04 | body type (2,3,4,5,0x12 = projectiles) | 0x6ad660, 0x6a8060 | CONFIRMED |
| +0x08 | owner rec: +8 owner kind (6..9 = throw items, 0xb unit, 1 map object), +0xc owner ptr | 0x507fc0, 0x514c40 | CONFIRMED |
| +0x0c.. | copy of previous state; +0xc0 previous position (m) | 0x6a8060 | CONFIRMED |
| +0x142 | velocity (m/s, 3 floats) | 0x6a8060 | CONFIRMED |
| +0x1e6 | orientation (4 floats) | 0x6a8060 | HIGH |
| +0x1f6 | position (m) | 0x6a8060 | CONFIRMED |
| +0x216 | short "below terrain" (ground-hit) flag | 0x6a8060 tail | CONFIRMED |
| +0x21c | force vector; +0x228 start ms; +0x22c remaining duration ms; +0x230 end; +0x234 started | 0x6b11a0, 0x6a8060 | CONFIRMED |
| +0x27a | mass | 0x6b08e0 | CONFIRMED |
| +0x298 | short collided flag (set by actor sweep 0x68de40) | 0x68de40 | CONFIRMED |
| +0x29a | id of unit hit | 0x68de40 | HIGH |

---------------------------------------------------------------------------------------------------
### 3. Projectile lifecycle

#### 3.1 Spawn (0x4ea700 → 0x507fc0) — CONFIRMED
Unit fire state 0x33/0x38 (in 0x7c7ab0, §5.3) calls `ThrowItem_create(shooter, itemType)`:
- player: itemType = item(inventory ammo slot +0xe)->type (+2); AI: itemType = scene object (unit+0x1bd, the nocked arrow) ->+0x50.
- `+0x2a = shooter id`, `+0x2e = actor+0x5c`, type from table in §1, then 0x498e90 copies the THROWI row into +0x32..+0x52.
- ARW only: `DL = (RANGE_DISTANCE(0x10) * DL) / 100` (integer imul/idiv, asm 0x50808b-0x5080b6). RANGE_DISTANCE base is 0 in 0x497e10 → an ARW from a unit with no RANGE_DISTANCE item effect gets DL = 0 (HYPOTHESIS: all bows carry this effect; otherwise the arrow is removed by the manager on its first move).
- model: ARW/PROJ/EXPL from ammo item model; STC/CAN from class record +0x177 ammo item; body created with mass M and shape 2/3/4/5/0x12 (0x6adbf0/0x6af370). Bug kept: EXPL re-use branch passes PROJ_M.
- Then 0x431480 (only for the item with matching shooter and launch time 0) → `ThrowItem_launch` 0x5133e0(dirX,dirY,dirZ, offX,offY,offZ). Direction = shooter `unit+0x365` (normalized aim vector: AI 0x50aca0 §5, player 0x42ba40 §4.5). Offset = 0 except AI with crossbow weapon (item type 0xc in unit+0x1bf): (5.337, 0, 7.5).

#### 3.2 Launch 0x5133e0 — CONFIRMED (decomp + floats)
- Sets `+0x14 = now`. Launch point P0 = world origin of the bone the ammo item hangs from (item +0x9e attach id, else ItemRes AttachBoneName: ARROW/bolts "bip01 R Hand", CANNONBALL "cannon", FAUC_BALL "cylinder01", Bombard_BALL "Box05", TREB_ROCK "box07") in the shooter's current pose + offset; if shooter special (classRec+0x118) is 4 CANNON or 5 FAUCONNEAU: P0.xy += (actor+0x8c matrix row0)·68.695 + (row1 at +0x10)·12, P0.z += 10.122. Row0 = facing, row1 = facing + 90° (left): measured on a live actor (yaw 269°: row0 (−0.017, −0.9998), row1 (0.9998, −0.017)). Model space x = forward (siege barrels' bone x-axes point +x). Bone origins in the shooting clips: cannon.am2 "cannon" (1.63, 0, 33.5) in f_sht_ot (−1.17 elsewhere), Fauconneau "cylinder01" (33.85, 0, 36.65), Bombard "Box05" (−11.23, 0, 42.21). Trebuchet "box07" (sling) moves through f_sht_ot: (−259.9, 0, 26.9) at the start, (496.3, 0, 553.9) at the end, where the port releases (special 10, release at 0 ms left). Port: `World::launch_point` (fallback 0.75 × cylinder height only when the clip has no track for the bone).
- `+0x18..+0x20 = P0` then `+0x20 = 0`.
- Initial velocity is 0 (body reset 0x6b08e0); motion comes from a **force pulse** `0x6b11a0(F, now, durMs)`:

| type | target | F | dur |
|---|---|---|---|
| ARW (0) | any | dir·FC (dir = unit+0x365) | **100 ms** |
| PROJ/EXPL (1,2) | unit target (unit+0x2db) | solve(P0, T, …, minSin=0.25) | 1 ms |
| PROJ/EXPL | map-object target (unit+0x2dd) | solve(P0, objPos+objHeight, …, 0.25) | 1 ms |
| PROJ/EXPL | none | (dirX, dirY, dirZ+0.1)·FC | 1 ms |
| STC/CAN/STM (3,4,5) | unit target | solve(…, minSin=0.1) | 1 ms |
| STC/CAN/STM | object target | solve(…, 0.1), height by shape (below) | 1 ms |
| STC/CAN/STM | none | dir·FC | 1 ms |

  Force → Δv = F/M·dur(s). ARW: 200/1·0.1 = **20 m/s = 922 u/s straight line** (ARW_GL = 0 → no drop). Solver output is M·v·1000 for 1 ms → Δv = v exactly.
- Unit target point T: `T = targetUnit.pos(0x4fedb0)`; `T.z += classHeight(classRec+0x109)·0.8`. For PROJ/EXPL only, if |T−shooterPos| > 300: `T.x += spread(); T.y += spread()` (each call fresh RNG, §4.4).
  PROJ/EXPL extra: if dist > 300 and dot(normXY(v), normXY(shooterFacing actor+0x8c+0x58/0x5c)) < 0.9, re-aim at `shooterPos + facing·dist` (+spread, z = T.z) — i.e. cannot shoot >25.8° off facing.
- Object target height: PROJ/EXPL: +objRec+0x26 (or model bbox+0x40). STC/CAN: by objRec+0x1e9 shape: 1 → +rec+0x1f9; 2 → +(rec+0x211 − rec+0x205)/2; 3 → +rec+0x225/2; else +model bbox/4 or rec+0x26.
- Sets model yaw/pitch to dir; "ARROW" sound for ARW; ARW/PROJ/EXPL attach trail (0x687500(TL)).
- **Ammo**: if shooter is the player (`a5b9e8`): `0x497490(1)` → ammoSlot.count(+2) −= 1, if < 1 the slot is removed (0x497440). AI never consumes ammo. CONFIRMED.
- Shooter turns to the shot heading (0x50abd0) unless class name is "AUTOBOW"/"AUTOBOW2".

#### 3.3 Ballistic aim solver 0x496d40 (via 0x4945c0) — CONFIRMED asm 0x496d40-0x4970bf
Args: out, P0(src), T(target), M, MXD, MND(unused), AV, GL, minSin.
```
d   = |T - P0|                         (3D, u)
if d > MXD: T = P0 + norm(T-P0)*MXD;  d = MXD        (strict >)
t   = d / AV                          (s)
v.x = ((T.x-P0.x)/46.1)/t ; v.y = ((T.y-P0.y)/46.1)/t
v.z = (GL*9.81*t*t/2 + (T.z-P0.z)/46.1)/t           (m/s)
dh  = |(T.x,T.y,0) - (P0.x,P0.y,0)|
minS = (dh >= 800) ? minSin : dh*1.5/800 - 1.5
t = 0.1
while normalize(v).z < minS:          // raise the arc
    t += 0.2;  recompute v with this t (same formulas)
out = v * M * 1000
```
Note: the loop restarts t at 0.1 (first tried value 0.3), ignoring AV. minSin per caller: launch PROJ/EXPL 0.25, STC/CAN 0.1; 0x4945c0 wrapper: ARW 0, PROJ/EXPL 0.25 (EXPL uses PROJ constants), STM 0, STC 0.25, CAN 0.1. 0x4945c0 is only used by AI in 0x7c7ab0 to pick high/low aim animation (ARW variant has MXD=0 → degenerate 0/0; result only affects anim choice).

#### 3.4 Per-tick integration (0x6ad660 → 0x6a8060) — CONFIRMED
Called every game tick (from Game_process_tick) with `dtMs = now − lastPhysicsTime` (float).
```
if body.collided(+0x298): return
maxSteps = (body type 2 ARW) ? 2 : 5
h = 0.001 s
if dtMs > maxSteps: h = min(dtMs*0.001/maxSteps, 1.0); n = maxSteps else n = dtMs   // 1 ms steps
g = GL(of body type: 2→ARW_GL, 3→PROJ_GL, 4→EXPL_GL, 5→STC_GL, 0x12→CAN_GL)
types 3,4,5,0x12 return immediately (frozen) once +0x216 ground flag is set
repeat n steps (last fractional step h*=remaining<1):
   a = 0
   if force pulse active (start reached, remaining > 0.00015 ms):
        if remaining < h*1000: this step h' = remaining/1000 (split step)
        a = F / mass ; remaining -= h'*1000
   a.z += -g*9.81
   v_new = v + a*h                      // general form (v*(1-k)+a*h)/(1+k), k = 0
   p_new = p + 0.5*(v + v_new)*h        // trapezoid, metres
```
All sub-steps run first; the collision tests below run **once per call**, from the frame-start position p0 (+0xc0, also the actor position) along move = (p_new − p0)·46.1, and only when |move| > 0:
1. **Actors** `0x68de40` (Physics_sweep_actors; units within 2·(|move|+270+r) in x/y, then items/objects): pair test `Shape_sweep_pair 0x6a1bb0`, first hit in iteration order. Mover shape = the projectile actor's (`Item_setup_collision 0x41e850`): ITEMTYPE 13–18 (arrows, bolts) **sphere centre 0, radius 10.0** (confirmed live: arrow actor +0xb8 = 1, +0xbc..+0xc8 = (0, 0, 0, 10), scale 1); other items the model's bounding sphere. Unit shape = its CollisionType cylinder (actor +0xe4: centre (0, 0, CylinderCenterZ 40), r 15, h 80 for 94 of 97 unit types; confirmed live). A unit with +0x38e ≠ 0 or crewing a live vehicle (+0x384) is forced to shape 3 for this test and set to 0 afterwards. Sphere vs cylinder `0x699a20`: xy circles r+R (`0x6a0d80`, already overlapping → t 0); hit if the sphere centre's z at that t is within the cylinder height (not widened); else the top, then the bottom cap: sphere reaches the cap plane (`0x69e970`, within r → t 0) with its centre inside the cap disc, or touches the rim circle (`0x69e720`, quartic, 16-sample root scan `0x6a1460`). Net hit volume: lateral R + 10 = **25**, from 10 below the feet to **90** above them (rounded rims). Sets body+0x298=1. Ignored (flag cleared, projectile passes through) if: hit = shooter actor; or victim not enemy of shooter (0x5094d0); or victim state ∈ {0x40,0x41,0x42 (knocked down), 0x3c, 0x29, 0x4e}; or body type 9.
   **Item pass** (CONFIRMED decomp + live S4L1, GateMesh): only reached when no unit was met. Candidates: items with an actor whose physics body is set (actor +0x5c; live: exactly the 96 S4L1 items whose ItemRes has a CollisionType: gates, BOX crates, some tents), not of ITEMTYPE 8..12, of an ITEMCLAS 0x4a4050 accepts (all but TERRAIN 5, WAVE 12, ARMOR 16; or ItemRes +0x1c2 ≠ 0), within 1000 of the query point in x and in y (a third list, 0x54f960, uses the 2·(|move|+270+r) window). Item shape = `Item_setup_collision 0x41e850`: the model's bounding sphere, replaced by ItemRes CollisionType +0x1e9 (sphere +0x1ed → actor +0xbc, Box3d +0x1fd → +0xcc, cylinder +0x215 → +0xe4). Sphere vs Box3d (`0x697140`, in the item's frame: centre and move through the inverse item matrix, radius·moverScale/itemScale): the six faces grown by r (−x +x −y +y −z +z; `0x69e640`: approaching, or on the plane = t 0, then the centre inside the face), the twelve edges as radius-r cylinders (`0x6a09e0`; z edges, y edges, x edges), the eight corners (`0x6a0f90`); first feature hit wins. Quirk (asm 0x697825): the four vertical edges test the contact's **x** against the box's z range. An open gate has no shape (`0x48c760`: actor +0xb8 = 0 in gate states 1 / 4). A hit item's body (actor +8 == 1) makes `0x514c40` post the events of `0x4fbc90` (+0x26 = item +4).
2. **Structures/map objects** `0x693750`: mesh sweep over a look-ahead window of FT ms: end = p0 + FT·0.001·46.1·(vx, vy, vz − V0) with the frame-start velocity; recast at most every FT ms (+0x5a), the prediction (+0x5e time, +0x62 item) fires on the frame whose (now − dt, now] holds it; excludes weapon items (types 8..0xc) and object classes 7,8,9,0xf,0x13 and objects with rec+0x1e3 set (TERRAIN scenery is included); triangles with material +0x38 == 1 pass. Returns object id + hit fraction. The ray (`0x5d4a80`) runs on the item actor's mesh (actor +0x90, built by `0x5aab70` → `0x5cbbf0(model mesh, shared = 1)`): +0xb set, so it borrows the model mesh's (+0xc) vertex buffer, index list +0x48 (+0x40 triangles), per-triangle material +0x4c, transformed by mesh node 0's world matrix. Live (S4L1): for SMO models the buffer holds the model-space positions (abtower `abtowera.SMO` 3528 and `town.SMO` "Tourelles Castle" 3707 triangles equal the port's parse to 0.0005); for **AM1** models (the Tourelles gate's `Door.AM1`, ab_gate, Camp Gate, AM1 crates and tents) only uv / colour are filled and every position stays at the allocator fill 0xEEEEEEEE, so all triangles are degenerate: an AM1 item never meets this ray (the f_closed pose is irrelevant; only its CollisionType primitive collides).
3. If actor hit and (no structure hit or actor closer) → `0x514c40(actor)`; else if structure hit → `0x4fbc90(object)`. Either returns before the new state is stored: the body stays at p0, and 0x514c40's head test uses that position (the actor's).
4. Terrain: p_new·46.1 vs heightmap (Plasma_sample_tri); flag +0x216 = 1 if terrainHeight > z.

#### 3.5 Hit handling
**Unit/actor hit 0x514c40** (CONFIRMED asm): `+0x26 = victimId`, replaced by victim+0x1e6 if non-zero (meaning UNKNOWN, maybe mount/rider link). Valid only if `hitPoint.z < classRec+0x109 (height) + victim.z(+0x16)`; otherwise no event (body+0x298 stays set → projectile removed next manager pass, HIGH).
Events posted (0x4eef00, §6), position = victim pos (ARW/PROJ) or projectile pos 0x4e5d90 (others), radius = +0x46 TR:
- ARW/PROJ: (2, unit, victimId) — single target.
- EXPL: (0x2000, units, 0=area), (0x2000, objects, area), (0x2000, objects, victimId); random sound EXPLOSIVEHIT1..3 (rand(3)).
- STM/STC/CAN: (0x40, units, area), (0x40, objects, area); sound "CANNONHL".
**Map object hit 0x4fbc90** (CONFIRMED): `+0x26 = objId`; ARW/PROJ: (2, object, obj+6); EXPL: (0x2000, object, obj+6), (0x2000, objects, area), (0x2000, units, area); STC/CAN/STM: sound CANNONHH if objRec+0x1c ∈{1,2} else CANNONHM, then (0x40, object, obj+6), (0x40, units, area), (0x40, objects, area). Projectile destroyed.
**Ground**: ThrowItem_update 0x4fcc00 (CONFIRMED asm 0x4fcfae-0x4fd1d8): once body+0x216 set and not yet exploded:
- EXPL: (0x2000, units, area) + (0x2000, objects, area) at projectile pos, **radius 150.0 hard-coded** (`0x43160000`), EXPLOSIVEHIT1..3.
- STM/STC: (0x40, units, area) radius 150.0, "CANNONHL".
- CAN and ARW/PROJ: nothing. Every projectile with +0x216 set is destroyed the same tick.
Arrows therefore never persist stuck in the ground or bodies: **ARW_ATH (a3cc40) and ARW_TIME (a3cc68) are loaded by 0x51c680 but never referenced** (CONFIRMED: no other asm reference).

#### 3.6 Manager / lifetime 0x4913c0 (per tick) — CONFIRMED asm 0x4914fe-0x491564
For each launched projectile (+0x14 ≠ 0):
```
remove if destroyed(+0x24) or model gone or body gone
dxy = |(modelPos.x,modelPos.y,0) - (launch.x,launch.y,0)|
remove if DL < dxy
remove if hitId(+0x26)==0 and (body.collided(+0x298) or launchTime+TL < now)
else ThrowItem_update 0x4fcc00 (orient model along velocity for PROJ/EXPL, trail, ground explosion, destroy if hit/ground)
```

---------------------------------------------------------------------------------------------------
### 4. Bow / crossbow

#### 4.1 Unit inventory (unit+0x1c1, 0x1e bytes, **all units**, built in Unit_init 0x4f8550 from class record) — CONFIRMED
| off | meaning |
|---|---|
| +0x00 short | weapon mode (0 none, 1 melee, 2 ranged) |
| +0x02/+0x06 | melee slots (classRec+0x171 / +0x173) |
| +0x0a | ranged weapon slot (classRec+0x175; only item type 0xb bow / 0xc crossbow) — getter 0x497560 |
| +0x0e | ammo slot (classRec+0x177) — getter 0x497580 |
Slot record 0x12 bytes: +0 short itemId, +2 short **count** (ammo: item+0x2a), +4 short level, +6/+0xa/+0xe ints.
Script `get_unit_arrow_quantity` (0x65c3a0) returns ammoSlot+2.

#### 4.2 Ranged base damage 0x510bf0 (this = attacker) — CONFIRMED asm 0x510bf0-0x510daf
```
W = 0; hasAmmo = 0
if inventory:
   if weaponSlot: W  = itemDmg(weapon, slot.level)
   if ammoSlot:   W += itemDmg(ammo,   slot.level); hasAmmo = 1
   W = (WEAPON_DAMAGE(7) + 100) * 0.01 * W
div = hasAmmo ? 200.0 : 100.0
R = (float)(DEX() * (short)DEX_DAM + 100) * W / div        // DEX_DAM a3cca8 is int 1
if inventory: R = (WEAPON_DAMAGE_MULTIPLIER(8) + 100) * 0.01 * R
if attacker.isEnemyOf(player a5b9e8) [0x5094d0]: R *= {EASY_D,NORM_D,DIFF_D,VDIFF_D}[byte a57fc2]
itemDmg(item,lvl) = item+0x4e + (lvl - item+0x36) * item+0x52     (int, 0x4a41c0; item DB 0xa58f50, rec 0x2bf)
```
(effect ids per 0x497e10, peer notes.) With bow+arrows, R = (DEX+100)·(bowDmg+arrowDmg)/200 ≈ average × (1+DEX/100).

#### 4.3 Ranged hit → damage (0x75d540 → 0x5051e0 → 0x509e90) — CONFIRMED asm
Event handler 0x75d540 for types 2 / 0x40 / 0x2000 from a unit source:
- ARROW_DEFLECT_OFF: for types 2 and 0x2000 only: `if rand(100) < effect(10)` → ignore event (0x590540).
- type 2/0x40: dmg = 0x5051e0(src, type, ignoreDown=0); type 0x2000: 0x5051e0(src, type, 1).

0x5051e0 (this = victim) returns 0 when: victim state 0x2a or 0x42d0e0(); src == self; state ∈ {0x1e,0x1f,0x20,0x21,0x53}; (unless ignoreDown) state ∈ {0x40,0x41,0x42,0x4e,0x29}; victim+0x1dd > 0; attacker+0x2ed == 0; attacker not enemy (0x5094d0).
**Arrow block**: if victim state 0x2b or 0x2d (guard) and −BLOCKANG ≤ angle ≤ BLOCKANG (70°, `a3ccb0`, angle = 0x505960(attacker) relative to facing) → sound GUARDARR, optional DEFEND effect, state 0x2d, damage 0.
Else `angle = |ftol(angle)|`: ≤45 → p=0; ≤135 → p=20; else p=40; dmg = 0x509e90(src, type, p). If dmg≠0 and victim+0x102≠0: victim+0x2df += 5.

0x509e90 (this = victim, all float32):
```
A  = rangedBase(att)                                     // 0x510bf0
A  = (float)(att.morale(+0x163) + 100 - MOR_NOR) * A / 100
A  = upg(att) * A,   upg = (RANGE_ATTACK upgrade value + 10) / 11      // 0x510e40 → 0x50d3d0(0xe); 0 if none ⇒ 10/11  [value field HYPOTHESIS]
arm = (float)(vic.armorClass(+0x15d) * AMR_DEF)          // int multiply
D  = (float)(vic.morale + 100 - MOR_NOR) * ( DEF()*(100 - p)*(short)DEF_RAT / 100 + arm ) / 100
       // DEF_RAT a3cce0 is int 0 ⇒ DEF stat contributes nothing (asm movsx/fimul)
dmg = A/2 + max(0, (A - D)/2)
dmg *= sustain(vic) = inventory ? (100 - DAMAGE_SUSTAINED(0x16))*0.01 : 1.0       // 0x510f10
if vic.classType(classRec+2) != 8 (not siege):
    dist = ftol(|vic.pos(+0xe) - att.pos(+0xe)|)          // 3D, truncated int
    if dist >= FALL_E:            dmg *= DIST_P
    elif dist > FALL_S:           dmg *= 1 - (float)(dist-FALL_S)*DIST_P/(FALL_E-FALL_S)
if vic.armorClass == 3 and type ∈ {0x40, 0x2000}: dmg *= AMREXDEF (0.5)
if type == 0x40:                                  dmg *= SIE_UDAM (1.5)
if player unit exists:
    if game mode a58fa7 == 8: if att.nation≠player.nation: dmg *= F_ATTACK/250 ; if vic.nation≠player.nation: dmg *= F_DEFEND/100
    if att.nation == player.nation: dmg *= F_ATTACK/100 (1.3)
    if vic.nation == player.nation: dmg *= F_DEFEND/100 (0.7)
L = troopLeader(att) [0x4216d0]; if L && L != att: dmg *= (min(L.LEAD raw +0x15b, 100) + 100)/100
vic.setHP(ftol((float)vic.HP(+0x135) - dmg))             // 0x4fe510
return dmg
```
Falloff is applied to **all** event types (arrows, stones, cannon, explosions) based on shooter–victim distance; only skipped for siege-weapon victims. Falloff at 1600 u: ×0.75; ≥2400: ×0.5.
Splash damage has **no radius falloff**: every unit inside TR gets the full formula.

Reaction (0x75d540): type 2/0x40 path: if 0x508fb0 (state 0x40..0x42) → +0x1e1=1, state 0x29; else for 0x40 when not mounted (+0x1e2==0) → knock-down 0x509000(0); mounted & 0x693170 → state 0x4e; otherwise directional hit anim by angle: [−45,45] 0x23, [−135,−45) 0x27, (45,135) 0x28, else 0x26 (and turn toward attacker unless player or special 0xc/0xd). Knock-back force (sin,cos)·3000 xy, z 500 for 100 ms (×2.5 if state 0x4e, ×0.7 if 0x29), skipped for AUTOBOW class; **fall gauge** `unit+0x1cb −= ARR_FALL (25)` (0x4fea80). 0x2000 path: same reactions (0x2000 also knocks down unmounted), knock-back ×3.0 for 0x4e, no ARR_FALL.

#### 4.4 Accuracy spread 0x512130 (this = shooter) — CONFIRMED asm 0x51216a
```
if shooter is player or classType == 8 (siege): return 0
s = rand(ARR_ACCU)          // 0x590540(50): 0..49
neg = rand(2) != 0
return neg ? -s : s         // short, u
```
Used only for PROJ/EXPL shots with dist > 300 (separately for x and y). Normal arrows (ARW), stones and cannonballs have **no** random spread.

#### 4.5 Player range mode (input side; PlayerControl object = ecx) — HIGH
- Target pick 0x42c260: yaw diff (camera vs character +0xb1, skipped if mode +0x10==3) and pitch diff (+0xb5) must be ≤ AIM_HA (60°). Candidate: enemy, alive, special ≠ HORSE(2)/ANIMAL(7). Aim point = unit pos + classHeight·0.5. With d = aimPoint − cameraPos, v = camera forward: `perp = sqrt(|d|² − (d·v)²/|v|²)`; accept if `perp ≤ |d|/100 + AIM_R (25)` and `|d| < best` (best starts AIM_MXD 2500); nearest wins. CONFIRMED.
- Lock keep 0x42c830: drop lock (ctrl+0xbb, unit+0x2db) if distance > AIM_MXD or yaw/pitch diff > AIM_HA. CONFIRMED.
- Draw 0x42ad50: start (state 0x30) when fire input (0x508ab0) and bow slot and ammo count ≠ 0; ctrl+0x97 = now. Nock → state 0x36 when `now − ctrl+0x97 ≥ drawAnimLen·100/(RATE_OF_FIRE(0x11)+100)`. CONFIRMED.
- Release 0x42ba40 (in state 0x36 with bow+ammo): dir = normalize(lockTarget.pos + (0,0,60) − cameraPos) if locked, else camera forward from camera angles; store unit+0x365; state 0x33. CONFIRMED.

---------------------------------------------------------------------------------------------------
### 5. Firing state machine (Unit anim-end handler 0x7c7ab0, called from Unit_process 0x52c490) — CONFIRMED
`rem = 0x5045f0()` = remaining ms of current animation (0 when finished).

| state | meaning | transition |
|---|---|---|
| 0x30 | draw / load (attach ammo model to unit+0x1bd) | AI with a live +0x2db target: aimDir re-solved, `Aim_solve_by_type 0x4945c0` type 1 from the ammo model's bone origin to the target's feet (+0x0e); rem==0: player → hold; AI: weapon(+0x1bf) type 0xc crossbow → 0x32; else aimDir.z<0.1 → 0x36 else 0x39 (port `World::ai_reaim`) |
| 0x32 | crossbow aim | AI: aimDir re-solved as 0x30 with type 0; rem==0 → 0x36 (z<0.1) / 0x39 |
| 0x36 | aim hold (level) | rem==0: player waits for release (0x42ba40→0x33); AI → 0x33 |
| 0x39 | aim hold (high) | rem==0: AI → 0x38 |
| 0x33 / 0x38 | shoot / shoot-high | **spawn when rem ≤ release** (once; ammo model consumed via 0x49f370); rem==0: TREBUCHET → 0x34, else idle (0x508b90) |
| 0x34 | trebuchet recover | rem==0 → idle |

Release threshold (ms of anim remaining): default 650 (`0x28a`); CANNON(4)/BOMBARD(9)/FAUCONNEAU(5) 1667 (`0x683`); TREBUCHET(10) 0; AI with crossbow weapon item 1300 (`0x514`). Before spawning, AI aim dir = 0x50aca0: normalize(target unit pos − own pos), else target object, else actor forward (+0.15 z for CANNON/FAUCONNEAU).
Rate of fire = sum of animation durations (data in model files) — no separate reload timer for AI; player draw time scaled by RATE_OF_FIRE (§4.5).

---------------------------------------------------------------------------------------------------
### 6. Events (0x4eef00 post → 0x4ef090 dispatch) — CONFIRMED
Record 0x43 bytes (init 0x47e200): +0 type, +4 targetKind (1 unit, 2 map object, 4 …), +8 short targetId (≤0 → area), +0xa srcKind (1 unit, 2 object), +0xe short srcId, +0x13 pos[3], +0x1f float radius, +0x26 deliverTime (= now), +0x2a time2, +0x2e flag.
Types: 1 melee, 2 arrow, 0x40 stone/cannon (siege), 0x2000 explosive arrow, 0x4000 explosive keg, 0x200 interact, 0x20, 0x10, 0x400.
Dispatch: units receive only types present in classRec+0x11c trigger mask (TRIGGER_ARROW_ATTACK 2, TRIGGER_STONE_ATTACK 0x40, TRIGGER_EXPLOSIVE_ARROW_ATTACK 0x2000, TRIGGER_EXPLOSIVE_KEG_ATTACK 0x4000, TRIGGER_ATTACK 1, TRIGGER_DOWN_ATTACK 0x400, TRIGGER_THROW 0x10, TRIGGER_INTERACT 0x200; parsed in 0x525080).
Area to units (skip the source): types 0x40/0x2000/0x4000: victim shape 2 (box, classRec+0x1c3==2) → AABB [p−r, p+r] in victim local space vs box (0x60d6b0); else cylinder `dx²+dy² ≤ r² and |dz| ≤ r`. Other types: sphere `|p−u|² ≤ r²`.
Area to objects: 0x41c5e0(pos, ftol(r)) overlap test → object vtable+0x48.

---------------------------------------------------------------------------------------------------
### 7. Siege weapons, structures

#### 7.1 Unit-side
Siege weapons are units of class type 8 (SIEGE_WEAPON, 0x527220). Special (classRec+0x118, 0x525080): 0 NORMAL, 1 MAIN, 2 HORSE, 3 ELEPHANT, 4 CANNON, 5 FAUCONNEAU, 6 CROSSBOW, 7 ANIMAL, 8 SEIGE_TOWER, 9 BOMBARD, 10 TREBUCHET, 11 BATTERING_RAM, 12 AUTOBOW, 13 CART, 14 SPECIAL. CONFIRMED.
Stats (units.tsv): CANNON HP200 DEX75; BOMBARD HP200 DEX80 (walk 0); TREBUCHET HP300 DEX130; FAUCONNEAU HP150 DEX60; CROSSBOW (siege) HP100 DEX60; AUTOBOW HP100 DEX40, AUTOBOW2 DEX60; BATTERRAM HP500; SIEGE_TOWER HP200.
- Ranged siege fire = same pipeline as archers: ammo item from classRec+0x177 (cannonball → CAN, stone → STC), damage = rangedBase with DEX (siege divisor 200 since ammo slot exists), events 0x40 → ×SIE_UDAM vs units, ×AMREXDEF vs armorClass 3; no spread (class 8).
- Siege-only classRec+0x165 int (parsed only if class type 8) — UNKNOWN meaning.
- Crew: regular infantry units with an anim-variant `unit+0x21d` set by script `change_animation_special` (0x65c050 ← 0x416aa0: DEFAULT0 TIRED1 HAPPY2 POISON3 CAMP4 CANNON5 FAUCONN6 BOMBARD7 CROSSBOW8 SIEGET9 TREB_L10 TREB_R11 TREB_F12 BATTER13 BATTER_L14 BATTER_R15); anim tables give PUSH / SHTLOADC / SHTOUTC / BATTER_C clips per variant (e.g. ELINFANT.csv). Crew attachment points: classRec+0x1ad count, +0x1b1 xyz, +0x1b5 angle, +0x1b9 role 'M'1/'F'2/'B'3, +0x1bd item. Crew logic beyond this is scripted (UNKNOWN). `mount_siege_tower_to_wall` script 0x64dc9a (not analysed).
- Battering ram: melee attacker (anim script BATTERRAM_ATTACK, DAMAGE column 100 in BATTER.csv); damages gates/walls through melee event type 1 (below).
- 0x546520 = skeleton/animation-set name → id (human_cannon 8, autobow 9, batterram 10, human_trebuchet 0xe …); not gameplay.

#### 7.2 Gates (i_gate.cpp; vtable 0x9d388c, onEvent 0x48caf0) — CONFIRMED asm
Fields: +0xc2 int HP, +0xc6 max HP, +0xcf invincible, +0xd0 attacked flag, +0xd1 open state, +0x52 damage stage index; objRec+0x63+stage*4 = HP threshold of stage, objRec+0x96+stage = stage kind, +0x87+stage*4 = stage anim.
```
event 1 (melee, src unit): only if attacker class "LAHIRE" or special BATTERING_RAM(11):
      dmg = meleeBase(att)[0x5104f0] * 0.5 ; if LAHIRE: dmg *= 3.0
      HP = max(0, ftol(HP - dmg))                                    // 0x48c8c0
event 0x40 / 0x2000 (src unit, gate not invincible, closed):
      dmg = rangedBase(att)[0x510bf0] * 0.5 ; HP = ftol(max(0, HP - dmg))   // 0x48c990
event 0x4000 (src object = keg): dmg = keg+0x72 ; HP = ftol(max(0, HP - dmg)) // 0x48ca30
event 2 (arrows): ignored
then 0x48e430: if HP <= stageThreshold[stage]: stage -= 1 (one stage per event) + anim/shake (+0x30 = -0.3)
if stage == 0 and HP == 0: destroyed effect objRec+0x239, 0x48d4b0(0) (collision opened)
```
Damage dealt to gates ignores falloff/morale/difficulty modifiers of 0x509e90 (only those inside rangedBase/meleeBase).

#### 7.3 Walls (i_wall.cpp; vtable 0x9d3c88, onEvent 0x54ffd0) — CONFIRMED
Fields: +0xc6 HP, +0xca max HP, +0xe6 invincible.
- event 1: only attacker special BATTERING_RAM: dmg = meleeBase·0.5 (0x451f20).
- event 0x40 (any game mode) and 0x2000 **only if game mode a58fa7 == 4**: dmg = rangedBase·0.5 (0x492b10).
- event 0x4000 from object: dmg = keg+0x72 (0x492bb0). Arrows ignored.
- HP = ftol(max(0, HP − dmg)); if HP ≤ stageThreshold[stage] advance one stage (kinds 0..4; kind 3 = break segment 0x492e20). Script `break_wall(obj, src)` posts (0x40, object, obj, radius 1000).

#### 7.4 Other objects
- Explosive keg (vtable 0x9d3a18, 0x48fa50): hit by 0x2000 (from unit) or 0x4000 (from object) → stage −1 and posts (0x4000, objects, area) + (0x4000, units, area), **radius 200**, srcKind 2 → chain reactions. Keg damage to units 0x505650/0x49d5f0: `A = keg+0x72; dmg = (A/2 + max(0,(A−D)/2))·sustain·nation factors` (D as §4.3 with p=0; no falloff, no AMREXDEF); victim+0x2df += 10.
- Breakable boxes (0x48a1f0) / misc (0x48ef50, 0x493680): any qualifying hit advances one stage; at stage 0 drops item +0xc2.

---------------------------------------------------------------------------------------------------
### 8. Constants used (GAMEPLAY.RES)

| key | value | global | parse | used by |
|---|---|---|---|---|
| DEX_DAM | 1.5→**1** | a3cca8 | atoi (word) | 0x510bf0 |
| ARR_ACCU | 50 | a3cca4 | atoi | 0x512130 |
| ARR_FALL | 25 | a3cc08 | atoi | 0x75d540 (fall gauge) |
| ARW_ATH | 10 | a3cc40 | atoi | unused |
| ARW_TIME | 30000 | a3cc68 | atoi | unused |
| FALL_S | 800 | a3cc0c | atoi | 0x509e90 |
| FALL_E | 2400 | a3cc6c | atoi | 0x509e90 |
| DIST_P | 0.5 | a3cc20 | atof | 0x509e90 |
| AMR_DEF | 1 | a3cc00 | atoi | 0x509e90, 0x49d5f0 |
| AMREXDEF | 0.5 | a3cd0c | atof | 0x509e90 |
| SIE_UDAM | 1.5 | a3cbec | atof | 0x509e90 |
| DEF_RAT | 0.3→**0** | a3cce0 | atoi (word) | 0x509e90, 0x49d5f0 |
| MOR_NOR | 50 | a3cc8c | atoi | 0x509e90 |
| F_ATTACK / F_DEFEND | 130 / 70 | a3ccf4 / a3cc2c | atoi | 0x509e90 |
| BLOCKANG | 70 | a3ccb0 | atoi | 0x5051e0 |
| AIM_R / AIM_MXD / AIM_HA | 25.0 / 2500.0 / 60.0 | a3373c / a33758 / a33754 | atof | 0x42c260, 0x42c830 |
| EASY_D..VDIFF_D | 0.7/1.0/1.5/2.0 | a391ac/a3918c/a391b0/a39194 | atof | 0x510bf0 |
Hard-coded: 46.1 u/m, 9.81, release 650/1667/1300/0 ms, spread gate 300 u, facing dot 0.9, aim lift 0.8·height, ground blast radius 150, keg radius 200, knock-back 3000/500, force pulse 100 ms (ARW) / 1 ms, physics sub-steps 2 (ARW) / 5.

---------------------------------------------------------------------------------------------------

---------------------------------------------------------------------------------------------------
## 8. Original-game quirks a faithful port must reproduce (or consciously fix)

All of these were found in code (CONFIRMED unless noted). Section references are to the parts above.

1. **atoi on fractional keys.** STR_DAM "3.0" → 3, **DEX_DAM "1.5" → 1**, **DEF_RAT "0.3" → 0**. With DEF_RAT = 0 the DEF stat contributes nothing to melee, ranged or keg defence. Only armour level (`U+0x15d`·AMR_DEF) and morale count, and the front/side/back defence-ignore buckets do nothing. DEF still feeds HP regen (×0.1) and fall-gauge recovery (Part A §3). §3.
2. **Difficulty B row never read.** `EASY_B..VDIFF_B` are never loaded, and the `_A` row is loaded twice: into guard chance 0x511f80 and BLOCKPER 0x512070. Enemy damage uses the `_D` row inside the base damage of melee 0x5104f0 and ranged 0x510bf0. §3, Part C §4.1/§5.1.
3. **Parsed but unused:** HATK_MP (heavy-attack EP is the move's ENERGY_PT column), JUMPMIND, COM_TIME/COM_LIM/COM_CONS, ARW_ATH/ARW_TIME (arrows never stick), HORSE_WD, THROWI `*_MND`. Also unused: the unit.res RegenHp/RegenEp columns, and the STM projectile row (trebuchets use STC).
4. **Timing.**
   - Logic is 30 Hz, but timers use the last loop iteration's ms (`dt`) or absolute `now` (§1.1).
   - The player controller and input consumption run **per drawn frame**, one event per frame.
   - Horse turning is 6° **per frame** (Part B).
   - The unit scheduler's adaptive budget makes the update set CPU-dependent once a tier holds more than 50 units (§1.3).
   - Generals and kings (rank ≥ 4) near the player run AI phase 3 **twice** per tick (§1.3).
5. **HP truncation.** `HP = ftol(HP − dmg)`, so any positive damage costs ≥ 1 HP. FATAL_KILL still passes through the F_DEFEND 0.7 and leader multipliers, so a player-side unit can survive a "fatal" hit (Part C §4.2).
6. **Late damage sampling.** Damage is computed when the trigger fires (`now ≥ start`), from the attacker's *current* animation record. If the next combo step has already started, that step's DAMAGE/Range is used (Part C §4.2).
7. **Melee reach.**
   - The down-attack trigger ignores weapon reach: both slots are written to the same local.
   - `Melee_tryHit` uses the slot-1 reach if present, otherwise slot-0, with no max (Part C §3.1/§3.3).
8. **Morale setter 0x4fe8c0.** A morale drop is ignored whenever the current morale is above the unit's MORALE_BASE item bonus (default 0). Every unit gets an inventory in Unit_init, so troop members and leaders effectively never lose morale through this setter (Part A §5, Part C §6). HIGH for the in-game effect.
9. **Keyboard-only jump never fires.** The code requires `held(Up)+held(Down)+held(Left)+held(Right) == 1`, but a held key reads 0x80 (Part B §3.3).
10. **Combo-hit counter.** It resets after >1999 ms without a hit; the manual says 1 s (Part C §4.5).
11. **Skill multiplier mismatches.** Horse attacks and the jump 'D' attack never match an upgrade record, so they always use skill level 0 (×0.909) (Part C §2.5; data-dependent, HIGH).
12. **Ammo.** Only the player consumes ammo; AI archers have infinite arrows (Part D §3.2).
13. **Cannonballs** that hit the ground do nothing; splash damage has no distance falloff (Part D §3.5).
14. **Dead code:** the unit snapshot pair 0x462610/0x466320 has no callers (Part A §6).
15. **Name conflict.** `names.tsv` labels 0x42e1a0 `Game_quick_save`, but it is `PlayerCtrl_process`, the per-frame controller dispatcher. F5/F6 quick save/load are just two of its hotkeys. `names_combat.tsv` carries the correction.

## 9. Gaps and open questions

The per-part "Open items" lists above hold the details. Summary:
- **Animation timing data.** Flinch, knockdown, stand-up and jump arc durations live in model/animation data (`ANIM\`, see docs/formats/MODELS_AND_ANIMATION.md), not in code. The combo input windows come from `std_set/<HERO>.csv` START_TIME/END_TIME, and the hit times from `animscr.res` StartTime. These are data, and the reimplementation must load them.
- **Speed units.** Speeds are about 46.1 units/s per unit-res speed point (Part B §5). HYPOTHESIS; needs an in-game measurement.
- **RelAngle sign.** Whether a positive relative angle means right is a HYPOTHESIS (from the LATTKED/RATTKED mapping).
- **Unidentified tick work.** Several Game_process_tick subsystems in §1.2 are only HYPOTHESIS-labelled. They do not affect combat, except that **they consume the shared RNG**.
- **Siege crews.** Crew operation and siege AI appear to be scripted (Part D §7). Gate and wall damage rules are CONFIRMED.
- **Game mode 8.** RESOLVED: `0xa58fa7` = scenario no; the F_ATTACK/250 branch runs in campaign scenario 8 (S6L1). Same branch in 0x49d5f0 (keg) and 0x509e90; the only other `== 8` reads are UI (GameEndMenu 0x7226a0/0x720ca0, HUD text 0x6f9a60).
- **No dynamic verification.** None of the above was checked in-game. The DynamicRef agent's captures can validate the HP regen period, the knockdown durations and the damage numbers.

---------------------------------------------------------------------------------------------------
### Item models, armour meshes, floor items (EquipVisuals)

- **Unit model level** `Unit+0x3c5` (int): class record `+0x84 + n*4` mesh / `+0x90 + n*4` animation group; n = 0 is
  unit.res `Model`/`Animation`, n ≥ 1 the n-th `ModelName`/`AnimationName` after `ModelLevel` (UnitRes_load_record
  0x404afc). CONFIRMED. Starts at 0; copied with the hero across missions (0x4c96f0, last line); saved in the unit image.
- `Unit_set_model_level` 0x511060(level): when the handle changes, destroys the actor and builds a new one with the
  level's mesh and animation group (+0x311). CONFIRMED. Callers: armour use 0x6d8ac0 and Unit_die 0x408571 only
  (script `set_unit_armor_id` 0x6562b0 writes +0x15f without a mesh change).
- **Armour use** 0x6d8ac0 (inventory right-click, CONFIRMED): class 0x10 item usable by the unit (+0x255 = 0 or the
  unit's class): `if armor_level(+0x15d) < item.Defense(+0x56) { armor_level = Defense; set_model_level(item.Damage
  +0x4e); armor_item(+0x15f) = code }`, and the armour item leaves the bag either way. Joan: LV1 (Def 1, Dmg 0)
  joan_armorC, LV2 (Def 2, Dmg 1) Joan_of_Arc.am1, LV3 (Def 3, Dmg 2) joan_armorB.
- **Broken siege weapons**: Unit_die, class unit_class 8 with special 4,5,6,9,10,11,12 → set_model_level(1)
  (broken_cannon.am1 + BROKEN_CANNON animation group). CONFIRMED. The renderer holds that group's idle (HYPOTHESIS:
  wrecks are static).
- **Item model levels** ItemRes_model 0x4a3f20(level): 0 = `Model`, n = n-th `ModelName` (+0x73 + n*4), `Model` when
  empty. CONFIRMED. Item vtable +0x18 sets the level: 1 = carried in a hand (range-mode exit 0x42b6e0 re-equips the melee
  item at level 1), 2 = projectile (ThrowItem_create 0x507fc0, every type), 0 = ItemArray_create default (floor drops).
- Projectile orientation: launch 0x5133e0 turns the model to the shot direction; ThrowItem_update 0x4fcc00 re-orients
  PROJ/EXPL along the velocity. Impact emitter = item EmitterName slot 3 (explosion_arrow / CannonImpact) at the last
  position: HYPOTHESIS (slot naming; spawn site not decoded). Trail 0x687500(TL) not reproduced.
- **Range mode** 0x42b3c0 clears the right-hand item (0x50a480(0,0,0,0)) and hides the hero actor (0x42a6c0 →
  FUN_0042cf20(0), actor +0x10a; exit 0x42b6e0 → (1)). HIGH (ECX of the call inferred).
- **Unit_dropItem** 0x406631: ItemArray_create(drop item) at the unit position/heading (+0x1a), floor marker 0x456cd0
  with quantity = item Quantity (+0x2a). CONFIRMED.
- **Unit_auto_pickup** 0x49da30 (unit phase 0 head, CONFIRMED structure): rank (+0x104) > 3, active body, LOD tier
  (+0x22) ≤ 3, nation (+2) == the player unit's nation (0x49da91; the port let enemy generals pick up too); for each
  floor item with Pickable (+0x1c4) and (class != MISSION or hero +0x120): horizontal distance ≤ item radius + unit
  radius (actor +0xc8 / +0xf0 × scale), |dz| ≤ unit actor height (+0xf4). Then: red potion (type 0x15) whose amount
  (+0x4e) < max HP − HP → used (0x50b4f0); MISSION S7_GOLDBAR → gold-crate counter + message; MISSION S7_food* → food
  counter (gated by set_food_can_be_picked); GOLD → DAT_00a596c4 += max(1, qty) × Price (+0x2e) + message; else
  add to inventory 0x495f40 (may fail → item stays). Each pickup: item emitter (+0x17a, the EmitterName slot-2
  "*_eaten" pscript, HIGH) at the item for 5000 ms, sound "PICKUP" at the unit, item deleted 0x49f370.
  Native: items_pickup.cpp walks World drops and MapObjects items with a floor marker (item radius fixed at 15:
  HYPOTHESIS). Marker (0x456cd0): +0 item, +2 quantity (ItemRes +0x2a), +4 level, +6/+10/+14 ints = 0, so GOLD adds
  Price once. No .pro sets Pickable 1 and .stg CreateObject names only .pro files: every pickable map item is
  runtime-created (box drop FUN_00463ed0, script add_item 0x651ad0), so no static scene copy exists to hide.
- **Pickup messages** (player only, `*unit == DAT_00a5b9e8`): FUN_006e5700 (interfac\i_sms.cpp) queues an SMS line.
  Inventory pickup: 0x5a6660(MessageName +0x18, marker quantity) into t_interf SMS 3 "You have picked up %1s."
  (FUN_004dd6a0); GOLD: same with the gold value; script give_item 0x65c5f0: SMS 4 "You have been given %1s!"
  (FUN_004dd6e0); S7_GOLDBAR: SMS 11/12 "You have collected %1d barrel(s) of gold" with type 1 (replaces queued
  counter lines); S7_food* while food_can_be_picked is clear: attention message MESSAGE 3, item stays. Format codes of
  0x5a6660: %Ns/%Nd, %N?{a|b}, %Nj{1st|2nd|..} (none out of range), %Np{one|many} (0/1 = one), %Nq{zero|one|many}.
  Items without MessageName (potions) give "You have picked up ." (empty format, HYPOTHESIS of the NULL case).
  SMS widget (setup 0x6e4f10, slide 0x6e5280, draw 0x6e5140): QSAVE QLC_BAR at (x, 516), Goudy14(22) black text at
  (x, 518), in from -width at 2700 px/s (-1000 px/s^2) fading in 1.5/s, hold 3000 ms, text out (+1600 px/s^2), bar
  follows at 20 %. Native: GameUiBridge::sms_line (BottomLeft frame).
- **Arrow orientation**: arrow01_hold.smo lies along mesh +X (x 8..69, head end narrower than the vanes);
  ThrowItem_launch sets the item yaw to the velocity heading, so +X along the velocity is correct (engine.equip_visuals
  asserts head . velocity > 0.9; side/behind captures in engine.gl_/vk_equip_visuals).
