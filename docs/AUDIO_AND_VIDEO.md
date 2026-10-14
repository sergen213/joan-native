# Audio and video

How the original plays sound, music and movies, and how the native port reproduces it.
Code: `src/engine/audio_decode.*`, `audio_mixer.*`, `audio_system.*`, `video_player.*`.
Tests: `tests/engine/audio_decode_test.cpp`, `audio_mixer_test.cpp`, `video_decode_test.cpp`.
Addresses are `joan.exe` (GOG); the decompile is `work/ghidra/export/joan_decomp.c`.

## 1. Original data

| Data | Format | Notes |
|---|---|---|
| `Resource/WAVE.RES` | Name34 RES (`docs/formats/CONTAINERS_WORLD_SAVES.md`), 503 entries | RIFF WAVE, PCM only: 16-bit 11025 Hz stereo (234, with `cue `/`LIST` chunks), 16-bit 22050 Hz mono, 16-bit 44100 Hz mono. Two briefings (`M7BRIEF`, `M8BRIEF`) are 120–133 s long. CONFIRMED (`audio_decode_test`) |
| `Resource/A_WAVE1.RES` / `A_WAVE2.RES` | Name34 RES, 11 / 1 entries | PCM WAVE (one 8-bit 11025 Hz). Loaded at start-up by `FUN_0045a350` / `Audio_init`; purpose of the 2D (`ilib\se.cpp`) set UNKNOWN |
| `Resource/S<n>L1_tactic.RES`, `S<n>L1_finish.RES` | Name34 RES, entries named `0001`..`0140` | Dialog voice: 16-bit 11025 Hz stereo PCM. Entry `NNNN` voices the script text tagged `[NNNNN]` in `script/scenario/S<n>L1_<part>.sce` (e.g. `set_dialog_voice(..., "[0014N]...")` ↔ `S1L1_tactic.RES:0014`; only voiced lines have an entry). HIGH |
| `music/*.wma` (40) | ASF, WMA v2, 44100 Hz stereo, 128 kbit/s | Music and environment loops. `JOA_Encounter.wma` ends in a truncated packet: FFmpeg decodes 43.7 of the 44.9 s the header claims (the last 1.2 s are missing from the file). CONFIRMED |
| `movie/enlight.wmv`, `movie/intro.wmv` | ASF, WMV3 Main 512x288 / 800x450 + WMA v2 stereo | Nominal 30 fps, variable: 398 / 3170 coded frames for 13.2 / 108.0 s (the encoder dropped frames). The exe also names `TREVOR.WMV`, `WW_CAP.WMV` (not shipped) |
| `Resource/STD.SET` table `SOUND` | dBase III, 505 records | see §2 |
| `Resource/STD.SET` table `MUSIC` | dBase III, 42 records | see §4 |
| `Resource/GAMEPLAY.RES` `{MUSIC}` | text | `[BATTLE_R]1500.0` battle-music radius (`FUN_00472ee0` → `DAT_00a39180`) |

## 2. Sound effects (SOUND table, `sound_res`)

`FUN_004d0de0` loads table `SOUND` into 0x8F-byte records and **sorts them by `SOUND_CODE`**; the
1-based position in that sorted array is the sound id used everywhere (animation `SOUND`/`SOUND2`
fields resolved through `FUN_004d12f0`, UI, scripts). CONFIRMED.

| Field | Record | Use |
|---|---|---|
| SOUND_CODE C48 | +0x06 | lookup key (binary search `FUN_004d1220`) |
| WAVE_FILE C48 | +0x37 | entry name in `RESOURCE\WAVE.RES` (loaded lazily into an AL buffer, `FUN_004d0a60`; buffers with no users are freed every 400 calls, `FUN_004d1340`) |
| WF_RANDMAX N2 | +0x68 = max(n,1) | variants: the **next n−1 records** in sorted order. Unit animation sounds play id + `unit_recno % n` (`FUN_0050db80`). E.g. `HIT_H` (5) → `HIT_H`..`HIT_H5` |
| GAIN N6 | +0x69 | `AL_GAIN = volume% * GAIN * 0.01` |
| MIN_PITCH / MAX_PITCH | +0x6D / +0x71 | all 1.0 in the shipped data; no pitch call observed |
| MIN_DIST / MAX_DIST / ROLLOFF | +0x75 / +0x79 / +0x7D | `AL_REFERENCE_DISTANCE` / `AL_MAX_DISTANCE` / `AL_ROLLOFF_FACTOR` (`FUN_0059f970/920/9c0`) |
| SOUNDTIME N5 | +0x81 | ms (animation timing) |
| LOOP_FLAG / SPEED_INDE | +0x85 bits 0 / 2 | `AL_LOOPING = flags & 3` (`FUN_0059fa80`) |
| DELAY_BEG / DELAY_END | +0x86 / +0x88 | all 0 |
| MAIN_CHAR | +0x8A | animation sound only when the unit is the player's (`DAT_00a5b9e8`) |

Play paths (all CONFIRMED, identical source setup, `AL_SOURCE_RELATIVE = 0`):
- `FUN_00494810(code, pos)` / `FUN_004950a0(id, pos)`: 3D at a world position.
- `FUN_00492c40(code)`, `FUN_004cf0f0(id)`: at the listener position (`DAT_00a57ef8+5`), i.e. centred.
- `FUN_004ceef0(code)`: UI sounds (33 interface callers), at the listener, gain uses CONFIG 0x41.
- `FUN_004c0680(emitter)`: map ambient emitters; culled beyond the emitter radius, steals the farthest source (`FUN_00493340`) when all are busy.
- 12 OpenAL sources (`FUN_0045f450` scans 1..12); a sound is dropped when none is free.

Distance model (`Sound3D_init` 0x5a0330 → `FUN_00454000`): "3D Sound On" → `alDistanceModel(0xD002)`
= **AL_INVERSE_DISTANCE_CLAMPED**, off → `AL_NONE` (no attenuation). Doppler factor 0, Doppler
velocity 343. Device `alcOpenDevice("DirectSound3D")`. CONFIRMED. So:

    d' = clamp(d, MIN_DIST, MAX_DIST);  gain = MIN_DIST / (MIN_DIST + ROLLOFF * (d' - MIN_DIST))

Shipped values: ROLLOFF 1.0 everywhere; MIN/MAX 100/1500 (attacks, voices), 70–100/5000 (hits),
10000/15000 for 346 records (effectively un-attenuated within a battlefield).
OpenAL never spatialises multi-channel buffers, so the 234 stereo WAVE entries always play
centred and un-attenuated. Listener position/orientation are set from the camera (`FUN_0059ff70`,
`FUN_005a0050`).

## 3. Volumes (CONFIG.DAT, audio options page)

The audio page handler (`FUN_007413e0`, labels Music / Environmental / Sound Volume / 3D Sound On) writes:

| Offset | Default | Meaning | Applied by |
|---|---|---|---|
| 0x3A | 77 | Music volume % | `FUN_0059b150` → music stream gain |
| 0x3C | 39 | Environmental volume % | `FUN_00586620` → the 3 environment streams |
| 0x41 | 100 | Sound volume % (3D effects + UI) | `FUN_0045f6b0` → 12 SFX sources; `FUN_004ceef0` |
| 0x43 | 1 | 3D sound on | `FUN_00454000(on ? 2 : 0)` |
| 0x40 | 1 | sound enabled | gates `ilib\se.cpp` playback (`FUN_0059e400`, `FUN_0059ebd0`) |
| 0x3E | 100 | UNKNOWN volume; used for the native Voice bus (HYPOTHESIS) | — |

(The "Default" button sets 77 / 39 / 100 / on.)

## 4. Music and environment (`music_res`, `FUN_004e6fe0`)

`MUSIC` table → `FUN_004b3360`: TYPE `N`=1 normal, `B`=2 battle, `M`=3 main menu, `E`=4 environment,
`S`=5 side environment, `G`=6 game over, `W`=7 scenario won, `C`=8 church, `1`=9 good ending,
`0`=10 bad ending; E_TYPE `M A N R S C I B W` = 1..9 (morning, afternoon, night, rain, –, city peace,
city war, bells, war). `FUN_004b3940(type, etype, scenario)` picks uniformly at random among
records of that type whose STARTSCE..ENDSCE contains the scenario; `FUN_004b3c70` picks
environment tracks by minute of day (`STARTMIN..ENDMIN`, wrapping past midnight). CONFIRMED.

Mood each tick (`FUN_004e6fe0`, CONFIRMED): menu screens → 3; in game 1 normal, 8 inside a church
building, **2 when an enemy is within BATTLE_R = 1500 of the player** (or the player's troop is
engaged), 6 game over, 7 won, 9/10 endings. Switching rules (music channel: one WMF stream, played
at the listener as an OpenAL source):
- same mood → keep the current track; a finished track restarts from 0 (`FUN_0059b1c0`): loop.
- nothing playing, current track is the menu theme, or mood is won / game over → stop and start the new track at once (no crossfade).
- battle → normal only after **8000 ms** of continuous normal mood; any other change immediately.
- a change = fade-out: gain falls linearly over **8000 ms** (`FUN_0059b330(2)`, `FUN_0059b1c0`),
  then the next tick starts the new mood's track at full volume. A fade in progress always completes.
- Environment: 3 more WMF streams (`FUN_00586210`), slot 0 = time-of-day loop (`E`, rain variant),
  slots 1–2 = side sounds (`S`) every 30–60 s; their fade-out is 5000 ms (`FUN_005868f0`).
- Observed (`ORIGINAL_BEHAVIOR.md`): main menu plays `JOA_In_your_name.wma` (`MAIN_BACK`).

## 5. Movies (`uti3d\video.cpp`)

`Video_open_wmf` (WMF 9 synchronous reader) decodes `MOVIE\ENLIGHT.WMV` then `MOVIE\INTRO.WMV` at
start-up; Escape skips the current movie (one press per movie). At 1024x768 the 16:9 intro is
letterboxed to 1024x576 at y = 96 (`work/reference/shots/s01_intro_movie_*.png`). The captures are
named by launcher wall-clock: "20s" shows intro t = 2.666 s and "42s" shows t = 22.733 s (best match
over every frame; mean abs difference 3.4 / 2.2 per 255 in `video_decode_test`).

## 6. Native implementation

- **Decoding**: `decode_wav` (PCM 8/16-bit, any rate, skips unknown chunks); `AudioFileStream` /
  `FfAudioDecoder` (FFmpeg libavformat + libavcodec `wmav1/wmav2`, libswresample → float stereo 44.1 kHz).
- **Mixer** (`Mixer`): software, float stereo 44.1 kHz; linear-interpolation resampling; buses
  Music / Env / Sfx / Voice / Movie × master; `distance_gain` = the clamped inverse model above
  (3D off = no attenuation); constant-power pan from the listener's right vector (centre −3 dB);
  stereo buffers and streams bypass spatialisation; 12-voice limit on the Sfx bus; linear fades.
  HRTF: none (the original had none either).
- **Output**: SDL3 `SDL_OpenAudioDeviceStream` (F32 stereo 44.1 kHz) with a pull callback; no new
  dependency. Missing device → silent, game continues.
- **Game API** (`AudioSystem`, `audio_system.hpp`):
  - `play_sound(code, std::optional<vec3> pos, variant_seed)` — SOUND table entry; `nullopt` = at the listener (UI/2D).
  - `play_voice(pack, id)` — e.g. `("S1L1_tactic", "0014")`.
  - `play_music(code_or_file, loop)`, `fade_out_music(8)`, `stop_music()`, `play_env(slot, …)`, `fade_out_env(slot, 5)`.
  - `set_listener({position, forward, up})` (renderer world space, right-handed), `apply(AudioSettings::from_config(cfg))`, `update()` once per frame (refills streams).
  - `MusicDirector::update(audio, MusicMood, scenario, now_ms)` — the rules of §4 for the music channel.
- **Movies** (`video_player.hpp`): `VideoDecoder` (WMV → sRGB RGBA via libswscale, soundtrack into a
  `StreamQueue`); `play_movie()` draws one letterboxed `UiQuad` per frame through `IRenderer`
  (GL and Vulkan), syncs video to the audio clock (frames consumed by the mixer), skips on
  Esc / left click. CLI: `joan-native --play-movie movie/intro.wmv [--movie-time 2.6667 --screenshot out.png]`.

## 7. FFmpeg licensing (release builds)

Local builds use the system FFmpeg via pkg-config (Arch's is GPL-3.0: never ship it).
Releases link the shared libraries built by `packaging/ffmpeg/build_ffmpeg.py` from the
signature-verified FFmpeg 8.1.3 release (`packaging/ffmpeg/ffmpeg.lock.json`): `--disable-gpl
--disable-nonfree --disable-version3 --disable-everything` plus only the decoders `wmav1 wmav2 wmv1
wmv2 wmv3`, demuxer `asf`, protocol `file`, libraries avcodec/avformat/avutil/swscale/swresample —
LGPL-2.1-or-later. The recipe refuses GPL/nonfree configurations, checks the exact installed file
set and sonames, and writes license/notice files (digests pinned in the lock) plus the source proof
under `share/licenses/FFmpeg`. Ship the libraries as separate `.so` files (users can replace them),
and publish the byte-exact source archive, its `.asc`, the recipe and the lock with each release.
Verified: a configure/make of exactly these arguments (with `--disable-x86asm`, nasm absent here)
produced the five locked sonames/versions, and all three audio/video tests pass against it.
The install prefix must not contain whitespace (FFmpeg's `pkgconfig_generate.sh` writes an empty
`prefix=` otherwise; the recipe rejects such prefixes).

## 8. Gaps

- Game integration is not wired yet: nobody calls `play_sound` from animations (`ANIMATE` `SOUND`/`SOUND_TIME`, `SOUND2`/`SOUNDTIME2`), the projectile `ARROW` loop, scripts, or the UI, and `MusicDirector` is not fed by a game loop (needs the mood inputs above: enemy within BATTLE_R, church, game over/won).
- Environment selection (slot 0 by time of day/rain, side sounds every 30–60 s) is documented, and the API exists (`play_env`, `MusicTable::by_time`), but there is no director for it.
- Map ambient emitters (`FUN_004c0680`, culling radius, source stealing) are not ported; the native mixer drops the sound when all 12 Sfx voices are busy.
- `A_WAVE1/2.RES` (2D `ilib\se.cpp` set) users are not traced; CONFIG 0x3E is used for the Voice bus only as a HYPOTHESIS; movie audio uses master volume only (HYPOTHESIS).
- After `--movie-time` seeks, audio restarts at the first packet after the shown frame (≈ 1 packet of drift). Movie frames are uploaded with destroy + create per frame (30 Hz); add `IRenderer::update_texture` if it ever shows in profiles.
- `WAVE.RES` (80 MB) is read into memory on first `play_sound`.
- The minimal LGPL build has no PNG decoder, so `video_decode_test` skips its reference-capture comparison there (it runs with the system FFmpeg).
