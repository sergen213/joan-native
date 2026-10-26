#pragma once
// Game-facing audio: SOUND table + WAVE.RES sound bank, MUSIC table, voice packs, SDL3 output
// device, and the original music selection rules (FUN_004e6fe0). docs/AUDIO_AND_VIDEO.md.
#include "engine/audio_mixer.hpp"
#include "formats/res_archive.hpp"
#include "formats/vfs_fs.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>

struct SDL_AudioStream;
namespace joa::formats { struct GameConfig; }

namespace joa::engine {

// STD.SET table SOUND (505 records), parsed like FUN_004d0de0 and sorted by SOUND_CODE (the record
// number = index + 1 is what animations, scripts and the UI use).
struct SoundDef {
    std::string code, wave;      // SOUND_CODE, WAVE_FILE (entry name in Resource/WAVE.RES)
    int randmax = 1;             // WF_RANDMAX: variant = record + seed % randmax (neighbouring records)
    float gain = 1, min_pitch = 1, max_pitch = 1;
    Attenuation attenuation;     // MIN_DIST, MAX_DIST, ROLLOFF
    int sound_time = 0;          // SOUNDTIME ms
    bool loop = false;           // LOOP_FLAG
    bool speed_index = false;    // SPEED_INDE (unused by shipped data)
    int delay_beg = 0, delay_end = 0;
    bool main_char = false;      // MAIN_CHAR: animation sound only for the player's own unit
};

class SoundBank {
public:
    bool load(const formats::Vfs& vfs, std::string* err = nullptr);
    const std::vector<SoundDef>& defs() const { return defs_; }
    int find(std::string_view code) const;  // case-insensitive; index or -1
    int variant(int index, int seed) const;
    // Decoded WAVE.RES entry for defs()[index] (cached); nullptr if missing.
    std::shared_ptr<const Pcm> pcm(int index) const;
    const formats::ResArchive* waves() const;  // Resource/WAVE.RES, opened on first use

private:
    std::vector<SoundDef> defs_;
    std::filesystem::path waves_path_;
    mutable std::optional<formats::ResArchive> waves_;  // mmapped (ResArchive::open): pages load on first use
    mutable std::vector<std::shared_ptr<const Pcm>> cache_;
};

// STD.SET table MUSIC (42 records). Types (FUN_004b3360): N=1 normal, B=2 battle, M=3 main menu,
// E=4 environment, S=5 side environment, G=6 game over, W=7 scenario won, C=8 church, '1'=9 good
// ending, '0'=10 bad ending. E_TYPE: M=1 morning, A=2 afternoon, N=3 night, R=4 rain, S=5,
// C=6 city peace, I=7 city war, B=8 bells, W=9 war.
struct MusicTrack {
    std::string code, file;
    int type = 0, env_type = 0;
    float start_min = 0, end_min = 0;  // minutes of the day for E/S tracks (may wrap past midnight)
    int start_sce = 0, end_sce = 0;
};

class MusicTable {
public:
    bool load(const formats::Vfs& vfs, std::string* err = nullptr);
    const std::vector<MusicTrack>& tracks() const { return tracks_; }
    const MusicTrack* find(std::string_view code_or_file) const;
    // FUN_004b3940: uniform random among tracks of `type` (env_type must match if != 0) whose
    // STARTSCE..ENDSCE contains `scenario` (0 = any). Type 5 ignores the scenario.
    const MusicTrack* pick(int type, int env_type, int scenario, std::minstd_rand& rng) const;
    bool any(int type, int env_type, int scenario) const;  // pick() would return a track (no draw)
    // FUN_004b3c70: first track of `type` whose [start_min, end_min) contains `minute` (wrapping).
    const MusicTrack* by_time(int type, float minute) const;

private:
    std::vector<MusicTrack> tracks_;
};

struct AudioSettings {
    float master = 1.0f;
    float music = 0.77f;  // CONFIG.DAT 0x3A
    float env = 0.39f;    // 0x3C "Environmental"
    float sfx = 1.0f;     // 0x41 "Sound Volume" (12 3D sources, UI sounds)
    float voice = 1.0f;   // 0x3E, HYPOTHESIS
    bool three_d = true;  // 0x43 "3D Sound On"
    bool enabled = true;  // 0x40 sound on
    static AudioSettings from_config(const formats::GameConfig& c);
};

class AudioSystem {
public:
    static constexpr int kEnvSlots = 3;  // original: 3 environment streams (FUN_00586210)

    // open_device = false: no SDL output (tests, offline renders via mixer().mix()).
    AudioSystem(formats::Vfs vfs, const AudioSettings& s = {}, bool open_device = true);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool device_open() const { return device_ != nullptr; }
    Mixer& mixer() { return mixer_; }
    const SoundBank& sounds() const { return bank_; }
    const MusicTable& music_table() const { return music_; }
    void apply(const AudioSettings& s);
    void set_listener(const Listener& l) { mixer_.set_listener(l); }

    // SOUND-table entry by code. pos = world position (3D), nullopt = at the listener (UI/2D).
    // variant_seed: unit record number for WF_RANDMAX variants. steal: SE_play_trigger source stealing
    // (PlayParams::steal). 0 = unknown code / all 12 sources busy without steal.
    VoiceId play_sound(std::string_view code, std::optional<glm::vec3> pos = {}, int variant_seed = 0, bool steal = false);
    // Dialog voice: entry `id` ("0014" from a "[0014N]" text tag) of Resource/<pack>.RES (e.g. S1L1_tactic).
    VoiceId play_voice(std::string_view pack, std::string_view id);

    // Music channel: MUSIC CODE or file name in music/. Stops the current track first (FUN_0059ae60).
    bool play_music(std::string_view code_or_file, bool loop = true);
    void stop_music();
    void fade_out_music(float seconds = 8.0f);  // FUN_0059b1c0: linear 8000 ms fade
    bool music_playing() const;                  // false once a fade has finished
    bool music_fading() const { return music_fading_; }
    const MusicTrack* current_music() const { return channels_[0].track; }

    bool play_env(int slot, std::string_view code_or_file, bool loop);
    void fade_out_env(int slot, float seconds = 5.0f);
    void stop_env(int slot);
    bool env_playing(int slot) const;  // false once its track ended or a fade finished
    const MusicTrack* current_env(int slot) const { return channels_[1 + slot].track; }

    void update();  // refills music/env streams; call once per frame

private:
    struct Channel {
        std::unique_ptr<AudioFileStream> file;
        std::shared_ptr<StreamQueue> queue;
        VoiceId voice = 0;
        bool loop = false;
        const MusicTrack* track = nullptr;
    };
    bool start(Channel& c, std::string_view code_or_file, Bus bus, bool loop);
    void refill(Channel& c);
    static void callback(void* self, SDL_AudioStream* s, int additional, int total);

    formats::Vfs vfs_;
    Mixer mixer_;
    SoundBank bank_;
    MusicTable music_;
    std::map<std::string, std::optional<formats::ResArchive>, std::less<>> voice_packs_;
    Channel channels_[1 + kEnvSlots];  // [0] music, [1..3] environment
    bool music_fading_ = false;
    bool enabled_ = true;
    SDL_AudioStream* device_ = nullptr;
    std::vector<float> device_buf_ = std::vector<float>(2 * 4096);  // callback buffer, preallocated
    std::vector<float> refill_buf_;  // refill() decode chunk, reused
};

// Port of the music half of FUN_004e6fe0 (called every game tick).
enum class MusicMood { Normal = 1, Battle = 2, Menu = 3, GameOver = 6, Won = 7, Church = 8, GoodEnd = 9, BadEnd = 10 };

class MusicDirector {
public:
    static constexpr std::uint32_t kBattleHoldMs = 8000;  // battle -> normal only after 8 s without combat
    void update(AudioSystem& audio, MusicMood mood, int scenario, std::uint32_t now_ms);
    std::minstd_rand rng{1};

private:
    std::uint32_t calm_since_ = 0;  // now+1 when the calm period started, 0 = not calm
};

}  // namespace joa::engine
