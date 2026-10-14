# Installer / original-game import (`joa_install`, `joa-install`)

Code: `src/install/` (static lib `joa_install`, CLI `joa-install`, no Qt). Tests: `tests/install/`.
The native port ships no game data; users import their own copy. The source is only ever read.

## Layout

| Path | Owner | Content |
|---|---|---|
| `${XDG_DATA_HOME:-~/.local/share}/joan-native/game/<id>-<UTC ts>/` | joa-install | imported original files + `.joa-install.json` marker (`version`, `label`, `source`, `imported`) |
| `…/joan-native/game/current` | joa-install | relative symlink to the active version dir (what the engine reads) |
| `…/joan-native/game/<id>-<ts>.staging/`, `.extract-<pid>/`, `.lock` | joa-install | transient staging, installer extraction, `flock` |
| `…/joan-native/app/` | joa-updater | native binaries (see updater docs); never touched here |
| `${XDG_DATA_HOME}/applications/joan-native.desktop` | joa-install | desktop entry, `Exec="<launcher>"`: `$APPIMAGE` when run from the AppImage, else the `joa-launcher` next to the running binary, else `joa-launcher` on `PATH` (CLI and launcher use the same rule) |
| `${XDG_CONFIG_HOME:-~/.config}/joan-native/settings.ini` | launcher | see `docs/LAUNCHER.md` |

## Import algorithm

1. **Detect** (`detect(path)`): directory with `joan.exe` → `installed_dir`; `setup_wars_and_warriors_joan_of_arc*.exe` or an MZ file whose first 4 MiB contain `Inno Setup`/`JR.Inno.Setup` → `gog_installer`; anything else → `unknown`.
2. **Version** = sha256 of `joan.exe` looked up in `src/install/known_versions.json` (embedded into the binary with `.incbin`). Unknown hashes are refused.
3. **Validate** (cheap): every required path exists with the manifest size. Exact-case paths.
4. **Installer sources** are extracted first into `game/.extract-<pid>/` with `innoextract --extract --exclude-temp --silent --output-dir …` (no shell; must be on `PATH`), then imported from `<extract>/app`. Without innoextract the import fails with a message naming the missing tool. `--use-wine` (opt-in only) instead runs the installer under Wine in a private, temporary `WINEPREFIX` with Inno Setup's `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /NOICONS /DIR=Z:\…`. The installed game never needs Wine.
5. **Copy** each manifest file into `game/<id>-<ts>.staging/` (`FICLONE` reflink when source and destination share a btrfs/XFS filesystem, else a regular copy), then sha256 the copy against the manifest. `copy_if_present` entries (`CONFIG.DAT`, `Save/`, `map/scenario1/S1L1.PLN`) are copied unverified because the original game writes them at runtime.
6. **Publish**: `rename(staging, <id>-<ts>)`, then a new `current` symlink is created beside it and `rename(2)`d over the old one (atomic switch).
7. **Rollback**: any exception removes the staging dir (and the published dir if `current` was not switched); `current` keeps pointing at the previous install. Afterwards older version dirs/staging/extract dirs created by joa-install are pruned (only dirs carrying our marker or our naming; foreign files are never deleted).
8. Desktop entry written (unless `--no-desktop`).

Safety: the destination may not lie inside the source (checked before anything is created) and vice versa; one operation at a time via `flock(game/.lock)`.

**Repair** verifies `current`, then re-copies only missing/mismatched files from a source of the same version (directory or installer), each via `<file>.joa-repair` + rename, after checking the source copy's sha256. **Uninstall** removes the version dirs, `current`, staging/extract dirs, `.lock`, the desktop entry, and `game/` only if empty. Settings, logs, saves and `app/` stay.

## CLI contract

```
joa-install [--json] [--dest GAME_ROOT] detect <path>
joa-install [--json] [--dest GAME_ROOT] import <path> [--use-wine] [--no-desktop]
joa-install [--json] [--dest GAME_ROOT] verify
joa-install [--json] [--dest GAME_ROOT] repair <source> [--use-wine]
joa-install [--json] [--dest GAME_ROOT] uninstall
joa-install manifest <dir> --id ID --label LABEL      # dev: prints a known_versions.json document
joa-install [--json] [--dest GAME_ROOT] hd-import <dir> [--name N]   # HD texture pack, see below
```

`GAME_ROOT` defaults to `$XDG_DATA_HOME/joan-native/game`. Exit: 0 ok, 1 failure (or verify/detect not ok), 2 usage.
Progress: stderr lines `progress <0-100> <message>` (deduplicated per percent). With `--json`, stdout is exactly one JSON object:

- detect: `{"ok","kind":"installed_dir|gog_installer|unknown","path","exe_sha256","version":{"id","label","required_files"}|null,"missing":[],"wrong_size":[],"extractor_available","notes":[]}` (`ok` = known version with all files, or installer with innoextract available)
- import: `{"ok":true,"installed","current","version","desktop_entry"}`
- verify / repair: `{"ok","version","checked","missing":[],"mismatched":[]}`
- uninstall: `{"ok":true,"removed"}`
- hd-import: `{"ok":true,"pack","name","copied","identical_skipped"}`
- any error: `{"ok":false,"error":"…"}`

## HD texture packs (`hd-import`)

`hd-import <dir>` copies the `texture/**` `.dds`/`.png` files of an already-extracted HD pack (e.g. the
user's own download of the community HD Update) into `<GAME_ROOT>/../hd/<N>` (default
`$XDG_DATA_HOME/joan-native/hd/hd-update`, where the engine's `hd_packs=<N>` looks). Files byte-identical
to `GAME_ROOT/current` (case-insensitive path) are skipped; executables, `CONFIG.DAT`, models and `int8x6/`
are never copied. Staged in `hd/.<N>.staging`, then renamed; an existing `<N>` is refused (never
overwritten). The source is only read. Layout, priority and rights: `docs/HD_ASSETS.md`.

## Known versions

| id | label | joan.exe sha256 | required files |
|---|---|---|---|
| `gog-59908888400692417` | GOG build 59908888400692417 (joan.exe 2004-01-19) | `b25e2154af3ffc876545af60a2c8b90380d4327541cd38841ef327411c3b2fa5` | 9564 |

CONFIRMED: generated from this GOG install (`goggame-1421210170.info` gameId 1421210170, buildId 59908888400692417) with `joa-install manifest . --id gog-59908888400692417 …`; `sha256sum joan.exe` gives the hash above. The manifest holds paths, sizes and sha256 only (no game data).
Excluded from the manifest: port/repo files, GOG metadata (`goggame-*`, `goglog.ini`, `gog.ico`, `support.ico`, `unins000.*`, the `.lnk`, `weblinks/`), the Windows Media redistributable `util/wmfdist.exe`, and the runtime-written `CONFIG.DAT`, `Save/`, `map/scenario1/S1L1.PLN` (copied unverified). HIGH CONFIDENCE that `S1L1.PLN` is generated by the game: its mtime (20:43:27) matches `CONFIG.DAT`'s, ~3 min after the other files, and it is absent from GOG's hashdb.

Cross-check, CONFIRMED: `goggame-1421210170.hashdb` (ZIP; 9567 records of `path[1024]`+`md5[32]`, format from the ModsResearch agent) lists exactly the 9564 manifest paths plus `util/wmfdist.exe` and the two `weblinks/*.url`; no manifest path is missing from it.

Adding a release: run `manifest` on it and merge the `versions` entry. Regenerating for this install is deterministic (only diff after the PLN change was that line).

## Source formats

| Source | Status |
|---|---|
| Installed GOG dir | CONFIRMED working: `detect` + `import` + `verify` + `repair` run against this install (see below). |
| GOG offline installer `setup_wars_and_warriors_joan_of_arc_*.exe` (+ `-N.bin`) | HYPOTHESIS: Inno Setup (GOG's standard; files under innoextract's `app/`); no installer has been available to test. Flow covered by a test with a fake `innoextract`. innoextract is not installed on the dev host (`pacman -Si innoextract` → cachyos-extra-v3 1.9). |
| Retail CD (2004) | UNKNOWN: no sample seen; `detect` reports it as unsupported. A retail install directory whose `joan.exe` hash is unknown is refused until its manifest is added. |
| Wine-assisted | Opt-in only (`--use-wine` / launcher checkbox); untested without an installer. |

## Evidence (2026-10-13)

- `joa-install detect "$JOA_GAME_DIR" --json` → `kind installed_dir`, version `gog-59908888400692417`, 0 missing, 0 wrong size.
- Import to a tmpfs XDG dir (`XDG_DATA_HOME=/tmp/joa-li-xdg-*/data`): ok, 932 MiB, 1.1 s warm cache; `verify` → all files checked, ok. After corrupting `Resource/ANIM_AM3.RES` and deleting `texture/3dmapicon.dds`, `verify` exits 1 listing both; `repair "$JOA_GAME_DIR"` → ok. `uninstall` then leaves `joan-native/` empty.
- Import with `XDG_DATA_HOME` inside the game dir is refused before writing ("lies inside the source").
- Manifest generation over the cold HDD install: 57 s.

## Gaps

- Case-insensitive source lookup (differently-cased retail copies) not implemented; added when such a release appears.
- Hashing is sequential (sources are often on HDDs).
- Original `Save/` is copied into the version dir as-is; migration into native saves belongs to the engine.
