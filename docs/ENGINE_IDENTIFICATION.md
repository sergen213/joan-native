# Engine identification — Wars and Warriors: Joan of Arc (`joan.exe`, 2004-01-19)

Evidence and addresses: `docs/reverse-engineering/ENGINE_IDENTIFICATION.md`, `EXECUTABLE_ANALYSIS.md`, `FUNCTION_MAP.md`.

| aspect | finding | confidence |
|---|---|---|
| Engine | Enlight Software in-house C++: `ilib` (Seven Kingdoms-lineage utility library), `en3d` 3D engine, `en2d` UI, `uti3d`, game code in `d:\ww\`; engine shared with Restaurant Empire (2003) | CONFIRMED (source paths), HIGH (lineage) |
| Compiler | MSVC 6.0 (cl 12.00.8168, link 6.00), `/ZI`, `/INCREMENTAL`, unoptimised, no RTTI, `#pragma pack(1)` structs | HIGH |
| Graphics | Direct3D 8 (SDK 8.0 headers), fullscreen 1024×768 only, fixed-function + vertex shaders assembled at runtime by NVIDIA NVLink from `NVFO` fragment files + `ps.1.0` binary pixel shaders; static D3DX8 and DX8 SDK sample framework | CONFIRMED |
| Math | `v3d_87.dll` (per-CPU x87/3DNow!/SSE builds copied in at startup), incl. software skinning | CONFIRMED |
| Audio | DirectSound (2D, MCI, mixer), OpenAL 1.0 "DirectSound3D" device for 3D sound, DirectMusic 8, WMF 9 (`wmvcore`) for `.wma` music | CONFIRMED |
| Video | WMF 9 synchronous reader for `MOVIE\*.WMV` | CONFIRMED |
| Input | DirectInput 8 keyboard + mouse; no joystick | CONFIRMED / HIGH |
| Scripting | in-house compiler (`uti3d\esstate2.cpp`) for `SCRIPT\SCENARIO`, `SCRIPT\UNIT`, particle scripts `SCRIPT\PSCRIPT` | CONFIRMED |
| Physics | in-house (`physis_sys`, slope height, pass-through, polygon path finding) | HIGH |
| Image libs | IJG libjpeg 6, libpng 1.0.5, zlib (static) | CONFIRMED |
| Main loop | Win32 message pump + `MsgWaitForMultipleObjectsEx`, 30 Hz frame/logic timer, speed factor = elapsed_ms/1000·30 clamped to 5 | CONFIRMED |
| Config | `CONFIG.DAT` = raw 0xB05-byte dump of the config object; written by the game and `VSetting.exe` | CONFIRMED |

Port implications: 4:3 / 1024×768 is a hard assumption of the original renderer (projection aspect is viewport W/H, so Hor+ is natural); the 30 Hz tick must be preserved for gameplay timing; the D3D8 shader fragments are NVIDIA-linker assembly and must be re-expressed in GLSL.
