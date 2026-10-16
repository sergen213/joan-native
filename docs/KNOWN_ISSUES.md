# Known issues and limitations

Living list; each entry links the document holding evidence. Severity: **B** blocks Definition of Done,
**D** deliberate/measured deviation from the original, **M** minor.

## Original game facts that constrain the port
- Original `joan.exe` renders only 1024×768 regardless of `CONFIG.DAT` (Wine capture, [ORIGINAL_BEHAVIOR](reverse-engineering/ORIGINAL_BEHAVIOR.md)); other resolutions are native design anchored on the confirmed 45° vertical FOV ([CAMERA_AND_WIDESCREEN](CAMERA_AND_WIDESCREEN.md)).
- Original RNG is seeded with `time(0)` and not saved, so original runs are not reproducible; the port seeds explicitly ([GAMEPLAY_IMPLEMENTATION](GAMEPLAY_IMPLEMENTATION.md)).
- Original saves are raw memory dumps: header, flags, objectives, heroes and bag layout are imported; the mission restarts from its map ([SAVES_AND_CAMPAIGN](SAVES_AND_CAMPAIGN.md)).
- Trigger processing order in the original depends on a pointer-byte sort (effectively arbitrary); the port uses a fixed documented order.
- 15 texture names referenced by models are absent from the GOG install (e.g. `pinealpha`, `sky_night01..04`) ([MODELS_AND_ANIMATION](formats/MODELS_AND_ANIMATION.md)).
- Throw is unreachable in the original; nation strength is written but never read — both intentionally not simulated ([GAMEPLAY_IMPLEMENTATION](GAMEPLAY_IMPLEMENTATION.md)).

## Deviations (D)
- Fixed 33 ms logic tick; every unit processed every tick (original: CPU-budgeted round-robin passes); controller sampled per tick.
- Cosmetic randomness on a separate generator (STOP_CC stays on the gameplay stream because it changes timing).
- Wall contact while strafing into a near-perpendicular navmesh wall: the port slides, the original stops (79 vs 198 u/s, [DIFFERENTIAL_VERIFICATION](reverse-engineering/DIFFERENTIAL_VERIFICATION.md)); `Poly_clip_circle_move` 0x671fb0 not ported.
- Point & Click mode keeps Esc live (dead in the original) so the menu stays reachable.
- Sky uses a matched gradient, not the original scattering sky shader; units do not receive stencil shadows (hypothesis).

## Open (B)
- **Campaign bot chain: Easy from mission 5.** The test bot wins all 8 missions legitimately in one CTest fixture chain through the Ending (missions 1–4 on Normal, 5–8 on Easy, a difficulty switch the original's options allow); it has not won S9L1 on Normal (stalls at Orleans / Meung's cannons), and no human play-through has been recorded ([SAVES_AND_CAMPAIGN](SAVES_AND_CAMPAIGN.md) §8.1 "Full legitimate chain").
- Differential verification against the running original covers mission 1 (stats, movement, damage, blocking, archers, potion, XP/level-up, aggro); later missions are verified statically only.

## Minor (M)
- v0.1.0-alpha.1 binaries report version `0.1.0` (pre-release suffix missing), so its updater never offers newer pre-releases; install 0.1.0-alpha.2 or later manually once.
- RT: blotchy reflections at 1 rpp, GI darker in enclosed spaces than the Original ambient ([RAYTRACING](RAYTRACING.md)); TAA (Ultra / `aa=taa`; off in Enhanced) ghosts and softens animated characters without per-object motion vectors; HD packs stored uncompressed ([HD_ASSETS](HD_ASSETS.md)).
- Mission brief: commander 3D figure and siege-weapon model not drawn; battleground camera not draggable ([UI_SYSTEM](reverse-engineering/UI_SYSTEM.md)).
- `JOA_Encounter.wma` is truncated in the shipped file (last 1.2 s missing) — [AUDIO_AND_VIDEO](AUDIO_AND_VIDEO.md).
- GPU tests share one Xvfb display; run them with `ctest -j1`.
