# Release process and signed updates

Code: `updater/` (lib `joa_update`, CLI `joa-updater`), `packaging/release/`, `packaging/appimage/`,
`.github/workflows/`. Design adapted from the Kabus22 native updater by the same author
(Ed25519-signed manifest, staged install, atomic symlink swap, rollback); crypto here is libsodium.

## Installed layout

`${XDG_DATA_HOME:-~/.local/share}/joan-native/` is the data root. The updater owns **only** `app/`:

```
joan-native/app/<version>/{bin,lib,share,.joan-native-payload.json}
joan-native/app/current -> <version>     relative symlink; rename(2) swap = commit point
joan-native/app/.state                   active=/previous= (atomic temp+fsync+rename)
joan-native/app/.lock                    flock; one apply/rollback at a time
joan-native/app/.stage-*                 staging + downloads; removed on failure / next run
```

`game/` (imported original data, owned by the launcher/installer), saves, `$XDG_CONFIG_HOME/joan-native/`
settings and HD asset packs are never opened for writing. The updater refuses to run as root.
`.joan-native-payload.json` lists every installed file with size and SHA-256 (for "Verify Files").

## CLI contract

`joa-updater COMMAND [--channel stable|experimental] [--json]` (default channel `stable`).
Exit 0 ok, 1 error (with `--json`: `{"error":"..."}` on stdout), 2 usage. Progress goes to
stderr as `progress <0-100> <message>`; the result is one line on stdout.

| Command | `--json` result |
|---|---|
| `check` | `{"current":"0.1.0","latest":"0.2.0"\|null,"available":true,"notes":"<release body>"}` |
| `apply` | `{"applied":true,"from":"0.1.0","to":"0.2.0"}`; up to date: `{"applied":false,...}` |
| `rollback` | `{"from":"0.3.0-beta.1","to":"0.2.0"}` |
| `status` | `{"current":"0.2.0"\|null,"previous":...\|null,"installed":[...],"app_dir":"..."}` |

`current` in `check`/`apply` is the active `app/current` version, or the binary's own version when
nothing is installed. `stable` considers non-prerelease GitHub releases only; `experimental` also
prereleases. Drafts and tags that are not `vSEMVER` are ignored. `--version`, `--help`.

The repository is fixed at build time: CMake cache var `JOA_UPDATE_REPO` (default
`sergen213/joan-native`; the release workflow sets `${{ github.repository }}`).

### apply sequence

1. Lock `app/.lock`; recover (delete `.stage-*`/`.tmp-*` leftovers and versions other than
   active, previous and the running one).
2. GET `https://api.github.com/repos/<repo>/releases?per_page=50`; pick the highest version in the
   channel. Lower than current → **error "Refusing downgrade"** (only `rollback` goes back);
   equal → no-op.
3. Download `manifest.json` (≤256 KiB) and `manifest.json.sig`; verify the detached Ed25519
   signature over the exact bytes with the compiled-in key **before** parsing. Then require
   `product == joan-native`, manifest version == release tag, stable channel only accepts
   `"channel":"stable"`, running glibc ≥ `min_glibc`.
4. Download every artifact into `app/` (size bounded by the signed size, GitHub asset size must
   agree) and compare SHA-256 with the manifest. **All** hashes are verified before any extraction.
5. Extract with libarchive into a fresh 0700 stage: only `bin/ lib/ share/`, no absolute/`..`
   paths, no escaping symlinks, no hard links/devices/setuid/setgid/sticky, no path delivered by
   two artifacts, ≤2 GiB, ≤16384 entries. The stage must contain executable `bin/joa-updater`.
6. `rename(stage, app/<version>)`, fsync, atomic `current` swap, write `.state`, prune to
   {new, previous, running}. Nothing downloaded is executed by the updater.

Crash recovery: a crash before step 6's swap leaves only scratch entries (removed next run); after
the swap but before `.state`, `previous` is derived from the stale `.state` (recorded active ≠
current ⇒ it is the rollback target).

## Manifest schema (format 1)

```json
{
  "format": 1,
  "product": "joan-native",
  "version": "0.2.0",
  "channel": "stable",
  "min_glibc": "2.42",
  "artifacts": [
    {"name": "joan-native-0.2.0-linux-x86_64-engine.tar.gz", "component": "engine",
     "sha256": "<64 lowercase hex>", "size": 123456}
  ]
}
```

- `component` ∈ {engine, launcher, updater, shaders, tools}, each at most once; `name` must equal
  `joan-native-<version>-linux-x86_64-<component>.tar.gz`; `size` > 0, total ≤ 2 GiB.
- `manifest.json.sig`: base64 of the 64-byte Ed25519 detached signature of the exact file bytes.
- Release assets: `manifest.json`, `manifest.json.sig`, the artifacts, plus (not updater inputs)
  `JoanNative-<version>-x86_64.AppImage`, `joan-native-<version>-third-party-source.tar` (FFmpeg and
  AppImage-runtime corresponding source), the project source tarball and `SHA256SUMS`.

Component split (`make_release.py`, first match wins): `bin/joa-updater` → updater;
`bin/joa-launcher`, `bin/joa-install`, `share/{applications,icons,metainfo}/*` → launcher;
`share/joan-native/shaders/*`, `*.spv` → shaders; other `bin/joa-*` → tools; everything else → engine.
Every update installs all components of the version (each `app/<version>` is complete). The payload
is the same tree as the AppImage's `usr/` (bundled Qt, FFmpeg, SDL3, curl … under `lib/`, DT_RPATH
`$ORIGIN/../lib`), so `app/current/bin/*` run without the AppImage. Shaders are compiled into
joan-native and no `joa-*` tools are installed, so current releases have three components
(engine, launcher, updater).

## Key management

- Production public key: `packaging/release/update_pubkey.txt` (base64 raw 32 bytes), compiled into
  `joa-updater` at configure time. Current key: `dtJpUuwS8OO5af1s3I4aC4/tfJaQMJd5Eoh0Her5coo=`.
- Production private key: generated on the maintainer workstation at
  `~/.local/share/joan-native-release-keys/update-ed25519.key` (mode 0600, base64 raw 32-byte seed,
  outside the repository). Never commit or print it; keep an offline backup. Losing it means
  installed updaters can no longer verify releases (users must reinstall from a new AppImage).
- CI signing: repository secret **`JOA_UPDATE_SIGNING_KEY`** = the same base64 seed (contents of the
  key file). It must be configured before tagging; the release workflow aborts if it is missing or
  its public key differs from `update_pubkey.txt`.
- Dev/test keys: `packaging/release/sign_manifest.py --generate-key work/updater/dev-ed25519.key`
  (work/ is gitignored). The CTest generates throwaway keys in a temp dir.
- Rotation: generate a new key, put its public key in `update_pubkey.txt`, ship one release signed
  with the **old** key (its payload contains the updater with the new key), then switch the CI
  secret. A compromised key cannot be revoked remotely: publish an advisory and require reinstall.

```sh
python3 packaging/release/sign_manifest.py --generate-key NEWKEY   # 0600; prints public key only
python3 packaging/release/sign_manifest.py --public-key --key KEY
python3 packaging/release/sign_manifest.py dist/manifest.json --key KEY   # or env JOA_UPDATE_SIGNING_KEY
```

## Building release artifacts

One entry point for CI and local releases: `packaging/release/build_release.py` (needs Arch: it reads
pacman metadata for the license inventory; CI uses the `archlinux:latest` container with
`packaging/ci/arch-packages.txt`).

```sh
python3 packaging/ffmpeg/build_ffmpeg.py --prefix work/tools/ffmpeg-prefix --tools-dir work/tools   # once; nasm needed
python3 packaging/release/build_release.py --version 0.2.0 --channel stable --repo OWNER/NAME \
  --ffmpeg-prefix work/tools/ffmpeg-prefix --work-dir work/release/w --output-dir work/release/dist \
  --sign-key ~/.local/share/joan-native-release-keys/update-ed25519.key   # or env JOA_UPDATE_SIGNING_KEY
```

1. Release CMake build of the four programs (`BUILD_TESTING=OFF`, `JOA_REPRODUCIBLE=ON`,
   `CMAKE_BUILD_WITH_INSTALL_RPATH`, DT_RPATH `$ORIGIN/../lib` via `--disable-new-dtags`, so the RPATH
   also covers transitive and dlopen'ed libraries) against the LGPL FFmpeg prefix; `cmake --install --strip`.
2. Bundling: the FFmpeg `.so` files + `share/licenses/FFmpeg`; Qt plugins (platforms wayland, xcb,
   offscreen, minimal; xdg-shell/qt-shell, bradient decorations, wayland-egl/dmabuf/shm client
   buffers, xcb GLX/EGL, svg/ico image formats, svg icon engine, compose/ibus input, xdg portal theme)
   under `lib/qt6/plugins` — Arch's relative layout, so their `$ORIGIN/../../../` RUNPATH keeps
   working — with `bin/qt.conf`; then the DT_NEEDED closure of every shipped ELF via `readelf` +
   `ldconfig -p`. **Never bundled** (host): glibc, libstdc++/libgcc_s/libatomic, libGL/GLX/EGL/
   OpenGL/GLES/GLdispatch, libvulkan, libdrm/libgbm, X11/xcb core + Xau/Xdmcp/Xext/Xrender/Xi/Xfixes,
   wayland client/cursor/egl, fontconfig/freetype/harfbuzz/expat, zlib, udev, asound, systemd, dbus
   (`HOST` in the script). The build fails on an unresolved library, an absolute RUNPATH or a
   program without the `$ORIGIN/../lib` RPATH.
3. License inventory `share/doc/joan-native/THIRD_PARTY_INVENTORY.json`: for each bundled file its
   SHA-256 and owning package/version/SPDX license/upstream URL (`pacman -Qo/-Qi`), the shipped license
   texts (`/usr/share/licenses/<pkg>` + SPDX texts, plus `packaging/licenses/overrides.json` where
   Arch metadata is non-SPDX or package-wide: e2fsprogs/libcom_err, lz4, xz, libidn2, libunistring,
   keyutils, acl, zstd), compiled-in components (GLM, nlohmann/json, Vulkan-Headers, FSR 1), the
   host-provided sonames and the AppImage runtime. `packaging/NOTICE.txt` → `share/doc/joan-native/`.
   A package without any license text fails the build.
4. Outputs: `make_release.py` tarballs + manifest; the AppImage = pinned official type2 runtime
   (`packaging/appimage/runtime.lock.json`, immutable asset 596078161, SHA-256 checked) + zstd
   squashfs of `AppDir/{AppRun, joan-native.desktop, joan-native.svg, .DirIcon, usr/=stage}` (modes
   normalised, root-owned, times = `SOURCE_DATE_EPOCH`); `*-third-party-source.tar` (FFmpeg archive +
   signature + key + recipe + lock; type2-runtime commit, libfuse 3.15.0, squashfuse, musl, zstd, zlib,
   mimalloc archives); `manifest.json.sig` when a key is given; `SHA256SUMS`.

`AppRun` runs `usr/bin/joa-launcher`; `JOA_APPIMAGE_ENTRY=joan-native|joa-install|joa-updater` runs
another program. The AppImage contains no game data (users import theirs) and no GPU driver.

### Reproducible builds

`SOURCE_DATE_EPOCH` (default: HEAD commit time) fixes all timestamps; `JOA_REPRODUCIBLE`
(top-level CMakeLists.txt) adds `-ffile-prefix-map` for the source and build dirs and
`--build-id=sha1`; tarballs are sorted, root-owned, mtime 0, gzip mtime 0; the squashfs uses
`-all-root`, normalised modes and `SOURCE_DATE_EPOCH`; the FFmpeg build is path-free (see
docs/AUDIO_AND_VIDEO.md §7); Ed25519 signatures are deterministic. `release.reproducible` rebuilds
everything in a second directory (different path depth with a space, umask 077, other TMPDIR) and
byte-compares every artifact; `release.appimage` scans every shipped file for the source/build paths.

## Releasing

CI (`.github/workflows/ci.yml`, archlinux container, packages from `packaging/ci/arch-packages.txt`):
- `build-test`: all components (system FFmpeg), `ctest -E '\.vk_'` as user `ci` — non-GPU tests and the
  GL tests on Mesa llvmpipe under private `xvfb-run`; asset-backed tests exit 77 without game data.
- `sanitizers`: `JOA_SANITIZE=ON` Debug build, same tests under ASan+UBSan (LeakSanitizer needs
  `--cap-add=SYS_PTRACE`).
- `reproducibility`: LGPL FFmpeg (cached by `actions/cache` on the lock/recipe hash), then
  `ctest -L release` (dist, reproducible, appimage, product_update, license_audit).

Tag release (`.github/workflows/release.yml`), on `vMAJOR.MINOR.PATCH[-pre]`:
1. tag core must equal `project(joan_native VERSION ...)`; `-pre` tags → experimental + GitHub prerelease;
   `SOURCE_DATE_EPOCH` = tagged commit time;
2. verify signing secret ↔ `update_pubkey.txt`;
3. LGPL FFmpeg (cached), Release build + ctest against it;
4. `build_release.py` (signs with `JOA_UPDATE_SIGNING_KEY`), `sign_manifest.py --verify` against
   `update_pubkey.txt`, `release_checks.py appimage` + `license-audit` on the result;
5. `git archive` source tarball, `SHA256SUMS` regenerated over all assets;
6. **draft** GitHub release. Edit the notes (they become `check`'s `notes`), qualify, then publish.

Local run of any workflow job in its container (no `act`; publishing steps are only printed):
`python3 tests/release/run_workflow_local.py .github/workflows/release.yml release --source DIR
--cache DIR --signing-key work/release/dev-ed25519.key` (a source copy whose `update_pubkey.txt` holds
the dev public key; the production key is never used locally).

`make_release.py` refuses anything but regular files, directories and in-tree relative symlinks
under `bin/ lib/ share/`, setuid/setgid bits, and original-data-like files (`.res .am2 .am3 .smo
.dds .scr .wma .wmv .sav .exe .dll …`). `min_glibc` defaults to the build host's glibc.

## Tests

`ctest -R updater`:
- `updater.unit`: semver precedence, release selection, signature (wrong key, tampered bytes, bad
  base64) and schema rejection (product, artifact name, duplicate component, format).
- `updater.e2e` (`tests/updater/updater_e2e.py`, local `127.0.0.1` fixture, no network) with the
  test-only binary `joa-updater-testhttp` (`JOA_UPDATER_TEST_TRANSPORT`: admits
  `http://127.0.0.1:` and env `JOA_UPDATER_TEST_API`/`JOA_UPDATER_TEST_PUBKEY`; built only with
  `BUILD_TESTING`, never installed; the test asserts the shipped binary lacks these strings).
  Covers: check JSON, atomic apply of all 5 components, up-to-date no-op, rejection of hash mismatch,
  wrong-key and post-signing-edit signatures, experimental manifest on stable, path traversal,
  manifest/tag version mismatch, missing updater, downgrade — each leaving the tree byte-identical;
  lock contention; experimental apply; rollback; interrupted-update cleanup; crash between swap
  and `.state`; game/, saves, settings and HD pack untouched. `JOA_UPDATER_E2E_KEY=work/updater/dev-ed25519.key`
  signs the fixtures with the dev key instead of a throwaway one.

`ctest -L release` (`tests/release/release_checks.py`, registered by `tools/release-checks/`; all but
`release.selftest` need `-DJOA_FFMPEG_PREFIX=<LGPL prefix>`, else they exit 77):
- `release.selftest`: make_release determinism across mtimes/umask, game-data refusal, signature
  verify + tamper detection, workflow YAML structure, known_versions hashes present.
- `release.dist`: `build_release.py` → `<build>/tools/release-checks/out/dist`, signed with a throwaway key.
- `release.reproducible`: second build elsewhere, byte-identical artifacts.
- `release.appimage`: payload (4 programs, Qt wayland/xcb/offscreen plugins, LGPL FFmpeg matching
  `ffmpeg-source.json`, SDL3, notices), no host GPU/C runtime libraries, no build paths, `ldd` clean;
  `joan-native --version/--help`, `joa-updater --version`, `joa-install --help` through `AppRun`;
  launcher screenshots on offscreen, private Xvfb (xcb) and private `kwin_wayland --virtual` (wayland),
  each proving with `LD_DEBUG=files` that the platform plugin and every bundled library came from
  the AppImage.
- `release.product_update`: the real product tarballs served by a local fixture: `joa-updater-testhttp
  apply` 0.1.0 into an isolated XDG root (installed `joan-native --version`, launcher screenshot and
  shipped `joa-updater status` run from `app/current`), `check`/`apply` 0.1.1 (`current` switch,
  marker), tampered 0.1.2 rejected with the tree unchanged, `rollback` to 0.1.0, the AppImage's own
  updater reports current/previous; game/, saves, settings and HD packs untouched throughout.
- `release.install_flow` (needs `JOA_GAME_DIR`): clean XDG root: `joa-install import` + `verify` from
  the AppImage, desktop entry `Exec` = the AppImage, `joa-launcher --play -- --no-movies --frames 120`
  on a private Xvfb (llvmpipe) with the launcher's settings, game log written, `uninstall`.
- `release.license_audit`: no shipped file matches any `known_versions.json` hash or game-data name;
  the inventory covers every shipped ELF; every listed license text exists; every bundled library
  has an LGPL/permissive option; FFmpeg configure flags LGPL-only; runtime notices and the
  third-party source tarball (FFmpeg archive digest) present.

## Known gaps

- No delta/partial-component updates; each version downloads all artifacts.
- Release artifacts built on Arch need a recent glibc and libstdc++ (`min_glibc` = build host; libstdc++
  is host-provided); older distros are refused by the updater and need a build in an older container.
- No signature on the AppImage itself (it is listed in `SHA256SUMS` only; first-install trust
  comes from GitHub HTTPS).
- Bundled Arch libraries other than FFmpeg (Qt 6 LGPL-3.0, glib2, libidn2/libunistring, acl,
  keyutils … LGPL) are unmodified distribution builds: the inventory points at the exact package
  version and Arch packaging repository, but their source archives are not mirrored in the release
  (FFmpeg's and the AppImage runtime's are). Mirroring them is a publication decision.
- A desktop entry written from an updater-installed `app/<version>/bin/joa-install` points at that
  version directory (pruned after two updates); from the AppImage it points at the AppImage.
