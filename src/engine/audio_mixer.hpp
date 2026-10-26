#pragma once
// Software mixer: interleaved float stereo at kOutputRate. Replaces the original's DirectSound +
// OpenAL "DirectSound3D" pair (docs/AUDIO_AND_VIDEO.md §3D). Thread-safe: game thread calls
// play/stop/set_*, the SDL audio callback (or an offline render) calls mix().
#include "engine/audio_decode.hpp"

#include <glm/glm.hpp>

#include <cfloat>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace joa::engine {

// Volume buses (CONFIG.DAT sliders, percent / 100): Music 0x3A, Env 0x3C, Sfx 0x41; Voice HYPOTHESIS 0x3E.
// Movie: intro/logo movies, master volume only (HYPOTHESIS: the WMF movie path has no slider).
enum class Bus : std::uint8_t { Music, Env, Sfx, Voice, Movie, Count };

struct Listener {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};  // right = cross(forward, up) (right-handed world, renderer.hpp)
};

// SOUND table MIN_DIST / MAX_DIST / ROLLOFF -> AL_REFERENCE_DISTANCE / AL_MAX_DISTANCE / AL_ROLLOFF_FACTOR.
struct Attenuation {
    float ref = 1.0f;
    float max = FLT_MAX;
    float rolloff = 1.0f;
};

// OpenAL AL_INVERSE_DISTANCE_CLAMPED (alDistanceModel(0xD002) in FUN_00454000 when "3D Sound" is on):
// d' = clamp(d, ref, max); gain = ref / (ref + rolloff * (d' - ref)).
float distance_gain(float distance, const Attenuation& a);
// Constant-power stereo pan of a world position relative to the listener: {left, right}.
// Sources at the listener (or straight ahead/behind/above) are centred: {0.7071, 0.7071}.
glm::vec2 pan_gains(const glm::vec3& position, const Listener& listener);

struct PlayParams {
    Bus bus = Bus::Sfx;
    float gain = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool positional = false;  // false = at the listener (UI, music); mono buffers only are spatialised
    glm::vec3 position{0.0f};
    Attenuation attenuation;
    bool steal = false;  // SE_play_trigger 0x4c0680: all Sfx voices busy -> stop the one farthest from the listener (FUN_00493340)
};

// FIFO of interleaved stereo frames at kOutputRate fed by a decoder thread/loop (music, movies).
// frames_played() is the audio clock used for A/V sync. Ring buffer: pop() (the audio callback) never
// allocates or frees; push() grows it, allocating with the lock released.
class StreamQueue {
public:
    void push(const float* stereo, std::size_t frames);
    std::size_t queued() const;       // frames waiting
    std::uint64_t frames_played() const;
    void finish() { std::lock_guard l(m_); finished_ = true; }
    bool drained() const;              // finished and empty
    std::size_t pop(float* stereo, std::size_t frames);  // mixer side; returns frames taken
    void clear();

private:
    // refill keeps ~1 s + one 4096-frame chunk queued; movies decode ~0.3 s ahead (larger bursts grow it).
    static constexpr std::size_t kInitialFrames = std::size_t(kOutputRate) * 2;
    std::size_t capacity() const { return buf_.size() / 2; }  // frames

    mutable std::mutex m_;
    std::vector<float> buf_;           // interleaved stereo ring
    std::size_t head_ = 0, size_ = 0;  // frames
    std::uint64_t played_ = 0;
    bool finished_ = false;
};

using VoiceId = std::uint32_t;  // 0 = none

class Mixer {
public:
    static constexpr int kMaxSfxVoices = 12;  // the original allocates 12 OpenAL sources (FUN_0045f450)

    VoiceId play(std::shared_ptr<const Pcm> pcm, const PlayParams& p);
    VoiceId play_stream(std::shared_ptr<StreamQueue> stream, Bus bus, float gain = 1.0f);
    void stop(VoiceId v);
    void stop_all();
    bool playing(VoiceId v) const;
    void set_gain(VoiceId v, float gain);
    void set_position(VoiceId v, const glm::vec3& position);
    // Linear ramp of the voice gain to 0 over `seconds`, then stop (music 8 s, env 5 s in the original).
    void fade_out(VoiceId v, float seconds);

    void set_listener(const Listener& l);
    void set_bus_volume(Bus b, float volume);  // 0..1
    float bus_volume(Bus b) const;
    void set_master(float volume);
    void set_3d(bool on);  // off = AL_NONE: no distance attenuation, panning kept

    // Writes `frames` interleaved stereo frames (overwrites `out`).
    void mix(float* out, std::size_t frames);
    // Game thread, once per frame: frees the buffers of voices that finished in mix() (the audio
    // callback must not drop the last reference to a Pcm/StreamQueue).
    void update();
    std::size_t active_voices() const;

private:
    struct Voice {
        VoiceId id = 0;
        std::shared_ptr<const Pcm> pcm;
        std::shared_ptr<StreamQueue> stream;
        PlayParams p;
        double cursor = 0;     // source frames
        float fade_step = 0;   // gain lost per output frame while fading
        float fade = 1.0f;
        bool done = false;
    };
    glm::vec2 voice_gains(const Voice& v) const;
    const Voice* find(VoiceId v) const;
    Voice* find(VoiceId v) { return const_cast<Voice*>(std::as_const(*this).find(v)); }

    static constexpr std::size_t kGraveyard = 64;  // preallocated: mix() only moves into reserved slots

    mutable std::mutex m_;
    std::vector<Voice> voices_;
    std::vector<std::shared_ptr<const void>> graveyard_ = reserved(kGraveyard);  // finished voices' buffers
    static std::vector<std::shared_ptr<const void>> reserved(std::size_t n) {
        std::vector<std::shared_ptr<const void>> v;
        v.reserve(n);
        return v;
    }
    std::vector<float> scratch_ = std::vector<float>(2 * 4096);  // stream pop buffer: no allocation in mix() up to 4096 frames
    Listener listener_;
    float bus_[std::size_t(Bus::Count)]{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float master_ = 1.0f;
    bool three_d_ = true;
    VoiceId next_ = 1;
};

}  // namespace joa::engine
