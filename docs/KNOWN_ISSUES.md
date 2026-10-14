# Known issues and limitations

Living list; each entry links the document holding evidence. Severity: **B** blocks Definition of Done, **D** deviation from the original, **M** minor.

## Original game facts that constrain the port
- Original `joan.exe` renders only 1024×768 regardless of `CONFIG.DAT` (Wine capture, [ORIGINAL_BEHAVIOR](reverse-engineering/ORIGINAL_BEHAVIOR.md)); all other-resolution behaviour of the port is native design, anchored on the confirmed 45° vertical FOV ([CAMERA_AND_WIDESCREEN](CAMERA_AND_WIDESCREEN.md)).
- Original RNG is seeded with `time(0)` and not saved, so original runs are not reproducible; the port seeds explicitly ([GAMEPLAY_IMPLEMENTATION](GAMEPLAY_IMPLEMENTATION.md)).
- Original saves are raw memory dumps: only header, flags, objectives and hero records are imported; mission restarts from its map ([SAVES_AND_CAMPAIGN](SAVES_AND_CAMPAIGN.md)).
- Trigger processing order in the original depends on a pointer-byte sort (effectively arbitrary); the port uses a fixed documented order.
- 15 texture names referenced by models are absent from the GOG install (e.g. `pinealpha`, `sky_night01..04`) ([MODELS_AND_ANIMATION](formats/MODELS_AND_ANIMATION.md)).

## Deviations (D)
- Fixed 33 ms logic tick; every unit processed every tick (original: CPU-budgeted round-robin passes) — [GAMEPLAY_IMPLEMENTATION](GAMEPLAY_IMPLEMENTATION.md).
- Player controller sampled per logic tick, not per drawn frame.
- Cosmetic randomness on a separate generator.
- Original stencil shadows are reproduced; units do not receive shadows (hypothesis) — [GRAPHICS_ROADMAP](GRAPHICS_ROADMAP.md).
- Sky uses a hand-matched gradient, not the original scattering sky shader.

## Open (B)
- End-to-end playable flow (menu → missions → transitions) in `joan-native`: in progress.
- Horses, carts, siege crews, ladders, gates, throw/interact, multi-section maps: in progress.
- 20v20 English vs French test battle is lopsided (20:1); under investigation (data vs parsing).
- HD/PBR presets and hardware ray tracing: in progress / not started.
- CI workflows never executed on GitHub; no public release published; GitHub repository name is a placeholder (`JOA_UPDATE_REPO`).

## Minor (M)
- `JOA_Encounter.wma` is truncated in the shipped file (last 1.2 s missing) — [AUDIO_AND_VIDEO](AUDIO_AND_VIDEO.md).
- VSetting.exe needs a registry key the GOG install does not create (irrelevant to the native port).
- GPU tests share one Xvfb display; run them with `ctest -j1`.
