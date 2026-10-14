# Executable analysis

All addresses are VAs. Evidence commands: `pefile` (Python), `objdump -D -b binary -mi386 -Mintel`, Ghidra 12.1.2 headless (`tools/ghidra/run_headless.sh`).

## Binaries

| file | role | SHA-256 (first 16 hex) | PE timestamp | linker | notes |
|---|---|---|---|---|---|
| `joan.exe` | game | `b25e2154af3ffc87` | `0x400ba40a` (2004-01-19) | 6.0 | 6,677,952 bytes, ImageBase 0x400000, entry `0x7b71e6`, PDB path `D:\ww\Joan_Release\joan.pdb` |
| `JoanofArc.exe` | launcher | `1616e1f8f9947633` | `0x3ff52ba2` (2004-01-02) | 6.0 | imports KERNEL32/USER32/WINMM only; string `JOAN.EXE` (ref. `FUN_00408540`); its padding contains leaked Enlight source fragments and mangled symbols |
| `VSetting.exe` | settings tool | `15f923090869b9ce` | `0x400b8eb4` (2004-01-19) | 6.0 | writes `CONFIG.DAT`/`OPTION.DAT`, registry `Software\Enlight Software\Joan of Arc`, launches `JoanofArc.exe`, DirectX 8.1 redist paths |
| `v3d_87.dll` (= `_k3d`/`_sse`/`_x87` variants) | math/skinning | `3d60844e1acc322a` | `0x3d61eb41` (2002-08-20) | 6.0 | exports C++ `vector_*`, `matrix_*`, `skin_deform`, `SkinVertex::*`, `Box3d`, `Sphere3d`, `vertexf_check_cpu` |

CONFIRMED (pefile).

## joan.exe sections

| name | VA | virtual size |
|---|---|---|
| .text | 0x401000 | 0x5cd700 |
| .rdata | 0x9cf000 | 0x24c86 |
| .data | 0x9f4000 | 0x78d44 |
| .idata | 0xa6d000 | 0x25fd |
| .rsrc | 0xa70000 | 0x13d0 |
| .reloc | 0xa72000 | 0x23699 |

## Imports (CONFIRMED)

| DLL | imports | users (Ghidra xrefs) |
|---|---|---|
| d3d8 | `Direct3DCreate8` | `0x5f6b60` |
| DINPUT8 | `DirectInput8Create` | `0x592380` |
| DSOUND | ordinal 1 (`DirectSoundCreate`) | `0x57a740` |
| OpenAL32 | 32 `al*`/`alc*` (sources, buffers, listener, queueing) | `0x7a9420` (device "DirectSound3D"), `0x4907e0`, `0x59ee00..0x59f1a0` |
| v3d_87 | 23 math/skin exports | throughout `en3d` |
| ole32 | `CoInitialize`, `CoCreateInstance`, `CoUninitialize` | `0x582d20` (DirectMusic 8 loader/performance) |
| WINMM | `timeGetTime`, `mciSendString/Command`, `mixer*` | `0x591050`, ilib audio `0x57a8xx`, `0x57e7xx` |
| comdlg32 | `GetOpenFileNameA` | `0x52f930` (debug "load script/world" dialog) |
| ADVAPI32 | `RegOpenKeyEx/QueryValueEx/SetValueEx/CloseKey` | `0x5bf130..0x5bf580` (DX8 SDK DXUtil registry helpers) |
| KERNEL32/USER32/GDI32 | standard | — |

Dynamically loaded (LoadLibraryA + GetProcAddress, CONFIRMED by strings in the same functions): `V3D_SSE.DLL`/`V3D_K3D.DLL`/`V3D_X87.DLL` (`0x447150`), `SHADER\nvlinker.dll` `CreateLinker` (`0x618990`), `wmvcore.dll` `WMCreateSyncReader` (`0x684650`), `d3d8.dll` inside the static D3DX shader assembler (`ValidateVertexShader`/`ValidatePixelShader` strings), `d3dxof.dll` (D3DX .x loader).

## Toolchain (see ENGINE_IDENTIFICATION.md for the conclusion)

Rich header of `joan.exe` (prodid, build, count), CONFIRMED by `pefile.parse_rich_header()`:
`(12,7291,15) (14,7299,50) (48,9044,21) (4,8168,2) (4,8447,2) (28,8830,4) (37,8755,9) (29,8830,71) (19,8034,15) (1,0,251) (18,8444,42) (10,8168,210) (11,8168,373) (6,1720,1)`.

- prodid 10/11 build 8168 = MSVC 6.0 C / C++ compiler (`cl 12.00.8168`) — 583 objects. HIGH CONFIDENCE.
- prodid 4 build 8168/8447 = VC6 linker; optional header linker version 6.0. HIGH CONFIDENCE.
- prodid 28/29/37 builds 8755/8830 (75 objects) and prodid 48 build 9044: a second, newer Microsoft toolchain. HYPOTHESIS: these are the statically linked DirectX 8 SDK libraries (`d3dx8.lib`, `dxguid`), which shipped built with a VC7-era compiler; `v3d_87.dll` carries the same 29/8830 entry.
- Code generation is unoptimised debug-style: every function has `push ebp; mov ebp,esp; sub esp,0x40+locals; push ebx/esi/edi`, locals are spilled, `__LINE__` is read from a per-function static (e.g. `DAT_009f7728 + 5` in `0x423600`), which is what `/ZI` (Edit & Continue) produces. The 8,015-entry incremental-link thunk table at `0x401005..0x40ac8c` confirms `/INCREMENTAL`. HIGH CONFIDENCE. Practical effect: the decompile is close to the source; thunks hide `this`.
- No RTTI: the only type descriptor is `.?AVtype_info@@` (`work/ghidra/export/rtti.tsv`), so the game was built without `/GR`. CONFIRMED. C++ class names come from leaked symbols instead (see ENGINE_IDENTIFICATION.md).
- Structures are byte-packed (`#pragma pack(1)`): field offsets such as `+0x19`, `+0x2f`, `+0x2db`, `+0x307` in hot objects. HIGH CONFIDENCE.

## Statically linked third-party code (CONFIRMED by strings)

- D3DX 8 (`D3DXERR_*` names, `D3DX8 Shader Assembler Version 0.91`, `.x` templates, `ID3DXMeshOptimize`), e.g. `D3DXMatrixPerspectiveFovLH` at `0x7674a4`, `D3DXMatrixOrthoLH` `0x767723`, `D3DXCreateTexture` `0x7648ba`.
- DirectX 8 SDK sample framework: `DXUtil_GetDXSDKMediaPath` (`0x5bf130`, "DX8SDK Samples Path"), CD3DFont (`CreateFontA` `0x5baae0`), CD3DCamera (`0x5bec90`).
- IJG libjpeg ("Copyright (C) 1996, Thomas G. Lane" → v6), libpng 1.0.5, zlib, MSVC 6 CRT.

## Startup sequence (CONFIRMED from disassembly of `WinMain` 0x420ea0)

1. `V3d_select_cpu_dll` `0x447150`: picks the fastest `V3D_*.DLL` whose `vertexf_check_cpu()` passes and copies it over `v3d_87.dll` if different (the game imports `v3d_87.dll` statically).
2. `Config_init` / `Config_load("CONFIG.DAT")` on the config object `0xa57fb8`; if missing: `CpuSpeed_measure` (`0x45c8c0`, object `0xa485a8`) and `Config_set_defaults`.
3. `Sys_init` (`0x4d6c50`, object `0xa50088`) → `Vga_init` (`0x603710`, D3D object `0xa52420`) → `Sys_init_subsystems` (`0x4d6fe0`: audio, OpenAL, code page, fonts, …).
4. Error-box hooks on error object `0xa47d78`, `Config_validate_caps`, optional 1024×1024 render-target depth surface (`config+0x9d == 2`), 512×256 render texture.
5. `Game_main` (`0x479720`, object `0xa58f90`) → `Game_run` (`0x42ca90`) → `Sys_main_loop` (`0x499960`).
6. `Sys_deinit` (`0x4d6ce0`).

## Debug / override flag files

Presence checks through `Misc_is_file_exist` (`0x590b80`, FindFirstFileA), CONFIRMED: `MIXEDVP.SYS`, `SOFTVP.SYS` (vertex processing), `SOFTBONE.SYS` (disable hardware skinning), `free_world_video_memory.sys` (text file with a VRAM threshold, `0x44c800`), `no_save_preview_pict.sys` (`0x4731e0`).
