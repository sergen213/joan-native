# Existing Project Audit

This audit covers six earlier game-preservation projects by the same developer. For each one it records what can be reused in the Joan of Arc (JoA) Linux port.

**Method**
- All six directories were inspected read-only (`read`/`grep`/`glob`).
- Every path cited in the reuse plan was checked to exist with `wc -l` / `test -e` on 2026-10-13: 98 spot-checked paths, none missing.
- Git state was taken from `git -C <dir> log --oneline -3` and `git status --short | wc -l`.
- No builds or tests of the prior projects were re-run.

**Status labels**
- **EVIDENCED:** backed by a file on disk, such as a CTest `LastTest.log`, a release artifact or a git log.
- **CLAIMED:** stated only in that project's docs.

**JoA context.** JoA is an *installed* GOG directory (Inno uninstaller `unins000.*`, `goggame-1421210170.hashdb`) using D3D8/DInput8/DSound/OpenAL. The importer source is therefore an installed tree, with the GOG offline `setup_*.exe` handled through innoextract as an optional second path.

## 1. Summary table

| Project | Status | Relevant tools | Useful patterns | Verified reusable components (exact paths) |
|---|---|---|---|---|
| **Kabus 22**<br>`/mnt/1st4TB/Kabus 22`<br>Acknex A6 | **EVIDENCED** tests and releases:<br>• Native CTest 11/11 pass (`build-integration/Testing/Temporary/LastTest.log`, Oct 10), except `reference_comparison`.<br>• Installer CTest 5/5 (Oct 11).<br>• `release-0.2.0`…`release-0.4.4` exist, each with an AppImage, signed manifest and SHA256SUMS.<br>• Git HEAD `f6cab95 release: 0.4.4`, clean tree. | • Ghidra headless Java exporter<br>• Wine reference capture + SSIM compare<br>• Texture upscaler<br>• AppImage packager<br>• CI and release workflows<br>• Ed25519 signer | • Versioned `versions/<v>` + `current` symlink installs<br>• Verify-before-parse update manifests<br>• Fixture-gated CTest<br>• SPIR-V embed<br>• Hor+ camera policy | `installer/update.{hpp,cpp}`, `installer/updater.cpp`, `installer/layout.{hpp,cpp}`, `tools/release/sign_manifest.py`, `tools/release/make_update_manifest.py`, `installer/check_updater_e2e.py`, `tools/package_kabus_appimage.py`, `.github/workflows/{ci,release}.yml`, `tools/wine-reference/{wineref,compare}.py`, `tools/texture-upscaler/upscale.py`, `native/texture_pack.cpp`, `native/render_vulkan_rt.cpp`, `native/render_policy.hpp`, `reverse/scripts/RecoverKabus.java`, `docs/RELEASE_PROCESS.md`, `docs/WIDESCREEN_IMPLEMENTATION.md` |
| **etiyami**<br>`/mnt/1st4TB/etiyami`<br>ETI Yami | **EVIDENCED:**<br>• 17 CTests registered in CMake.<br>• Git HEAD `ee07a4e` (v1.1.1 proof), but **36 uncommitted files** (FSR/frame-gen work labelled "UNRELEASED").<br><br>**CLAIMED:** public v1.1.1 on GitHub (`sergen213/etiyami`), RT verified on an RX 7900 XTX. | • Ghidra `RecoverEngine.java` / `TraceStartup.java`<br>• SHA-guarded reference exe patcher<br>• AppImage packager with pinned uruntime<br>• Multi-arch release workflow | • SDL-loaded Vulkan via X-macro dispatch<br>• Optional RT feature gating<br>• Widescreen approach: keep half-height FOV, anchor HUD, fit menus to 1024×768<br>• Journaled update transaction | `native/vulkan_api.hpp`, `native/vulkan_renderer.cpp:242-354`, `native/vulkan_effects.{hpp,cpp}`, `native/renderer.hpp:17-18`, `tools/package_appimage.py`, `.github/workflows/release.yml`, `prepare_reference.py`, `analysis/TraceStartup.java`, `native/update_install.cpp`, `third_party/fidelityfx-sdk/` (MIT, patched) |
| **Pusu: Uyanış**<br>`/mnt/1st4TB/Pusu: Uyanış` | **EVIDENCED:**<br>• `.work/github-publication/bugfix-release/completed-proof.json` records 9/9 CTest and v0.1.1-linux published 2026-10-08.<br>• Not a git repo locally.<br><br>**Open issue:** flicker/noise. | • `PusuEvidence.java` (Ghidra)<br>• AppImage builder + license bundler<br>• Bubblewrap/Xvfb Wine diagnosis harness | • `embed_spirv.cmake`<br>• 4:3 reference frustum → Hor+<br>• MSI/CAB importer with staged publish | `cmake/embed_spirv.cmake`, `src/renderer_vulkan.cpp:30-300`, `src/renderer_vulkan_rt.cpp:80-230`, `src/renderer.cpp:485-488`, `src/importer.cpp`, `packaging/build_appimage.py`, `packaging/license_bundle.py`, `.work/analysis/PusuEvidence.java`, `.work/diagnosis/run_diagnosis.py`. **Licensed GPL-3.0-only.** |
| **IKV**<br>`/mnt/2nd4TB/IKV`<br>online MMO | **EVIDENCED:**<br>• 18 CTests; the last full run recorded a `world-state` failure (`build-full/Testing/Temporary/LastTestsFailed.log`), contradicting the README's "14/14".<br>• `dist/manifest.{json,sig}` + 0.7.0 tarball.<br>• Git HEAD `acdaf36` (0.7.0), clean tree. | • 5 Ghidra headless scripts<br>• `pe_inventory.py`<br>• AppImage packager in a pinned Docker image<br>• Local `release.py`<br>• Update integration test with a fake GitHub TLS origin | • Abstract `Renderer` interface with GL/VK factories<br>• `file(CONFIGURE)` baking the trusted public key<br>• Sequence/replay floor in updates<br>• Native-ELF innoextract check | `include/ikv/renderer.hpp:38-303`, `src/renderer_gl.cpp`, `src/vulkan_rt.cpp`, `shaders/vulkan/{rt_lighting.glsl,temporal.frag}`, `include/ikv/updates.hpp`, `src/launcher_updates.cpp`, `tools/sign_manifest.py`, `tools/release.py`, `tests/update_integration.py`, `src/launcher_extraction.cpp`, `src/launcher_assets.cpp`, `tools/package_appimage.py`, `tools/appimage/Dockerfile.ubuntu24`, `packaging/NOTICE.txt`, `tools/ghidra/{ExportEvidence,GraphicsEvidence,AssetReturnTypes}.java`, `tools/pe_inventory.py` |
| **CitroenC4Robot**<br>`/mnt/2nd4TB/CitroenC4Robot`<br>Acknex | **EVIDENCED:**<br>• 28 `add_test` calls (13 asset-gated).<br>• The sanitized tree's last failures: `original-material-behavior`, `launcher-smoke`.<br>• Signed-package receipts in `work/verification/`.<br>• No published release (KNOWN_ISSUES I-12).<br>• Not a git repo. | • `tools/ghidra/analyze.py` (headless driver)<br>• `ExportEvidence.java`<br>• `observe_original.py` (Wine/Xvfb observer)<br>• `import_assets.py` (innoextract + SHA-256) | • **Qt6 launcher**: QProcess children, QtConcurrent validation, `--smoke` offscreen, scripted integration<br>• **Separate Qt6 updater** (OpenSSL Ed25519)<br>• SHA-pinned CI with reproducibility `cmp`<br>• glslc DEPFILE rule | `launcher/src/{main,hardware,quality_controls,assets,integration}.cpp`, `updater/src/main.cpp`, `updater/tests/safety.py`, `.github/workflows/native.yml`, `packaging/appimage/{build,acquire}.py`, `packaging/appimage/citroen-c4.desktop`, `packaging/package_native.py`, `tools/ghidra/{analyze.py,ExportEvidence.java}`, `tools/observe_original.py`, `tools/import_assets.py`, `src/game/CameraProjection.hpp`, `src/rendering/vulkan/{VulkanDispatch.{hpp,cpp},raytracing.cpp,CMakeLists.txt}`, `src/assets/GpuImageBc.cpp`, `src/platform/runtime.{hpp,cpp}` |
| **SuperCan**<br>`/mnt/2nd4TB/SuperCan`<br>A8 Acknex + PhysX | **EVIDENCED:**<br>• 30 CTests pass (`build-release/Testing/Temporary/LastTest.log`, Oct 9).<br>• Not playable: no Vulkan, launcher, updater or CI.<br>• Not a git repo. | • Ghidra `ExportEvidence.java` with reviewed-function merge<br>• `RepairInlineConstants.java`<br>• Wine probe sandbox (bwrap) + `live_observer.py` (Xvfb/XTest/ffmpeg)<br>• SDL3/GL scene inspector | • Inode-pinned staging + `renameat2(RENAME_NOREPLACE)` importer<br>• Skip-77 tests<br>• Float-determinism flags<br>• D3D9 fixed-function on GL | `src/assets/main.cpp`, `docs/INSTALLER_ARCHITECTURE.md`, `tools/ghidra/{ExportEvidence,ExportVtables,RepairInlineConstants}.java`, `docs/REVERSE_ENGINEERING.md:21-110`, `src/render/d3d9_device.{hpp,cpp}` (unbuilt), `src/assets/d3d9_glsl.{hpp,cpp}`, `tools/asset-inspector/scene.cpp`, `analysis/reference/live_observer.py` (gitignored), `CMakeLists.txt:151-174` |

**Licensing across the set**
- Only Pusu has a root `LICENSE`, and it is GPL-3.0.
- Kabus 22, etiyami, IKV, CitroenC4Robot and SuperCan have no root LICENSE or COPYING file. Kabus 22 README:258 says "No project-wide license has been assigned."
- Because all six projects have the same author, code can be relicensed into JoA. That decision belongs to the developer and must be written down before any copying (see §3, Licensing gate).

## 2. Per-project sections

### 2.1 Kabus 22: `/mnt/1st4TB/Kabus 22`
- **Exists:** yes. A git repo; HEAD `f6cab95 release: 0.4.4`; 0 uncommitted files.
- **Stack:**
  - C++20 engine in `native/`, about 47 files, plus 23 GLSL shaders.
  - GTK3 installer, launcher and updater in `installer/`, built as a separate CMake project.
  - Python tools; Ghidra Java scripts in `reverse/scripts/`.
  - SDL2, not SDL3 (documented deviation).
  - libepoxy GL, Vulkan raster + RT, FSR 3.1.4.
  - ODE 0.16.6 via `cmake/KabusODE*.cmake`.
- **Status:**
  - **EVIDENCED:** native CTest 11 tests (`gameplay_scenarios` 29 scenarios, `level_sweep`) pass. `reference_comparison` is in `LastTestsFailed.log` with a 0 s cost, which suggests it never launched [INFERENCE].
  - **EVIDENCED:** installer CTest 5/5, including `updater_end_to_end`.
  - **EVIDENCED:** release dirs 0.2.0–0.4.4 each contain an AppImage, `kabus22-update.json` + `.sig`, `SHA256SUMS` and a sources tarball.
  - **CLAIMED:** 101/101 levels, 27 original saves load.
  - Releases are built and signed locally ("CI cannot sign"). `docs/KNOWN_ISSUES.md` is stale about 0.2.0 publication.
- **Reusable components**
  - **Updater and signing:**
    - `installer/update.{hpp,cpp}`: GitHub releases API, semver, stable/experimental channels, signature checked before parsing, glibc gate, hardened tar extraction.
    - `installer/updater.cpp`: `--check/--apply/--rollback/--status`.
    - `installer/layout.{hpp,cpp}`: `versions/<v>` + atomic `current` symlink + `.state`.
    - `tools/release/sign_manifest.py`: Python `cryptography` Ed25519 keygen, public key and signing.
    - `tools/release/make_update_manifest.py`.
    - The public key is baked in with `-DK22_UPDATE_PUBLIC_KEY`.
    - Spec, including key rotation: `docs/RELEASE_PROCESS.md`.
    - Tests: `installer/check_update.cpp` and `installer/check_updater_e2e.py`, the latter using a test-only build that allows `http://127.0.0.1`.
  - **CI:**
    - `.github/workflows/ci.yml`: Ubuntu 24.04, two CMake trees, CTest, packager self-check.
    - `.github/workflows/release.yml`: on a `v*` tag, checks the tag against both CMake versions, checks the secret matches the embedded public key, fetches innoextract and uruntime pinned by SHA, builds in Docker, signs, creates a draft release with `gh`.
  - **Packaging:**
    - `tools/package_kabus_appimage.py`: library closure, notices plus corresponding sources, `.desktop` template at line 81, `--self-check`.
    - `tools/Dockerfile.ubuntu24`.
    - `packaging/runtime-uruntime/`, `packaging/helper-innoextract/`, `packaging/NOTICE.txt`.
  - **Wine reference and visual compare:**
    - `tools/wine-reference/wineref.py`: private prefix, XTest keysyms, `x11grab`.
    - `tools/wine-reference/compare.py`: ffmpeg SSIM with crop, masks and thresholds; writes original|native|diff montages.
    - Thresholds: `tests/reference/thresholds.tsv`.
  - **Texture upscaling:**
    - `tools/texture-upscaler/upscale.py`: Scale2x for UI, Lanczos or optional realesrgan-ncnn-vulkan for world textures, writes a hash-keyed pack; tested by `test_upscale.py`.
    - `native/texture_pack.cpp`: runtime loader for the pack.
  - **Vulkan and RT:**
    - `native/vulkan_loader.cpp`: dlopen dispatch.
    - `native/render_vulkan_rt.cpp`: acceleration structures; ray-query shadows, GI and reflections.
    - `native/shaders/{ray_query.glsl,rt_signals.comp,rt_filter.comp,pbr.glsl}`.
    - `native/fsr_vk.cpp`.
    - Design doc: `docs/RAYTRACING.md`.
  - **Camera and widescreen:**
    - `native/render_policy.hpp` `camera_matrix`: Hor+ mode with a 4:3 `referenceAspect`.
    - Methodology docs: `docs/CAMERA_SYSTEM.md`, `docs/WIDESCREEN_IMPLEMENTATION.md`.
  - **Installer:**
    - `installer/setup.{hpp,cpp}`: content-based source detection (ISO, setup.exe+bin, *installed Windows folder*), innoextract helper, `.kabus22-inventory.json`, verify/repair with staged atomic renames.
    - `installer/check_setup.cpp`: security boundary tests.
  - **Controller:** `native/controller.cpp` (SDL2 GameController) and `tools/virtual-gamepad/vpad.py` (uinput pad for live tests).
  - **Ghidra:**
    - `reverse/scripts/RecoverKabus.java`: symbols, strings, xrefs, decompile. Run with `analyzeHeadless … -noanalysis -readOnly -postScript RecoverKabus.java OUT 120 20` (README:238-250).
    - `reverse/scripts/ExportEmbeddedPcx.java`.
- **Blockers and failed approaches** (`docs/KNOWN_ISSUES.md`):
  - Wine/wined3d reference reaches only SSIM 0.69–0.77 on static scenes because of filtering differences (not investigated further).
  - Wine ignores `SetGammaRamp` when windowed (`--ref-gamma 30` workaround).
  - Vulkan timing under Xvfb is meaningless (llvmpipe), so a headless gamescope is used.
  - RT has a 1024-sampler ceiling.
  - No crash reporter.
  - Requires glibc ≥ 2.38.
  - A legacy flat install migrates under the updater's version (`docs/RELEASE_PROCESS.md`).
- **Licenses:**
  - No root license.
  - `third_party/FidelityFX-SDK/` (MIT).
  - ODE (BSD; `third_party/ode-LICENSE-BSD.TXT`).
  - uruntime (MIT; bundled squashfuse is GPL/LGPL).
  - innoextract (zlib).
  - FFmpeg is LGPL.
  - `native/assets.cpp` contains MIT libgamestudio code; keep its header if reused.
- **Do not copy:**
  - The WDL interpreter, `native_calls.inc`, A6 address contracts, `native/assets.cpp` formats, `tests/scenarios/*.k22s`, `tests/levels/levels.tsv`.
  - `reverse/` exports and captures.
  - The ISO, `setup*.bin`, `Kabus22Patch/`, `Drivers/`.
  - The signing key in `~/.local/share/kabus22-release-keys/`.

### 2.2 etiyami: `/mnt/1st4TB/etiyami`
- **Exists:** yes. A git repo; HEAD `ee07a4e` "docs: record public v1.1.1 installation and upgrade proof"; **36 uncommitted files** (`native/vulkan_*`, shaders, FSR glue).
- **Stack:**
  - C++20 `native/` (about 75 files), SDL3, epoxy GL/GLES, Vulkan 1.3 with optional RT, FidelityFX 1.1.4 (FSR 3.1.4 + frame interpolation).
  - One root `CMakeLists.txt`: `YAMI_RELEASE_VERSION`, `YAMI_UPDATE_REPO`, `YAMI_ENABLE_VULKAN`.
  - Built with `VK_NO_PROTOTYPES`, `$ORIGIN` rpath.
- **Status:**
  - **EVIDENCED:** 17 CTests in CMake (15 GL-only), plus `build-{vulkan,gl,setup,uninstall}-proof/` dirs.
  - **CLAIMED:** public v1.1.1 with 6 assets (x86_64/ARM64 ZIP, `.run`, AppImage); RT smoke at 2560×1440 on an RX 7900 XTX.
  - Not verified: a full campaign, and Khronos validation on the latest source.
- **Reusable components**
  - **Vulkan:**
    - `native/vulkan_api.hpp`: X-macro global/instance/device dispatch through SDL.
    - `native/vulkan_renderer.cpp:242-354`: SDL instance and surface, Vulkan 1.3 device pick, RT enabled only if AS + ray query + BDA + descriptor indexing are all present.
    - `native/vulkan_effects.{hpp,cpp}` with shaders `effects_rt.{glsl,comp}`, `temporal.frag`, `sun_*.comp`: RT shadows, AO, GI, reflections, denoiser.
  - **FSR:** `third_party/fidelityfx-sdk/` (MIT, 7 documented patches in its README) and `native/shaders/fsr_inputs.frag`.
  - **Widescreen:**
    - `native/renderer.hpp:17-18` (`fit_original_interface`, `frustum_projection`).
    - Method in `REVERSE_ENGINEERING.md:176`: keep the recovered half-height and near/far planes, widen only the aspect; HUD anchors left/centre/right; menus and video fit a 1024×768 canvas.
  - **Packaging:**
    - `tools/package_appimage.py`: pinned uruntime 0.8.1, reproducible mksquashfs, per-crate attribution, self-test with corrupted images.
    - `tools/package_setup.py` (`.run`) and `tools/package_release.py` (glibc ceiling).
  - **CI:** `.github/workflows/release.yml`.
    - Actions pinned by SHA; ubuntu-22.04 on x86_64 and ARM64.
    - SDL 3.4.18, FFmpeg 7.1.5 and curl built from SHA-pinned sources.
    - Draft release → per-asset digest check → publish.
  - **Updater:** `native/update.cpp:72-460` trusts the GitHub `sha256:` asset digest only, with **no signature**. `native/update_install.cpp` / `updater_main.cpp` apply it as a journaled transaction.
  - **RE helpers:**
    - `prepare_reference.py`: SHA- and byte-guarded patch to a *copy* of the exe (windowed, borderless).
    - `check_activation.c`: runs the original 32-bit handler with `gcc -m32`.
    - `analysis/RecoverEngine.java` and `analysis/TraceStartup.java`: regex string/symbol xrefs, e.g. `DirectInput8Create`.
  - **Tests:**
    - `native/check_vulkan.cpp:552`: tolerance `compare` on `capture_rgba()` readback.
    - `setup_gui` runs headless with `SDL_VIDEO_DRIVER=dummy`.
    - The offscreen verification procedure lives outside the repo in skill `etiyami-offscreen-gpu-verify`: kwin_wayland `--virtual`, ImageMagick diff, `fixed_time.so` LD_PRELOAD.
- **Blockers and failed approaches** (`REVERSE_ENGINEERING.md`):
  - :232 — raising the frame-interpolation bias exponent tore moving characters; reverted.
  - :235 — RADV reused a freed `VkBuffer` handle, so handle-compare reallocation detection is unsafe.
  - :273 — a jitter phase repeated, giving a false temporal pass.
  - :280 — ground-stability warning still open.
  - :296 — no Khronos validation run on the latest source.
  - :462 — Ghidra prototypes were wrong despite zero decompile errors (calling conventions, dropped float argument).
  - :128 — installer deadlocked: mutex held across `SDL_PollEvent` (fixed).
- **Licenses:** no root license; FidelityFX is MIT. Bundled at release: 7-Zip `7zz`, uruntime.
- **Do not copy:** `game/`, `Yami.msi`, `Data1.cab`, `setup.exe`, `drivers/`, `.wine-yami/`, `analysis/recovered/`, `analysis/Yami.*`, the MSI table and code-page constants, and the patch offsets.

### 2.3 Pusu: Uyanış: `/mnt/1st4TB/Pusu: Uyanış`
- **Exists:** yes. Not a git repo locally; published as `github.com/sergen213/pusu-uyanis-linux`.
- **Stack:**
  - C++20 `src/` (53 files): SDL2, GLEW GL 3.3, Vulkan raster + RT, GTK3 launcher and installer.
  - Ninja is required because the path contains `:`.
  - `cmake/embed_spirv.cmake`.
- **Status:**
  - **EVIDENCED** (`.work/github-publication/bugfix-release/completed-proof.json`): 9/9 CTest; v0.1.1-linux AppImage published 2026-10-08; GL and Vulkan RT launches on an RX 7900 XTX.
  - **CLAIMED open:** flicker/noise, campaign validation, slow shutdown.
  - No CI.
- **Reusable components**
  - **CMake and Vulkan:**
    - `cmake/embed_spirv.cmake`: `.spv` → `constexpr std::array<uint32_t>` with magic/length validation.
    - `src/renderer_vulkan.cpp:30-300`: instance, surface, device capability report, swapchain.
    - `src/renderer_vulkan_rt.cpp:80-230`: BLAS/TLAS and scratch.
    - `src/shaders/vulkan/ray_query.glsl`.
  - **Camera:** `src/renderer.cpp:485-488` and `renderer_horizontal_fov()` (4:3 reference frustum → Hor+); `interface.cpp` `mask_legacy_margins`.
  - **Importer:** `src/importer.cpp` — CUE/BIN → ISO (libarchive) → MSI (libmsi) → CAB (libgcab), MSI/CAB cross-check, staged publish, SHA-256 at lines 198-209; tested by `checks/importer.cpp`.
  - **Updater:** `src/updater.cpp` — GitHub API host allowlist that covers redirects, SHA-256 digest only, **no signature**.
  - **Packaging:**
    - `packaging/build_appimage.py`: patchelf closure, source tars split under 1.9 GB.
    - `packaging/license_bundle.py`: obligation checker against `provenance.json`.
    - `packaging/check_packaging.py`.
  - **Ghidra:** `.work/analysis/PusuEvidence.java` — arguments `out` + addresses; `@` = direct only, `!` = create function; dumps blocks, xrefs, decompile, disasm.
  - **Wine diagnosis:**
    - `.work/diagnosis/run_diagnosis.py`: bwrap with no network, Xvfb 1024×768.
    - `.work/reference/observe_reference.py`.
    - `.work/diagnosis/mission/mission_ui.py`: scripted input, `input-events.jsonl`.
  - **Tests:** `checks/game_cli.py` (headless CLI via a bogus `SDL_VIDEODRIVER`).
- **Blockers and failed approaches:**
  - Original exe crashes under Wine at 1280×720 (NULL texture manager); forcing 1024×768 avoids it.
  - Absolute X pointer motion overshoots; relative motion works.
  - WineDbg minidump attempts failed.
  - Several `installed-ui-proof-*/failed.json` runs.
  - `PUSU_MSI_ROOT` defaults to an unpublished path.
- **Licenses:**
  - Root `LICENSE` = **GPL-3.0** (README: GPL-3.0-only). Copying code imports GPL-3.0 obligations unless the author relicenses it.
  - No `third_party/`.
  - Deps: system libmsi, libgcab, squish, GTK3, SDL2.
- **Do not copy:** `packaging/art/*.png` (derived from original art; `packaging/art/NOTICE.txt` grants no license), the CD and patch media, the decryption key in `.work/diagnosis/findings.json`, `src/game_*.cpp`, `assets.cpp`, `resources.cpp`, `script_vm.cpp`.

### 2.4 IKV: `/mnt/2nd4TB/IKV`
- **Exists:** yes. A git repo; HEAD `acdaf36` (0.7.0); 0 uncommitted files.
- **Stack:**
  - C++20 client: SDL3, GL 3.3, Vulkan + RT, FSR 3.1.4.
  - Qt6 launcher.
  - Server stack: PostgreSQL, Boost.
  - mingw Windows cross-build: `tools/windows/mingw-w64.cmake`, `packaging/windows/ikv.nsi`.
- **Status:**
  - **EVIDENCED:** 18 CTests; the latest full run failed `world-state` (`build-full/Testing/Temporary/LastTestsFailed.log`), which contradicts the README's "14/14".
  - **EVIDENCED:** `dist/manifest.{json,sig}` and `ikv-linux-x86_64-0.7.0.tar.gz`.
  - No GitHub Actions; releases come from `tools/release.py`.
- **Reusable components**
  - **Renderer interface:** `include/ikv/renderer.hpp:38-303` — `RendererOptions`; abstract `Renderer` with render/resize/screenshot/set_camera/overlay/upscaling; `make_gl_renderer` / `make_vk_renderer`.
  - **GL:** `src/renderer_gl.cpp`.
  - **Vulkan:** `src/renderer_vk.cpp` (instance at :932, barrier helpers at :540-570).
  - **RT:** `src/vulkan_rt.cpp` with `include/ikv/vulkan_rt.hpp` (BLAS/TLAS :207-238) and `shaders/vulkan/rt_lighting.glsl` (ray query with alpha test).
  - **Denoise:** `shaders/vulkan/temporal.frag` (à-trous).
  - **FSR:** `src/fsr_vk.cpp`, `tools/ffx_fsr3_blobs.py` (Linux glslc replacement for AMD's compiler).
  - **Updater:**
    - `include/ikv/updates.hpp` + `src/launcher_updates.cpp`: Ed25519 via OpenSSL `EVP_DigestVerify` at :172-178, libarchive, sequence/replay floor, current/previous swap.
    - `src/update_helper_main.cpp`.
    - `tools/sign_manifest.py`: schema-2, raw 64-byte signature.
    - Spec: `docs/LAUNCHER_UPDATES.md`.
    - Tests: `tests/update_integration.py` (ephemeral key, owned TLS origin, tamper/replay/rollback) and `tests/update_version_test.cpp`.
    - The public key is baked in through `file(CONFIGURE)` → `generated/ikv/release_defaults.hpp` (root CMakeLists ~210-222).
  - **Importer:**
    - `src/launcher_extraction.cpp`: private snapshot, SHA-256 of the exact bytes innoextract consumes, native-ELF innoextract check.
    - `src/launcher_assets.cpp`: per-file SHA-256 manifest, verify/repair.
  - **Packaging:**
    - `tools/package_appimage.py`: DT_NEEDED closure against an excludelist, uruntime, `.DirIcon`, `--self-check`.
    - `tools/appimage/Dockerfile.ubuntu24`.
    - `packaging/NOTICE.txt`.
  - **Release:** `tools/release.py` — bump → build → smoke → deterministic tar → sign → verify → optional publish.
  - **Ghidra:**
    - `tools/ghidra/ExportEvidence.java`.
    - `tools/ghidra/GraphicsEvidence.java`: Direct3D call sites and globals; directly relevant to JoA's d3d8.
    - `tools/ghidra/AssetReturnTypes.java`: fixes wrong no-return propagation.
    - Run recipes: `docs/REVERSE_ENGINEERING.md:92-125`.
  - **PE inspection:** `tools/pe_inventory.py`.
  - **Tests:**
    - `tests/launcher_gui_smoke.cpp`: Qt GUI smoke.
    - `analysis/final_launcher_smoke.py:183`: Xvfb + Openbox, `MESA_VK_WSI_DEBUG=sw`.
    - `src/image_write.cpp` + `--screenshot PPM`.
    - `analysis/porting-wave17/MisirLights/zone_match.py:114`: mean |diff| plus the fraction of pixels above 24/255.
- **Blockers and failed approaches** (`docs/KNOWN_ISSUES.md`):
  - Mesa X11 present-timing raised SIGFPE in RADV, so display timing can only be measured on Wayland.
  - Intermittent shutdown timeout, unattributed.
  - Half-rate far skinning was rejected (judder).
  - Removing `drain_presents(1)` was rejected (+9.2 ms frame period).
  - FSR Ultra Performance ghosts.
  - Vulkan memory from a retained reload is not reused until the block is destroyed.
  - No Wine reference was ever run.
- **Licenses:**
  - No root license.
  - `third_party/FidelityFX-SDK/` (MIT, `IKV-PROVENANCE.txt`).
  - `third_party/stb_vorbis/` (public domain/MIT).
  - The AppImage bundles Qt 6 (LGPL-3.0), SDL3 (zlib), OpenSSL (Apache-2.0), uruntime (MIT; libfuse LGPL-2.1), innoextract (zlib).
- **Do not copy:**
  - **All network and protocol code:** `src/protocol.cpp`, `src/original_wire.cpp`, `src/world*.cpp`, `src/store.cpp`, `src/https_service.cpp`, `src/server_main.cpp`, `src/auth_main.cpp`, `src/client_session.cpp`, `src/launcher_server.cpp`, `src/bots.cpp`, `docs/network/*`.
  - Server deployment: `database/`, `deploy/`, `web/`, `Dockerfile`, `compose.yaml`.
  - Original material: `original/`, `istanbulkur6_0.exe`.
  - `src/launcher_skin.cpp` (reads the original launcher PE).
  - IKV addresses inside the Ghidra scripts.
  - `packaging/ikv-release-public.pem` and the private key.

### 2.5 CitroenC4Robot: `/mnt/2nd4TB/CitroenC4Robot`
- **Exists:** yes. Not a git repo.
- **Stack:**
  - C++20 game: SDL3, GL, Vulkan + RT.
  - **Qt6 launcher** (`launcher/`) and a **separate Qt6 updater** (`updater/`).
  - Python packaging and tools.
  - CMake 3.24; `C4_VERSION` semver regex; `-ffp-contract=off`.
- **Status:**
  - **EVIDENCED:** 28 `add_test` calls; the last sanitized failures were `original-material-behavior` and `launcher-smoke`; signed-release receipts in `work/verification/`.
  - **CLAIMED:** 28/28 Release pass; GL and Vulkan two-lap race.
  - KNOWN_ISSUES I-12/I-13: no published repository or trusted key; launcher self-update unproven.
- **Reusable components**
  - **Qt6 launcher** (`launcher/src/`):
    - `main.cpp`: `QMainWindow`, five tabs, EN/TR `t(en,tr)`, `QSaveFile` JSON config, `QProcess` children (import, update, game, graphics probe), `QtConcurrent` validation, hourly update timer, AppImage environment scrub.
    - `hardware.cpp`: Vulkan and GL probe.
    - `quality_controls.cpp`: graphics panel.
    - `assets.cpp`: `validateAssets`, `isNativeElf`.
    - `integration.cpp`: scripted end-to-end driving widgets by `accessibleName`.
    - `--smoke` runs with `QT_QPA_PLATFORM=offscreen`.
  - **Updater** (`updater/src/main.cpp`):
    - OpenSSL Ed25519 verify before parse (:124-133, :173-174).
    - Per-file SHA-256; libarchive with size enforcement.
    - `versions/<semver>` + atomic `current` symlink rename with fsync; `--rollback`.
    - Pinned key path outside the install root.
    - Test: `updater/tests/safety.py`.
    - `updater/src/runtime_policy_data.hpp.in` embeds the policy.
  - **CI:** `.github/workflows/native.yml`.
    - SHA-pinned actions; SDL3 3.4.18 built from source if missing.
    - On tag: the `ED25519_PRIVATE_KEY` secret is written under umask 077 and removed by a trap.
    - Packages twice + `cmp` (reproducibility), `openssl pkeyutl -verify -rawin`, `--appimage-extract` smoke, draft → publish.
  - **Packaging:**
    - `packaging/appimage/build.py`: appimagetool with `--runtime-file`, zstd, `SOURCE_DATE_EPOCH`.
    - `packaging/appimage/acquire.py`: pinned tools.
    - `packaging/appimage/citroen-c4.desktop`.
    - `packaging/package_native.py`: manifest + Ed25519 + patchelf.
    - `packaging/runtime_bundle.py`: 1934 lines; reuse selectively.
  - **Ghidra:**
    - `tools/ghidra/analyze.py`: headless driver, project named by SHA, `invocations.json` log, regex seed categories.
    - `tools/ghidra/ExportEvidence.java`: TSVs for functions, calls, imports, strings, xrefs, FPU control words; `define:ADDR`. Strip the C4 `0x238` entity scans.
  - **Wine observer:** `tools/observe_original.py` — Xvfb `:91`, fresh prefix, `WINEDEBUG`, timed input plans, periodic screenshots, `wineserver -k`, JSON report.
  - **Importer:** `tools/import_assets.py` — innoextract `--list-checksums`, installer SHA before and after, no overwrite, process-group cancel.
  - **Vulkan:**
    - `src/rendering/vulkan/VulkanDispatch.{hpp,cpp}`: `VK_NO_PROTOTYPES` table.
    - `VulkanScene.cpp:375-591`: validation messenger, persisted pipeline cache with fallback.
    - `raytracing.cpp`: `C4_RT_PROC` guard.
    - `shaders/rt.comp`, `temporal.comp`.
    - `src/rendering/vulkan/CMakeLists.txt`: glslc `-MD` DEPFILE rule.
  - **Camera:** `src/game/CameraProjection.hpp` (horizontal arc + pixel aspect, D3D→GL/VK clip conversion); `src/game/ui/Viewport.cpp` (fixed UI tiers, centre-fit, pointer mapping).
  - **Images:** `src/assets/GpuImageBc.cpp`, `Images.cpp` (BC/DDS).
  - **Platform:** `src/platform/runtime.{hpp,cpp}` (SDL3 window, swap interval, diagnostics).
- **Blockers and failed approaches** (`docs/KNOWN_ISSUES.md`):
  - I-04: the innoextract signing key was fetched over the same channel, so it is not independently anchored.
  - I-13: self-update needs a separate process or versioned dirs.
  - I-14: not a clean-machine release.
  - "texture resolve framebuffer incomplete" in early Enhanced Apply.
  - LSFG implicit-layer environment filter errors.
- **Licenses:** no LICENSE anywhere and no `third_party/`. Deps: Qt6 ≥ 6.4, SDL3 ≥ 3.2, OpenSSL, LibArchive, libsndfile, an LGPL FFmpeg 8.1.3 build.
- **Do not copy:** `work/original/`, `work/reference/`, the installer exe, `work/ghidra/`, `tools/original-assets.json`, `src/game/*` (except the tiny `CameraProjection.hpp` math pattern), `src/assets/*.inc`, `docs/FUNCTION_MAP.md`, `work/tools/gnupg/private-keys-v1.d`.

### 2.6 SuperCan: `/mnt/2nd4TB/SuperCan`
- **Exists:** yes. Not a git repo; no README, LICENSE or CI.
- **Stack:**
  - C++20: SDL3, GL; OpenSSL for SHA-256 only.
  - Vendored modified libvorbis (`src/audio/vendor/original_numeric_vorbis/`, BSD).
  - Single `CMakeLists.txt`: `SUPERCAN_SANITIZERS`; float-determinism flags `-frounding-math -ffp-contract=off`.
- **Status:**
  - **EVIDENCED:** 30 CTests pass (`build-release/Testing/Temporary/LastTest.log`, Oct 9).
  - **CLAIMED** (`docs/PROGRESS.md`): not playable; the level runs up to the start of actions.
  - Not done: Vulkan, RT, launcher, updater, packaging.
  - `src/render/d3d9_device.{hpp,cpp}` is not in CMake (unbuilt).
- **Reusable components**
  - **Importer:** `src/assets/main.cpp` (`inventory|verify|inspect|import`).
    - Pinned installer SHA-256; innoextract via `posix_spawnp`.
    - Inode-pinned staging; `renameat2(RENAME_NOREPLACE)`; per-file SHA-256 manifest.
    - Docs: `docs/INSTALLER_ARCHITECTURE.md`. Test: `tests/native_assets.py`.
  - **Ghidra** (`tools/ghidra/`):
    - `ExportEvidence.java`: function + callees, JSON, merges `reviewed-functions.json`, 600 s decompile timeout.
    - `ExportVtables.java`, `ExportMachineEvidence.java`, `ExportFloatingEnvironment.java`.
    - `RepairInlineConstants.java`: MSVC x87 CALL+8/POP inline constants.
    - Recipes: `docs/REVERSE_ENGINEERING.md:21-110`.
  - **D3D→GL:**
    - `src/render/d3d9_device.{hpp,cpp}`: D3D9 fixed-function subset on GL 3.3; documents clip-z, half-pixel, y-flip and winding. Pattern only, untested.
    - `src/assets/d3d9_shader.cpp` and `d3d9_glsl.{hpp,cpp}`: SM1–3 bytecode → GLSL. JoA ships NV assembly `.nvo/.pso`, not D3D9 bytecode, so this does not apply directly.
  - **Inspector:** `tools/asset-inspector/scene.cpp` — SDL3/GL orbit viewer, `--screenshot` BMP (:1231), `--self-check`, `glReadPixels` asserts.
  - **Wine reference** (`analysis/reference/`, gitignored):
    - `original_*_probe-parent.sh`: clang `i686-pc-windows-msvc` probe DLL, bwrap `--unshare-all`, fresh prefix, SHA before/after.
    - `live_observer.py`: Xvfb `:87`, XTest via ctypes, ffmpeg x11grab, stdin command loop.
  - **Validation:** `tools/validation/source_inventory.py`.
- **Blockers and failed approaches:**
  - Wine D3DX disassembler returned S_OK/NULL, unusable as an oracle.
  - ffmpeg x11grab MIT-SHM error at capture end.
  - Ghidra 45 s timeouts and 50 MB responses (fixed: 600 s, no syntax tree).
  - Zero-filled LZSS dictionary was wrong.
  - ASan reports a 592 KB fontconfig/GTK leak chain, left open.
  - `_proj.dat` password not cracked.
- **Licenses:** no root license; vendored libvorbis is BSD.
- **Do not copy:** everything under `analysis/` except the generic probe and observer scripts (copy deliberately), `docs/FUNCTION_MAP.md`, `tools/ghidra/reviewed-functions.json`, `docs/manifests/`, the Acknex-specific scripts (`ApplyCompiledBindings`, `RecoverOverlay`, `LocateFx42Semantics`), and `src/game/*`.

## 3. Reuse plan for Joan of Arc

### Licensing gate
- Five of the six projects have no license, and Pusu is GPL-3.0. Any code copied verbatim needs a recorded relicensing decision by the author in `docs/THIRD_PARTY_LICENSES.md`.
- Until then, implementers should **adapt the pattern and rewrite** rather than copy files.
- Third-party code keeps its own license:
  - FidelityFX: MIT, take it from IKV's `third_party/FidelityFX-SDK/`.
  - libgamestudio header: MIT.
  - ODE: BSD.
- Shipped binaries need NOTICE entries: Qt6 (LGPL-3.0, dynamic link), SDL3 (zlib), OpenSSL (Apache-2.0), libarchive (BSD), FFmpeg (LGPL build only), uruntime (MIT + LGPL squashfuse/libfuse), innoextract (zlib).

### Component map

| JoA component (path owner) | Best source to adapt (exact path) | Secondary / notes | Must not copy |
|---|---|---|---|
| **Formats lib** (`src/formats/`) | Pattern only:<br>• `/mnt/2nd4TB/SuperCan/src/assets/{resource,image}.cpp` (bounds-checked parser hardening)<br>• `/mnt/2nd4TB/SuperCan/CMakeLists.txt:151-174` (skip-77 asset-gated tests) | `/mnt/2nd4TB/CitroenC4Robot/src/assets/GpuImageBc.cpp` for DDS/BC decode; IKV `decompress_dds` in `/mnt/2nd4TB/IKV/src/assets.cpp` | Every prior format (WRS/WMB/MDL/SKYF/ADF/MSI tables). JoA formats (RES/AM2/AM3/SMO/AM1/scr/fin) must be reverse-engineered independently. |
| **Engine GL backend** | `/mnt/2nd4TB/IKV/include/ikv/renderer.hpp:38-303` (abstract `Renderer` + GL/VK factories), `/mnt/2nd4TB/IKV/src/renderer_gl.cpp` | `/mnt/2nd4TB/CitroenC4Robot/src/rendering/quality/` (CSM, settings); D3D→GL conventions in `/mnt/2nd4TB/SuperCan/src/render/d3d9_device.cpp` (JoA is D3D8, same fixed-function conventions) | IKV/C4 material shaders, game render code |
| **Engine Vulkan init** | `/mnt/1st4TB/etiyami/native/vulkan_api.hpp` (X-macro dispatch via SDL3), `/mnt/1st4TB/etiyami/native/vulkan_renderer.cpp:242-354` (instance/surface/device + RT feature gate) | `/mnt/2nd4TB/CitroenC4Robot/src/rendering/vulkan/VulkanScene.cpp:375-591` (validation messenger, pipeline-cache fallback); `/mnt/1st4TB/Pusu: Uyanış/src/renderer_vulkan.cpp:30-300` is GPL | — |
| **Vulkan RT** | `/mnt/2nd4TB/IKV/src/vulkan_rt.cpp` + `include/ikv/vulkan_rt.hpp`, `/mnt/2nd4TB/IKV/shaders/vulkan/{rt_lighting.glsl,temporal.frag}` | `/mnt/1st4TB/Kabus 22/native/render_vulkan_rt.cpp` + `docs/RAYTRACING.md`; `/mnt/1st4TB/etiyami/native/vulkan_effects.cpp` | Do not detect reallocation by comparing handles (etiyami RE:235). Budget for the 1024-sampler ceiling (Kabus). |
| **FSR** | `/mnt/2nd4TB/IKV/third_party/FidelityFX-SDK/` (MIT), `/mnt/2nd4TB/IKV/tools/ffx_fsr3_blobs.py`, `/mnt/2nd4TB/IKV/src/fsr_vk.cpp` | etiyami `third_party/fidelityfx-sdk/README.md` (7 patches) | — |
| **Shader build (CMake)** | `/mnt/2nd4TB/CitroenC4Robot/src/rendering/vulkan/CMakeLists.txt` (glslc `-MD` DEPFILE), `/mnt/1st4TB/Pusu: Uyanış/cmake/embed_spirv.cmake` (pattern; GPL file) | IKV root `CMakeLists.txt` ~80-115 (per-shader defines) | — |
| **Camera / widescreen** | Method: `/mnt/1st4TB/etiyami/REVERSE_ENGINEERING.md:176` + `native/renderer.hpp:17-18` (keep vertical FOV, Hor+, anchored HUD, fitted 4:3 menus) | `/mnt/1st4TB/Kabus 22/native/render_policy.hpp` (`camera_matrix`, `referenceAspect`); `/mnt/2nd4TB/CitroenC4Robot/src/game/CameraProjection.hpp`; `/mnt/2nd4TB/CitroenC4Robot/src/game/ui/Viewport.cpp` (pointer mapping); doc template `/mnt/1st4TB/Kabus 22/docs/WIDESCREEN_IMPLEMENTATION.md` | JoA's projection constants must come from `joan.exe` RE and the community FOV-fix analysis, not from these games |
| **Platform (SDL3)** | `/mnt/2nd4TB/CitroenC4Robot/src/platform/runtime.{hpp,cpp}` | `/mnt/1st4TB/etiyami/native/main.cpp` CLI (`--capture`, `--smoke`, `--gpu-timings`); Kabus `native/controller.cpp` is SDL2 (port the logic) | — |
| **Launcher (Qt6)** | `/mnt/2nd4TB/CitroenC4Robot/launcher/src/main.cpp` (QProcess children, QtConcurrent validation, QSaveFile config, `--smoke` offscreen), `launcher/src/hardware.cpp` | `/mnt/2nd4TB/CitroenC4Robot/launcher/src/integration.cpp` (scripted UI test); `/mnt/2nd4TB/IKV/tests/launcher_gui_smoke.cpp` | IKV `launcher_skin.cpp` and `launcher_server.cpp`; C4 TR/EN strings and branding |
| **Installer / importer** | `/mnt/1st4TB/Kabus 22/installer/setup.cpp` (detects an *installed Windows folder* as a source, inventory JSON, verify/repair) + `/mnt/2nd4TB/SuperCan/src/assets/main.cpp` (inode-pinned staging, `renameat2(RENAME_NOREPLACE)`) | GOG `setup_*.exe` path: `/mnt/2nd4TB/IKV/src/launcher_extraction.cpp` (native-ELF innoextract, hash consumed bytes), `/mnt/2nd4TB/CitroenC4Robot/tools/import_assets.py`. The JoA hash source is `goggame-1421210170.hashdb` / our own manifest. | Other games' manifests and SHA pins; MSI/CAB code-page tables; never import `.exe`/`.dll` except as optional RE input |
| **Updater + signing** | `/mnt/2nd4TB/CitroenC4Robot/updater/src/main.cpp` (separate binary, Qt6Core/Network + OpenSSL Ed25519, verify-before-parse, `versions/` + `current`, rollback) + `updater/tests/safety.py` | `/mnt/2nd4TB/IKV/src/launcher_updates.cpp` (replay/sequence floor), `/mnt/2nd4TB/IKV/tools/sign_manifest.py`, `/mnt/2nd4TB/IKV/tests/update_integration.py` (fake GitHub TLS origin); `/mnt/1st4TB/Kabus 22/installer/{update,layout}.cpp` + `docs/RELEASE_PROCESS.md` (key rotation). Every prior project uses OpenSSL `EVP_DigestVerify`; using libsodium instead would be a new dependency for no gain. | All existing keys (`~/.local/share/{kabus22-release-keys,ikv-dev}`, `packaging/ikv-release-public.pem`) and repo names. Generate a new JoA keypair. |
| **CI (GitHub Actions)** | `/mnt/2nd4TB/CitroenC4Robot/.github/workflows/native.yml` (SHA-pinned actions, secret under umask 077, double-package `cmp`, `openssl pkeyutl -verify`, draft → publish) | `/mnt/1st4TB/Kabus 22/.github/workflows/{ci,release}.yml` (tag-vs-CMake check, secret-matches-pubkey check); `/mnt/1st4TB/etiyami/.github/workflows/release.yml` (per-asset digest check, ARM64) | — |
| **Packaging / AppImage** | `/mnt/2nd4TB/IKV/tools/package_appimage.py` + `/mnt/2nd4TB/IKV/tools/appimage/Dockerfile.ubuntu24` + `/mnt/2nd4TB/IKV/packaging/NOTICE.txt` | `/mnt/1st4TB/etiyami/tools/package_appimage.py` (reproducible mksquashfs, pinned uruntime); `/mnt/2nd4TB/CitroenC4Robot/packaging/appimage/{build,acquire}.py`; local release driver `/mnt/2nd4TB/IKV/tools/release.py` | Pusu `packaging/art/*` (original-derived); other games' `.desktop`/icons |
| **Tests / screenshot comparison** | `/mnt/1st4TB/Kabus 22/tools/wine-reference/compare.py` (ffmpeg SSIM, masks, montages) + `tests/reference/thresholds.tsv` | `/mnt/1st4TB/etiyami/native/check_vulkan.cpp:552` (readback tolerance); `/mnt/2nd4TB/IKV/analysis/porting-wave17/MisirLights/zone_match.py:114` (mean diff + >24/255 fraction); skill `etiyami-offscreen-gpu-verify` (kwin `--virtual`, `fixed_time.so`). Use skip-77 asset-gated tests. | Reference images or captures from other games |
| **Wine reference capture** | `/mnt/2nd4TB/CitroenC4Robot/tools/observe_original.py` (Xvfb, fresh prefix, timed input, JSON report) | `/mnt/1st4TB/Kabus 22/tools/wine-reference/wineref.py`; sandbox in `/mnt/2nd4TB/SuperCan/analysis/reference/original_*_probe-parent.sh` (bwrap `--unshare-all`); `/mnt/1st4TB/etiyami/prepare_reference.py` (SHA-guarded patch of a *copy* only). Known Wine pitfalls: gamma ramp ignored windowed; resolution modes (Pusu crashed off 1024×768); relative mouse deltas. | Wine prefixes; patch offsets |
| **Ghidra scripts** | `/mnt/2nd4TB/CitroenC4Robot/tools/ghidra/analyze.py` (headless driver) + `ExportEvidence.java` (strip the `0x238` scans) | `/mnt/2nd4TB/IKV/tools/ghidra/GraphicsEvidence.java` (D3D call sites; JoA uses d3d8); `/mnt/2nd4TB/SuperCan/tools/ghidra/RepairInlineConstants.java` (MSVC x87; JoA is VC6); `/mnt/2nd4TB/IKV/tools/ghidra/AssetReturnTypes.java`; `/mnt/1st4TB/Pusu: Uyanış/.work/analysis/PusuEvidence.java`; `/mnt/1st4TB/etiyami/analysis/TraceStartup.java` (regex string xrefs, e.g. `DirectInput8Create`); `/mnt/2nd4TB/IKV/tools/pe_inventory.py`. Caveat (etiyami RE:462): verify prototypes against disassembly. | Address lists, reviewed-function JSON, and FUNCTION_MAPs of other binaries |
| **Texture upscaling** | `/mnt/1st4TB/Kabus 22/tools/texture-upscaler/upscale.py` + `native/texture_pack.cpp` (hash-keyed pack, Scale2x/Lanczos/realesrgan) | Only one source exists | The HD Update's textures (third-party, rights unknown) |
| **Crash reporting** | **None in any project.** Must be built new (`sigaction` + `backtrace` or minidump). | — | — |

### Gaps no prior project covers
- Crash reporting.
- A D3D8 or NV vertex/pixel shader assembly (`.nvo`/`.pso`) translator. SuperCan's `d3d9_glsl` handles D3D9 bytecode only.
- DirectInput8 or OpenAL behaviour mapping.
- A golden-image perceptual diff integrated into CTest; Kabus's `reference_comparison` never launched.
