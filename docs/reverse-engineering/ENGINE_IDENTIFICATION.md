# Engine identification — evidence

Conclusion summary lives in `docs/ENGINE_IDENTIFICATION.md`; this file holds the evidence.

## Engine: Enlight Software in-house C++ codebase (CONFIRMED)

- 117 MSVC `__FILE__` strings under `d:\ww\` ("ww" = Wars & Warriors), grouped into libraries: `d:\ww\*.cpp` (game), `ilib\` (Enlight I-Lib: `audio, bitmap*, blob, db, dynarr, dynarrb, filemem, filetxt, flc, gameset, mousecr, resdb, reslx, resx, se, sound3d, txtres, vqueue`), `elib\jpeg.cpp`, `en3d\` (3D engine: `actor, actordef, actordeg, animdef, bonetree, morph, plane, plane_sh, pshader, reorder, scene, scene_s, skin, sysen, texdir, texturea, vgabuf, vgalock`), `en2d\` (`bmpres, font2d, image2d, popupwin, txtbox2d, widget2d`), `uti3d\` (`blkarrb, dlgres, esstate2, lflare, pathsp, spindex, video, videocap`), `interfac\` (menus). Full address ranges: `FUNCTION_MAP.md`.
- I-Lib naming and idioms match Enlight's open-sourced Seven Kingdoms code (GPL, 7kaa): `DynArray`/`DynArrayB` with 1-based record numbers (`DynArrayB_get_ptr` `0x41b3f0`), `ResourceIdx` (`resx.cpp`, `0x59c550`), `mem_add/mem_del(size, __FILE__, __LINE__)` (`0x58e7b0`/`0x58ed70`), `err.run()` message box (`0x586bc0`), `Sys`/`Vga`/`Config` global singletons, `misc.is_file_exist`. HIGH CONFIDENCE (structural match, not code identity).
- Leftover strings from Enlight's Restaurant Empire (2003) share the binary: `MusicRestaurant%d`, `MusicCookingContestGo`, `TalkingWithCustomer`, `FinancialReport` (`0x9d58d8..0x9d5ad0`) — the `en3d` engine is shared between the two titles. HIGH CONFIDENCE.
- Window class / title `JoanOfArc` / "Wars & Warriors: Joan of Arc" (`Sys_create_window` `0x4d6e00`). PDB `D:\ww\Joan_Release\joan.pdb`.

## C++ class names

No RTTI (`/GR` off): only `.?AVtype_info@@`. Class names recovered from:
- Mangled symbols leaked into `JoanofArc.exe` padding (CONFIRMED, regex `\?\w+@\w+@@` over the file): `Scene::{render(View3D*), render_ordered(RenderOrder*,View3D*), render_reflection, draw_shadow(View3D*,SceneSector**,int,int), render_sector_plane_with_shadow, set_shadow_enabled, get_dot3_light/set_dot3_light(Light*), set_dot3_constant, set_bump_mapping_light, create_occluder/turn_on_occluder/turn_off_occluder, add_billboard(Billboard*), build_sector_struct_recur(SceneSectorTreeNode*)}`, `TriggerSystem::{init,deinit,process,add_trigger(TriggerType,SourceType,…,_D3DVECTOR,…,SceneSector*),del_trigger,is_deleted}`, `Trigger::{init,deinit,update_pos,is_expired,trigger_name_to_type}`, `Box3d`, `Sphere3d`, `SkinVertex`. Also fragments of `pchara.cpp` ("player char array", includes `../dbserver/db…`).
- Profiler section table at `0x9fb2b8` (99 entries × 0x32 bytes, CONFIRMED): `View3D::render`, `Scene::render`, sector/plane/actor render stages, `RenderOrder set key`, `unit process …`, `physics …`, `find path`, `find polygon`, `particle …`, `dynamic grass process`, `flip (Awaiting GPU to render)`.
- `Loading <x>` strings in `game.cpp` init (`0x9d1ed8..0x9d3498`) name the global singletons: `unit_array`, `item_array`, `nation_array`, `world_array`, `trigger_system`, `physis_sys`, `sound_man`, `music_res`, `model_res`, `animation_res`, `particle_emitter_res`, `dynamic_grass`, `water_array`, `glowing_light_array`, `lens_flare_res`, `profile_array`, `game_file_array`, ~60 `*_interface(_res)` objects.

## Compiler / runtime

MSVC 6.0 (C/C++ build 8168, linker 6.00), `/ZI` + `/INCREMENTAL` + unoptimised codegen, no RTTI, packed structs, static MSVC 6 CRT. HIGH CONFIDENCE. Details: `EXECUTABLE_ANALYSIS.md`.

## Graphics: Direct3D 8 (CONFIRMED)

- `Direct3DCreate8(0x78)` at `0x5f6b60`: `D3D_SDK_VERSION` 120 = DirectX 8.0 headers.
- Device: fullscreen only, 1024×768 hard-coded, HAL, hardware/mixed/software vertex processing chosen by caps and `*.SYS` flags, no vsync (`D3DPRESENT_INTERVAL_IMMEDIATE`). See `RENDERING_PIPELINE.md`.
- Mixed fixed-function + programmable: NVIDIA NVLink (`SHADER\nvlinker.dll` `CreateLinker`) assembles vertex shaders from `.nvo` fragment objects (magic `NVFO`, fragments `NVFN…`; files `default_shader, transform, lighting, fog, scatter, hardware_skin, clip_plane`); binary pixel shaders `clip_am.pso`/`clip_at.pso` (version token `0xFFFF0100` = ps.1.0, `xxd`); `item\dot3_directional.vsc`. Statically linked D3DX8 incl. its shader assembler.
- Software skinning fallback in `v3d_*.dll` (`skin_deform`, `SkinVertex`) selected per CPU (x87 / 3DNow!(k3d) / SSE).

## Audio (CONFIRMED)

- DirectSound (`DSOUND` ordinal 1, `Audio_init_dsound` `0x57a740`) in `ilib\audio.cpp`, plus MCI (`mciSendString/Command`) and the WinMM mixer for volume.
- 3D positional sound via OpenAL 1.0 (`OpenAL32.dll` import, device `"DirectSound3D"` opened in `0x7a9420`, sources/buffers/queueing in `ilib\sound3d.cpp`, `ilib\se.cpp`). EAX: `eax.dll`/`EaxAc3.dll` ship in the install dir; no direct `EAX` string use was found in `joan.exe` (HYPOTHESIS: EAX is reached only through the Creative OpenAL implementation).
- DirectMusic 8 (`CoCreateInstance` CLSID_DirectMusicLoader `d2ac2892-…`, CLSID_DirectMusicPerformance `d2ac2881-…`, IID_*8) at `0x582d20`.
- Music `.wma` and movies `.wmv` via Windows Media Format 9 (`wmvcore.dll` `WMCreateSyncReader`, `uti3d\video.cpp` `0x684650`); error strings say WMF 9 is required for "background and environmental music". Movies: `MOVIE\ENLIGHT.WMV`, `TREVOR.WMV`, `INTRO.WMV`, `WW_CAP.WMV`.

## Input (CONFIRMED)

DirectInput 8 mouse + keyboard (`Input_init_dinput` `0x592380`, "Failed creating mouse/keyboard interface from DirectInput"), `GetCursorPos`/`GetKeyState` for Win32 state, key remapping in `remapkey.cpp` (`0x4cae70..0x4cb310`). No joystick device creation was found.

## Scripting (CONFIRMED present)

Own script compiler in `uti3d\esstate2.cpp` (`Script_open_compile` `0x645de0`; errors "Expecting FUNC/DIALOG/CAMERA/ACTION!", "Unhandled exception in ScriptStatementCompile::compile."). Script dirs `SCRIPT\SCENARIO\`, `SCRIPT\UNIT\`, `SCRIPT\PSCRIPT\*.txt` (particle scripts, `pemires.cpp`). Native bindings are a name table in `.data` (`call_gameover_msg`, `save_game`, `start_cut_scene_camera`, `turn_camera`, `mount_siege_tower_to_wall`, …, `0xa1a308..0xa200c8`). Detailed grammar: ScriptsGameplay agent's docs.

## Physics / collision / AI (HIGH CONFIDENCE, names only)

Profiler labels: `physics process`, `physics check pass through`, `physics get slope height`, `collision detection process`, `find path`, `find polygon` (polygon navigation, `uti3d\pathsp.cpp` `0x670980..0x6745e0`), `unit process {action,move,rotate,script}`, `flocksys.cpp` (`0x46dfa0..0x46eed0`), `uti3d\spindex.cpp` spatial index. Loaded as `physis_sys`. No third-party physics library.

## Save / config

`CONFIG.DAT` = raw dump of the 0xB05-byte config object (`Config_load` `0x45aaf0`). Saves `SAVE\*.SAV` via `GameFile_save` `0x478180` / `GameFile_load` `0x477e80`; player profile in `SAVE\` (`profile.cpp`). See `FUNCTION_MAP.md` → save.
