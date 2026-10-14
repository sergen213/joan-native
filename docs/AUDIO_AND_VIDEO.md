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
wmv2 wmv3 vc1 pcm_s16le`, demuxer `asf`, protocol `file`, libraries avcodec/avformat/avutil/swscale/
swresample — LGPL-2.1-or-later. The recipe refuses GPL/nonfree configurations, checks the exact
installed file set and sonames, and writes license/notice files (digests pinned in the lock) plus the
source proof under `share/licenses/FFmpeg`. `packaging/release/build_release.py` ships the libraries
as separate `.so` files (users can replace them) and puts the byte-exact source archive, its `.asc`,
the key, the recipe and the lock into the release's `*-third-party-source.tar`.
The prefix is relocatable (`*.pc` use `${pcfiledir}`) and path-free: configure sees a placeholder
prefix, bare tool names and relative sources (the build runs in `$TMPDIR`, which must not contain
whitespace), so two builds anywhere are byte-identical (verified 2026-10-14). Local release build:
`python3 packaging/ffmpeg/build_ffmpeg.py --prefix work/tools/ffmpeg-prefix --tools-dir work/tools`
(nasm required), then `PKG_CONFIG_PATH=$PWD/work/tools/ffmpeg-prefix/lib/pkgconfig cmake …`.
Verified 2026-10-14 (x86asm on): `ldd joan-native` resolves all five libraries to the prefix and
`engine.audio_decode` + `engine.video_decode` pass against it.

## 8. Gaps

- In-game gaps (§9): footstep surface is always `GROUND`; rain environment variant; source stealing for ambient emitters (the native mixer drops a sound when all 12 Sfx voices are busy); `PICKUP` (no world pickup event yet), gate/wall/box hit sounds (`FUN_004950a0` with ItemRes sound ids: the map-object layer emits no events for them), the RTS death voice in `Unit_die`; the church mood (`8`) needs `Territory::scale == 4`, untested on data.
- `A_WAVE1/2.RES` (2D `ilib\se.cpp` set) users are not traced; CONFIG 0x3E is used for the Voice bus only as a HYPOTHESIS; movie audio uses master volume only (HYPOTHESIS).
- After `--movie-time` seeks, audio restarts at the first packet after the shown frame (≈ 1 packet of drift). Movie frames are uploaded with destroy + create per frame (30 Hz); add `IRenderer::update_texture` if it ever shows in profiles.
- `WAVE.RES` (80 MB) is read into memory on first `play_sound`.
- The minimal LGPL build has no PNG decoder, so `video_decode_test` skips its reference-capture comparison there (it runs with the system FFmpeg).

## 9. In-game audio (`engine/game_audio.{hpp,cpp}`)

`GameAudio` turns one session tick into audio. The app calls `on_tick(mission, world events, mission
events, current snapshot)` after each 33 ms step, `frame(camera pose, dt)` every rendered frame
(listener at `pose.eye`, forward `target − eye`, up +Y; then `AudioSystem::update()`), `menu()` on menu
screens (MUSIC type `M` until the next `on_tick`; the director keeps running from `frame()`), and
`stop()` when the mission is left, retried or replaced by a load. `voice_active()` feeds
`GameUiBridge::voice_active`. It only reads the mission. Positions: `to_engine(UnitView::pos)`.

World events (`GameEvent`) → 3D sounds at the unit / projectile:

| Event | Sound | Original | Confidence |
|---|---|---|---|
| `Ev::Sound`, unit cue (move-record `SOUND`/`SOUND2`) | the code; `MAIN_CHAR` codes only for the player's unit; variant `recno % WF_RANDMAX` | `FUN_0050db80` | CONFIRMED |
| `Ev::Sound`, code `TS_*` | `<code>_<surface>` footstep (surface = `GROUND` here); not in cut scenes; `SOUND_TIME == -1` steps only for the player outside RTS; one step per unit per composite `SOUNDTIME` | `AnimGrp_load` (`TS_` → id −99), `FUN_0050db80` | HIGH (surface: gap) |
| `Ev::Sound` with a projectile (`other`) | at its last position; `EXPLOSIVEHIT` → `EXPLOSIVEHIT1..3` | `ThrowItem_on_hit_actor`, `ThrowItem_update` | CONFIRMED |
| projectile types 0–2 in the snapshot | looping `ARROW` that follows the projectile, stopped when it disappears | `ThrowItem_launch`, `ThrowItem_update` (+0x10) | CONFIRMED |
| `Ev::Blocked` | `GUARD` (melee) / `GUARDARR` (ranged) at the defender | `Melee_tryHit`, `Ranged_tryHit` | CONFIRMED |
| `Ev::LevelUp` (player) / `Ev::Effect "EXPUP"` | `LEVELUP` / `EXPUP` | `Unit_check_level_up`, `Unit_add_exp` | CONFIRMED |

Map `.stg` `PLAY_SOUND` / `PLAY_AMB_SOUND` triggers (`MapObjects::triggers()`, e.g. `CAMPFIRE`,
`BLACKSMITH`, `WATER_F`): the emitter plays at its position while the player unit is inside its radius
and stops when the player leaves (`SE_play_trigger 0x4c0680`). Hits, deaths and knockdowns have no
sound call of their own: their reaction animations carry the `SOUND` cue.

Mission events: `Sound` with `b == 0` (`OBJECTIVE_ADD/UPDATE/FINISH`, `ATTENTION_CALL`) plays at the
listener. Dialog voices: a dialog box with a voice (`add_dialog` text tag or `set_dialog_voice`) starts
`play_voice(<script stem>, "%04d")` once, when the letterbox is fully shown (`cs.status == 2`, fade 255:
`FUN_0063b5e0`, the same gate as the typing); `Dialog` (id reuse after `del_all_dialog`) and
`DialogClosed` stop it. Attention voices start when the message enters state 4 (`FUN_004ace90`) and stop
when it leaves. CONFIRMED.

Music (`MusicDirector`, §4) gets the mood each tick (`GameAudio::mood`, `Music_process` 0x4e6fe0):
battle when the scenario mode is BATTLE (step +0x1ae == 3), when a hostile unit is within
`BATTLE_R = 1500` of the player (or of the player's leader): `Unit_search_for_unit("E", 1500, 1)`, the
type string read from `0x9cfc2c` (CONFIRMED), or when the player stands in a territory whose owner is
hostile (`FUN_00509590`) or that is under attack; church (8) inside a territory of scale 4; game over
/ won from `game_finish` 2 / 1; endings 9 / 10 (HYPOTHESIS: `show_ending` id 0 is the good one). The
native search is read-only (no sticky target, no sim RNG) and has no line-of-sight test (deviation).

Environment (3 streams): slot 0 loops the `E` track for `MissionState::minute` (`by_time`); slot 1 plays
a side sound every 30–60 s in a city (`S` / E_TYPE I at war, C at peace) or during battle elsewhere
(E_TYPE W); slot 2 plays bells (B) in cities every 30–60 s, elsewhere — only while
`MissionUi::music_side_env` (`set_music_side_environment_enable`) — an `S` track of the current time
track's E_TYPE every 60–120 s. A changed track fades out over 5 s first; everything stops at game over
or an ending. Side-sound timing and variants use GameAudio's own RNG, never the simulation's.

Verified by `engine.game_audio` (mission 1, offline mixer): Joan's scripted attacks produce 3D voices;
an injected dialog with `S1L1_tactic:0014` makes `voice_active()` true for 4.4 s, then false; `menu()`
selects `MAIN_BACK`; a spawned English archer beside Joan switches normal → battle music after the
8 s fade, the battle track holds through 7.6 s of calm and fades back to normal after > 8 s.
