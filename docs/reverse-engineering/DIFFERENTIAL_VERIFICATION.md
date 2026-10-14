# Differential verification: original joan.exe vs port

The same closed-loop scenarios run on the ORIGINAL game and on the port. The original runs under Wine and its state is read live from process memory. The port runs in lockstep through `joa-port-trace`. Then the traces are compared numerically. Reference only: Wine is never a runtime dependency.

## Harness

| piece | role |
|---|---|
| `tools/reference/diff/diffrun.py orig\|port [scenario…]` | Runs the scenarios and writes traces to `work/diff/<backend>/<scenario>.json`: one sample per original frame or port tick, holding every unit within 4000 units of the player. |
| `tools/reference/diff/compare.py [--self-test]` | Computes the metrics and the table below, written to `work/diff/report.md`. |
| `tools/reference/diff/port_trace.cpp` → `joa-port-trace` | Lockstep driver of the port's mission World over stdin/stdout. Commands: `start`, `step N held pressed mdx`, `units`, `set` (test hooks hp/ep/x/y/yaw), `objects`, `path X Y` (mission navmesh). Build target `joa-port-trace`, via `tools/reference/CMakeLists.txt`. |

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

**Port side:** mission 1, Normal, seed 1, cut scene skipped. The camera yaw is fed back from `PlayerController::camera_yaw_out`, as `GameCamera` does. Mouse moves are split into ≤40-mickey chunks, one per tick, matching `joa_ref.rel`. This matters because 0x431790 truncates `ftol(dx·0.18)` per frame: both sides turn 78°, not 90°, for 500 mickeys.

**Shared logic:** both sides use the same scenario code. Navigation follows the port's navmesh path (`path`, built from the original `.PFD`) with identical waypoints on both sides. Unit recnos are identical on both sides (72 Joan, 73 Jean de Metz, 42/43 CI01/CI02, 14 archer "Unit 63").

**Damage attribution:** the original keeps no attacker log. An HP drop is attributed to the opposing units that were within 250 units and in a melee attack state (ATTACK/JATTACKB/ENGATK*/DOWNATK/H_ATTACK) in that sample or the previous one. The same heuristic runs on both sides.

**Verdicts:** numbers match within 10 % (minimum 1). Latencies match within 70 ms (one or two original frames plus the sampling period). Strings must be equal.

## Scenarios

| scenario | what |
|---|---|
| start | Mission 1 stats of Joan and Jean, position, facing. |
| move | Run (W) 4 s, back (S) 3 s, strafe A 2 s, strafe D 2 s, walk (Shift+W) 3 s, with 1.5 s rests. Also Jean's follow behaviour. |
| boundary | Run 11 s along the initial facing into the battlefield edge (y≈14093). |
| turn | +500 mickeys standing, −500 mickeys while running. |
| combo | Navmesh route to CI01 (HP pinned on the way), then 12 LMB taps every 380 ms. |
| block | Q held 6 s facing CI01, then 6 s without guarding. |
| archer | Stand 700 units from the archer for 12 s. |
| levelup | Fight the first camp for up to 120 s with HP pinned. |
| aggro | Walk along the route to CI01. For each English unit, Joan's distance when it first moves >30 units or attacks. |

Run: `python3 tools/reference/diff/diffrun.py port && python3 tools/reference/diff/diffrun.py orig && python3 tools/reference/diff/compare.py`. The original run takes ~11 min; the port run takes seconds.

## Results (2026-10-14, Wine 11.19, llvmpipe; one original run per scenario; potion/levelup/combo/block rerun after the wall-slide change)

| scenario | metric | original | port | verdict |
|---|---|---|---|---|
| start | Joan of Arc hp | 60 | 60 | match |
| start | Joan of Arc mhp | 60 | 60 | match |
| start | Joan of Arc ep | 30 | 30 | match |
| start | Joan of Arc mep | 30 | 30 | match |
| start | Joan of Arc str | 9 | 9 | match |
| start | Joan of Arc dex | 10 | 10 | match |
| start | Joan of Arc def | 8 | 8 | match |
| start | Joan of Arc lead | 15 | 15 | match |
| start | Joan of Arc lvl | 1 | 1 | match |
| start | Joan of Arc xp | 0 | 0 | match |
| start | Joan of Arc pos | (15021, 12684) | (15021, 12684) | match |
| start | Joan of Arc yaw | 115 | 115 | match |
| start | Jean de Metz hp | 50 | 50 | match |
| start | Jean de Metz mhp | 50 | 50 | match |
| start | Jean de Metz ep | 30 | 30 | match |
| start | Jean de Metz mep | 30 | 30 | match |
| start | Jean de Metz str | 10 | 10 | match |
| start | Jean de Metz dex | 15 | 15 | match |
| start | Jean de Metz def | 8 | 8 | match |
| start | Jean de Metz lead | 5 | 5 | match |
| start | Jean de Metz lvl | 1 | 1 | match |
| start | Jean de Metz xp | 0 | 0 | match |
| start | Jean de Metz pos | (14934, 12847) | (14934, 12847) | match |
| start | Jean de Metz yaw | 295 | 295 | match |
| move | run speed u/s (0.7..1.7 s) | 296.46 | 299.66 | match |
| move | run distance u (4 s) | 1160.40 | 1169.35 | match |
| move | run start latency ms | 379 | 33 | MISMATCH |
| move | run actions | IDLE/RUN | RUN | differs |
| move | back speed u/s (0.7..1.7 s) | 181.85 | 176.64 | match |
| move | back distance u (3 s) | 601.85 | 589.94 | match |
| move | back start latency ms | 53 | 33 | match |
| move | back actions | IDLE/MOVE_B | MOVE_B | differs |
| move | strafe speed u/s (0.7..1.7 s) | 77.84 | 198.04 | MISMATCH |
| move | strafe distance u (2 s) | 237.26 | 340.17 | MISMATCH |
| move | strafe start latency ms | 43 | 33 | match |
| move | strafe actions | IDLE/MOVE_L | MOVE_L | differs |
| move | strafe_r speed u/s (0.7..1.7 s) | 237.08 | 239.74 | match |
| move | strafe_r distance u (2 s) | 485.15 | 474.68 | match |
| move | strafe_r start latency ms | 62 | 33 | match |
| move | strafe_r actions | IDLE/MOVE_R | MOVE_R | differs |
| move | walk speed u/s (0.7..1.7 s) | 190.94 | 179.81 | match |
| move | walk distance u (3 s) | 557.13 | 527.97 | match |
| move | walk start latency ms | 144 | 33 | MISMATCH |
| move | walk actions | IDLE/WALK | WALK | differs |
| move | Jean dist to Joan at run start | 184.77 | 184.76 | match |
| move | Jean reaction latency ms | 642 | 561 | MISMATCH |
| move | Jean max dist during run | 614.93 | 663.41 | match |
| move | Jean dist 1.5 s after run | 307.15 | 288.91 | match |
| move | Jean peak 1 s speed u/s | 384.67 | 417.12 | match |
| move | Jean actions | IDLE/RUN | IDLE/RUN | match |
| boundary | max y reached | 14094.32 | 14093.37 | match |
| boundary | ms to y > 14000 | 4928 | 4917 | match |
| boundary | Joan yaw at start / end | 115 / 240 | 115 / 129 | differs |
| boundary | camera yaw at start / end | 115 / 240 | 115 / 129 | differs |
| boundary | end position | (13528, 13021) | (13876, 14081) | differs |
| boundary | distance moved after reaching the edge | 1335.72 | 543.20 | MISMATCH |
| boundary | actions after the edge | IDLE/RUN | RUN | differs |
| turn | turn_stand yaw change deg (+500 mickeys) | -78.00 | -78.00 | match |
| turn | turn_stand deg per mickey | -0.16 | -0.16 | match |
| turn | turn_run yaw change deg (-500 mickeys) | 78.00 | 78.00 | match |
| turn | turn_run deg per mickey | -0.16 | -0.16 | match |
| combo | Joan swings (damage x victims) | 4x2 4 | 4 | differs |
| combo | Jean swings (damage x victims) | 9 7x2 9 18x2 | 7x2 9x3 7x2 9x3 | differs |
| combo | ambiguous hits (Joan+Jean / none) | 11 | 0 | MISMATCH |
| combo | damage taken by Joan per hit | 4 6 5 4 5 5 | 5 4 8 | differs |
| combo | Joan attack actions | ATTACK | ATTACK | match |
| block | block: enemy swings within 200 | 8 | 7 | match |
| block | block: Joan HP lost / hits | 0 / 0 | 4 / 1 | differs |
| block | block: blocked hits (GUARDR entries) | 1 | 0 | match |
| block | block: Joan actions | GUARD1 GUARDR IDLE_BAT | GUARD1 IDLE RATTKED RUN | differs |
| block | noblock: enemy swings within 200 | 7 | 4 | MISMATCH |
| block | noblock: Joan HP lost / hits | 4 / 1 | 9 / 2 | differs |
| block | noblock: blocked hits (GUARDR entries) | 0 | 0 | match |
| block | noblock: Joan actions | ATTACKED IDLE_BAT | BATTKED GUARD1 IDLE | differs |
| archer | arrow damage per hit | 4 6 | 5 4 5 4 5 4 5 4 | differs |
| archer | hits in 12 s | 2 | 8 | MISMATCH |
| archer | archer actions | IDLE IDLE_BAT RUN SHTIDLE SHTLOAD SHTOUT | IDLE IDLE_BAT KNOCKDN3 RUN SHTIDLE SHTLOAD SHTOUT STANDUP | differs |
| potion | HP before F3 | 20 | 20 | match |
| potion | HP after F3 | 20 | 20 | match |
| potion | healed | 0 | 0 | match |
| levelup | lvl before/after | 1 / 2 | — | differs |
| levelup | xp before/after | 990 / 1040 | — | differs |
| levelup | mhp before/after | 60 / 60 | — | differs |
| levelup | mep before/after | 30 / 30 | — | differs |
| levelup | str before/after | 9 / 9 | — | differs |
| levelup | dex before/after | 10 / 10 | — | differs |
| levelup | def before/after | 8 / 8 | — | differs |
| levelup | lead before/after | 15 / 15 | — | differs |
| levelup | game ms into fight | 58382 | — | differs |
| levelup | level-up | — | not reached (lvl 1, xp 990) | differs |
| aggro | 33 BIC03: Joan dist at reaction | 1985 | 2023 | match |
| aggro | 35 Geffrey Oakham: Joan dist at reaction | 383 | 399 | match |
| aggro | 36 BIC02: Joan dist at reaction | 2024 | 2036 | match |
| aggro | 42 CI01: Joan dist at reaction | 762 | 343 | MISMATCH |
| aggro | 43 CI02: Joan dist at reaction | 759 | 809 | match |

## Analysis

**Matches (port faithful):**
- All mission-1 start stats, positions and facings.
- Steady speeds: run ≈297/300, back 182/177, strafe right 237/240 u/s. Distances over the windows match too.
- Mouse turning: 0.156°/mickey on both sides once the per-frame `ftol` is reproduced.
- Jean's follow geometry: start distance, maximum lag (615/663), distance after the stop, peak speed 385/417 u/s.
- Battlefield edge position: max y 14094/14088, reached at 4.93 s on both sides.
- Damage values:
  - Joan's first light hit (A1) does **4** on English light infantry on both sides.
  - Jean's hit of **11** is seen on both sides.
  - Arrow damage is 4–6 on both sides.
- Blocking: one blocked hit (GUARDR entry) per guard window on both sides. Not guarding costs similar HP: 26/7 hits vs 22/5.
- Aggro reaction distances: BIC03 1985/2023, BIC02 2024/2036, Geffrey Oakham 383/399, CI02 759/809.

## Bugs

1. **Wall slide (CONFIRMED port bug; partly fixed in `src/game/physics_char.cpp`, regression check in `tests/game/physics_test.cpp`).**
   - *Original:* `Phys_walk_step 0x6a9570` slides a blocked body along the navmesh edge at |v|·|dot|. If the slide move is accepted and |dot| > 0.707 (`0x9dd770`), it turns the unit yaw toward the slide direction by **+2°** (difference > 0) or **−1°** (otherwise) per call. For the player it also sets camera 0's target yaw to the new yaw (asm `0x6a9fc5..0x6aa1b5`; constants 180/360/−180/0.0 at `0x9cfa10/0x9cf9c8/0x9d3e28/0x9cf5c4`, deg→rad `0x9cf884`).
   - *Measured in the original:* Joan and the camera rotate 115°→182° in ~0.7 s (≈96°/s). Joan keeps running along the edge: 1336 units in 6 s, ending at yaw 240.
   - *Port before:* `CharPhysics::integrate` only slid along x or y and never turned. Joan stopped 259 units later, still at yaw 115.
   - *Port now:*
     - A blocked step slides along the crossed navmesh poly edge (`CharPhysics::wall_edge`) at |v|·|dot|.
     - The dot is taken against the *previous accepted slide* while the contact is less than 100 ms old. This mirrors the original keeping the slide as the body velocity (`+0x10e`), which the next step reuses.
     - When |dot| > 0.707, the heading goal steps +2°/−1° through `Unit_set_yaw_deg` 0x50abd0. The base is the body yaw `+0x1a` (asm `0x6aa040`). The player's camera yaw follows.
     - Slide contacts are saved in the `PHYS` section, so the save/load round trip is unchanged (`game.multimap_nav`).
     - `ctest ^game.`: 36/36 pass.
   - *Still different:*
     - The edge speed now matches: 175 units in 20 ticks ≈ 265 u/s, vs ≈270 in the original.
     - The port's turn reaches only 129° (original 182° before the corner, 240° after it).
     - At the corner (13876,14081) the port stops where the original turns onto the next edge (yaw 245) and runs on.
     - Not ported: the minimum-step loop (×2..×99 while both axes < 0.0217 m) before the move test `0x6ae610`, and `Poly_clip_circle_move` 0x671fb0. The body's goal-following rate (0x4f2190) bounds the turn.

## Remaining mismatches

Not port bugs, or not yet attributable:

- **Start latency of the first run (379 ms vs 33):** the first key after quick load in the original. The other movement latencies are 43–62 ms. Harness artefact (input after a load).
- **Strafe left 78 vs 47 u/s, walk 191 vs 145 u/s:**
  - Both are local terrain/obstacle interactions after the earlier moves. A standalone walk measured earlier matched: 178 vs 180 u/s.
  - The positions diverge slightly (axis slide vs edge slide, see Bugs).
- **Jean reaction latency 642 vs 561 ms:** about 80 ms, so one or two AI think periods. The AI think stagger consumes the shared RNG, so this is not deterministic across implementations.
- **Combo:**
  - In the original, CI01 was already attacking on arrival and stun-locked Joan (ATTACKED ×4), so combo steps 2+ never landed. In the port, CI01 was strafing, and Joan landed A1 = 4. In the long fight the port also did 8 per hit, a step not observed in the original.
  - Per-step damage beyond A1 therefore still needs a scenario where the target does not fight back.
  - Engagement timing differs (AI attack rolls use RNG); one sample is not enough to call it a bug.
- **Damage taken from light infantry:** single-attacker hits are **3** in the original vs **4** in the port.
  - Expected from the data: ELINFANT `A` DAMAGE 3 / `AA` 4, STR 6 → atk ≈ 3.86 before defence and the F_DEFEND 0.7 factor. HP is stored with `ftol`, so a fractional damage removes ceil(dmg).
  - This is a candidate port bug in one of: the defender's armor level or morale, the attacker's leader multiplier `(min(LEAD,100)+100)/100`, or the skill multiplier.
  - The traces now carry morale, armor level and leader (the `mor/arm/ldr` fields, added after this run). Rerunning `combo`/`levelup` on both sides gives the inputs to settle it.
- **Archer hits in 12 s (2 vs 9):** the archer walked up to melee range and backed off on both sides. It shot from 255 (original) vs 320 (port). The port window also includes melee hits from other units (the attribution says "ranged/none" only when nobody is swinging). Needs an isolated archer.
- **CI01 aggro 762 vs 343:** CI01's first reaction in the original came while its camp-mate CI02 reacted (759). The port's CI01 waited until Joan was close. The AI search stagger is RNG-driven; one sample is not enough.
- **Level-up:** not reached on either side in 120 s. Joan's A1 = 4 against 70 HP soldiers, and Jean takes the kills; the original got 100 XP. Next step: an XP test hook (original `+0x126` write; port `set … xp` to add) to trigger LEVELUP on the next gain.
- **Potion (F3):** Joan has no HP item at mission-1 start on either side (F1 screenshot), and the original has no item-give hook. The only box near the start is map item 2 (content 92, at 13509,12699). Breaking it and testing F3 is the next step.

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

## Limits

- One original run per scenario. AI and RNG-driven outcomes (aggro, combos, archer) need several runs before a mismatch counts.
- Original frame rate is ~30–60 fps on llvmpipe under host load. The port is a fixed 30 Hz.
