# Resource formats — reverse-engineering index

Byte layouts, coverage and confidence labels are in the format documents; this page maps each format to the
`joan.exe` code that reads it. Format table and parser headers: [../ASSET_FORMATS.md](../ASSET_FORMATS.md).

| Format | joan.exe reader (VA) | Confidence | Detail |
|---|---|---|---|
| File open / path resolution | `0x586da0` | CONFIRMED | [FUNCTION_MAP](FUNCTION_MAP.md) |
| `.RES` index (Name9 / Name34) | `0x59c550` | CONFIRMED | [CONTAINERS_WORLD_SAVES §2](../formats/CONTAINERS_WORLD_SAVES.md) |
| `*.SET` (dBase III tables) | `0x58ae90` | CONFIRMED | same, §2.2 |
| `.SMO` static mesh v500 | `0x5e0530` (model dispatcher `0x5af5d0`) | CONFIRMED | [MODELS_AND_ANIMATION](../formats/MODELS_AND_ANIMATION.md) |
| `.AM1` skinned mesh v500 | `0x5dc420` | CONFIRMED | same |
| `.AM3` skeleton | `0x5b9af0` | CONFIRMED | same |
| `.AM2` animation clips | `0x5b23d0` | CONFIRMED | same |
| Texture folder selection / DDS | `0x608e10` | HIGH CONFIDENCE | same, §DDS |
| `.GRI` terrain grid | `0x4f6eb0` (RLE escape `0xFF` at `0x4f7286`) | CONFIRMED | [CONTAINERS_WORLD_SAVES §4.1](../formats/CONTAINERS_WORLD_SAVES.md) |
| `.PFD` navigation mesh | see Part C | CONFIRMED (all 38 files parse) | [GAMEPLAY_AI_TROOPS Part C](GAMEPLAY_AI_TROOPS.md) |
| `.fin` fonts | `0x624810` | CONFIRMED | [CONTAINERS_WORLD_SAVES §5](../formats/CONTAINERS_WORLD_SAVES.md) |
| `CONFIG.DAT` (0xB05-byte dump of `0xa57fb8`) | load `0x45aaf0`, save `0x459e90` | CONFIRMED structure | same, §6 |
| `.SAV` header / body | write `0x478cc0`, read `0x478520`, world `0x4735c0` | CONFIRMED header; body = raw object dumps | same, §7 |
| `Player.dat` | `0x4c8810` / `0x4c8bb0` | HIGH CONFIDENCE | same, §7 |
| `.sce` mission scripts | loader `0x645de0`, compiler/executor | CONFIRMED | [SCRIPT_VM](SCRIPT_VM.md), [SCRIPTS_AND_GAME_DATA](../formats/SCRIPTS_AND_GAME_DATA.md) |
| Builtin table (407 entries) | table `0xa1a250`, dispatcher `0x649920` | CONFIRMED | [SCRIPT_VM](SCRIPT_VM.md) |
| `.nvo` vertex shaders | linked by `shader/nvlinker.dll`, loader `0x618990` | CONFIRMED | [RENDERING_PIPELINE](RENDERING_PIPELINE.md) |
| `.wma` / `.wmv` | Windows Media Format 9 (via ole32/COM) | CONFIRMED import evidence | [ENGINE_IDENTIFICATION](ENGINE_IDENTIFICATION.md) |

Load order observed at runtime (Wine `+file`): [ORIGINAL_BEHAVIOR](ORIGINAL_BEHAVIOR.md).
