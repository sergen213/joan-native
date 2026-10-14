# Source audit — original installation

Audited tree: the user's GOG install (repo root), 2026-10-13. Original files are untracked (whitelist `.gitignore`) and were only read.
Method: Python (`hashlib`, `pefile`), `7z`, `openssl pkcs7`. Hashes below are SHA-256 unless stated.

## 1. Identification

- GOG product `goggame-1421210170.info`: gameId/rootGameId `1421210170`, buildId `59908888400692417`, language en-US; play tasks `JoanofArc.exe` (primary, category launcher) and hidden `joan.exe` (category game).
- Installer: Inno Setup uninstaller `unins000.exe` (file version 51.1052.0.0, link time 2018-08-20) + `unins000.dat/.ini/.msg`; `goglog.ini`; icons; `Launch Wars and Warriors - Joan of Arc.lnk`.
- **Integrity: CONFIRMED pristine.** Every one of the 9,567 entries in `goggame-1421210170.hashdb` exists and its MD5 matches (0 mismatches, 0 missing).

### 1.1 `goggame-1421210170.hashdb` format (CONFIRMED by parsing)

- Outer: ZIP archive (241,801 bytes) with one deflated member named `goggame-1421210170.hashdb` (10,102,764 bytes uncompressed).
- Inner, little-endian:
  ```
  u32 12                  // header size (HYPOTHESIS; value constant)
  u32 1                   // format version (HYPOTHESIS)
  u32 count               // 9567
  count × {
      char path[1024];    // relative path, '\' separators, NUL-padded, original case
      char md5_hex[32];   // lowercase hex MD5 of file contents, no terminator
  }
  ```
  12 + 9,567 × 1,056 = 10,102,764 — exact.
- Four entries are zero-length files (MD5 `d41d8cd98f00b204e9800998ecf8427e`), e.g. `Resource\ANIM_AM3.RES`, `fastflip.sys`.
- Usable for "Verify Files" against a GOG install (MD5 only; our importer additionally pins SHA-256 in `src/install/known_versions.json`).
- Not covered by the hashdb (expected: generated at runtime or by the installer): `CONFIG.DAT`, `Save/Player.dat`, `Save/Mission1-AutoSave.SAV/.BMP`, `map/scenario1/S1L1.PLN` (5,735,818 B; HYPOTHESIS: runtime-generated cache), `goggame-1421210170.{hashdb,info,ico}`, `unins000.*`, `gog.ico`, `support.ico`, `goglog.ini`, the `.lnk`.

## 2. Inventory (hashdb-covered files)

| Dir | Files | Bytes | Main types |
|---|---|---|---|
| (root) | 13 | 9,861,125 | 8 `.dll`, 3 `.exe`, `fastflip.sys` (0 B), `readme.htm` |
| `anim` | 247 | 105,264,355 | `.am2` 123, `.am3` 122, `.am1` 2 |
| `des` | 25 | 10,405 | `.txt` |
| `font` | 19 | 60,496 | `.fin` |
| `images` | 2 | 13,967 | `.jpg` |
| `int8x6` | 532 | 145,977,322 | `.dds` 469, `.tga` 63 |
| `item` | 2,924 | 51,647,392 | `.smo` 1,067, `.pro` 819, `.am3` 459, `.am1` 243, `.am2` 231, `.nod` 46 |
| `item_des` | 81 | 14,094 | `.txt` |
| `manual` | 1 | 17,286,386 | `JOA manual.pdf` |
| `map` | 394 | 51,652,046 | `.pfd` 38, `.map` 37, `.lgt` 34, `.por` 32, `.pla` 32, `.gri` 32, … |
| `model` | 79 | 3,430,901 | `.smo` 67, `.am3` 6, `.am1` 6 |
| `movie` | 2 | 20,798,462 | `.wmv` |
| `music` | 40 | 40,955,446 | `.wma` |
| `Resource` | 35 | 142,302,152 | `.res` 23, `.pro` 5, `.set` 2, `.txr` 2, `.cm` 2 |
| `scenario` | 8 | 1,762 | `.scr` |
| `script` | 345 | 2,779,852 | `.txt` 266, `.sce` 35, `.res` 29, `.obj` 8, `.bm` 7 |
| `shader` | 10 | 329,583 | `.nvo` 7, `.pso` 2, `nvlinker.dll` |
| `texture` | 4,013 | 335,320,948 | `.dds` 3,876, `.bmp` 102, `.tga` 35 |
| `unit` | 794 | 22,050,016 | `.am3` 357, `.am1` 337, `.smo` 94, `.am2` 3, `.res` 2, `.mda` 1 |
| `util` | 1 | 4,085,904 | `wmfdist.exe` |
| `weblinks` | 2 | 154 | `.url` |
| **Total** | **9,567** | **953,842,768** | |

## 3. Executables and DLLs (all PE32 i386)

| File | Size | SHA-256 | Link time (UTC) | Origin (version resource / evidence) | Authenticode |
|---|---|---|---|---|---|
| `joan.exe` | 6,677,952 | `b25e2154af3ffc876545af60a2c8b90380d4327541cd38841ef327411c3b2fa5` | 2004-01-19 | Enlight; CodeView `D:\ww\Joan_Release\joan.pdb` | Enlight Software Limited (Sectigo R36) |
| `JoanofArc.exe` | 271,800 | `1616e1f8f9947633ac913088c44d3a316fee474d087b771c2a87b61e3b46ca8b` | 2004-01-02 | Enlight launcher | Enlight Software Limited |
| `VSetting.exe` | 1,500,616 | `15f923090869b9cedb083a81390aa0bee95a77501cc847146d59f38e71ddecd6` | 2004-01-19 | "RESetup MFC Application" | yes |
| `unins000.exe` | 1,343,048 | `5761e7789d813626cd68ee1e62429cfeb92bdd814cd29ef12fc4ae9ec1dbaff3` | 2018-08-20 | Inno Setup (GOG) | yes |
| `util/wmfdist.exe` | 4,085,904 | `fd0754a2ef3567859db0bf3c75f18ec50aaeae6a7561aff9e7f6c7775a945ed7` | 2001-08-18 | Microsoft Windows Media Format 9 redistributable | yes |
| `OpenAL32.dll` | 114,688 | `9dc35dfd8eec9a22ffc991b644dad713b277cc2d70ea2e0867a54b55eacc70d6` | 2003-09-04 | Creative Labs "OpenAL32 Wrapper 0.099" | no |
| `ALut.dll` | 5,120 | `b67a9afbf0bdbb6a41d6efce77ba6c301235c883aa5b99a57e635a5309fda331` | 2002-09-09 | NVIDIA OpenAL Utility Library 5.10.2879.0 | no |
| `eax.dll` | 98,304 | `b029ad5a101fc31a346d4d49a0de5f48836c4bad430474b9f8824c4bbc3bde04` | 2001-02-22 | Creative Technology EAX 2.06 | no |
| `EaxAc3.dll` | 61,440 | `7a4b2845c3174021d5ed070d0d14b83f76fd03b7b202c22a386f22a4e4d29e51` | 2001-06-05 | Creative Labs EAX-AC3 1.12 | no |
| `v3d_87.dll` | 270,375 | `3d60844e1acc322a9484b3c944a858f6cb6397c89e84bd4f02f5cb11e3e893f9` | 2002-08-20 | no version info; exports `SkinVertex`, `dot_product`, … (HYPOTHESIS: Enlight in-house math/skinning) | no |
| `v3d_sse.dll` | 270,375 | identical to `v3d_87.dll` (CONFIRMED same SHA-256) | 2002-08-20 | as above | no |
| `v3d_k3d.dll` | 270,375 | `f41cdabc7c1f82bb28f115912c80e35e3bbbc40a96640cc190b3b2423a82d7a7` | 2002-09-27 | as above (3DNow! variant by name) | no |
| `v3d_x87.dll` | 270,374 | `64e38da951cc42800f28b93cfb681646b6094a0edeaf77284e3ce272d2fe1484` | 2002-08-20 | as above | no |
| `shader/nvlinker.dll` | 319,567 | `d905e61ce8993064600a967ef9511d2e904bdc55e1c39cc2afd24660447defd0` | 2001-08-09 | NVIDIA NVLink "Vertex Shader Combiner" 2.1 | no |
| `fastflip.sys` | 0 | `e3b0c442…b855` (empty) | — | placeholder | — |

`joan.exe` imports KERNEL32, USER32, GDI32, comdlg32, ADVAPI32, ole32, WINMM, d3d8, DINPUT8, DSOUND, v3d_87, OpenAL32 (from the coordinator brief; `work/ghidra/export/imports.tsv`). The GOG exe is the 6,672,428-byte Steam/unsigned layout plus a 5,520-byte Authenticode blob (see CAMERA_AND_WIDESCREEN.md §3).

## 4. Classification

| Category | Items | Treatment in this project |
|---|---|---|
| Proprietary game code (Enlight) | `joan.exe`, `JoanofArc.exe`, `VSetting.exe`, `v3d_*.dll`, scripts `script/**` (as data) | Never redistributed, never linked. Used only as RE input on the user's machine. Behaviour reimplemented independently (formats, engine, launcher, settings). |
| Proprietary game data (Enlight) | every data dir in §2 (`Resource`, `anim`, `unit`, `item`, `model`, `map`, `scenario`, `script`, `texture`, `int8x6`, `font`, `des`, `item_des`, `shader/*.nvo/*.pso`, `music`, `movie`, `manual`, `readme.htm`, `images`) | **User-supplied.** Read in place or imported from the user's own GOG/Steam/retail install; verified against hashes; never in git or release packages. |
| Third-party proprietary runtime (Creative, NVIDIA, Microsoft) | `OpenAL32.dll`, `ALut.dll`, `eax.dll`, `EaxAc3.dll`, `nvlinker.dll`, `wmfdist.exe` | Not needed natively and not redistributed. Replaced by OpenAL Soft or miniaudio (audio), our own shader path (Vulkan/GL), FFmpeg (WMA/WMV decode). |
| Platform APIs | D3D8, DirectInput8, DirectSound, WinMM, GDI, COM | Replaced by SDL3 + Vulkan/OpenGL + our audio backend. |
| Store/installer artefacts (GOG) | `goggame-*`, `unins000.*`, `.lnk`, icons, `goglog.ini`, `weblinks/` | Ignored, except `goggame-1421210170.hashdb` read for file verification. |
| User state | `CONFIG.DAT`, `Save/*` | Imported (copied) on request; format reimplemented; never shipped. |

Must be independently implemented: all file-format readers (`.RES`, `.SMO`, `.AM1/2/3`, `.pro`, `map/*`, `.scr`, `.fin`, `CONFIG.DAT`, `.SAV`), renderer, camera/projection (see camera doc), input, audio mixing, script interpreter, game simulation, UI, launcher/settings (`VSetting` replacement), installer/importer, updater.
Decompiler output and disassembly guide the reimplementation but are never copied into tracked source.
