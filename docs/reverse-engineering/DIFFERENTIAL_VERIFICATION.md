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

## Round 4 (2026-10-15): missions 4/5, siege, horse, costs, AI reaction

**Harness additions:**
- **Any campaign mission in the original:** after the mission-1 cut scene, `Orig.jump()` writes Scenario.no `0xa58fa7` = N−1 and the Sys requested screen mode `0xa500c4` (Sys+0x3c) = 0xd. `ModeNextScenario_process 0x755d80` then runs `Battle_start_scenario(no+1, 1, 1)` and the loading screen, as CONTINUE does after a win (fresh profile). Escape skips the opening cut scene (flag `0xa596d4`) and closes the brief (detected by a stopped clock). Missions 4 and 5 load this way (577 / 2040 units). `Orig(mission=N)` / `Port(mission=N)`; `MISSION` maps scenarios to missions; `run_on(d, names)` runs on an open backend.
- **Teleport hook (original):** rewriting every heap copy of the unit's exact (x, y) float pair at once (unit +0x0e, model +0x490/+0x4e8/+0x1008, physics body at z+50, AI targets …), plus the same dz on the float after each copy. Writing only the unit / model / body copies leaves the body stuck. Right after an F6 quick load the body copy is not yet the exact pair, so the teleport reverts; `run_on` settles 1 s before a scenario starts. Port: `set REC x|y|z`.
- **Other hooks/reads:** item array `0xa460a0` is a vtable then a DynArrayB (body `0xa460a4+0x19`); gate/obstacle HP +0xc2/+0xc6, wall +0xc6/+0xca, gate state +0xd1. Item recnos equal the port's (mission 4: 134/160/161/165/191). Unit-type table `*0xa48d88` (stride 0x203, code +4, Gold +0x161, Resource +0x165). Scenario bit flags at step record +0x18a (`set_flag`). Gold `0xa596c4`. Yaw +0x1a (rad) / +0x1e (deg). Unit reads add +0x1e2 mount, +0x1e6 rider, +0x384/+0x388 crew vehicle/slot, +0x2a5/+0x2aa task. Game-over box → Restart Mission + new checkpoint.
- `joa-port-trace`: `watch`, `setitem`, `flag`, `gold`, `objects all` (+hp/max/state), `classes` (+gold/resource), `rts_move`, `bribe_cost`, `set … z`; U lines add mount rider vehicle slot task task_p1.

| scenario | what |
|---|---|
| m4start / m5start | All units (per-recno class, nation, level, stats), structures, gold, unit-type costs. |
| cannon | Joan next to the four French cannons of S4L1; hooks: scenario bit 27 (Jean freed) + abtower HP = max−1 → script bit 31, the cannons SHOOT the abtower, then the Tourelles gate. Structure HP every 250 ms for 100 s. |
| horse | Horse 89 + Joan moved to the road north of the start, horse yaw 90°; R mounts; W 4 s; W + −300 mickeys; English light infantryman put 90 units in front, 3 × LMB. |
| m4aggro | Joan moved to the navmesh waypoint ~2500 from the west crossbow group, walks in, stands 15 s (HP pinned). |

**Results (one original run each; mission 4 Normal, seed 1):**

| scenario | metric | original | port | verdict |
|---|---|---|---|---|
| m4start | units alive (nation 0 / 1 English / 2 French) | 561 (5 / 491 / 65) | 561 (5 / 491 / 65) | match |
| m4start | per-recno stat mismatches (561 units) | — | 0 after fix (47 before) | match |
| m4start | Tourelles gate / abtower / ab_gate / Camp Gate hp/max/state | 1500/1500/0, 1000/1000/0, −1/−1/0, 0/0/1 | same | match |
| m4start | gold | 100 | 100 | match |
| m4start | gold / resource: CANNON, BOMBARD, FAUCONNEAU, TREBUCHET, BATTERRAM, SIEGE_TOWER, siege CROSSBOW | 1500/1600, 2500/2500, 1000/1200, 2000/8000, 3000/5000, 2500/2500, 100/100 | same | match |
| m4start | recruit gold: FRENCH light inf / poleman / heavy inf / archer / crossbow; MERCENARY_LIGHT_INFANTRY; MER_EX_ARCHER | 100/100/200/200/250; 30; 80 | same | match |
| cannon | cannon shots (SHTOUT entries, 4 cannons, 100 s) | 60 | 63 | match |
| cannon | cannon / crew actions | IDLE IDLE_BAT SHTIDLE SHTLOAD SHTOUT / PUSH_L SHTIDLEC SHTLOADC SHTOUTC WALK | same (crew: no RUN) | match |
| cannon | cannon yaw after the abtower falls (turn to the Tourelles gate) | 283 287 289 281 | 283 287 289 282 | match |
| cannon | abtower first HP drop after trigger (rerun, idle host) | 5.4 s | 5.8 s | match |
| cannon | abtower HP per ball (rerun, every HP change polled ~5 ms) | 40 × 25 balls (21 changes, 4 of them two balls in one poll) | 40 per ball | match |
| cannon | abtower to 0 HP (rerun) | 45.0 s | 44.4 s | match |
| cannon | volley period (rerun) | 6.6 s | 6.4 s | match |
| cannon | first run (host loaded by a concurrent port run): first drop / to 0 / drop sizes | 8.4 s / 55.0 s / 19 20 40 60 80 | — | discarded |
| cannon | Tourelles gate after the wall falls (rerun) | −40 per ball, 2 hits per volley, 1340 at 60 s → 780 at 104 s | 1500 (no hit) | MISMATCH |
| cannon | crew attached when the player arrives (probe, 8 s) | 8/8 after ~5 s near the player (0 while the player is far: LOD) | 8/8 from the start | differs |
| horse | mount actions / ms to H_IDLE / horse | IDLE H_MNT_L H_IDLE / 936 / 89 | IDLE H_MNT_L H_IDLE / 957 / 89 | match |
| horse | horse speed 0.2–0.9 s (walk gear) | 196 u/s | 230 u/s | MISMATCH |
| horse | horse RUN after | 1082 ms | never (stays WALK) | MISMATCH |
| horse | horse speed 1.5–3.5 s / distance 4 s | 341 u/s / 1243 | 461 u/s / 1590 | MISMATCH |
| horse | rider actions | H_IDLE H_WALK H_RUN H_STOP_B | H_IDLE H_WALK | differs |
| horse | turn (−300 mickeys while moving) | 0° | 0° | match (turn not exercised: see below) |
| horse | H_ATTACK entries / damage per hit to the infantryman | 2 / 9 12 12 9 18 | 2 / 9 9 9 9 7 | differs |
| m4aggro | reacting English units / hits on Joan | 10 / 0 | 4 / 14 | MISMATCH |
| m4aggro | reaction distances (Edmund Bothe, Units 1/4/5) | 1881, 2112/2001/2234 | 3077, 3456/3360/3066 | MISMATCH |

**Bug fixed (CONFIRMED port bug; `src/game/unitclass_db.cpp`; regression in `game.progression_level`).** Every level-13 ENGLISH_CROSSBOW_MAN in mission 4 (47 units) had STR/DEX/DEF/max HP 32/35/32/57 in the original and 26/43/30/61 in the port. Its UPGRADE code has two rows in STD.SET; the port appended both lists, so the AI auto-upgrade (0x4a95a0) found HP+4 and ten DEX+2 unlocks. `UpgradeRes_find_by_code 0x531df0` returns the **first** row whose code matches (stricmp), so only the first list (the +1…+5 STR/DEX/DEF pairs) is used: 2+30 / 5+30 / 2+30, HP unchanged. After the fix all 561 mission-4 units match per recno.

**Remaining mismatches (not root-caused this round):**
- **Cannon vs wall: resolved, not a port bug.** The decompile gives the port's model: `ThrowItem_on_hit_object 0x4fbc90` case 3–5 (cannon / fauconneau / bombard balls) sends (0x40, object = hit item +6), (0x40, units, area) and (0x40, objects, area) at the contact point (`0x4e5d90`: a 500-unit ray along the body velocity onto the hit item's model). `Trigger_process 0x4ef090` delivers the object-area event to each modify item whose footprint holds the point (`ModItem_footprint_contains 0x41c5e0`: item +0x56 position, −yaw +0x62, ItemRes +0x24/+0x26 width/height, plus ftol(trigger radius +0x1f)), as `MapObjects::footprint` does. `Wall_on_event 0x54ffd0` has no duplicate filter, and `Wall_take_ranged 0x492b10` takes rangedBase·0.5 = 20 per event, so a ball that hits the wall takes 40. The first run's single 20s and its 55 s came from host load: a port scenario ran concurrently and the original's frame time jumped (real_dt clamps at 1000 ms). The rerun on an idle host polls every HP change: every ball takes 40, and the wall falls at 45.0 s (port 44.4 s). `game.siege_crew` (40 per ball) stands.
- **Tourelles gate:** after the wall falls, both sides turn to the gate (same yaws), but only the original's balls damage it (−40 per ball). The port's cannons fire 8–9 balls each at it from their places and none registers, also after MissionBot4's f_closed gate-mesh pose. MissionBot4 owns this.
- **Siege crew LOD:** the original attaches crews (+0x384/+0x388) only once the player is near; the port attaches at mission start.
- **Horse gear:** the port never switches the horse to RUN / the rider to H_RUN, yet moves at 461 u/s (10 m/s cap) after ~1 s; the original runs at ~341 u/s in RUN (host frame rate on llvmpipe is low, and the original's turn and gear code are per drawn frame, so part of this is frame-rate dependence [INFERENCE]). Walk-gear speed 196 vs 230.
- **Host load caveat:** the original's `horse` and `m4aggro` runs overlapped the same concurrent port run that distorted the first `cannon` run. Their mismatches (horse speeds, reaction distances) need an idle-host rerun before they count. Never run the port and the original at the same time.
- **Horse turn:** −300 mickeys turned neither horse (0° both); the mounted turn (`Ctrl_horse_turn 0x43dab0`, ≤ 6° per drawn frame) is not exercised by this sequence.
- **Mounted attack damage:** 9/12/18 vs 9/7: rider combo / charge triggers differ; one run each.
- **m4aggro:** the routes diverge (the original's Joan was not hit; the port's took 14 hits of 9–13 and the group reacted from ~3000–3450 vs ~1900–2200). Not attributable without a second run.
- **Not measured this round:** RTS move/attack orders and formation spacing (mission 5: needs the RTS box-select/RMB click sequence; `rts_move` port hook is in place), bribe cost (mission 7 only; port hook `bribe_cost` in place), live recruit/workshop purchases (prices compared from the type table instead), mission-5 battle sample (`m5start` port trace only).

## Round 5 (2026-10-15): arrow collision shape (ArrowHit)

**Recipe (original, mission 4 Normal; scratch driver in `work/ArrowHit/`, run from a kernel session, not a diffrun scenario yet):**
- `Orig(mission=4)`, F6 checkpoint.
- Joan's ammo slot (unit +0x1c1 → inventory +0x0e → instance: +0 item id, +2 count) is rewritten to ARROW (item 92) × 99 and refilled before every shot. A slot that reaches 0 is freed; reload F6 then.
- Every unit within 3500 of the site is killed with HP 0, except Joan, the target and La Hire (teleported away).
- Joan stands at (18750, 3400, −1.41) on the flat road (port terrain spread 4 over 1500). The target, ENGLISH_ARCHER rec 8, is placed 300/600/900/1200 along +y.
- Shooting: range mode Z, yaw aimed at the target by mouse (lock = unit +0x2db == target), LMB.
- Arrows are tracked live:
  - ThrowItem manager `0xa48500` (DynArrayB: +0x0c count, +0x19 body). Record +0x04 body id, +0x08 item, +0x0c kind, +0x26 hit recno, +0x2a shooter.
  - Bodies `0xa570a8+0xb20` (DynArrayB of pointers). Body +0x1f6 position (m), +0x142 velocity.
- The target is pinned by a 2 ms writer thread: unit +0x0e, body +0x1f6 (and +0x142 = 0), and the actor node +0x8c → local +0x30 and world +0x88 translations.
- Lateral offsets: moving the pinned target sideways right after the ThrowItem appears makes the already-solved arrow pass at a known offset.
- Heights: changing the target type's Height (+0x109, type table `*0xa48d88`, stride 0x203) moves the aim point (0.8·Height) and the head-height test together, while the cylinder stays 80 high. Offsetting the target's own z does not hold: the physics snaps it back within the frame.

**Decode (GAMEPLAY_CHARACTER_COMBAT.md Part D §3.2, §3.4):** an arrow is a sphere of radius 10 (live: arrow actor shape 1, r 10, scale 1). A unit is a cylinder of radius 15 and height 80, centred 40 above the feet (live: actor shape 3, (0, 0, 40, 15, 80)). They are tested with 0x699a20 once per frame from the frame-start position, so arrows hit within lateral 25 of the axis and up to 90 above the feet.

| test (original) | arrow at the target | result |
|---|---|---|
| lateral offset (300, 3 shots each) | 21.0 / 22.5–23.0 / 24.0–24.5 | 9/9 hits |
| lateral offset | 25.0, 27, 30, 34 | 0/10 |
| aim height 0.8·H, H = 105 / 110 / 113 / 116 | ~84 / 88 / 90 / ~91 at the axis | 12/12 hits |
| H = 120 | 91.3 at 25 before the axis, 95.5 at the axis | 0/3 |
| natural, 300 / 600 / 900 / 1200 (6 each) | through the axis at ~74–83 | 5 / 5 / 6 / 5 (the miss traced was a KNOCKDN3 target, which 0x68de40 skips) |
| target floated 150 up, 900 (pitch-locked) | | 1/1 hit (the hit unsticks the body, so later shots are invalid) |

**Bug fixed (CONFIRMED port bug; `World::integrate_projectiles` + `sweep_sphere_cylinder` in `src/game/projectile_sim.cpp`, `UnitClass::cyl_center_z`, `GameData::item_sphere`; regression in `game.projectile_arrow`).**
- *Before:* the port tested a point arrow against the cylinder (lateral 15, top 80) at every sub-step, and stopped the body at the closest point.
- *Now:* the projectile's sphere (arrows r 10, other ammo its model's bounding sphere) is swept against each unit's cylinder with 0x699a20 (side, caps, rims), once per frame from the frame-start position, as 0x6a8060 does. On a hit the body stays at the frame start, as in the original.
- *Port check* (`work/ArrowHit/arrows.cpp`: same site, player input path, target pinned), collisions / shots after the fix:
  - lateral offset 0, 12, 18, 22, 24: 10/10 each;
  - lateral offset 26, 28: 0/10.
- *Before the fix, by HP drop:* 15 / 16 / 0 / 0 of 20 at 300 / 600 / 900 / 1200 with a 1.5 s window (MissionBot8b's ground.cpp: 15/28). The zeros at 900+ came from the window being too short: with 3.6 s it was 19–20 / 20.
- *After the fix, by HP drop:* 20 / 19 / 19 / 19 of 20.
- *Untested before the fix:* the lateral and height runs. The old point test misses beyond lateral 15 and above 80 by construction.

**Launch point (decoded and implemented, not measured):** `World::launch_point` replaces the 0.75 × cylinder-height HYPOTHESIS, which is kept only where no skeleton track exists. It is the sum of:
- the origin of the ammo's AttachBoneName bone in the shooter's current clip pose;
- for CANNON/FAUCONNEAU, the muzzle offset (forward 68.695, left 12, up 10.122);
- for AI crossbowmen, the offset (5.337, 0, 7.5).

**Not done:**
- S4L1 cannons vs the Tourelles gate, port re-measured after the launch-point change (`work/ArrowHit/tourelles.cpp`; MissionBot4's `work/MissionBot4/siege.cpp` agrees): 0 gate hits. **Resolved in round 6.**
  - Cannon 1 and Cannon 4: their balls end on a terrain ridge about 700 in front of the cannons (port terrain 174 at (21765, 25911); ball z 141–147 there). With 4 cannons, this is consistent with the original's "2 hits per volley".
  - Cannon 2 and Cannon 3: their balls cross the gate plane at gate-local z ≈ 114, in a gap between the portcullis bars of the posed Door.AM1 mesh, and land behind the gate.
  - The cadence is not the cause. 0x693750 (Part D §3.4) is a ray from the frame-start position along the frame-start velocity, recast every FT ms, as in the port, and its primitive colliders only act once +0x26 is set.
  - What remains is the mesh the ray hits: 0x5d4a80 tests the gate actor's collision mesh (actor +0x90), whose pose and source the port does not reproduce. (Round 6: the ray never hits the gate; its CollisionType box stops the ball in Physics_sweep_actors.)
- A target behind a window or crenel: the tower-archer missions were not loaded in the original.

## Round 6 (2026-10-15): gate collision geometry (GateMesh)

**Recipe (original, mission 4 Normal; kernel session on `Orig(mission=4)`, scratch in `work/GateMesh/`):** Joan teleported next to Cannon 1's crew (as `cannon`); the crews attach (8/8). Scenario bit 27 + abtower HP 999; after the abtower's first HP drop its HP is set to 1, so the cannons turn on the Tourelles gate one volley later. A 3 ms poller follows every cannon ThrowItem (manager `0xa48500`: +0x26 hit id, +0x5e/+0x62 0x693750's predicted time / item; body `0xa570a8+0xb20`, +0x1f6 position).

**Reads:**
- Gate item 160 → actor (item +0x86, the 0x10c Actor of `Item_create_actor 0x42f210`): +8 = 1 (item body), +0xc = item, +0x5c = body 0x1da, **+0xb8 = 2 (Box3d), +0xcc..+0xe0 = (−15, −200, 0)..(15, 200, 265)** = ItemRes CollisionType (`Item_setup_collision 0x41e850`); node +0x8c world matrix yaw 160°, translation (22230, 23920, 0).
- Actor +0x90 = mesh instance of `0x5cbbf0(model mesh, 1)`: format 1, 2 nodes (identity), +0xb = 1 (shares the model mesh +0xc: 193 vertices, 212 triangles, all material 0). Every vertex position and normal is **0xEEEEEEEE** (uv and colour f0f0f0ff are set), before and after the gate is on screen. Same for ab_gate, Camp Gate (2102 vertices), AM1 crates and tents. SMO items hold real positions (abtower `abtower_ext.SMO` 3924 / `abtowera.SMO` 3528 triangles, "Tourelles Castle" `town.SMO` 3707): the port's parse matches them to 0.0005 (`orig_*.npz` vs `meshdump`). So 0x5d4a80 can never hit an AM1 item.
- Items with a body (actor +0x5c ≠ 0): 96 in S4L1, all shape 2 boxes: the gates 160/161, 81 BOX crates, 13 tents. They are exactly the port's items with an ItemRes CollisionType, same recnos and box values.

| test (original) | result |
|---|---|
| cannon balls per volley after the abtower falls | Cannon 2 (341) and Cannon 3 (444): +0x26 = 160 (the gate), −40 each; Cannon 1 (344) / Cannon 4 (443): ground at gate-local ≈ (1220, −1810) / (1280, −2070), z ≈ 70–100 (the rise in front of them) |
| 0x693750's prediction for the gate-hitting balls | +0x62 = 5062 ("Tourelles Castle", TERRAIN, `town.SMO`), not the gate: the ball meets the gate's box first (Physics_sweep_actors, item pass) |
| gate actor +0xb8 set to 0 live (box off), 30 s | balls of Cannon 2/3 fly on and stop on the castle mesh behind the gate (gate-local ≈ (−72..−87, 95..107, 124), +0x26 = 5062); the gate still loses 40 per ball through the (0x40, objects, area) event |
| box restored | gate hits resume |

**Port change:** `MapObjects::sweep_items` (World::item_sweep) = Physics_sweep_actors' item pass with Sphere / Box3d (`sweep_sphere_box` = 0x697140, quirk included) / Cylinder primitives; projectile hits on it post 0x4fbc90's events (0x514c40's item branch). `MapObjects::mesh` gives AM1 models no triangles (the f_closed pose is gone). The bot's own-gate test (`clear_shot`) uses `sweep_items`.

| metric (S4L1, cannons vs Tourelles gate) | original | port | verdict |
|---|---|---|---|
| cannon yaws after the abtower falls (344 / 341 / 444 / 443) | 283 287 289 281 | 283 287 289 281 | match |
| gate hits / ball | Cannon 2 and 3 every ball, Cannon 1 and 4 none | same (60 s: 9 + 9 balls, gate 1500 → 780) | match |
| gate HP per ball | −40 | −40 | match |
| Cannon 1 / 4 ball end | ground ≈ (1220, −1810) / (1280, −2070) gate-local | ground (1166, −1785, 141) / (1211, −1959, 114) | match |

Regression: `game.siege_crew` `cannon_gate_part` (the gate's mesh is no ray target, a sphere meets its box face; the volley test above).

**Found, not changed:** TERRAIN scenery is not in the port's item array (`mission_objects.cpp` header), so the port's 0x693750 never tests it, while the original's does (the castle stopped the balls with the gate box off). With the gate box in place this does not change the S4L1 result. Also: the port's crews attach only near the player as in the original (the round 4 "8/8 from the start" no longer holds); with Joan far away only Cannon 1 is fully crewed.

## Limits

- One original run per scenario. AI and RNG-driven outcomes (aggro, combos, archer) need several runs before a mismatch counts.
- Original frame rate is ~30–60 fps on llvmpipe under host load. The port is a fixed 30 Hz.
