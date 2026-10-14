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

The repository is fixed at build time: CMake cache var `JOA_UPDATE_REPO` (default placeholder
`OWNER/joan-native`; the release workflow sets `${{ github.repository }}`).

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
  `JoanNative-<version>-x86_64.AppImage`, a source tarball and `SHA256SUMS`.

Component split (`make_release.py`, first match wins): `bin/joa-updater` → updater;
`bin/joa-launcher`, `bin/joa-install`, `share/{applications,icons,metainfo}/*` → launcher;
`share/joan-native/shaders/*`, `*.spv` → shaders; other `bin/joa-*` → tools; everything else → engine.
Every update installs all components of the version (each `app/<version>` is complete).

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

## Releasing

CI (`.github/workflows/ci.yml`): archlinux container (Ubuntu 24.04 lacks SDL3), configure, build,
`ctest` as an unprivileged user. Asset-backed tests exit 77 without game data.

Tag release (`.github/workflows/release.yml`), on `vMAJOR.MINOR.PATCH[-pre]`:
1. tag core must equal `project(joan_native VERSION ...)`; `-pre` tags → experimental + GitHub prerelease;
2. verify signing secret ↔ `update_pubkey.txt`;
3. Release build with `-DJOA_UPDATE_REPO=$GITHUB_REPOSITORY`, ctest, `cmake --install` to a stage;
4. `make_release.py` + `sign_manifest.py`; AppImage via pinned, SHA-256-checked linuxdeploy
   `1-alpha-20251107-1` + linuxdeploy-plugin-qt `1-alpha-20250213-1`;
5. **draft** GitHub release. Edit the notes (they become `check`'s `notes`), qualify, then publish.

Local signing alternative (no CI secret), from a clean checkout:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DJOA_UPDATE_REPO=OWNER/NAME
cmake --build build-release && ctest --test-dir build-release
cmake --install build-release --prefix work/release/stage
python3 packaging/release/make_release.py --prefix work/release/stage --version 0.2.0 --channel stable --output-dir work/release/dist
python3 packaging/release/sign_manifest.py work/release/dist/manifest.json --key ~/.local/share/joan-native-release-keys/update-ed25519.key
LINUXDEPLOY=/path/linuxdeploy-x86_64.AppImage QMAKE=qmake6 sh packaging/appimage/build_appimage.sh work/release/stage 0.2.0 work/release/dist
gh release create v0.2.0 --draft --title "Joan of Arc native 0.2.0" --notes-file NOTES.md work/release/dist/*
```

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

## Known gaps

- `JOA_UPDATE_REPO` is a placeholder until the public repository exists.
- No delta/partial-component updates; each version downloads all artifacts.
- Release artifacts built on Arch need a recent glibc (`min_glibc` = build host); older distros are
  refused by the updater and need a build in an older container.
- The release AppImage bundles libraries via linuxdeploy; it has only been smoke-tested with the
  updater binary, not with the launcher/engine (not yet built).
- No signature on the AppImage itself (it is listed in `SHA256SUMS` only; first-install trust
  comes from GitHub HTTPS).
