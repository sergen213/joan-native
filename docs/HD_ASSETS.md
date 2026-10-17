# HD textures and materials (override packs)

Code: `src/engine/hd_pack.{hpp,cpp}` (pack resolution, lookup, material class, PNG/DDS loading),
`src/engine/assets_cache.cpp` (overrides + `IRenderer::set_material_maps`), `src/engine/png.cpp`
(`decode_png`), `tools/hd-upscale/`, `joa-install hd-import` (`src/install/`). Tests: `engine.hd_pack`,
`tools.hd_upscale`, `install.hd_import`. Spec: `goal.txt` §13. Rights: `docs/ASSET_RIGHTS.md`.

The port ships no HD content. A pack is a read-only directory the user creates (from their own game
with `hd-upscale`) or imports (the community HD Update, downloaded by the user). Packs are never
written by the engine; original files are always the fallback.

## Layout and priority

A pack mirrors the original relative texture paths, matched **case-insensitively per path component,
either slash** — exactly the community "Joan of Arc: HD Update v0.95" layout:

```
<pack>/texture/item/Church.dds        overrides texture/item/church.dds
<pack>/texture/unit/talbot.png        overrides texture/unit/talbot.dds
<pack>/texture/terrain/grass01_n.png  normal map for texture/terrain/grass01.dds
<pack>/int8x6/menu1024.png           UI art / font atlas (int8x6/menu1024.dds), see "UI" below
<pack>/resource/i_cursor/0.png        UI pointer: Resource/I_CURSOR.RES record at byte offset 0
<pack>/save/…                         indexed too
```

- Only `texture/`, `int8x6/`, `save/` and `resource/` below the pack root are indexed (one walk at startup),
  so a pack may be a whole mod extraction; executables, `CONFIG.DAT`, models etc. are ignored.
- Accepted files: same stem with `.dds` (DXT1–5 and bit-mask formats via `formats::parse_dds`, file mip
  chain kept while each level halves exactly, else GPU mips) or `.png` (non-interlaced 1–16 bit grey,
  RGB, palette+tRNS, grey-alpha, RGBA; GPU mips). `.dds` wins over `.png` of the same stem in one pack.
  TGA/BMP overrides are not read (no decoder in `joa_formats`; the engine loads `.dds` originals only).
- An override is found through the **original** file: the engine's texture rule (base name in
  `INT8X6\`, `INT8X6\ITEM\`, `SAVE\`, `TEXTURE\`, `TEXTURE\{ITEM,UNIT,TERRAIN,PARTICLE,MISC}\`, first
  wins) resolves the original, and its path relative to the game root is looked up in the packs. A pack
  file without an original counterpart is never used.
- Packs are searched in order; **the first pack holding the albedo wins**. Any size is accepted
  (non-power-of-two too); `has_alpha` (alpha-test foliage, fences) is recomputed from the override,
  `repeat`/clamp is kept from the caller.
- Unreadable/corrupt override → one log line, original used.
- Lightmaps (`texture/terrain/<map>_scene_sector*`) go through `AssetCache::texture_pixels` and are
  overridable, but the atlas needs every sector tile of a map at one square size: override all tiles of
  a map or none (`hd-upscale` skips them by default).

### Settings

| Where | Key | Value |
|---|---|---|
| `settings.ini` | `[graphics] hd_packs=a,b` | comma list, first wins |
| CLI | `--hd-packs a,b` | same; overrides the file |

Entries are pack **names** under `${XDG_DATA_HOME:-~/.local/share}/joan-native/hd/<name>/` or
**absolute directories**. Missing entries are skipped with a log line. HD packs are independent of the
graphics preset (they also work with `Original`). Log at startup: `joan-native: hd pack <dir>: N textures`;
at shutdown: `joan-native: hd: N textures overridden (M pack files)`.

## UI (menus, HUD, briefing, fonts, cursor)

`Ui` (`ui_core.cpp`) uses the same `hd_packs` (no separate setting): every `int8x6/<name>.dds` it loads —
menu/HUD/brief/minimap atlases, inventory icons (`int8x6/item/`), and the bitmap font atlases — and the
`CURSOR.RES` pointers (`resource/i_cursor/<I_CURSOR.RES byte offset>.png`, 32x32 originals) are replaced
by a pack file of any size. Layout never changes: sprite UVs (`INTERFAC.SET` texel rects), glyph rects and
widths (`.fin` / grid metrics, measured on the **original** atlas) are normalized by the original size, so
an HD atlas is sampled at the same relative positions; rotated `INTERFAC.SET` records are baked from the
scaled rect. HD UI images are uploaded with mipmaps (a 4x pack is minified below UI scale 4, e.g. 1440p and
1080p) and the UI shaders sample with a −0.5 LOD bias so a 4K screen (UI scale 2.81) stays on the sharp
level. The atlas-bleed clamp stays half an **original** texel (`UiQuad::clamp_inset`): the upscaler softens
alpha across sprite-rect borders by ~2 pack texels, which a half-pack-texel clamp would show as a faint seam
(minimap tile edges). Without a pack nothing changes (no mips, no inset, same texels as before;
`*_ui_frontend` 1024x768 1:1). Log at shutdown: `ui: hd: N images overridden`.

Text: the fonts are the original bitmap atlases upscaled like the art (no TTF ships with the game and none
is bundled; Goudy/Verdana/Celtic glyph shapes and metrics stay the original ones).

## Companion material maps

Next to an albedo `<stem>` a pack may hold `<stem>_n`, `<stem>_r`, `<stem>_m`, `<stem>_ao` (`.dds`/`.png`):

| Suffix | Meaning | Channel |
|---|---|---|
| `_n` | tangent-space normal map | RGB = XYZ * 0.5 + 0.5 |
| `_r` | perceptual roughness | R |
| `_m` | metalness | R |
| `_ao` | ambient occlusion | R |

- Source pack: if the albedo is overridden, companions come **only from that same pack** (a pack's maps
  belong to its own pixels). If the albedo is the original, each companion comes from the first pack
  that has it (a "materials only" pack works for original textures).
- Uploaded as `Rgba8Unorm` (no sRGB decode), same repeat/clamp as the albedo, mipmapped.
- **Normal map convention: green = +V pointing UP in the image (OpenGL-style, "Y+")**: UV (0,0) is the
  top-left texel, so a green value of 255 tilts the normal toward the top edge of the image (toward
  decreasing v). Blender/Substance "OpenGL" exports match; DirectX-style maps (3ds Max, Unreal,
  Substance "DirectX") need their green channel inverted. Blue is the outward (+Z) axis.
- Every loaded texture gets `IRenderer::set_material_maps(albedo, maps)` (also without companions) with a
  `MaterialClass` from the original's directory/name (`material_class()`):

| Original | Class |
|---|---|
| `texture/terrain/*` | Terrain |
| `texture/unit/*` | Character |
| `texture/particle/*` | Particle |
| `texture/item/*` weapons/armour/loot: `sword`/`sowrd`, `axe`, `mace`, `spear`, `lance`, `shield`/`sheld`, `helm`, `armor`/`armour`, `bow` (not `bowl`), `arrow`, `halb`, `hammer`, `dagger`, `flail`, `club`, `weapon`, `itm_`, `bmlb`, `gold`, `key`, `crystal`, `quartz`, prefixes `sw_ cb_ rg_ at_ gs_ pa_ ma_ hr_` | Item |
| `texture/item/*` with `tree`, `grass`, `bush`, `leaf`/`leaves`, `plant`, `flower`, `pine`, `corn`, `vegetation`, `straw`, `root` | Vegetation |
| other `texture/item/*` (houses, walls, props) | Building |
| everything else (`texture/misc` sky/water, `texture/`, `int8x6`, `save`) | Generic |

The keywords were checked against the 654 shipped `texture/item` names (HYPOTHESIS per name: e.g.
`armor_alencona` is a character armour texture stored in `item/` and becomes Item).

## Importing the community HD Update (user's own download)

The HD Update (FanSH, ModDB `HDUpdate.rar`, MD5 `73c1eebb67386df510df4123ffc48fd3`) has no license; we
never bundle, mirror or download it (`docs/ASSET_RIGHTS.md`). The user downloads and extracts it, then:

```
bsdtar -xf HDUpdate.rar -C ~/Downloads/hdupdate      # or unrar x
joa-install hd-import ~/Downloads/hdupdate            # -> ~/.local/share/joan-native/hd/hd-update
joan-native --hd-packs hd-update                      # or settings.ini [graphics] hd_packs=hd-update
```

`hd-import` copies only `texture/**` `.dds`/`.png`, skips files byte-identical to the imported game
(`$XDG_DATA_HOME/joan-native/game/current`), refuses an existing pack name, and stages into
`hd/.<name>.staging` then renames. It never copies the mod's `joan.exe`, `VSetting.exe`, `CONFIG.DAT`,
the `Better Trees …/joantree05.SMO` model, or `int8x6/` (those UI files were shrunk to hide menu overlap
at the patched exe's fixed 1080p; our UI scales itself). Pointing `--hd-packs` straight at the extracted
directory (absolute path) also works without importing.

What it contains (`work/third_party_ref/hd_update/inventory.tsv`, analysis in
`docs/EXISTING_MODS_AND_TOOLS.md` §3.1; per-file classes are heuristic, counts CONFIRMED):

| Class | Files |
|---|---|
| upscale of the original | 506 |
| upscaled and repainted ("edited") | 92 |
| replaced / new content (larger) | 185 |
| same-resolution edit / replacement | 8 / 17 |
| identical to the original (skipped by `hd-import`) | 377 |
| undecoded by the analysis (sky boxes, water, …) | 10 |
| non-texture (exe, VSetting, CONFIG.DAT, tree model) | 4 + 2 |

Engine check (2026-10-13, S1L1, 1920x1080, GL, offscreen): the pack indexes 1059 textures and 79 S1L1
textures are overridden (terrain, grass, rocks, trees, armour). Captures: `work/remaster/hd_check_orig.png`
(original) vs `work/remaster/hd_check.png` (`--hd-packs <abs>/work/third_party_ref/hd_update/x`).

**Models are not overridden.** Packs only replace pixels; geometry, collision and placement always come
from the original data. The HD Update's optional `Better Trees …/joantree05.SMO` changes a mesh (its own
README notes "rare trees hanging in the air"), would change shadow volumes/placement, and lives outside
the texture tree; it is ignored.

## hd-upscale: an enhanced pack from your own textures

`tools/hd-upscale/hd-upscale` (Python 3 stdlib; Pillow optional for faster Lanczos resizing) decodes the
original DDS files (DXT1/2/3/4/5, A8R8G8B8 and other bit-mask formats, top level), upscales colour and
alpha **separately** (alpha as a grey image, re-merged), rounds each edge to the nearest power of two
(capped by `--max-size`, default 4096), and writes a pack mirroring the original paths. No network.

```
tools/hd-upscale/hd-upscale --dry-run                             # list planned operations, write nothing
tools/hd-upscale/hd-upscale --scale 4 --model realesrgan-x4plus   # -> ~/.local/share/joan-native/hd/upscaled
tools/hd-upscale/hd-upscale --include 'int8x6/*' --include 'resource/i_cursor/*'   # UI only (~15 min)
joan-native --hd-packs upscaled                                    # or settings.ini [graphics] hd_packs=upscaled
```

| Option | Default | Meaning |
|---|---|---|
| `--game-dir` | `$XDG_DATA_HOME/joan-native/game/current` | original game (read only) |
| `--out` | `$XDG_DATA_HOME/joan-native/hd/upscaled` | pack directory |
| `--scale 2\|4` | 4 | target factor (before power-of-two rounding) |
| `--model` | `realesrgan-x4plus` | `-n` model; `realesrgan-x4plus(-anime)` run at x4 then resize, others with `-s <scale>` |
| `--format png\|dds` | png | `dds` = DXT1 (opaque) / DXT5 (alpha) through `compressonatorcli`, `nvcompress` or `crunch` if on PATH, else PNG with a message (encoder command lines UNTESTED here: none installed) |
| `--include GLOB` / `--exclude GLOB` | `texture/*`, `int8x6/*` (UI art, fonts), `resource/i_cursor/*` (UI pointers) / `*_scene_sector*` | fnmatch on the lowercase relative path (cursors: `resource/i_cursor/<offset>.png`), repeatable |
| `--tool PATH` | `realesrgan-ncnn-vulkan` from PATH | upscaler executable |
| `--force` | off | redo textures whose output exists (default: skip, so runs resume) |
| `--dry-run` | off | print `<rel> WxH -> TxT <out>` lines, write nothing, exit 0; automatic when the upscaler is missing |

Installing the upscaler (Vulkan, any GPU): Arch/AUR `realesrgan-ncnn-vulkan`, or the release zip from
<https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases> (binary + `models/` directory; pass
`--tool <dir>/realesrgan-ncnn-vulkan`). Models shipped there: `realesrgan-x4plus` (photo-like, best for
these painted/photo textures), `realesrgan-x4plus-anime` (flat colour), `realesr-animevideov3` (x2–x4).
License: the ncnn-vulkan program is MIT (Real-ESRGAN's PyTorch code is BSD-3-Clause); the model weights
are distributed by the Real-ESRGAN project under its own terms — check them before redistributing
outputs. Outputs are derivatives of Enlight's textures: keep them local, never redistribute
(`docs/ASSET_RIGHTS.md`).

Status on the development machine (2026-10-17): the UI set (`int8x6/*` + cursors) was generated with
the release zip's `realesrgan-x4plus` (RX 7900 XTX); world textures still only through `tools.hd_upscale`
with a fake nearest-neighbour upscaler. `x4plus` was kept for the UI too: `x4plus-anime` gave equally
clean glyphs but repainted the HUD portraits (`work/uiupscale/models_*.png`).

## Gaps

- The briefing's 3D map (`hud_brief.cpp` `map_render`) is CPU-rasterised from the original
  `texture/*.dds` files (nearest texel), not through `AssetCache`: packs do not affect it.
- Water bump frames and `texture/misc/waterpool.tga` (`water.cpp`) and the video player bypass the
  cache too. Sky dome clouds, particles, terrain, models, grass and lightmaps go through it.
- No compressed GPU upload: overrides are uploaded as RGBA8 (a 4096² texture with mips is ~89 MB of
  VRAM). The HD Update's S1L1 set ran fine on a 24 GB GPU; small GPUs may need fewer/smaller packs.
- Pack files are only indexed at startup (no hot reload).
