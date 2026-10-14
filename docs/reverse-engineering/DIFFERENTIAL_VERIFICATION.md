# Differential verification: original joan.exe vs port

The same closed-loop scenarios run on the ORIGINAL game and on the port. The original runs under Wine and its state is read live from process memory. The port runs in lockstep through `joa-port-trace`. Then the traces are compared numerically. Reference only: Wine is never a runtime dependency.

## Harness

| piece | role |
|---|---|
| `tools/reference/diff/diffrun.py orig\|port [scenario…]` | Runs the scenarios and writes traces to `work/diff/<backend>/<scenario>.json`: one sample per original frame or port tick, holding every unit within 4000 units of the player. |
| `tools/reference/diff/compare.py [--self-test]` | Computes the metrics and the table below, written to `work/diff/report.md`. |
| `tools/reference/diff/port_trace.cpp` → `joa-port-trace` | Lockstep driver of the port's mission World over stdin/stdout. Commands: `start`, `step N held pressed mdx`, `units`, `set` (test hooks hp/ep/xp/x/y/yaw), `objects` (map items: recno, cls, type, content, position, res, stage, on-floor), `path X Y` (mission navmesh). Build target `joa-port-trace`, via `tools/reference/CMakeLists.txt`; `JOA_PORT_BUILD=<build dir>` selects the build. |

**Original side** (`Orig` class):
- **Isolation:** a private Wine prefix and a reflink game copy under `work/diff/wine`, plus a private Xvfb `:92`. It never touches `work/wine` or the real install.
- **Menu flow:** movies are skipped by screen mode, then New Campaign → Normal. Escape skips the cut scene. F5 then saves the scenario checkpoint. Each scenario starts with F6 + OK (quick load), verified by Joan's position and HP.
- **Memory access:** the harness makes itself a child subreaper (`prctl(PR_SET_CHILD_SUBREAPER)`). The double-forked `joan.exe` then becomes its child, so Yama `ptrace_scope=1` allows `/proc/<pid>/mem`. Under WoW64 the 32-bit VAs are used as-is.
- **Reads:**
  - UnitArray `0xa47c08`: a DynArrayB with `+0x0c` count, `+0x10` element size 4, `+0x19` array of unit pointers (recno-1).
  - Player recno `0xa5b9e8`, game clock `0xa500ba`.
  - Camera yaw: `*(0xa48ccc)+0xb0` (View3D current CameraState +0x10).
  - Screen mode `Sys+0x3a`. Observed values: 0 enlight.wmv, 14 intro.wmv, 3 main menu, 12 loading, 9 in-game.
  - Unit fields (GAMEPLAY_CHARACTER_COMBAT.md Part A §1): `+0x0e` pos, `+0x1a` yaw in radians, `+0x124` level, `+0x126` XP, `+0x135` HP, `+0x13d` max HP, `+0x145`/`+0x149` EP, `+0x155..` STR/DEX/DEF/LEAD, `+0x163` morale, `+0x15d` armor level, `+0x37` leader, `+0x215` action.
- **Writes:** the RNG seed `0xa46022` is set to 1 after every reset. HP pinning writes `+0x135` (test hook).
- **Not possible:** writing unit `+0x0e` (teleport) does not stick. The physics body position wins on the next move.
- **Inputs:** xdotool, as in `ORIGINAL_BEHAVIOR.md`. Sampling is a 10 ms polling thread. Every wait uses the game clock (real_dt is clamped to 1000 ms under host-load hitches).

**Port side:** mission 1, Normal, seed 1, cut scene skipped. The camera yaw is fed back from `PlayerController::camera_yaw_out`, as `GameCamera` does. Mouse moves are split into ≤40-mickey chunks, one per tick, matching `joa_ref.rel`. This matters because 0x431790 truncates `ftol(dx·0.18)` per frame: both sides turn 78°, not 90°, for 500 mickeys. Like `rel()` on the original side, a mouse move blocks until all its chunks are sent (Round 3: queued chunks used to pile up across the 150 ms re-aims of `goto`, so the port over-turned and orbited waypoints).

**Shared logic:** both sides use the same scenario code. Navigation follows the port's navmesh path (`path`, built from the original `.PFD`) with identical waypoints on both sides. Unit recnos are identical on both sides (72 Joan, 73 Jean de Metz, 42/43 CI01/CI02, 14 archer "Unit 63").

**Damage attribution:** the original keeps no attacker log. An HP drop is attributed to the opposing units that were within 250 units and in a melee attack state (ATTACK/JATTACKB/ENGATK*/DOWNATK/H_ATTACK) in that sample or the previous one. The same heuristic runs on both sides.

**Verdicts:** numbers match within 10 % (minimum 1). Latencies match within 70 ms (one or two original frames plus the sampling period). Positions match within 50 units. Strings must be equal.

## Scenarios

| scenario | what |
|---|---|
| start | Mission 1 stats of Joan and Jean, position, facing. |
| move | Run (W) 4 s, back (S) 3 s, strafe A 2 s, strafe D 2 s, walk (Shift+W) 3 s, with 1.5 s rests. Also Jean's follow behaviour. |
| boundary | Run 11 s along the initial facing into the battlefield edge (y≈14093). |
| turn | +500 mickeys standing, −500 mickeys while running. |
| combo | Navmesh route to CI01 (HP pinned on the way), then 12 LMB taps every 380 ms. |
| block | Q held 6 s facing CI01, then 6 s without guarding. |
| archer | Stand 700 units from the archer for 12 s (HP kept ≥ 30). Per shot: distance to the nearest opponent at SHTLOAD entry (its target) and whether that target loses HP with no melee attacker within 1 s of SHTOUT. |
| potion | Break map item 1 (bread box, content 36 SMALL_BREAD, a POTION), walk over the drop at full HP, HP = 20 (hook), F3. |
| levelup | XP hook 990; fight the first camp with HP pinned. The target is kept until it dies and set to 1 HP before each of Joan's swings (hook), so Joan lands the kill. XP gains of Joan and Jean are listed per kill. |
| aggro | Walk along the route to CI01. For each English unit, Joan's distance when it first runs/walks/attacks or moves >30 units. |

Run: `JOA_PORT_BUILD=<build dir> python3 tools/reference/diff/diffrun.py port && python3 tools/reference/diff/diffrun.py orig && python3 tools/reference/diff/compare.py`. The original run takes ~11 min; the port run takes seconds.

## Results (2026-10-14, Round 3: full set on both sides after the fixes; Wine 11.19, llvmpipe; one original run per scenario, `levelup` rerun on both sides after its first-target change)

| scenario | metric | original | port | verdict |
|---|---|---|---|---|
| start | Joan of Arc hp / mhp / ep / mep | 60 / 60 / 30 / 30 | 60 / 60 / 30 / 30 | match |
| start | Joan of Arc str / dex / def / lead / lvl / xp | 9 / 10 / 8 / 15 / 1 / 0 | 9 / 10 / 8 / 15 / 1 / 0 | match |
| start | Joan of Arc pos / yaw | (15021, 12684) / 115 | (15021, 12684) / 115 | match |
| start | Jean de Metz hp / mhp / ep / mep | 50 / 50 / 30 / 30 | 50 / 50 / 30 / 30 | match |
| start | Jean de Metz str / dex / def / lead / lvl / xp | 10 / 15 / 8 / 5 / 1 / 0 | 10 / 15 / 8 / 5 / 1 / 0 | match |
| start | Jean de Metz pos / yaw | (14934, 12847) / 295 | (14934, 12847) / 295 | match |
| move | run speed u/s (0.7..1.7 s) | 296.71 | 299.66 | match |
| move | run distance u (4 s) | 1178.82 | 1169.35 | match |
| move | run start latency ms | 35 | 33 | match |
| move | run actions | IDLE/RUN | RUN | differs |
| move | back speed u/s (0.7..1.7 s) | 184.82 | 176.64 | match |
| move | back distance u (3 s) | 608.64 | 589.94 | match |
| move | back start latency ms | 50 | 33 | match |
| move | back actions | IDLE/MOVE_B | MOVE_B | differs |
| move | strafe speed u/s (0.7..1.7 s) | 78.61 | 198.05 | MISMATCH |
| move | strafe distance u (2 s) | 236.46 | 339.93 | MISMATCH |
| move | strafe start latency ms | 58 | 33 | match |
| move | strafe actions | IDLE/MOVE_L | MOVE_L | differs |
| move | strafe_r speed u/s (0.7..1.7 s) | 237.27 | 239.71 | match |
| move | strafe_r distance u (2 s) | 483.43 | 474.60 | match |
| move | strafe_r start latency ms | 56 | 33 | match |
| move | strafe_r actions | IDLE/MOVE_R | MOVE_R | differs |
| move | walk speed u/s (0.7..1.7 s) | 196.74 | 178.74 | match |
| move | walk distance u (3 s) | 569.82 | 514.69 | match |
| move | walk start latency ms | 83 | 33 | match |
| move | walk actions | IDLE/WALK | WALK | differs |
| move | Jean dist to Joan at run start | 184.77 | 184.76 | match |
| move | Jean reaction latency ms | 587 | 561 | match |
| move | Jean max dist during run | 635.01 | 663.41 | match |
| move | Jean dist 1.5 s after run | 301.85 | 288.91 | match |
| move | Jean peak 1 s speed u/s | 371.48 | 417.12 | MISMATCH |
| move | Jean actions | IDLE/IDLE2/RUN | IDLE/RUN | differs |
| boundary | max y reached | 14094.24 | 14093.32 | match |
| boundary | ms to y > 14000 | 4918 | 4917 | match |
| boundary | Joan yaw at start / end | 115 / 245 | 115 / 262 | differs |
| boundary | camera yaw at start / end | 115 / 245 | 115 / 262 | differs |
| boundary | end position | (13527, 13022) | (13527, 13022) | match |
| boundary | distance moved after reaching the edge | 1335.12 | 1326.49 | match |
| boundary | actions after the edge | IDLE/RUN | RUN | differs |
| turn | turn_stand yaw change deg (+500 mickeys) | -78.00 | -78.00 | match |
| turn | turn_stand deg per mickey | -0.16 | -0.16 | match |
| turn | turn_run yaw change deg (-500 mickeys) | 78.00 | 78.00 | match |
| turn | turn_run deg per mickey | -0.16 | -0.16 | match |
| combo | Joan swings (damage x victims) | 4x3 4 | 4 | differs |
| combo | Jean swings (damage x victims) | 11 11x3 7 7 9x2 13 | 7 9 7 9 | differs |
| combo | ambiguous hits (Joan+Jean / none) | 1 | 2 | match |
| combo | damage taken by Joan per hit | 4 5 8 | 4 5 8 | match |
| combo | Joan attack actions | ATTACK | ATTACK | match |
| block | block: enemy swings within 200 | 7 | 3 | MISMATCH |
| block | block: Joan HP lost / hits | 0 / 0 | 0 / 0 | match |
| block | block: blocked hits (GUARDR entries) | 1 | 1 | match |
| block | block: Joan actions | GUARD1 GUARDR IDLE_BAT | GUARD1 GUARDR RUN | differs |
| block | noblock: enemy swings within 200 | 10 | 10 | match |
| block | noblock: Joan HP lost / hits | 15 / 3 | 4 / 1 | differs |
| block | noblock: blocked hits (GUARDR entries) | 0 | 0 | match |
| block | noblock: Joan actions | ATTACKED GUARDR IDLE_BAT | ATTACKED GUARDR IDLE | differs |
| archer | archer shot distance (mean of SHTLOAD entries) | 262.69 | 265.37 | match |
| archer | archer shot distances | 254 271 262 | 266 275 256 | differs |
| archer | arrow hits / shots | 0 / 3 | 0 / 3 | match |
| archer | melee hits on Joan in 12 s | 2 | 2 | match |
| archer | archer actions | IDLE IDLE_BAT RUN SHTIDLE SHTLOAD SHTOUT | IDLE IDLE_BAT RUN SHTIDLE SHTLOAD SHTOUT | match |
| potion | HP before F3 | 20 | 20 | match |
| potion | HP after F3 | 60 | 60 | match |
| potion | healed | 40 | 40 | match |
| levelup | Joan kill XP | CI01 L2 +100 | CI01 L2 +100 | match |
| levelup | Jean kill XP | none | none | match |
| levelup | lvl before/after | 1 / 2 | 1 / 2 | match |
| levelup | xp before/after | 990 / 1090 | 990 / 1090 | match |
| levelup | mhp / mep / str / dex / def / lead before/after | 60 / 30 / 9 / 10 / 8 / 15, unchanged | 60 / 30 / 9 / 10 / 8 / 15, unchanged | match |
| levelup | game ms into fight | 270 | 297 | match |
| aggro | 33 BIC03: Joan dist at reaction | 2040 | 2063 | match |
| aggro | 35 Geffrey Oakham: Joan dist at reaction | 448 | 426 | match |
| aggro | 36 BIC02: Joan dist at reaction | 2011 | 2079 | match |
| aggro | 42 CI01: Joan dist at reaction | 752 | 790 | match |
| aggro | 43 CI02: Joan dist at reaction | 753 | 785 | match |

`compare.py` prints every start/levelup stat on its own row; they are grouped here. The "actions" rows that differ only by the original's leading IDLE sample are a sampling artefact.

## Analysis

**Matches (port faithful):**
- All mission-1 start stats, positions and facings.
- Steady speeds: run ≈297/300, back 185/177, strafe right 237/240, walk 197/179 u/s. Start latencies 35–83 ms vs one 33 ms tick.
- Mouse turning: 0.156°/mickey on both sides once the per-frame `ftol` is reproduced.
- Jean's follow: start distance, reaction 587/561 ms, maximum lag 635/663, distance after the stop.
- Battlefield edge: max y 14094/14093 at 4.92 s. After the edge both slide west, turn the corner at (13876, 14081) and end at (13527, 13022), 1335/1326 units moved.
- Damage: Joan's A1 = **4** on English light infantry. Damage taken per hit: 4 5 8 on both sides.
- Blocking: one blocked hit (GUARDR entry) per guard window, no damage while guarding.
- Archer: mean shot distance 263/265, level aim pose (SHTIDLE/SHTOUT), 0/3 arrow hits in the window.
- Potion: the bread box breaks, the drop goes to the bag, and F3 heals 20 → 60 on both sides.
- Kill XP and level-up: CI01 (level 2, Experience 50) gives +100 on both sides. Level 2 at 1090, no stat change.
- Aggro reaction distances: BIC03 2040/2063, BIC02 2011/2079, Geffrey Oakham 448/426, CI01 752/790, CI02 753/785.

## Bugs

1. **Wall slide (CONFIRMED port bug; fixed in `src/game/physics_char.cpp`; regressions `game.wall_slide_mission` and `game.physics`).**
   - *Original:* `Phys_walk_step 0x6a9570` slides a blocked body along the navmesh edge at |v|·|dot|. If the slide move is accepted and |dot| > 0.707 (`0x9dd770`), it turns the unit yaw toward the slide direction by **+2°** (difference > 0) or **−1°** (otherwise) per call. For the player it also sets camera 0's target yaw to the new yaw (asm `0x6a9fc5..0x6aa1b5`; constants 180/360/−180/0.0 at `0x9cfa10/0x9cf9c8/0x9d3e28/0x9cf5c4`, deg→rad `0x9cf884`).
   - *Round 1–2 port:*
     - The edge slide runs at |v|·|dot|, with the dot taken against the previous accepted slide (the original keeps the slide as the body velocity `+0x10e`).
     - The turn is +2°/−1° through `Unit_set_yaw_deg` 0x50abd0, from the body yaw `+0x1a`.
     - Slide contacts are saved in the `PHYS` section.
   - *Round 3 root cause: the turn rate.*
     - The original trace turns exactly **+2° per frame**, at ~19 ms frames (115° → 182° in 0.65 s).
     - The port took one +2° step per 33 ms tick. Its second 20 ms substep runs on the stored slide and is not blocked.
     - So the heading was still 129° when a slight bend of the edge (x ≈ 14311) rejected the slide. The slide memory then expired, and the raw step met the edge at dot 0.63 < 0.707.
     - The turn stopped, and Joan stopped dead at the corner (13876, 14081).
   - *Fix:* the turn is taken once per tick, scaled by frame_ms / kSubstepMs. That is 1.65, so 3.3° per tick ≈ 100°/s, as measured.
   - *Not ported* (`ponytail:` note in the code):
     - `Poly_clip_circle_move` 0x671fb0.
     - The velocity-rotation fallback after a rejected slide (asm `0x6aa1c3`: ±0.01, ±0.21, … rad). Its step multiplier `ftol(1 unit / max axis step)` is 0 when running, so it only re-aims `+0x10e`.
2. **AI aim pose at the end of the draw (CONFIRMED port bug; fixed in `World::ai_reaim`, in `src/game/projectile_sim.cpp` and `world_sim.cpp`; regression in `game.projectile_arrow`).**
   - *Original:* `Unit_updateAction 0x7c7ab0` case 0x30 (SHTLOAD) re-solves the aim of an AI shooter with `Aim_solve_by_type 0x4945c0` (type 1; case 0x32: type 0). The solve runs from the ammo model's bone origin to the target's feet. It then picks SHTIDLE when dir.z < 0.1, else SHTIDLEH.
   - *Port before:* it kept `ai_shoot`'s feet-to-feet direction. With the target 40 units up the slope at 260, that gave z = 0.15 → SHTIDLEH/SHTOUTH, where the original archer used SHTIDLE/SHTOUT.
3. **Harness bug (port side): queued mouse chunks.** `Port.mouse` queued its chunks while `goto` re-aimed every 150 ms. The turns piled up, Joan orbited waypoints, and she never reached the potion box or the second English group. `Port.mouse` now blocks, like `rel()` on the original side.

## Remaining mismatches

Not port bugs, or not yet attributable:

- **Strafe left 79 vs 198 u/s** (78 in round 1 too, so the original is deterministic here; strafe right matches 237/240).
  - About 0.8 s into the move, Joan's strafe (heading 205°, yaw 115°) meets a navmesh wall at (14584, 13114) at ~88° (the port's debug trace shows a non-walkable target, not a step-up).
  - The original stops dead there (0 units in the last 0.8 s, yaw unchanged). The port slides along the wall at full speed and turns, because its slide dot is taken against the previous slide: the first contact slides at |v|·0.03, and every later one at |v|·1.
  - The original's slide speed is `|+0x10e|`·|dot|. Its body velocity apparently stays at the reduced slide here, but the boundary run keeps ~285 u/s along an edge met at 65° [INFERENCE]. Reconciling both needs the `+0x10e/+0x11e/+0xf4` refresh rules of the caller of 0x6a9570. Not root-caused.
- **Jean peak 1 s speed 371 vs 417 u/s** (12 %): Jean's catch-up run. His reaction and lag match.
- **Boundary end yaw 245 vs 262:** both end at (13527, 13022). On the last edge, the original takes −1° steps (the slide on the far side of the heading), where the port takes +3.3° steps.
- **Combo / block engagement:** enemy swing counts, Jean's damage list, and HP lost without guarding depend on the AI attack rolls (shared RNG, think stagger), not on the damage formula. Damage taken per hit is 4 5 8 on both sides.

## Round 2 (after the coordinator's follow-up)

**Damage taken from light infantry (3 vs 4): resolved, not a bug.**
- The traces now record morale, armor level and leader. Every input is identical on both sides:
  - Joan: morale 70, armor level 1, DEF 8.
  - CI/BIC soldiers: level 2, STR 5, morale 70, armor level 1, leader Geffrey Oakham (35) with LEAD 4.
- Formula 0x509950 with ELINFANT DAMAGE `A` = 3:
  - atk = (5·3+100)·3/100 · 0.909 (skill level 0) · 1.2 (morale) = 3.764
  - def = 120·1/100 = 1.2
  - dmg = 3.764/2 + (3.764−1.2)/2 = 3.16
  - × F_DEFEND 0.7 × leader 1.04 = 2.30 → HP loss **3**
- With `AA` (DAMAGE 4) the same steps give 3.22 → loss **4**.
- Both values are legitimate. The single run of round 1 happened to catch mostly `A` hits in the original and `AA` hits in the port. The round-2 original rerun shows 4–6 per sample (simultaneous hits merge).

**Level-up (XP hook: original `+0x126` and port `set … xp` both set to 990):**
- In the original, XP went 990 → 1040 after 58 s of fighting and Joan reached level 2. Her HP/EP/STR/DEX/DEF/LEAD stayed the same: level-up grants attribute points and does not raise stats.
- In the port, Joan gained **no XP** in 120 s of the same fight (stuck at 990).
- The original's +50 is consistent with a team/leader XP share (EXPUP 0x5229a0 shares MEM_EXP 0.4 with the team) or an assist on a kill.
- Candidate port difference in the XP path. Not root-caused in this round.

**Potion:** the box near the start (map item 2) was struck 8 times on both sides, but no HP item reached the bag on either side, so F3 healed 0 on both. Inconclusive: the box needs more hits or a different object, or the drop needs an explicit pickup (R).

**Combo on a passive target:**
- Not done. A target that never fights back would need a script/AI hook on both sides; the original has no such hook.
- Joan's A1 = 4 is confirmed again on both sides. Jean's hits include 7 and 9 on both sides.

**Archer range and hit rate:** not rerun in round 2. The round-1 values stand: shot from 255 vs 320, and the 12 s hit counts were mixed with melee.

## Round 3 (items 1–4 of round 2)

**1. XP: not a port XP bug.** Both sides' round-2 traces show kill XP only, credited only to the killer.
- Jean took all five camp kills on both sides, with identical credits: CI01/CI02/BIC02/BIC03 (level 2) → +100 = Experience 50 × 2; Geffrey Oakham (level 2) → +200. Joan is not Jean's leader (`ldr` 0), so she gets no share on either side.
- The original's +50 was Joan's own kill of "Unit 56" (level 1, Experience 50) at the second group (10600, 14100).
- The port's Joan never got there. After the camp she was equidistant (~2300) from Hastulphus Chaffar (northwest) and Bertram Braunston (south), so the harness's nearest-foe choice flipped every 300 ms. Queued mouse chunks (bug 3) made her orbit too.
- Fix: `levelup` now keeps its target (CI01 first) and sets it to 1 HP before Joan's swings. Both sides then credit **+100** (Unit_die 0x7c6240 → EXPUP 0x5229a0) and reach level 2 at 1090, with no stat change.
- `game.progression_level` already checks the formula (50 × level × m, minimum 10).

**2. Wall-slide corner: fixed** (bug 1). Original / port: end (13527, 13022) / (13527, 13022); 1335 / 1326 units after the edge; 182° / 184° along the first edge.

**3. Archer.**
- The "320" came from a mixed window. The round-1 traces already showed shots from 276/255 vs 263/257.
- In every run the archer first follows its leader Anger Greetham (11) into melee range, then backs off and shoots at ~255. Mission 1 has no isolated archer: 11/14/15 are one group.
- The metrics now separate the shots: mean shot distance 263/265, 0/3 arrow hits, 2/2 melee hits.
- The aim pose differed (bug 2). It is fixed, and the archer's actions are now identical.

**4. Potion.**
- Map item 2 by the start holds content 92 = ARROW (item.res order), not an HP item. That is why F3 healed 0 on both sides.
- Map item 1 (12527, 14822) holds 36 SMALL_BREAD (ItemClass POTION). One hit breaks it, Joan walks over the drop at full HP (it goes to the bag), HP is set to 20, and F3 restores 60 on both sides. No R pickup is needed.

**Harness changes:**
- `Port.mouse` blocks (bug 3).
- `levelup` keeps and finishes its target.
- `archer` reports per-shot metrics.
- `potion` uses the bread box.
- `aggro` counts the first run/walk/attack as the reaction. The port's CI01/CI02 reacted at 790/785 with a 100 ms RUN burst of < 30 units, which the old metric missed.
- Positions match within 50 units.
- `objects` prints res, stage and on-floor.

`ctest -R '^game\.'`: 37/37 pass (`build-DiffVerify3`).

## Limits

- One original run per scenario. AI and RNG-driven outcomes (aggro, combos, archer) need several runs before a mismatch counts.
- Original frame rate is ~30–60 fps on llvmpipe under host load. The port is a fixed 30 Hz.
