# joan.exe function map

Generated from `tools/ghidra/names.tsv` (the same file `ApplyNames.java` applies to the Ghidra project). Addresses are VAs in `joan.exe` (ImageBase 0x400000). Decompiler output is never verified source; `CONFIRMED` means the behaviour is directly visible in disassembly/decompile (API call, string, constant), `HIGH` means strong contextual evidence, `HYPOTHESIS` means plausible role only.

Calling convention: nearly all game functions are MSVC `__thiscall` (object in ECX). Ghidra shows `this` as `in_ECX` because calls go through the incremental-link thunk table (ILT, `0x401005..0x40ac8c`, 8015 `jmp rel32` stubs). The object a call operates on is visible only in disassembly (`mov ecx, imm32` before the `call`).

Totals: 18,802 functions in Ghidra (6,308 of them ILT thunks); 134 named here.

## startup

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x420ea0` | `WinMain` | CONFIRMED | called from CRT entry 0x7b71e6 with GetModuleHandleA(0),cmdline,nShow; ret 0x10 |
| `0x447150` | `V3d_select_cpu_dll` | HIGH | LoadLibrary V3D_SSE/K3D/X87, GetProcAddress vertexf_check_cpu, CopyFileA over v3d_87.dll |
| `0x45c8c0` | `CpuSpeed_measure` | HIGH | rdtsc-like counter vs QueryPerformanceCounter over Sleep(100); only when CONFIG.DAT missing |
| `0x4d6c50` | `Sys_init` | HIGH | ecx=0xa50088; calls Vga_init then Sys_init_subsystems |
| `0x4d6ce0` | `Sys_deinit` | HIGH | ecx=0xa50088 after Game_main in WinMain |
| `0x4d6e00` | `Sys_create_window` | HIGH | RegisterClassA/CreateWindowExA class "JoanOfArc" |
| `0x4d6fe0` | `Sys_init_subsystems` | HIGH | Audio_init, Sound3D_init, CODEPAGE.RES, font loading |

## game

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x41b3c0` | `UnitArray_get` | HYPOTHESIS | wrapper of DynArrayB_get_ptr, 540 callers |
| `0x42ca90` | `Game_run` | HIGH | calls Sys_main_loop |
| `0x4715a0` | `Game_run_restartable` | HYPOTHESIS | loops Game_run while flag 0xa50098==4; no static refs |
| `0x479720` | `Game_main` | HIGH | ecx=0xa58f90 from WinMain; init, Game_run(0), deinit |
| `0x499540` | `Sys_set_screen_mode` | HYPOTHESIS | called when sys+0x3c != sys+0x3a |
| `0x499960` | `Sys_main_loop` | CONFIRMED | PeekMessage/MsgWaitForMultipleObjectsEx loop, frame timer, disp+process |
| `0x4e5fc0` | `Game_process_tick` | HIGH | called from main loop on logic tick; calls per-subsystem process |
| `0x58a830` | `FrameTimer_init` | HIGH | (30,30,1,now) in main loop: 30 fps draw/logic |
| `0x58aa90` | `FrameTimer_wait_ms` | HIGH | result passed to MsgWaitForMultipleObjectsEx timeout |
| `0x58ac80` | `FrameTimer_process_due` | HYPOTHESIS | gates Game_process_tick |
| `0x58acb0` | `FrameTimer_draw_due` | HYPOTHESIS | gates Sys_disp_frame |

## config

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x459e30` | `Config_set_defaults` | HIGH | calls 4 default groups when CONFIG.DAT missing |
| `0x459e70` | `Config_init` | HIGH | ecx=0xa57fb8, empty body |
| `0x459e90` | `Config_save` | CONFIRMED | Config_write_file("CONFIG.DAT") |
| `0x45a010` | `Config_default_display` | HIGH | 1024x768x32@60, detail by CPU MHz |
| `0x45a9f0` | `Config_write_file` | HIGH | File_create + write |
| `0x45aaf0` | `Config_load` | CONFIRMED | "CONFIG.DAT", requires file size 0xb05 |
| `0x45adc0` | `Config_read` | HIGH | reads 0xb05 bytes into config object, restores runtime fields |
| `0x45b240` | `Config_validate_caps` | HIGH | adapter id "geforce3", D3DXCreateTexture probes (RT 64x64, V8U8 256x256), clamps config+0x9c..0x9e |

## render

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x4189a0` | `ErrorBox_pre` | HIGH | registered via 0x4171b0 on error object 0xa47d78 |
| `0x418bc0` | `ErrorBox_post` | HIGH | registered via 0x4171b0 |
| `0x44c800` | `World_draw` | HIGH | culls sectors, copies camera->View3D, View3D_render |
| `0x4d48f0` | `Sys_disp_frame` | HIGH | switch on sys+0x3a screen mode; mode 9 -> World_draw |
| `0x5d8650` | `View3D_render_to_texture` | HIGH | Get/SetRenderTarget, Get/SetViewport, BeginScene/EndScene |
| `0x5e5a50` | `PixelShader_load` | HIGH | SHADER\clip_am.pso / clip_at.pso, CreatePixelShader |
| `0x5ebac0` | `Scene_render` | HIGH | called between BeginScene/EndScene |
| `0x5f6620` | `D3DDevice_init` | HIGH | Direct3DCreate8 then D3DDevice_create |
| `0x5f6b60` | `D3DDevice_create_d3d` | CONFIRMED | Direct3DCreate8(0x78) |
| `0x5f6c00` | `D3DDevice_create` | CONFIRMED | EnumAdapterModes 1024x768, CreateDevice, caps, "Cannot create z buffer" |
| `0x5ff0e0` | `D3D_present` | CONFIRMED | IDirect3DDevice8::Present(0,0,0,0), counts back buffers |
| `0x5ff180` | `D3D_present_until_front0` | HIGH | error-box helper |
| `0x5ff210` | `D3D_present_restore` | HIGH | error-box helper |
| `0x5ff920` | `D3D_is_display_format_ok` | HYPOTHESIS | filters modes in D3DDevice_create |
| `0x5ff9d0` | `D3D_choose_backbuffer_format` | HYPOTHESIS | BackBufferFormat in present params |
| `0x5ffc90` | `D3D_choose_depth_format` | HYPOTHESIS | AutoDepthStencilFormat |
| `0x5ffe60` | `D3D_format_class` | HYPOTHESIS | returns 4 for preferred (32-bit) format |
| `0x6002b0` | `D3D_error_string` | HIGH | maps HRESULT to "D3DERR_*" names |
| `0x601380` | `VertexShader_load_dot3` | HIGH | item\dot3_directional.vsc |
| `0x6032d0` | `D3D_create_rt_depth` | HIGH | CreateDepthStencilSurface(w,h); 1024x1024 when config+0x9d==2 |
| `0x603400` | `D3D_create_render_texture` | HIGH | "CreateTexture fails" (512x256) |
| `0x603710` | `Vga_init` | HIGH | creates window (vtbl+0x18), then Vga_init_d3d |
| `0x6037f0` | `Vga_init_d3d` | HIGH | D3DDevice_init, PAL_STD.RES, texture dir select, async loader |
| `0x603af0` | `Vga_flip` | HIGH | called after Sys_disp_frame; -> D3D_present |
| `0x608e10` | `Texture_select_dir` | HIGH | TEXTUREH\ / TEXTURE\ / TEXTUREL\ |
| `0x6141a0` | `View3D_update_matrices` | CONFIRMED | view=inverse(world), D3DXMatrixPerspectiveFovLH(atan(fovparam), w/h, zn, zf) or OrthoLH |
| `0x614760` | `View3D_render` | CONFIRMED | SetTransform(VIEW/PROJ), SetViewport, Clear, BeginScene, Scene_render, EndScene |
| `0x617370` | `View3D_render_prepass` | HYPOTHESIS | before BeginScene; per-light/sector pass |
| `0x618990` | `Shader_create_nvlinker` | CONFIRMED | SHADER\nvlinker.dll CreateLinker + 7 .nvo fragment files |
| `0x618ab0` | `Shader_add_fragment_file` | HIGH | called per .nvo |
| `0x618e40` | `Shader_build_vertex_shader` | HIGH | fragment names calculate_skin_vertex/write_d3d_fog_factor |

## d3dx

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x7648ba` | `D3DXCreateTexture` | HIGH | (dev,w,h,mips,usage,fmt,pool,out) |
| `0x7674a4` | `D3DXMatrixPerspectiveFovLH` | CONFIRMED | static d3dx8; fsincos(fovy*0.5); _11=cot/aspect; target of community FOV fix pattern |
| `0x767723` | `D3DXMatrixOrthoLH` | HIGH | ortho branch of View3D_update_matrices |

## dxsdk

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x5baae0` | `CD3DFont_InitDeviceObjects` | HIGH | CreateFontA in DX8 SDK framework region |
| `0x5bec90` | `CD3DCamera_SetProjParams` | HIGH | DX8 SDK d3dutil: stores fov/aspect/near/far then PerspectiveFovLH |
| `0x5bf130` | `DXUtil_GetDXSDKMediaPath` | CONFIRMED | "Software\Microsoft\DirectX","DX8SDK Samples Path" |

## camera

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x423600` | `CamMan_init` | HIGH | camman.cpp; t=1.0 |
| `0x4236d0` | `CamMan_deinit` | HIGH | camman.cpp list free |
| `0x4237a0` | `CamMan_update` | CONFIRMED | easing modes 1-4, wraps yaw, lerps 10-dword state, collision |
| `0x423e70` | `CamMan_reset_mode` | HIGH | mode=1, interp=1, no-collision=1 |
| `0x423ec0` | `CamMan_start_transition` | HIGH | from,to 10-dword states, start time |
| `0x423f40` | `CamMan_set_target` | HIGH | current->from, param->to, start time |
| `0x424010` | `CamMan_collision_adjust` | HYPOTHESIS | ray tests (0x5d4a80) push camera out of geometry |
| `0x425b90` | `Cam_lerp_angle` | HIGH | wrap-aware yaw interpolation |
| `0x425c80` | `Cam_lerp_float` | HIGH | per-field interpolation |
| `0x428e90` | `CamRes_load_tracks` | HIGH | CreateCameraTrackArray/CreateCameraTrack/CreateCameraAnimatedTrack |
| `0x42bcd0` | `Camera_mouse_aim` | HYPOTHESIS | mouse deltas 0xa516f4/f8, invert 0xa5802b, camera 1 |
| `0x43f000` | `Camera_follow_update` | HYPOTHESIS | uses config auto-turn angle 0xa5802e (default 90) |
| `0x445990` | `Camera_rts_edge_scroll` | HYPOTHESIS | screen-edge scroll with frame ms 0xa500b6 |
| `0x563c80` | `CameraArray_get` | CONFIRMED | this->array + idx*200 |
| `0x563dc0` | `CameraArray_current_index` | CONFIRMED | returns this+0x2f |
| `0x613730` | `View3D_init` | CONFIRMED | zn=1.0, zf persp=8193.0, zf ortho=16385.0, camera block defaults, SetMaterial(1.0) |
| `0x613bf0` | `View3D_set_viewport` | CONFIRMED | (x1,y1,x2,y2): w=x2-x1+1, h=y2-y1+1 |
| `0x614040` | `View3D_set_camera_params` | HIGH | copies 10-dword camera block to view+0xa0 |
| `0x6153c0` | `View3D_project` | HIGH | world->screen with bounds check |
| `0x6154d0` | `View3D_unproject` | HIGH | screen->ray, uses proj _11/_22 |
| `0x615840` | `View3D_point_visible` | HIGH | frustum test |
| `0x6159f0` | `View3D_sphere_visible` | HIGH | 6 frustum planes at view+0x326 + user planes |
| `0x616d50` | `CameraState_init` | CONFIRMED | zero 0x28 bytes; +0x1c=100.0, +0x20 k=1.0, +0x24=1.0 |
| `0x616ec0` | `View3D_get_view_matrix` | HIGH | returns view+0xee |
| `0x616ef0` | `View3D_get_proj_matrix` | HIGH | returns view+0x12e |

## input

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x592380` | `Input_init_dinput` | CONFIRMED | DirectInput8Create, mouse+keyboard devices |

## audio

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x4907e0` | `Sound3D_create_sources` | HIGH | alGenSources (sound3d.cpp) |
| `0x57a440` | `Audio_init` | HIGH | ilib audio.cpp object init |
| `0x57a740` | `Audio_init_dsound` | CONFIRMED | DSOUND ordinal 1 (DirectSoundCreate) |
| `0x582d20` | `DirectMusic_init` | CONFIRMED | CoCreateInstance CLSID_DirectMusicLoader/Performance, IID_*8 |
| `0x5a0330` | `Sound3D_init` | HIGH | sound3d.cpp; OpenAL device, alGetError |
| `0x7a9420` | `OpenAL_open_ds3d` | CONFIRMED | alcOpenDevice("DirectSound3D"), alcCreateContext, MakeCurrent |

## video

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x684650` | `Video_open_wmf` | HIGH | LoadLibrary wmvcore.dll WMCreateSyncReader (video.cpp) |
| `0x686350` | `VideoCap_open` | HYPOTHESIS | videocap.cpp CreateFile/ReadFile/LoadLibrary |

## ilib

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x4179c0` | `File_ctor` | HIGH | paired with File_dtor |
| `0x417ad0` | `DynArray_size` | HIGH | returns this+0xc; 322 callers, used as loop bound |
| `0x41b3f0` | `DynArrayB_get_ptr` | HIGH | 1-based recno, this+0x19 body, this+0x10 ele_size |
| `0x586bc0` | `Error_run` | CONFIRMED | vsprintf + MessageBox "Wars & Warriors: Joan of Arc" |
| `0x586da0` | `File_open` | CONFIRMED | CreateFileA; "Error opening file %s." |
| `0x586f00` | `File_create` | HIGH | CreateFileA write; Config/GameFile save |
| `0x587260` | `File_close` | HIGH | CloseHandle path |
| `0x5872b0` | `File_dtor` | HIGH | SEH unwinds |
| `0x5872e0` | `File_write` | HIGH | WriteFile |
| `0x5873a0` | `File_read` | HIGH | ReadFile |
| `0x587c20` | `File_get_short` | HYPOTHESIS | ResourceIdx count read |
| `0x587f30` | `File_size` | HIGH | compared to 0xb05 in Config_load |
| `0x589550` | `FileTxt_open` | HIGH | text resource reader (filetxt.cpp) |
| `0x589890` | `FileTxt_close` | HIGH | paired with FileTxt_open |
| `0x589f10` | `FileTxt_read_field` | HIGH | skips ' ,:\t', reads to CR |
| `0x58e7b0` | `Mem_add` | CONFIRMED | (size,__FILE__,__LINE__) allocator |
| `0x58e8d0` | `Mem_add_zero` | HIGH | new(size+0x11)+memset |
| `0x58ed70` | `Mem_del` | CONFIRMED | (ptr,__FILE__,__LINE__) |
| `0x590b80` | `Misc_is_file_exist` | CONFIRMED | FindFirstFileA; used for *.SYS flag files |
| `0x591050` | `Misc_get_time` | CONFIRMED | returns timeGetTime() |

## resource

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x577d10` | `AsyncFile_read` | HYPOTHESIS | CreateFile/SetFilePointer/ReadFile streaming |
| `0x578d70` | `AsyncLoader_init` | HYPOTHESIS | CreateEventA, ReadFileEx |
| `0x58ae90` | `GameSet_open` | HIGH | RESOURCE\*.SET enumeration |
| `0x59c550` | `ResourceIdx_init` | HIGH | resx.cpp: File_open, count via File_get_short |
| `0x5af5d0` | `Actor_load_model` | HIGH | "Error loading 3D Model : %1s" |
| `0x5b23d0` | `AnimDef_load` | HIGH | "Error loading Animation : %1s" |
| `0x5c6820` | `Morph_load` | HIGH | "Error loading Morph Data : %1s" |

## script

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x645de0` | `Script_open_compile` | HIGH | "Cannot open script" (uti3d esstate2.cpp) |

## save

| addr | proposed name | confidence | evidence |
|---|---|---|---|
| `0x42e1a0` | `Game_quick_save` | HYPOTHESIS | "-QuickSave", "Mission", "SAVE\" |
| `0x4735c0` | `GameFile_read_world` | HIGH | gamefil2.cpp bulk object read |
| `0x4770a0` | `GameFile_enum_saves` | HIGH | "*.SAV" directory scan |
| `0x477770` | `GameFile_make_name` | HIGH | "SAVE\%s%%04d.%s", "_AUTO%03d.SAV" |
| `0x477d80` | `GameFile_load_from_open` | HYPOTHESIS | "Error loading file." variant |
| `0x477e80` | `GameFile_load` | HIGH | "Error opening file."/"Error loading file." |
| `0x478180` | `GameFile_save` | HIGH | "Error creating file."/"Error saving file." |
| `0x478520` | `GameFile_read_header` | HIGH | "Invalid file format." |
| `0x4c8220` | `Profile_save` | HYPOTHESIS | profile.cpp writes SAVE\ (Player.dat) |

## Source-file address ranges

MSVC `__FILE__` strings (`d:\ww\…`) are passed to the ilib allocator `Mem_add`/`Mem_del` (`0x58e7b0`/`0x58ed70`) and to error paths. Each range below is first..last function that pushes that file's path (found by scanning `.text` for the string VA as an imm32 and mapping to the containing function). Object files are linked contiguously, so functions between two attributed ranges of the same file almost certainly belong to it (HIGH CONFIDENCE); gaps between different files are unattributed. Isolated single hits inside another file's range are inlined helpers.

| first fn | last fn | source file |
|---|---|---|
| `0x4149c0` | `0x414a30` | `animgrp.cpp` |
| `0x417060` | `0x4172a0` | `animres.cpp` |
| `0x418e00` | `0x418f30` | `animscr.cpp` |
| `0x41bf40` | `0x41ea60` | `battle.cpp` |
| `0x423600` | `0x4258f0` | `camman.cpp` |
| `0x428d50` | `0x429980` | `camres.cpp` |
| `0x42b270` | `0x42b270` | `world_s.cpp` |
| `0x42ce10` | `0x42ce10` | `world.cpp` |
| `0x43d340` | `0x43d340` | `world_s.cpp` |
| `0x43fc10` | `0x43fc10` | `nation2.cpp` |
| `0x440320` | `0x44a260` | `world.cpp` |
| `0x45c040` | `0x45c5c0` | `cpuusage.cpp` |
| `0x45cc20` | `0x45cc20` | `trackres.cpp` |
| `0x461f10` | `0x461f10` | `unit.cpp` |
| `0x463980` | `0x463aa0` | `ditemres.cpp` |
| `0x4641a0` | `0x466560` | `dyngrass.cpp` |
| `0x467260` | `0x467390` | `end_con.cpp` |
| `0x4699f0` | `0x4699f0` | `f_shop.cpp` |
| `0x46dfa0` | `0x46eed0` | `flocksys.cpp` |
| `0x46f200` | `0x471010` | `game.cpp` |
| `0x471420` | `0x471420` | `ilib\flc.cpp` |
| `0x472620` | `0x472c40` | `game.cpp` |
| `0x4735c0` | `0x476c40` | `gamefil2.cpp` |
| `0x477440` | `0x477520` | `gamefila.cpp` |
| `0x477d80` | `0x478cc0` | `gamefile.cpp` |
| `0x47c070` | `0x47cca0` | `gameplay.cpp` |
| `0x47de30` | `0x4864b0` | `gfile3.cpp` |
| `0x487460` | `0x487460` | `i_wall.cpp` |
| `0x487c40` | `0x487d60` | `grassres.cpp` |
| `0x488400` | `0x488590` | `help.cpp` |
| `0x48c410` | `0x48c410` | `i_gate.cpp` |
| `0x48d120` | `0x48d120` | `ilib\sound3d.cpp` |
| `0x48d4b0` | `0x48d4b0` | `i_gate.cpp` |
| `0x4907e0` | `0x490bc0` | `ilib\sound3d.cpp` |
| `0x490d30` | `0x490d30` | `ilib\se.cpp` |
| `0x4915f0` | `0x4941b0` | `ilib\mousecr.cpp` |
| `0x499380` | `0x499380` | `nation.cpp` |
| `0x49f620` | `0x4a37e0` | `itemres.cpp` |
| `0x4a4c30` | `0x4a4ca0` | `levelres.cpp` |
| `0x4a5970` | `0x4a7450` | `lightsys.cpp` |
| `0x4a95a0` | `0x4a95a0` | `unitlvl.cpp` |
| `0x4a9ac0` | `0x4a9ac0` | `trackres.cpp` |
| `0x4b0a10` | `0x4b18d0` | `modelres.cpp` |
| `0x4b2940` | `0x4b2f10` | `msglog.cpp` |
| `0x4b32f0` | `0x4b3360` | `musicres.cpp` |
| `0x4b8f90` | `0x4b9000` | `natres.cpp` |
| `0x4c19e0` | `0x4c1d80` | `pemires.cpp` |
| `0x4c56f0` | `0x4c5960` | `plasma.cpp` |
| `0x4c7e70` | `0x4c8bb0` | `profile.cpp` |
| `0x4ca350` | `0x4ca480` | `raceres.cpp` |
| `0x4ca640` | `0x4ca760` | `recrures.cpp` |
| `0x4cae70` | `0x4cb310` | `remapkey.cpp` |
| `0x4cc850` | `0x4cdf00` | `scenario.cpp` |
| `0x4ce070` | `0x4ce0f0` | `sdateres.cpp` |
| `0x4ce300` | `0x4ce420` | `sitemres.cpp` |
| `0x4d1770` | `0x4d27e0` | `speech_s.cpp` |
| `0x4dea50` | `0x4deb70` | `t_scrres.cpp` |
| `0x4eba00` | `0x4ec3d0` | `trackres.cpp` |
| `0x4f2f80` | `0x4f6eb0` | `world_s.cpp` |
| `0x4f8550` | `0x4f8550` | `unit.cpp` |
| `0x524d00` | `0x5273b0` | `unitres.cpp` |
| `0x530d00` | `0x530d70` | `unitures.cpp` |
| `0x531690` | `0x5319a0` | `upgrares.cpp` |
| `0x546140` | `0x5462b0` | `uscrres.cpp` |
| `0x547bf0` | `0x547bf0` | `world.cpp` |
| `0x54e930` | `0x560c70` | `world_s.cpp` |
| `0x57a440` | `0x57bca0` | `ilib\audio.cpp` |
| `0x57f080` | `0x57f080` | `ilib\bitmap.cpp` |
| `0x57f230` | `0x57f230` | `ilib\bitmapd.cpp` |
| `0x57f360` | `0x57f360` | `ilib\bitmapm.cpp` |
| `0x57f520` | `0x57f520` | `ilib\bitmapt.cpp` |
| `0x57f6c0` | `0x57f6c0` | `ilib\bitmapw.cpp` |
| `0x57f770` | `0x57f900` | `ilib\blob.cpp` |
| `0x580880` | `0x581df0` | `ilib\db.cpp` |
| `0x5843d0` | `0x585180` | `ilib\dynarr.cpp` |
| `0x585710` | `0x585bb0` | `ilib\dynarrb.cpp` |
| `0x588000` | `0x5886d0` | `ilib\filemem.cpp` |
| `0x589550` | `0x589890` | `ilib\filetxt.cpp` |
| `0x58a320` | `0x58a5e0` | `ilib\flc.cpp` |
| `0x58ae30` | `0x58ae90` | `ilib\gameset.cpp` |
| `0x59b440` | `0x59b830` | `ilib\resdb.cpp` |
| `0x59b920` | `0x59c440` | `ilib\reslx.cpp` |
| `0x59c550` | `0x59d050` | `ilib\resx.cpp` |
| `0x59e0c0` | `0x59e0c0` | `ilib\se.cpp` |
| `0x59f4b0` | `0x5a0cf0` | `ilib\sound3d.cpp` |
| `0x5a2070` | `0x5a28e0` | `ilib\txtres.cpp` |
| `0x5a8690` | `0x5a8ae0` | `ilib\vqueue.cpp` |
| `0x5a8d80` | `0x5a8fa0` | `elib\jpeg.cpp` |
| `0x5aaf10` | `0x5ae0c0` | `en3d\actor.cpp` |
| `0x5b03d0` | `0x5b0850` | `en3d\actordef.cpp` |
| `0x5b0ab0` | `0x5b0ab0` | `en3d\actordeg.cpp` |
| `0x5b4170` | `0x5b9710` | `en3d\animdef.cpp` |
| `0x5b9830` | `0x5ba760` | `en3d\bonetree.cpp` |
| `0x5c6820` | `0x5ca5d0` | `en3d\morph.cpp` |
| `0x5cbbf0` | `0x5d9be0` | `en3d\plane.cpp` |
| `0x5e3040` | `0x5e3040` | `en3d\plane_sh.cpp` |
| `0x5e5960` | `0x5e5f10` | `en3d\pshader.cpp` |
| `0x5e61c0` | `0x5e8f10` | `en3d\reorder.cpp` |
| `0x5e9b10` | `0x5eeca0` | `en3d\scene.cpp` |
| `0x5ef900` | `0x5f39a0` | `en3d\scene_s.cpp` |
| `0x5f46a0` | `0x5f47c0` | `en3d\skin.cpp` |
| `0x6035c0` | `0x603670` | `en3d\sysen.cpp` |
| `0x604150` | `0x604960` | `en3d\texdir.cpp` |
| `0x609260` | `0x60b2e0` | `en3d\texturea.cpp` |
| `0x6123f0` | `0x612dd0` | `en3d\vgabuf.cpp` |
| `0x613210` | `0x613270` | `en3d\vgalock.cpp` |
| `0x620ab0` | `0x620bc0` | `en2d\bmpres.cpp` |
| `0x623e90` | `0x624dc0` | `en2d\font2d.cpp` |
| `0x629830` | `0x6298c0` | `en2d\image2d.cpp` |
| `0x62a670` | `0x62a8e0` | `en2d\popupwin.cpp` |
| `0x62dd10` | `0x630f50` | `en2d\txtbox2d.cpp` |
| `0x634b90` | `0x634dd0` | `en2d\widget2d.cpp` |
| `0x6375b0` | `0x637cb0` | `uti3d\blkarrb.cpp` |
| `0x637ec0` | `0x6388b0` | `uti3d\dlgres.cpp` |
| `0x645de0` | `0x645de0` | `uti3d\esstate2.cpp` |
| `0x648ee0` | `0x648fd0` | `stackt.h` |
| `0x6626a0` | `0x662710` | `uti3d\lflare.cpp` |
| `0x670980` | `0x6745e0` | `uti3d\pathsp.cpp` |
| `0x681ee0` | `0x683bb0` | `uti3d\spindex.cpp` |
| `0x684650` | `0x684650` | `uti3d\video.cpp` |
| `0x686350` | `0x686350` | `uti3d\videocap.cpp` |
| `0x6b3130` | `0x6b3b90` | `interfac\combobox.cpp` |
| `0x6bc530` | `0x6bcf00` | `interfac\i_barr.cpp` |
| `0x6c27c0` | `0x6c8d00` | `interfac\i_buy.cpp` |
| `0x6e0520` | `0x6e0b70` | `interfac\i_letter.cpp` |
| `0x6e5700` | `0x6e5d00` | `interfac\i_sms.cpp` |
| `0x6e8f40` | `0x6ec480` | `interfac\i_work.cpp` |
| `0x6f8470` | `0x6f8670` | `interfac\list.cpp` |
| `0x71b080` | `0x71bf10` | `interfac\minires.cpp` |
| `0x71d2d0` | `0x71e800` | `interfac\objmenu.cpp` |
| `0x7246f0` | `0x7246f0` | `interfac\objmenub.cpp` |
| `0x73a320` | `0x73a320` | `interfac\objmenuw.cpp` |
| `0x7435e0` | `0x7478a0` | `interfac\optmenui.cpp` |

Region summary (HIGH CONFIDENCE, from the ranges above and library strings):

| VA range | contents |
|---|---|
| `0x401005-0x40ac8c` | ILT jump thunks (incremental link) |
| `0x40b000-0x579fff` | game code `d:\ww\*.cpp` (battle, camman, camres, world, world_s, game, gamefile*, unit*, …); a few ilib objects are linked inside it (`ilib\mousecr.cpp` `0x4915f0-0x4941b0`, parts of `ilib\sound3d.cpp` `0x48d120`, `0x4907e0`) |
| `0x57a000-0x5a8fff` | mostly Enlight `ilib` (audio, bitmap, db, dynarr, file, filemem, filetxt, flc, gameset, mem, resdb/reslx/resx, se, sound3d, txtres, vqueue) |
| `0x5a8d80-0x5a8fa0` | `elib\jpeg.cpp` wrapper |
| `0x5aaf10-0x61ffff` | Enlight `en3d` 3D engine (actor, animdef, bonetree, morph, plane, pshader, scene, skin, sysen, texdir, texturea, vgabuf, vgalock) + embedded DirectX 8 SDK sample framework (`0x5ba000-0x5bffff`: CD3DFont, CD3DCamera, DXUtil) |
| `0x620ab0-0x636fff` | `en2d` 2D/UI (bmpres, font2d, image2d, popupwin, txtbox2d, widget2d) |
| `0x6375b0-0x6b2fff` | `uti3d` (blkarrb, dlgres, esstate2 script compiler, lflare, pathsp, spindex, video, videocap) |
| `0x6b3130-0x75ffff` | `interfac` menus/HUD (combobox, i_barr, i_buy, i_letter, i_sms, i_work, list, minires, objmenu*, optmenui) |
| `~0x760000-0x7b0000` | statically linked third-party code (HYPOTHESIS for exact bounds): D3DX8 (observed functions `0x7648ba`, `0x766b4b`, `0x7674a4`, `0x767723`, `0x768495`, `0x769dc0`; strings for shader assembler v0.91 and .x loader), IJG libjpeg, libpng 1.0.5, zlib, ALUT-style OpenAL init (`0x7a9420`) |
| `0x7b0000-0x7cffff` | MSVC 6 CRT (entry `0x7b71e6`, math `atan` `0x7b2184`, …) |
