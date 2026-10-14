# Models, skeletons, animation and textures (.SMO / .AM1 / .AM2 / .AM3 / .DDS)

Implementation: `src/formats/model_mesh.{hpp,cpp}` (SMO, AM1), `src/formats/anim_clip.{hpp,cpp}`
(AM3, AM2), `src/formats/dds.{hpp,cpp}` (DDS). Tool: `tools/joa-model-dump`. Tests:
`tests/formats/{model_mesh,anim_clip,dds}_test.cpp` (CTest `formats.model_mesh`, `formats.anim_clip`,
`formats.dds`). RE scratch (Python prototype, Ghidra scripts, renders): `work/re-models/`.

Evidence base: `joan.exe` (sha256 prefix `b25e2154af3ffc87`), private Ghidra 12 project
`work/ghidra/ModelFormats`, decompiles in `work/re-models/ghidra/*.c` produced by
`work/re-models/ghidra/decomp.sh <addrs> <out> [depth]`. Addresses are VAs (ImageBase 0x400000).
Decompiler output is not verified source; every claim below was also checked against the files.

All values little-endian. `f32` = IEEE float, `char[N]` = NUL-padded fixed field.

## Roles of the files

| Ext | Role | Count in install | Confidence |
|-----|------|------------------|------------|
| `.SMO` | static mesh (props, buildings, items, UI 3D markers) | 1228 (model 67, item 1067, unit 94) | CONFIRMED |
| `.AM1` | skinned mesh (characters, animated props) | 588 (model 6, item 243, unit 337, anim 2) | CONFIRMED |
| `.AM3` | bone hierarchy (names + parent/child ids), no bind pose | 944 | CONFIRMED |
| `.AM2` | animation: clip table + one keyframe track per bone + events | 357 (anim 123, item 231, unit 3) | CONFIRMED |
| `.DDS` | textures (DirectDraw Surface) | 4346 (texture 3876, int8x6 469, item 1) | CONFIRMED |

Relations (CONFIRMED by data and loaders):
- `X.AM1` + `X.AM3` share bone ids: AM1 influence bone = AM3 node id. 575 of 576 checked pairs have
  `AM1.boneCount == AM3 node count`; exception `item/anim/fd_meat.AM1` (2 vs 1, original data).
- `anim/X.am2` + `anim/X.am3` share ids: AM2 track i = AM3 node i (354/354 pairs match).
  A unit's AM1 is animated by any AM2 whose AM3 has the same node list (e.g. `unit/Joan_of_Arc.AM1`
  with `anim/joan_of_arc.am2`).
- Mesh set loader FUN_005c2370 (pushes at 0x5c254a `"#"`, 0x5c26e0 `"#S"`, 0x5c2840 `"#V"`,
  0x5c2850 `".SMO"`) loads `base.ext`, then optional `base#1..#4.ext`, `base#S.ext`, and
  `base#V.SMO` (always SMO). HIGH CONFIDENCE for the names; roles are HYPOTHESIS: `#2/#3` = LODs,
  `#S` = low-poly skinned copy (Joan: 442 vs 2996 tris, same texture; shadow/far LOD),
  `#V` = untextured spindle hull (`unit/Joan_of_Arc#V.SMO`, 180 tris, 192 units tall) used as a
  picking/visibility volume.
- Extension dispatch: FUN_005b0000 picks `.SMO` (0x9d52b8) or `.AM1` (0x9d52b0) by a skinned flag,
  then loads `.AM3` (0x9cf5b4) for skinned models; AM2 path built in FUN_005b2f70 (0x9cf5bc).
  CONFIRMED.

## SMO / AM1 common container (version 500)

Loader: FUN_005ddd70 (static) / FUN_005da280 (skinned) read the version and dispatch:
0 → FUN_005dde90 / FUN_005da3d0, 50 → FUN_005decf0 / FUN_005db710, 100 → stub returning 0 /
FUN_005dc400, **500 → FUN_005e0530 (SMO) / FUN_005dc420 (AM1)**. Every shipped file is version 500
(CONFIRMED: all 1816 parse with the v500 layout); older versions are not implemented (UNKNOWN layout).

### Header

| Off | SMO | AM1 | Meaning | Confidence |
|-----|-----|-----|---------|------------|
| 0x00 | u32 | u32 | version = 500 | CONFIRMED (cmp 0x1f4 at 0x5da31e / 0x5dddcb) |
| — | — | u32 | boneCount (AM1 only, at 0x04) | CONFIRMED (FUN_005da280 second read) |
| +0 | i32 | i32 | submesh count | CONFIRMED |
| +4 | i32 | i32 | texture count | CONFIRMED |
| +8 | u32 | u32 | SMO: morph frame count; AM1: read, unused | CONFIRMED (SMO loop `local_20`); 0 in every shipped file |
| +12 | f32[4] | f32[4] | bounding sphere x,y,z,r (AM1 stores r/const) | HIGH CONFIDENCE |

### Submesh record (× submesh count)

| Field | SMO | AM1 | Confidence |
|-------|-----|-----|------------|
| i32 stripCount n | ✓ | ✓ | CONFIRMED |
| i32 vertexCount | ✓ | ✓ | CONFIRMED |
| colour0..colourK, each f32[4] **A,R,G,B** | 4 colours | 3 colours | CONFIRMED order (asm 0x5e0640: float1→byte R, float2→G, float3→B, float0→A of a D3DCOLOR) |
| f32[4] submesh bounding sphere | ✓ | ✓ | HIGH CONFIDENCE |
| i32 texture index (-1 = none) | ✓ | ✓ | CONFIRMED (matched against texture table loop) |
| u16 strip[n + 2] (only when n > 0) | ✓ | ✓ | CONFIRMED |
| vertices | 32-byte static | variable skinned | CONFIRMED |

Colour destinations (engine material object): colour0 → +4 (then overwritten with
DAT_00a69cd0 for every material after load), colour1 → +0, colour2 → +8 (specular, power at +0x10),
colour3 → +0xc (emissive, SMO only). Mapping to diffuse/ambient names: HYPOTHESIS. Typical values:
SMO `(1,.588,.588,.588) (1,.588,.588,.588) (1,.9,.9,.9) (1,0,0,0)`.

**Triangle strip** (CONFIRMED, FUN_005e0530 loop): indices are relative to the submesh's vertices.
For k ≥ 2, triangle `(s[k-2], s[k-1], s[k])` is emitted only if all three differ; when k is odd the
first two are swapped. Parity advances on every position, degenerates included. Implemented in
`strip_to_triangles`. Visual check confirms consistent winding (renders below).

**SMO vertex** (32 bytes, CONFIRMED by copy into the 0x24-byte engine vertex `pos,normal,color,uv`):
`f32 normal[3]; f32 pos[3]; f32 uv[2]`. UV origin top-left (D3D). Coordinates are Z-up
(Joan stands on z=0, 78.7 units tall).

**AM1 vertex** (variable, CONFIRMED FUN_005dc420 → `SkinVertex::add_bone`, `SkinVertex::normalize`):

| Field | Confidence |
|-------|------------|
| f32 uv[2] | CONFIRMED (read into engine vertex +0x1c) |
| u32 refCount r | CONFIRMED |
| u32 influenceCount m | CONFIRMED |
| u32 refs[r] — engine seeks past (`FUN_005888c0(r*4, SEEK_CUR)`) | CONFIRMED skipped; meaning HYPOTHESIS: ids of incident triangles (values < total triangle count, e.g. Joan max 1811 / 1812 tris) |
| m × { u32 bone; f32 weight; f32 pos[3]; f32 normal[3] } | CONFIRMED order; pos/normal are in bone space |

Skinning (CONFIRMED by rendering): `world = Σ weight · (BoneMatrix[bone] · pos)` with the
model-space AM2 bone matrices (see below). Weights sum to 1 (±0.01) in every shipped vertex; up to
7 influences (1: 305 539 vertices, 2: 35 098, 3: 1 179, 4: 199, 5: 60, 7: 5).
There is no separate bind pose anywhere: bone-local positions + animation matrices are sufficient.

### Texture table (× texture count, after all submeshes)

| Field | Confidence |
|-------|------------|
| char name[20] (e.g. `swordfrench.bmp`, `Joan.tga`) | CONFIRMED |
| i32 embeddedSize; if > 0, that many bytes of an image file follow (D3DXCreateTextureFromFileInMemory-style call FUN_00765c3d) | CONFIRMED; no shipped file embeds |

Resolution (FUN_00608e10 registry + `strrchr(name,'.')` cut, HIGH CONFIDENCE): the extension is
dropped and the base name is looked up case-insensitively among `.DDS`, `.TGA`, `.BMP` (strings
0x9d9c00, 0x9d9bf8, 0x9d353c) in `INT8X6\`, `INT8X6\ITEM\`, `SAVE\`, `TEXTURE\` and
`TEXTURE\{ITEM,UNIT,TERRAIN,PARTICLE,MISC}\` (`TEXTUREL\`/`TEXTUREH\` quality variants are absent
from this install). E.g. `Joan.tga` → `texture/unit/joan.dds`. 2485 references, 2465 resolve;
20 references to 15 names are missing from the GOG data itself (`pinealpha`, `meung01de2`,
`monastery`, `sky_night01..04`, `phillip`, `footstep`, `footstepalp`, `panda_nor`,
`snowtree_high`, `grass`, `gateroof`, `lc_road1`; checked with `find -iname`).

### SMO morph frames (only if header +8 > 0)

Per frame, per submesh: `i32 stripCount; i32 vertexCount; u16 strip[n+2]; vertex[32-byte]`.
CONFIRMED from the loader; no shipped file uses it (all have 0), so it is untested on real data.

### Tagged trailer

`i32 tag` repeated; the engine's read returns 0 at EOF, so EOF at a tag boundary ends the list
(`model/ripple.AM1`, `item/ripple.AM1` end there). `item/.../grass03.SMO` has 1 stray byte after
tag 0. CONFIRMED.

| Tag | Payload | Engine effect (confidence) |
|-----|---------|----------------------------|
| 1 | per submesh: i32 a, i32 b | a ≠ -1 → blended render state (CONFIRMED sets fields +0x34/+0x38/+0x40); values seen 0,1,2,3,5,6 (meaning per value UNKNOWN). b ≠ -1 → texture index used as stage-0 texture with the submesh texture on later stages (detail/env map, HIGH CONFIDENCE); set in 1 file |
| 2 | per submesh: i32 c, i32 d, u8 e | c == -1 → specular colour set to DAT_00a69cd4 / power 0, else power 50.0 (CONFIRMED); d unused (CONFIRMED); e ≠ 0 → material flags low nibble = 2 (HYPOTHESIS: two-sided / alpha test) |
| 3 | u32 v | v ≠ 0 → every vertex normal replaced by constant DAT_00a18a60 (vegetation lighting, HYPOTHESIS for purpose) |
| other | — | ends the list (CONFIRMED) |

## AM3 skeleton

Parser FUN_005b9af0 (CONFIRMED):

```
u32 count
count × { u32 id; char name[50]; u32 childCount; u32 child[childCount] }
```

Node table size = max(count, every id + 1, every child + 1); parent of each child = declaring node;
nodes not given a parent are roots. Names are 3ds Max Biped names (`Bip01`, `Bip01 Pelvis`, ...).
All 944 files parse with ids dense 0..count-1. The parser caps ids at 0xFFFF (ours, not the engine's).

## AM2 animation (`d:\ww\en3d\animdef.cpp`)

Header + key reader FUN_005b4b90 (full load) and FUN_005b5830 (header only; keys streamed later by
FUN_005b6dc0, which skips 16 + 62·clipCount bytes), trailer FUN_005b5350. CONFIRMED:

```
u32 trackCount            // == AM3 node count
u32 clipCount
f32 timelineStart, timelineEnd          // ms
clipCount × { char name[50]; u32 flags; f32 start; f32 end }      // 62 bytes, ms
trackCount × { u32 keyCount; keyCount × { f32 time; f32 m[12] } } // 52-byte keys
trailer: u32 tag repeated until 0 (EOF reads as 0); tag 1 =
    u32 n; n × { u32 a; a × f32[4]; u32 b; b × { f32 time; f32 value } }
    u32 eventCount; eventCount × { f32 time; i32 id (-1 → -999 in engine); char name[50]; u8 flag }
  other non-zero tags carry no payload.
```

- Times are milliseconds; key spacing is 16.67 ms (60 Hz) or 33.33 ms (30 Hz) in most tracks,
  some 50/100 ms (first-track survey over all AM2). Clip names: `default`, `f_run`, `f_walk`,
  `f_shift_left`, ...; all clip flags are 0. CONFIRMED.
- `m[12]` is a 3×4 row-major `[R|t]` (file rows `(r00 r01 r02 tx)`, ...); the engine transposes it into
  a D3D row-vector matrix (asm/decompile at 0x5b6dc0: m0→[0], m1→[4], m2→[8], m3→[12]). CONFIRMED.
- Matrices are **model space**, not parent-relative: `Bip01` and `Bip01 Pelvis` keys share
  translation (0,0,28.1) and `Bip01 Spine` sits above them in `anim/Flavy.am2`; skinning AM1 with them
  directly produces correct characters (renders below). CONFIRMED.
- Some keys carry scale (0.7% of sampled rows have |row| < 0.5 or > 2).
- Tag-1 scalar tracks fade values 0→1→0 (`item/anim/watersplash.AM2`: 0,1,1,0); HYPOTHESIS:
  per-track visibility/alpha. The 16-byte keys never occur in shipped data (UNKNOWN meaning).
- Events reference effect/script text files (`JoanAAAA.txt` at 366.7 ms, `bow_heal.txt`).

`sample_pose(anim, t)` lerps the two bracketing keys element-wise (ponytail: the engine converts
keys via FUN_005b8770 into 32-byte keys — probably quaternion + position, UNKNOWN — so its
interpolation may differ slightly).

## DDS

Standard DirectDraw Surface, 128-byte header, no DX10 extension. Shipped formats (CONFIRMED by
survey `work/re-models/dds_survey.py`): DXT1 3543, DXT3 490, DXT5 192, A8R8G8B8 73,
A4R4G4B4 48; 3 DXT1 cubemaps (6 faces, face-major, each with its mip chain); 10 DXT5 files carry
garbage bit-count/mask fields that are ignored because the FourCC flag is set. Decoder supports
DXT1 (incl. 1-bit alpha), DXT2/3, DXT4/5 (premultiplied variants decoded as 3/5), and any 8/16/24/32
bit RGB/luminance/alpha mask format. Mip count clamped to the full chain length; truncated
surfaces are rejected. Cross-check vs Pillow on `texture/3dmapicon.dds` (DXT1),
`texture/item/BigBlue.dds` (DXT3), `texture/item/WaterFal01b.dds` (DXT5): max channel diff 1
(565→888 expansion rounding); `texture/misc/sea1.dds` (A8R8G8B8), `int8x6/12pt small text.dds`
(A4R4G4B4): identical.

## Coverage (all original files)

| Test | Result |
|------|--------|
| `formats.model_mesh` | 1228 SMO + 588 AM1 parse; 2856 submeshes, 1 186 011 vertices, 963 025 triangles; all strip indices < vertex count, texture/bone indices in range, weights sum to 1; 576 AM1/AM3 pairs consistent; Joan posed at t=0 spans z 0..78.7 |
| `formats.anim_clip` | 944 AM3 (14 113 nodes), 357 AM2 (4 214 tracks, 2 134 315 keys, 2 078 clips, 107 events); key times sorted; 354 AM2/AM3 pairs match |
| `formats.dds` | 4346 files, 36 320 decoded images (all mips, all cube faces) + synthetic DXT1/A8R8G8B8/A4R4G4B4 known answers |

Robustness: ASan+UBSan runs of the model/anim tests and 2 700 mutated/truncated inputs
(`work/re-models/fuzz.cpp`) were clean.

Visual checks (C++ parser → `joa-model-dump --obj` → `work/re-models/render.py` numpy rasterizer):
`work/re-models/visual_joan_cpp.png` (Joan, AM1 + `anim/joan_of_arc.am2` t=0),
`visual_joan_run_cpp.png` (t=4600 ms, inside `f_run`), `visual_joan_sword_cpp.png`
(`item/Copy of joan_sword.SMO`), `visual_barack_cpp.png` (barracks building), all textured via the
resolution rule above.

## Gaps

- SMO/AM1 versions 0/50/100 (loaders exist in joan.exe; no shipped file uses them) — UNKNOWN layout.
- Tag-1 blend mode values → exact D3D render states not yet mapped (fields +0x34/+0x38/+0x40).
- Engine key representation after FUN_005b8770 and its interpolation — UNKNOWN.
- `.AMT`, `.MDA`, `.NOD`, `.MA`, `.pro` (item/unit side files) are not covered here.
