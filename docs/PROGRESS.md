# Progress

Status per roadmap phase (goal.txt §24). "Verified" means exercised by a command/test whose result is recorded in the
linked document; implementation, smoke tests and parity with the original are separate claims.

Last full verification (2026-10-15, RX 7900 XTX/RADV, CachyOS, `build/` RelWithDebInfo, `-DJOA_FFMPEG_PREFIX=work/tools/ffmpeg-prefix`):
`cmake --build build` clean; **181/181 CTests pass** — 102 non-GPU tests `ctest -j12`, 42 GPU tests (GL + Vulkan with
Khronos validation) `ctest -j1 -R '^engine\.(gl|vk)_'`, 7 `release.*` checks (dist, reproducible, appimage,
license_audit, install_flow, product_update, selftest), and the 30-test legitimate campaign fixture chain
`ctest -j4 -R 'campaign_(legit|S[5-9]L1_)'` (missions 1–8 won by the input-only test bot through the Ending event).

| Phase | Status | Evidence |
|---|---|---|
| 0 Existing projects | Done | [EXISTING_PROJECT_AUDIT](EXISTING_PROJECT_AUDIT.md) |
| 1 Community work | Done: FOV fix hooks byte-matched in GOG `joan.exe` (0x7674D1/0x7674DA); Camera Fix = NOP at 0x431893; HD Update v0.95 inventoried (not redistributable) | [EXISTING_MODS_AND_TOOLS](EXISTING_MODS_AND_TOOLS.md), [CAMERA_AND_WIDESCREEN](CAMERA_AND_WIDESCREEN.md) |
| 2 Source audit / engine ID | Done: Enlight in-house C++ engine, MSVC 6, D3D8 + DInput8 + DSound/OpenAL + WMF9; legal/asset policy | [SOURCE_AUDIT](SOURCE_AUDIT.md), [ASSET_RIGHTS](ASSET_RIGHTS.md), [ENGINE_IDENTIFICATION](ENGINE_IDENTIFICATION.md) |
| 3 Ghidra | Done: project + full decompile export (18,802 functions), ~1,100 named functions, rerunnable scripts `tools/ghidra/` | [REVERSE_ENGINEERING](REVERSE_ENGINEERING.md), [reverse-engineering/](reverse-engineering/) |
| Dynamic reference | Done: original run under Wine/Xvfb, 94 screenshots + videos, D3D8 params, file load order; differential original-vs-port harness with live memory reads (mission 1: stats, movement, damage, block, archers, potion, XP, aggro match; 2 port bugs fixed) | [ORIGINAL_BEHAVIOR](reverse-engineering/ORIGINAL_BEHAVIOR.md), [DIFFERENTIAL_VERIFICATION](reverse-engineering/DIFFERENTIAL_VERIFICATION.md) |
| 4 Assets / formats | Done: every format parsed and validated over all original files (meshes 1228+588, anims 357, DDS 4346, RES 35, maps, scripts, saves) | [ASSET_FORMATS](ASSET_FORMATS.md) |
| 5 Build infra | Done: CMake C++20, CTest, sanitizer option, own Ghidra/reference tooling | README |
| 6 Gameplay + OpenGL | Done: native World sim (combat, controller, root motion, items, progression), script VM (399/407 builtins; all 270 used), AI/troops/navmesh, mounts/siege/gates/multimap, interact; OpenGL original-look renderer | [GAMEPLAY_IMPLEMENTATION](GAMEPLAY_IMPLEMENTATION.md), [GRAPHICS_ROADMAP](GRAPHICS_ROADMAP.md) |
| 7 Camera / widescreen / input | Done: Hor+ preserving 45° vFOV (4:3…32:9, custom), 4:3 UI safe area + edge anchoring, original 55 bindings, remap, gamepad | [CAMERA_AND_WIDESCREEN](CAMERA_AND_WIDESCREEN.md), [PLAYING](PLAYING.md) |
| 8 Campaign progression | Verified by the test bot: one legitimate CTest chain wins missions 1–8 through the Ending (1–4 Normal, 5–8 Easy) with carried progression; scripted play-through menu → mission 1 → save/load → win → mission 2 in GL and VK | [SAVES_AND_CAMPAIGN](SAVES_AND_CAMPAIGN.md) §8.1 |
| 9 Vulkan raster | Done: Vulkan 1.3 backend, validation clean, GL/VK parity 46–60 dB PSNR | [GRAPHICS_ROADMAP](GRAPHICS_ROADMAP.md) |
| 10 HD + PBR | Done: Original (pixel-identical) / Enhanced / Ultra / Custom presets; HD pack overrides; Real-ESRGAN pack builder | [HD_ASSETS](HD_ASSETS.md) |
| 11 Ray tracing | Done (optional, Vulkan ray queries): reflections, soft shadows, DDGI-style GI, denoising, fallback | [RAYTRACING](RAYTRACING.md) |
| 12 Launcher + import | Done: Qt6 launcher, native importer (installed dir / GOG installer via innoextract), verify/repair, desktop entry | [LAUNCHER](LAUNCHER.md), [INSTALLER](INSTALLER.md) |
| 13 Signed updates | Done: Ed25519 manifests, hash checks, atomic switch, rollback, e2e tests; published at https://github.com/sergen213/joan-native (experimental channel) | [RELEASE_PROCESS](RELEASE_PROCESS.md) |
| 14 Release testing | Reproducible artifacts, AppImage, LGPL-only FFmpeg, license audit, install flow, product update; GitHub CI (build/test, sanitizers, reproducibility) and Release workflows green | [RELEASE_PROCESS](RELEASE_PROCESS.md), [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES.md) |

## Play it

```sh
./build/src/engine/joan-native --game-dir /path/to/original      # menus, campaign
./build/launcher/joa-launcher                                     # launcher (import, settings, updates)
```
Controls and CLI: [PLAYING](PLAYING.md).

## Next concrete engineering tasks
1. Mission 2+ legitimate wins (bot multi-hero play: Jean's explosive arrows, gate closing) or a recorded human play-through of the campaign.
2. Extend differential verification beyond mission 1 (siege, mounts, RTS).
3. Remaining fidelity gaps: [KNOWN_ISSUES](KNOWN_ISSUES.md).
