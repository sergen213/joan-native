# Containers, world/map data, fonts, config and saves

Owner: WorldFormats. Code: `src/formats/{vfs_bytes,vfs_fs,res_archive,world_text,map_grid,font_fin,config_dat,save_sav}.*`,
tests `tests/formats/{vfs_fs,res_archive,world_text,map_grid,font_fin,config_dat,save_sav}_test.cpp`, tool `tools/joa-res`.
All integers little-endian. Addresses are `joan.exe` VAs (image base 0x400000) from the Ghidra export in
`work/ghidra/export/joan_decomp.c` (decompiler output, not verified source) plus `objdump` where noted.
"Validated" = the CTest named in brackets parses every instance in the GOG install.

Confidence labels: **CONFIRMED** (code + every data file agree), **HIGH** (data fits exactly, code read partially),
**HYPOTHESIS**, **UNKNOWN**.

## 1. File I/O conventions of the engine (7KAA lineage)

- `File` object (`FUN_00586da0` open-read, `FUN_00586f00` open-write). Third open argument = *record-size mode* (byte +0x10d).
  **CONFIRMED** (`FUN_005872e0` write / `FUN_005873a0` read):
  - mode on: every `File::write(buf,n)` emits `u16 n` then `n` bytes; `n >= 0x10000` emits length `0`. The reader copies
    `min(stored, wanted)` bytes, skips the rest and zero-fills a short record.
  - `put_short`/`put_long` (`FUN_00587ba0`/`FUN_00587da0`) and `get_*` never carry a prefix.
  - Consequence: record-mode files (SAV, PFD, `.map`) are **not self-describing**; a generic walker fails as soon as a raw
    `put_*` or a >=64 KiB record appears.
- Paths are built from the executable's directory (`PTR_DAT_00a1579c`) + relative names with `\` and arbitrary case
  (`"SAVE\\"`, `"FONT\\"`, `"MAP\\..."`). The VFS (`vfs_fs.hpp`) resolves these case-insensitively. **CONFIRMED**.

## 2. Resource containers

### 2.1 Index container, two variants — HIGH (layout fits all 35 instances byte-exactly) [formats.res_archive]

| Variant | Header | Index record | Files |
|---|---|---|---|
| Name9 | `u16 count` | `char name[9]; u32 offset` | `Resource/RESOURCE`, `Resource/STD.SET`, `Resource/INTERFAC.SET`, `Resource/GAMEPLAY.RES` |
| Name34 | `u32 count` | `char name[34]; u32 offset` | `Resource/{WAVE,A_WAVE1,A_WAVE2,S*L1_tactic,S*L1_finish,animscr,dropitem,texteff}.RES`, `map/scenarioN/scenarioN.res`, `item/*.res`, `unit/{unit,recruit}.res`, `script/unit/unitAI.res` |

- `count + 1` index records; the last one holds the end offset (its name is blank). First offset == index size.
  Entry `i` = `[offset[i], offset[i+1])`. In every shipped file the end offset == file size.
- Names: NUL-terminated, upper/mixed case; lookups are case-insensitive in `ResArchive::find`.
- Payloads observed: `*.SET`/`RESOURCE` entries = dBase III tables (241 tables); `WAVE.RES`, `A_WAVE*`, `S*L1_*.RES` =
  RIFF WAVE (503 in WAVE.RES; scenario packs keyed by 4-digit ids); `scenarioN.res` = copies of the map text files
  (`S1L1_STG`, `S1L1_PLA`, `S1L1_UNI`, ...); `GAMEPLAY.RES` = text tables (ECON, THROWI, UNIT, CHARMOVE, WEATHER, GAME);
  `animscr.res`, `item/*.res`, `unit/*.res`, `DynGrass.res` = `VERSION 1.0` text scripts; `texteff.res` = `Version : 1.00` text.
- Format test: Name34 is tried first; a Name9 file read as Name34 yields a count larger than the file (and vice versa the
  first-offset check fails). No shipped file is ambiguous.

### 2.2 dBase III tables — CONFIRMED by data (all 242 tables) [formats.res_archive]

`Resource/CURSOR.RES` and every entry of `RESOURCE`/`*.SET` (7KAA `Database` / `GameSet`).

| Off | Size | Field |
|---|---|---|
| 0 | 1 | version `0x03` |
| 1 | 3 | YY MM DD (YY = years since 1900, e.g. `0x67` = 2003) |
| 4 | 4 | record count |
| 8 | 2 | header length |
| 10 | 2 | record length (includes 1 delete-flag byte) |
| 32 | 32*n | field descriptors: `name[11]`, `type` (`C`/`N`), `u32 address` (garbage in many files, e.g. 0 in STD.SET:ANIMATE; offsets are recomputed cumulatively), `u8 length`, `u8 decimals`, 14 reserved |
| hdr-1 | 1 | `0x0D` |
| hdr | count*reclen | records; byte 0 `' '` or `'*'` |

Optional trailing `0x1A`. `CURSOR.RES` field names carry a stray leading `H` (`HFILENAME`, `HBITMAPPTR`) — **HYPOTHESIS**:
editor artefact; real names are `FILENAME`, `HOTSPOT_X`, `HOTSPOT_Y`, `BITMAPPTR`.

### 2.3 Cursor bitmaps — HIGH [formats.res_archive]

`Resource/I_CURSOR.RES` = concatenated `{u32 size; u16 w; u16 h; u8 pix[w*h]}`, `size = 4 + w*h`. Two 32x32 records at
offsets 0 and 0x408. `CURSOR.RES` field `BITMAPPTR` (4 raw bytes) holds exactly these offsets (test checks). Pixels are
palette indices; `0xFF` fills the background (**HYPOTHESIS**: transparent).

### 2.4 Palette — HIGH

`Resource/pal_std.res`, 776 bytes: `u32 776` (file size), `u32 0x0000B123` (UNKNOWN), 256 x RGB8.

### 2.5 Text files in Resource/

`help.res` (`key\r\nTITLE\r\nbody\r\n~` records), `t_basic.txr`/`t_interf.txr` (`count\r\ncount\r\nNAME|n` lines),
`*.cm` (block text, §3.2), `*.pro` (kv text, §3.1). `ANIM_AM3.RES`, `anim_am2.res` are empty (0 bytes).
Not parsed here: `help.res`, `*.txr` (ScriptsGameplay owns text tables).

## 3. Text formats (world_text.hpp) — CONFIRMED by data [formats.world_text]

### 3.1 Key/value text

`Key<ws>:<ws>value` per CRLF line, `#` comments, blank lines separate groups; keys repeat per light/stage so order matters.

| File | Keys (in order of appearance) |
|---|---|
| `map/common.lgt`, `map/special.lgt`, `map/scenarioN/*.lgt` | `LightNum`, `StageNum`, then per light x stage: `IsOn`, `StageName`, `Type`, `Red`, `Green`, `Blue`, `Alpha`, `HDir`, `VDir` |
| `map/common.sca`, `map/special.sca`, `Resource/Scat.pro`, `Cloud.pro` | per stage: `StageName`, `Rayleigh`, `Mie`, `Inscattering`, `G`, `DistanceModifier`, `Red`, `Green`, `Blue`, `DirectionX/Y/Z` (atmospheric scattering) |
| `map/scenarioN/*.day`, `Resource/day.pro` | `StageCount`, `RainProbability`, `RainPercentage`, `SnowProbability`, `SnowPercentage`, `SkyMesh` (`model\sky01.smo`), `CloudTexture1/2`, per stage `StageName`, `StartTime`, `Duration` |
| `Resource/SkyBox.pro` | `LightNum`, `StageNum`, `Duration`, per stage `StageName`, `Red`, `Green`, `Blue`, `Alpha`, `Brightness`, `Is_On`, `V_Dir` |
| `Resource/WeatherEffect.pro` | `EffectCount`, `EffectStageName` (Rain, Snow) |

Engine readers: `.lgt`/`.sca`/`.day` names at `0x9d81xx`, loaded by the world loader `FUN_004f2f80` (§4). Value semantics
(colour 0..255, directions in degrees) are **HYPOTHESIS** from names/values.

### 3.2 Block text

```
TypeName\r\n{\r\n\tkey:value;\r\n ... }\r\n
```
`;` optional (`.trk`, `.pth` omit it), keys may repeat inside a block (camera/track points), no nesting in any shipped
file. Inside `.por` blocks lines are coordinate tuples (kept with empty key); `.por` top level is `Anti-Portal Number: N`
followed by exactly N `Anti-Portal` blocks.

Block types seen: `CreateGrid` (`X`,`Y` = cells), `CreateSceneSector` (`scene_id`, `X1..Z2` bounds, `is_grid_scene`,
`index_x/y`, `size_x/y` = cells per sector), `CreateObject` (`scene_name`, `ObjectName`, `ObjectFile`, `Path`
(`Object\Models_...\`), `X,Y,Z`, `HD,VD` (degrees), `WIDTH,LENGTH,HEIGHT`, `Kingdom`, `Treasure`, `TreasureLevel`, `Scale`,
`InitState`, `FinalState`), `CreateLight` (`light_type`, RGBA, `X,Y,Z`, `ATT0..2`, `Radius`, `Group`, ...), `CreateWater`,
`CreateTrigger`, `CreateParticle` (.stg); `CreatePlace`/`CreateFacility` (.pla); `CreateUnit` (.uni: `UnitFile`,
`Kingdom`, `Task`, `Group`, ...); `CreateGrid`/`CreateNode` (.wor world: 8x8 node grid, `NodeFile:S1L1.STG`);
`CreateTerritory` (.ter); `CreateTrackArray`/`CreateTrack` (.trk); `CreateCameraTrackArray`/`CreateCameraAnimatedTrack`
(.cm, also `Resource/rts.cm`, `cutscene.cm`); `CreatePathLineList` (.pth); `CreateWaveTrackArray` (map `.wav`).
Totals: 237 files, 46 127 blocks, 32 332 objects. `map/scenario6/S6L1w.pth` is empty.

## 4. Map directory (`map/scenarioN/`)

Loader: `FUN_004f2f80` (world_s.cpp) builds `<dir>\<name><ext>` for `.PFD`, `<name>w.PFD`, `.day`, `.lgt`, `.sca`, `.GRI`,
`.PLN`, `.STG`, `.POR`... Per map `SnLm[_k]`: `.stg .gri .pfd .pla .uni .lgt .map .por .trk .cm [.pth .wav]`; per
scenario world `SnL1w.{wor,ter,day,map[,pfd]}` and `scenarioN.res`.

### 4.1 `.GRI` terrain grid — CONFIRMED [formats.map_grid]

Reader `FUN_004f6eb0`; RLE `FUN_0059dc10` with escape byte `0xFF` (`push 0xff` at `0x4f7286`).

| Offset | Type | Field |
|---|---|---|
| 0 | i32 | cells_x (256 or 220) |
| 4 | i32 | cells_y |
| 8 | i32 | header_c (always 4; stored at world+0x50, clamped >= 1; meaning UNKNOWN) |
| 12 | i32 | nsec |
| 16 | char[32] x nsec | sector names `scene_sector_N` |
| | i32 | ntex |
| | {char[32], char[32]} x ntex | ground type name + second name (bytes after its NUL are uninitialised editor memory, e.g. `D:\editor\gridNod2.cpp`) |
| | 16 RLE streams | one byte-plane each, `(cells_x+1)*(cells_y+1)` bytes, node index `x*(cells_y+1)+y` |
| | 5 RLE streams per node with byte15 != 0 | 9 (level 1) or 25 (level 2) bytes each; none in shipped data |

RLE stream: byte `!= 0xFF` literal; `FF n v` = n copies of v; `FF 00` = end.
The 16 planes are bytes 0..15 of the engine's 0x54-byte node struct:

| Byte | Meaning | Confidence |
|---|---|---|
| 0 | vertex present (1) | CONFIRMED (loader tests `*node != 0`) |
| 1-3 | 0..255 values, UNKNOWN (smooth fields; possibly shade/colour) | UNKNOWN |
| 4 | 0..255, passed to `FUN_004c5a20`; HYPOTHESIS blend weight between tex1/tex2 | HYPOTHESIS |
| 5-6 | always 0 | — |
| 7-8 | `int16` height, world units (Z up) | CONFIRMED (min/max used for sector bboxes; objects sit on it, see below) |
| 9-10 | `u16` 1-based sector index into the name table | CONFIRMED (`local_243c[idx-1]`) |
| 11-12 | `u16` 1-based ground type #1 (copied as name to node+0x10) | CONFIRMED |
| 13-14 | `u16` 1-based ground type #2 (node+0x30) | CONFIRMED |
| 15 | detail level 0/1/2 | CONFIRMED |

- Vertex spacing 64 world units (`iVar6 * 0x40`), x = outer index. Cross-check: 25+ maps, objects from the matching `.stg`
  sit on the bilinear height (S1L1: 82% within 40 units, transposed axes: 4%). [formats.map_grid]
- Ground type names (`drygrass_L01`, `leaves_L01`, ...) match `item/DynGrass.res` entries — HIGH: they drive dynamic grass /
  ground type, not rendering textures.
- `.stg` `CreateGrid` X/Y == cells and `CreateSceneSector` count == GRI sector count for every map. Sectors are
  `size_x x size_y` cells (16x16 at 64 units = 1024 units on S1L1; 2048-unit sectors on 64-sector maps).
- `texture/terrain/S<n>l<m>_scene_sector_<N>.dds` (128x128, 2886 files) are per-sector baked **lightmaps**: a mosaic
  placed by `index_x/index_y` reproduces the hillshade of the decoded heights (`work/re-world/out/S1L1_textures_vs_height.png`,
  `S1L1_obj_hillshade.png`). HIGH.
- Export: `joa-res terrain map/scenario1/S1L1.gri work/re-world/out/S1L1.obj work/re-world/out/S1L1_height.png` (257x257,
  heights -1034..1538); viewed, matches the lightmaps.

### 4.2 Other map files

| File | Status |
|---|---|
| `.PLN` (only `S1L1.PLN`, 5.7 MB) | loaded only when `DAT_00a0b6ac` != 0 (`FUN_0055f320`). Header `u32 1, u32 3, u32 256, u32 256`, then records `char name[34]; u16 sector; u32 size` (`scene_sector_14`, 14, 0x2838). HYPOTHESIS: precomputed per-sector data. Not parsed. |
| `.map` | `u32 n` + n x 164-byte records, record mode: `u16 0x40 + char[64]` instance (`fcastle_s1l1`), `u16 8 + char[8]` model (`fcastle`), `u16 0x40 + 64 bytes`, then raw ints (`8116, 726, 2000`, ...). Fits 36/37 files (`S2L1w.map` is 328 = 4+2*162). HYPOTHESIS: 3D campaign-map building placements. Not parsed. |
| `.pfd` / `SnL1w.pfd` | record mode; `u32 nmodels`, n x `u16 0x80 + char[128]` model paths (`Object\Models_Unique\...`), then node/edge tables read by `FUN_00670980` (many `get_long`s). HYPOTHESIS: pathfinding data. Not parsed (owner: pathfinding/engine). |
| `.pla .uni .trk .cm .pth .wav .por .ter .wor .stg` | block text (§3.2), parsed generically. |

## 5. Fonts (`font/*.fin`) — CONFIRMED [formats.font_fin]

Reader `FUN_00624810` (en2d/font2d.cpp); file opened without record mode. Atlas texture = `int8x6/<same stem>.dds`
(`FUN_006099c0(name)`). All 19 files are 3184 bytes.

| Off | Type | Field |
|---|---|---|
| 0 | char[32] | Windows face name the atlas was rendered from (`GoudyOlSt BT`, `testfont`) |
| 32 | u16 | first char (32) → font+0x2a |
| 34 | u16 | last char (255) → +0x2c |
| 36 | i16 | metric_a → +0x2e (14 for `_10`, 74 for `_48`; HYPOTHESIS cell height) |
| 38 | i16 | line_height → +0x30, copied to +0x34, scaled by `FUN_00624eb0` |
| 40 | i16 | metric_c → +0x32 (UNKNOWN) |
| 42 | i16 | metric_d → +0x36, scaled by `FUN_00624e60` (UNKNOWN) |
| 44 | u32 | glyph count (read, ignored) |
| 48 | 14 x (last-first+1) | glyph: `i16 y_offset, x1, y1, x2, y2, width, height` |

`width == x2-x1+1`, `height == y2-y1+1` for all 4256 glyphs; the engine subtracts a per-font spacing argument from `width`.
Rects lie inside the atlas (one glyph, `Goudy_bold_italic_12` 'V', has `x2 == atlas width`).

## 6. CONFIG.DAT — CONFIRMED structure, field labels per row

Raw dump of the config object at `0x00A57FB8`, exactly `0xB05` bytes. Load `FUN_0045aaf0` (called from `FUN_00420ea0`
with `mov ecx,0xa57fb8` at `0x420ede`) accepts only `GetFileSize == 0xB05`; save `FUN_0045ad80` writes 0xB05 bytes;
missing/invalid file → defaults `FUN_00459e30` (= `0045acb0(1)`, `00459ec0`, `00459f30`, `00459fb0`, `0045a010`).
After load `FUN_0045b240` downgrades unsupported graphics options. VSetting.exe is the MFC launcher "RESetup"
(PLAY / Graphical Setting / README / EXIT) — not analysed further.

| Off | Type | Default | Shipped | Label (evidence) | Conf. |
|---|---|---|---|---|---|
| 0x00 | i32 | tbl | 1 | difficulty value = `short DAT_009fb1ca[d]` (`FUN_0045acb0`) | HIGH |
| 0x04 | u8 | tbl | 1 | `DAT_009fb1db[d]` | HIGH |
| 0x05 | u8 | 1 | 1 | UNKNOWN | — |
| 0x06 | u16 | 45 | 45 | UNKNOWN | — |
| 0x08,0x09 | u8 | 1,2 | 1,2 | UNKNOWN | — |
| 0x0A | u8 | 1 | 1 | difficulty 0..3 (written 0..3 in `FUN_0070acb0`, `switch` on it in option-menu code, `FUN_0045acb0(d)` derives 0x00/0x04) | HIGH |
| 0x10 | u32 | 20 | 20 | UNKNOWN | — |
| 0x14-0x16 | u8 | 0 | 0 | UNKNOWN | — |
| 0x17 | char[21] | "Player" | "Player" | player name (`FUN_00459fb0`; +0x2c cleared) | CONFIRMED |
| 0x2D,0x2E,0x32 | u8,u32,u8 | 1,1,0 | 1,1,0 | UNKNOWN | — |
| 0x33-0x38 | | 1,5,3,2,0 | same | UNKNOWN (0x37 tested ==0/==2 in input timing code) | — |
| 0x39 | u8 | 1 | 1 | UNKNOWN (saved/restored with audio fields) | — |
| 0x3A | i16 | 77 | 9 | music volume (`FUN_0059b150` → music stream gain *0.01) | HIGH |
| 0x3C | i16 | 39 | 15 | "Environmental" volume (`FUN_00586620` → the 3 environment streams; audio page `FUN_007413e0`) | HIGH |
| 0x3E | i16 | 100 | 100 | UNKNOWN volume | — |
| 0x40 | u8 | 1 | 1 | sound enabled (gates `ilib\se.cpp` playback `FUN_0059e400`, `FUN_0059ebd0`) | HIGH |
| 0x41 | i16 | 100 | 16 | "Sound Volume": 3D effects + UI (`FUN_0045f6b0` → 12 OpenAL sources, `FUN_004ceef0`) | HIGH |
| 0x43 | u8 | 1 | 1 | "3D Sound On": `FUN_00454000(on ? 2 : 0)` → `alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED 0xD002)` / `AL_NONE` (docs/AUDIO_AND_VIDEO.md) | CONFIRMED |
| 0x44-0x4E | u8 | 1.. | | UNKNOWN flags (`FUN_0045a8b0/0045a950`) | — |
| 0x4F-0x67 | i32 x7 | 20,0,100,100,90,100,70 | same | UNKNOWN | — |
| 0x6B | i32 | 3 | 3 | control/key preset 0..3 (`FUN_004cb5b0(preset,key)`) | HIGH |
| 0x6F | f32 | 0 | 0 | mouse sensitivity -1..1 (`(slider-10)/10`, `FUN_00432250`) | HIGH |
| 0x73,0x74,0x75 | u8 | 0 | 0 | control combo boxes (camera behaviour) | HYPOTHESIS |
| 0x76 | f32 | 90.0 | 90.0 | camera turn angle, degrees (used as `deg*0.0174533`) | HIGH |
| 0x7A,0x7E | i32 | 0 | 0 | UNKNOWN (0x7E compared during display-mode matching `FUN_005f6c00`) | — |
| 0x82 | i32 | 1024 | 1024 | screen width | CONFIRMED (defaults + mode matching) |
| 0x86 | i32 | 768 | 768 | screen height | CONFIRMED |
| 0x8A | i32 | 32 | 32 | bits per pixel | CONFIRMED |
| 0x8E | i32 | 60 | 60 | refresh rate | CONFIRMED |
| 0x92-0x97 | u8 | 1,2,3,5,1,1 | same | UNKNOWN (0x92 indexes a 3-column string table; 0x96 sound related) | — |
| 0x98 | i32 | 0x2001 | 0x4001 | view distance = `slider*0x3c00/20 + 0x401` → far plane `DAT_00a48cd4` | HIGH |
| 0x9C | u8 | by CPU MHz | 3 | shadow mode 0..3 (2,3 need >= 4 texture stages) | HIGH |
| 0x9D | u8 | by CPU MHz | 1 | shader/water quality 0..2 (2 needs 64x64 render targets) | HYPOTHESIS |
| 0x9E | u8 | 1 | 1 | effect gated by caps bit 0x200000 + 256x256 RT | UNKNOWN |
| 0x9F | u8 | by CPU MHz | 1 | dynamic grass on/off (`FUN_00464600`) | HIGH |
| 0xA0 | i32 | 500..1600 | 2000 | grass distance, multiple of 20 (dyngrass.cpp `FUN_004644d0`) | HIGH |
| 0xA4 | i32 | 30..80 | 100 | grass density % | HIGH |
| 0xA8 | f32 | -0.2 | -0.2 | brightness -1..1 (`FUN_00602a50`) | HIGH |
| 0xAC | f32 | 1.1 | 1.1 | contrast | HIGH |
| 0xB0 | f32 | 1.0 | 1.0 | gamma | HIGH |
| 0xB4 | u8 | 0 | 0 | custom key table valid (`FUN_004cc380` sets 1) | CONFIRMED |
| 0xB5 | 55 x 0x30 | — | 0 | key bindings copied from the controls page; per record bytes 0x22,0x23 and i32 0x24,0x28,0x2c are restored (`FUN_004cc220`) | HIGH |

Options-page handlers: audio `FUN_007413e0`, graphics `FUN_0074a700`, controls `FUN_00745b10`.

## 7. Saves

### 7.1 `Save/*.SAV` — header CONFIRMED, body split CONFIRMED, body semantics mostly UNKNOWN [formats.save_sav]

Record mode on. Header written by `FUN_00478cc0`, read by `FUN_00478520` (each line = one `u16 len + data` record):

| # | len | Content (shipped `Mission1-AutoSave.SAV`) |
|---|---|---|
| 1 | 4 | magic `RSav` (0x76615352) |
| 2 | 4 | save-object bytes +2..+5; reader takes 1 byte (flag, cleared by `FUN_00478480`); bytes 1..3 are the first chars of the path (`SAV`) |
| 3 | 8 | FILETIME UTC (`GetSystemTimeAsFileTime`) |
| 4 | 8 | FILETIME local |
| 5 | 1 | `DAT_00a500c2` game mode (9) |
| 6 | 1 | `DAT_00a500c3` (12) |
| 7 | 4 | `DAT_00a47bb0` (0x00223A26) UNKNOWN |
| 8 | 4 | `DAT_00a47bb4` (0x4434000D) UNKNOWN |
| 9 | 128 | description, NUL-terminated ("Mission 1 Auto Save") |
| 10 | 4 | save-object +0x19a (1) |
| 11 | 1 | save-object +0x19e (1) |

Body (`FUN_00474ec0`) starts at 189: 20 x { marker record `04 00 EE EB AC 2F` (u32 0x2FACEBEE), section } and a final
marker at EOF. Shipped section sizes: 82 19 555 3954 6 4 31227 16264 980 24462 408 8 103535 136 4762 1258 350 456 214 92.

| Sec | Writer | Observed shape |
|---|---|---|
| 1 | `FUN_00476890` | one 0x50-byte object; adds elapsed `FUN_007b33bf(0)` delta (HYPOTHESIS: `time(NULL)`, play time) to +0x40 before writing |
| 2 | `FUN_00476c40` | scalar fields (+0x44 u32, +0x48 u16, +0x4a u8, +0x32 u32, ...) |
| 3 | `FUN_004854c0` | two longs + array of records (`FUN_00485070` per live element) |
| 4 | `FUN_00484780` | 0x745-byte object + sub-tables + two u16 arrays |
| 5 | `FUN_00483b10` | 4 bytes |
| 6-13 | `FUN_00483130`, `0047fa50`, `0047fe80`, `00480c00` (virtual `+0x54` save per object), `00481fb0`, `00482b50`, `00482ea0`, `0047d7a0` | DynArray dumps: `put_long(count)` then per index a `put_short` liveness/class tag + object record |
| 14 | `FUN_004838e0` | DynArray |
| 15 | `FUN_004857c0` | 0x126e-byte object + state-dependent extra |
| 16 | `FUN_00485ab0` | 8 x u32 + 0x18c + 2 x 0x194 bytes |
| 17 | `FUN_004843d0` | scalar fields |
| 18 | `FUN_00485d30` | count + 0xe0-byte records |
| 19 | `FUN_00485e70` | 1 byte + per-element `FUN_00486090` |
| 20 | `FUN_00486110` | 3 longs + 2 x 9 longs + DynArray |

The body contains raw in-memory structs (heap pointers such as `0x0262BE20` are visible), so original-save loading in the
port needs the per-section object layouts — **not done**. The thumbnail beside each save is a plain BMP
(`BM`, 40-byte DIB, 512x256, 24 bpp, uncompressed, 393 270 bytes). Autosave names: `_AUTO%03d.SAV` (15-slot ring, oldest
FILETIME replaced, `FUN_00477770`); manual: `SAVE\<name>%04d.SAV`.

### 7.2 `Save/Player.dat` (profile) — HIGH

Written by `FUN_004c8810`, read by `FUN_004c8bb0` (profile.cpp, extension `.dat`, record mode **off**):

| Off | Size | Content |
|---|---|---|
| 0 | 0x48 | profile header; name at +0 ("Player"); +0x38 u32 = 100 (UNKNOWN) |
| 0x48 | 0x20 | second block, starts with a name ("Player") |
| 0x68 | 4 | N (0 in shipped file) |
| | N x 0x20 | entries (`FUN_004c93e0`) |
| | N x var | per entry 0x20 bytes + `FUN_004c9880` + `u16 m` + m sub-records (`FUN_004c9460`) |
| | 2 + 2a | `u16 a` + a x u16 |
| | 2 + 2b | `u16 b` + b x u16 |
| | 2 | u16 |

Shipped file: 0x48 + 0x20 + 4 + 2 + 2 + 2 = 114 bytes, exact. Entries for N > 0 are not decoded (parser rejects them).
What campaign state lives in these lists (completed missions, unlocked units) is **UNKNOWN**; it needs a profile with
progress to diff.

## 8. VFS and tool

- `Vfs::resolve("Map\\SCENARIO1\\s1l1.GRI")` → `map/scenario1/S1L1.gri`; rejects absolute, drive-letter and `..` paths;
  read-only. [formats.vfs_fs]
- `joa-res list <file>` (index or dBase), `joa-res extract <file> <outdir>` (adds `.dbf`/`.wav`),
  `joa-res terrain <gri> <obj> <png>`. Refuses outputs inside the install unless under `<install>/work/`.

## 9. Open gaps

- SAV body object layouts (20 sections) and Player.dat entry layout / campaign progress fields.
- CONFIG.DAT rows marked UNKNOWN; key-binding record layout beyond the restored fields.
- GRI planes 1-4 meaning; `header_c`; `.PLN`, `.pfd`, `.map` contents.
- FIN `metric_a/c/d` exact roles.
- Not covered here: `script/scenario/*EX.res`, `item/**/*.pro` item scripts, `*.txr`, `help.res` (script/text owner).
