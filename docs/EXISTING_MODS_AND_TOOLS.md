# Existing community mods and tools

Research date: 2026-10-13. Reference downloads are kept only in `work/third_party_ref/` (gitignored) and are **not** redistributed. Camera/FOV internals are in [CAMERA_AND_WIDESCREEN.md](CAMERA_AND_WIDESCREEN.md); rights in [ASSET_RIGHTS.md](ASSET_RIGHTS.md).

Our reference build: GOG `joan.exe`, 6,677,952 bytes, SHA-256 `b25e2154…3b2fa5` (full hashes in [SOURCE_AUDIT.md](SOURCE_AUDIT.md)).
Other known `joan.exe` sizes referenced by mods: **6,672,428** (= our GOG exe without its Authenticode signature, CONFIRMED in CAMERA_AND_WIDESCREEN §3) and **6,418,432** (the exe shipped inside the HD Update; a different build with extra sections `.ekvam`/`.znks` and an entry point outside `.text` — HYPOTHESIS: a protection-wrapped retail exe, PCGamingWiki lists SecuROM 5 / StarForce 3 for retail discs).

## Summary

| Project | Author | Available | Source | License | Targets | Mechanism | Native reproduction |
|---|---|---|---|---|---|---|---|
| FOV Fix v1.0 | AlphaYellow | yes (GitHub release, PCGW) | yes | MIT | any `joan.exe` containing the signature (GOG and HD exe both match) | ASI plugin, 2 runtime mid-hooks in the perspective builder | yes — Hor+ projection, see camera doc |
| Camera Fix Patch | Urielik (Beniamin Gajecki) | yes (GitHub repo, linked from ModDB) | patch only (xdelta) | none stated | 6,672,428 B (GOG unsigned) / 6,418,432 B (HD exe) | static exe patch: 6 × NOP over one `fmul` | yes — float yaw accumulation |
| Joan of Arc: HD Update v0.95 | FanSH (1080p exe patch credited to LexPest) | yes (ModDB, 1.21 GB) | n/a (assets) | none stated | replaces `joan.exe` with its own 6,418,432 B exe | asset replacement + replaced exe with patched 1920×1080 display profile | assets: user-installed override pack; resolution: native renderer makes it obsolete |
| Native 1080p & Mouse-Fix (Steam guide 2786233497) | aFF3_M!T_WaFF3 | guide; config on Google Drive | config | none | any | dgVoodoo 2 D3D8 wrapper config | obsolete natively |
| RTX Remix config (Steam guide 3716805017; ModDB file "Remix Config", 2026-05-03) | 1 MOL NH3 | yes | config | not reviewed | any | NVIDIA Remix runtime + `rtx.conf` | out of scope; reference for lighting only |
| Orlean | "Game modding" (ModDB) | yes (ModDB) | n/a | none stated | any | asset/script replacement (replaces Joan with "Orlean") | not needed |
| Arthur and Bastard replacer | ModDB upload 2023-01-13 | yes | n/a | none stated | any | asset replacement (Lahire/Alençon) | not needed |
| VSetting.exe registry fix | PCGamingWiki | yes | n/a | n/a | Steam/GOG | add `HKLM\SOFTWARE\WOW6432Node\Enlight Software\Joan of Arc` (`Path`, `Version`=`1.00`, `English`, `France`) | not needed: our launcher replaces VSetting |
| Fan translations (PL, RU, ES, DE Steam guides) | various | guides | n/a | none | any | text/audio file replacement | out of scope; asset-override system must not block them |

## Sources consulted

- PCGamingWiki: <https://www.pcgamingwiki.com/wiki/Wars_and_Warriors:_Joan_of_Arc> (wikitext via MediaWiki API, revision 1810276, saved in `work/third_party_ref/pages/pcgw_wikitext.txt`). Facts: Direct3D 8, 32-bit exe only, no 64-bit; widescreen "hackable" via the HD Update 1080p fix; ultrawide/4K/multi-monitor "hackable" via dgVoodoo 2 + FOV fix; no controller support; key remap, mouse sensitivity and invert-Y present; config `CONFIG.DAT`, saves `SAVE\`; Steam/GOG builds lack the registry keys VSetting needs; "Unresponsive camera controls → reduce mouse polling rate"; retail DRM SecuROM 5 (StarForce 3 Russian release); GOG/GamersGate DRM-free, Steam uses Steam DRM.
- Steam community app 294590: <https://steamcommunity.com/app/294590/guides/> — 11 guides; relevant: HD Update (811067496), Native 1080p & Mouse-Fix (2786233497), RTX Remix (3716805017), localizations.
- ModDB game mods: <https://www.moddb.com/games/wars-and-warriors-joan-of-arc/mods> (3 mods: Orlean, Camera Fix Patch, HD Update) and downloads: <https://www.moddb.com/games/wars-and-warriors-joan-of-arc/downloads> (4 files).
- AlphaYellow repo: <https://github.com/alphayellow1/AlphaYellowWidescreenFixes>; PCGW file: <https://community.pcgamingwiki.com/files/file/3309-wars-and-warriors-joan-of-arc-fov-fix>.
- Camera Fix: <https://www.moddb.com/mods/camera-fix-patch>, <https://github.com/bgajecki/joa-camera-fix-patch>.
- HD Update: <https://www.moddb.com/mods/joan-of-arc-hd-update>, <https://www.moddb.com/mods/joan-of-arc-hd-update/downloads/hd-update>, Steam guide <https://steamcommunity.com/sharedfiles/filedetails/?id=811067496>.

## 1. FOV Fix (AlphaYellow)

- Availability: GitHub release `warsandwarriorsjoanofarc` (2025-02-28) and PCGW community file 3309 (v1.0). Downloaded to `work/third_party_ref/fovfix_release/`; source cloned to `work/third_party_ref/AlphaYellowWidescreenFixes/` (commit `99a5f300`).
- Source: complete, `source/fixes/WarsAndWarriorsJoanOfArcFOVFix/dllmain.cpp` + shared `includes/helper.hpp`. License MIT.
- Version support: detects only the file name `joan.exe`; both signatures are present exactly once in our GOG exe (file `0x3674D1`, `0x3674D9`) and in the HD exe (`0x366403`) — CONFIRMED by byte search.
- Architecture: 32-bit ASI DLL (PE i386); needs Ultimate ASI Loader (`winmm.dll`, 32-bit). Deps: safetyhook, spdlog, inipp.
- Mechanism: mid-function hooks (no file patch). Replaces the 4:3 aspect argument of the perspective builder with `Width/Height` and optionally scales the 45° vertical FOV by `FOVFactor`. Does not touch resolution, UI or mouse.
- Limitations: needs a wrapper (dgVoodoo 2) to output widescreen at all; UI stays stretched; exact float compares; Windows/Wine only.
- Native: fully reproducible (Hor+ rule).

## 2. Camera Fix Patch

- Availability: ModDB page (2025-07-14) points to GitHub; repo cloned to `work/third_party_ref/joa-camera-fix-patch/` (2 commits, 2025-07-14). No ModDB-hosted file.
- Source: none beyond the binary deltas; no license file.
- Version support: `joan-636.xdelta` = GOG exe with signature stripped (CONFIRMED by Adler-32 reproduction); `joan-612.xdelta` = HD Update exe layout (HIGH CONFIDENCE).
- Mechanism: exe patch, 6 bytes `D8 0D B4 FB 9C 00` (`fmul dword [0x9CFBB4]`, ×0.2) → `90 90 90 90 90 90` at file `0x31893` / VA `0x431893` in our build. Removes the ×0.2 factor of the integer-truncated mouse-turn step (5× turn rate in the default mode).
- Not applicable directly to our signed GOG exe without first stripping the signature (xdelta3 does not verify the source; the result would be checksum-mismatched but functional).
- Native: reproducible by float yaw accumulation (no truncation) with the original sensitivity curve.

## 3. Joan of Arc: HD Update v0.95 [FINAL]

- Availability: ModDB file `HDUpdate.rar`, 1,302,778,072 bytes, MD5 `73c1eebb67386df510df4123ffc48fd3` (CONFIRMED: our download matches), uploaded 2020-08-28 by FanSH. Steam guide says Google-Drive mirrors were removed after complaints; ModDB is the only source. Non-interactive download works: `curl -sI` on `/downloads/mirror/199756/…` returns a signed `dl.dbolical.com` URL which plain `curl` can fetch (the `/downloads/start/` page itself is behind a Cloudflare challenge).
- Extracted to `work/third_party_ref/hd_update/x/` (1,203 archive entries, 1.99 GB unpacked). Inventory: `work/third_party_ref/hd_update/inventory.tsv`, produced by `work/third_party_ref/hd_inventory.py`.
- Contents:
  - `texture/**` (item, misc, particle, terrain, unit, root) and `int8x6/*.dds` (UI) — texture replacements.
  - `joan.exe` (6,418,432 B, SHA-256 `e4b3c08d60267314ac603d9c6382c6e807ba886a4634eaf2655cea9cf98c7351`, link time 2004-01-19 `0x400BA48D`), `VSetting.exe` (1,495,093 B), `CONFIG.DAT` (2,821 B) — also duplicated inside the nested `Joan_1080p.rar` (`FIX/`), byte-identical.
  - `Better Trees for all Worlds (install optional)/item/Object/Models_Items/Vegetation/Trees/HighPolyTrees/joantree05.SMO` (16,402 B, replaces our 25,448 B model) + README ("rare trees hanging in the air").
- 1080p mechanism: the bundled exe's single display profile has width/height `1920/1080` and bounds `1919/1079` where ours has `1024/768` and `1023/767` (our VA `0xA016A4..0xA016DC`; CONFIRMED by aligned `.data` comparison). The UI DDS files in `int8x6/` were edited/shrunk to hide menu overlap at 1080p (author's Steam comment). The game still has one fixed mode; other resolutions are impossible without further patching (ModDB user reports of 1080p not starting/windowed menus).
- `CONFIG.DAT` differs from ours in 9 bytes (offsets 55, 58, 60, 65, 153, 157, 160–161, 164) — option values, not resolution; semantics UNKNOWN here.

### 3.1 Texture inventory (HD vs original, 1,196 image files)

Classification: same path compared case-insensitively against the original install; DDS headers parsed for size/format/mip count; for differing images the HD texture is box-downscaled to the original size and the luminance correlation computed with Pillow/numpy. Thresholds are heuristic → classes are HYPOTHESIS per file, the counts are CONFIRMED.

| Class | Count | Meaning |
|---|---|---|
| IDENTICAL to original | 377 | byte-identical original files repackaged in the mod (SHA-256 equal) |
| UPSCALE-of-original (corr ≥ 0.85, larger) | 506 | derived from the original — consistent with the stated "AI upscale ×4 + Photoshop" |
| EDITED-from-original (0.5 ≤ corr < 0.85, larger) | 92 | upscaled and noticeably repainted |
| REPLACED/new-content (corr < 0.5, larger) | 185 | different image content (new photo-sourced or library textures) |
| SAME-RES edit / replaced | 8 / 17 | equal or smaller size; terrain tiles (`sand_L0*`, `grasspath*`, `chif00*`, …) and waterfalls replaced outright |
| UNDECODED | 10 | Pillow could not decode one side (sky boxes, `Slash_02`, `water`, `WhiteSmoke1`, `treeshadowmap`, `menu_a`) |
| non-image modified | 1 | `texture/misc/glow2dlight.tga` |

Per directory (HD scale factor = HD width / original width):

| Dir | HD files | Originals in dir | Scale factors |
|---|---|---|---|
| `texture/item` | 654 | 654 | 4× 437, 2× 139, 8× 28, 16× 2, same 3, identical 43, other 2 |
| `texture/unit` | 111 | 111 | 2× 77, 4× 29, 8× 3, identical 2 |
| `texture/terrain` | 57 | 2,886 | 2× 44, same-size 11, 4× 1, identical 1 (most of the 2,886 terrain files untouched) |
| `texture/misc` | 245 | 245 | identical 217, 2× 12, 4× 4, same-size 5 (sky boxes), 8×/16×/32× 4, non-uniform 2, `.tga` 1 |
| `texture/particle` | 73 | 73 | identical 70, 4×/8×/16× 1 each |
| `texture` (root) | 44 | 44 | identical 44 |
| `int8x6` | 12 | 532 | same size 7, downscaled ×0.5/0.25/0.125 4, 1×4 1 |

Most common HD sizes: 1024² (365), 2048² (173), 512² (112), 128² (95), 4096² (85). Format changes are common (e.g. 67 × DXT1→DXT3, 26 × DXT3→DXT1, 19 × DXT5→DXT3; HD unit textures often lose their mip chain: `williamdepole.dds` 512² DXT3 10 mips → 2048² DXT3 0 mips). Example: `texture/unit/talbot.dds` 512² DXT3 → 1024² DXT1, corr 0.94 (upscale).

- Dependencies: none beyond the game; installs by overwriting files (and the exe).
- Compatibility: replaces `joan.exe` → incompatible with the GOG/Steam exe-based fixes unless the matching `-612` Camera Fix is used; the FOV fix still hooks it.
- Native: textures can be loaded through our optional override directory (user installs the pack separately; we never ship it). The bundled exe and the 377 identical original files must be ignored/never redistributed. Missing mips must be generated at load time.

## 4. Other tools

- dgVoodoo 2 (<https://github.com/dege-diosg/dgVoodoo2>, freeware, closed source): D3D8→D3D11/12 wrapper used to force native resolution; the cause of the stretched 4:3 image the FOV fix compensates for.
- Ultimate ASI Loader (ThirteenAG, MIT): required by the FOV fix.
- RTX Remix runtime: config only; useful as a visual reference for path-traced lighting, not used.

## Compatibility matrix (Windows mods with each other)

| | FOV Fix | Camera Fix 636 | Camera Fix 612 | HD Update exe |
|---|---|---|---|---|
| GOG exe (signed) | yes | needs signature stripped | no | replaced |
| HD Update exe | yes | no | yes | — |

None of these is needed by the native port; they are references only.
