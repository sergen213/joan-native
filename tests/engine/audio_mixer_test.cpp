// Mixer math (OpenAL inverse-distance-clamped curve, constant-power pan, buses, fades, voice limit),
// music director rules, and an offline render of mixed game audio to $JOA_TEST_OUT/audio_mix.wav.
#include "check.hpp"
#include "engine/audio_system.hpp"
#include "wav_write.hpp"

#include <algorithm>
#include <cstdlib>

using namespace joa::engine;

namespace {

std::shared_ptr<const Pcm> tone(int rate, int channels, float seconds, float value = 0.5f) {
    Pcm p{rate, channels, std::vector<float>(std::size_t(rate * seconds) * std::size_t(channels), value)};
    return std::make_shared<const Pcm>(std::move(p));
}

glm::vec2 render_one(Mixer& m) {  // mean |L|, |R| of 1024 frames
    std::vector<float> out(2048);
    m.mix(out.data(), 1024);
    glm::vec2 s(0);
    for (std::size_t i = 0; i < 1024; ++i) s += glm::vec2(std::fabs(out[i * 2]), std::fabs(out[i * 2 + 1]));
    return s / 1024.0f;
}

}  // namespace

int main() {
    // Distance curve: AL_INVERSE_DISTANCE_CLAMPED with the SOUND table defaults (100 / 1500 / 1.0).
    const Attenuation a{100.0f, 1500.0f, 1.0f};
    CHECK_NEAR(distance_gain(0.0f, a), 1.0f, 1e-6);
    CHECK_NEAR(distance_gain(100.0f, a), 1.0f, 1e-6);
    CHECK_NEAR(distance_gain(200.0f, a), 0.5f, 1e-6);    // -6 dB per doubling at rolloff 1
    CHECK_NEAR(distance_gain(400.0f, a), 0.25f, 1e-6);
    CHECK_NEAR(distance_gain(1500.0f, a), 100.0f / 1500.0f, 1e-6);
    CHECK_NEAR(distance_gain(9000.0f, a), 100.0f / 1500.0f, 1e-6);  // clamped beyond MAX_DIST
    CHECK_NEAR(distance_gain(300.0f, {100.0f, 1500.0f, 2.0f}), 100.0f / 500.0f, 1e-6);
    CHECK_NEAR(distance_gain(12000.0f, {10000.0f, 15000.0f, 1.0f}), 1.0f / 1.2f, 1e-6);  // 346 "near-2D" records

    // Constant-power pan relative to the listener (forward -Z, up +Y, right +X).
    Listener l;
    CHECK_NEAR(pan_gains({10, 0, 0}, l).x, 0.0f, 1e-6);   // hard right
    CHECK_NEAR(pan_gains({10, 0, 0}, l).y, 1.0f, 1e-6);
    CHECK_NEAR(pan_gains({-10, 0, 0}, l).x, 1.0f, 1e-6);  // hard left
    CHECK_NEAR(pan_gains({0, 0, -10}, l).x, pan_gains({0, 0, -10}, l).y, 1e-6);  // ahead: centred
    CHECK_NEAR(pan_gains(l.position, l).x, 0.70710678f, 1e-6);
    l.forward = {1, 0, 0};  // turn right: +X is now ahead, -Z to the left
    CHECK(pan_gains({0, 0, -10}, l).x > 0.99f);
    for (float x = -1; x <= 1; x += 0.25f) {
        const glm::vec2 g = pan_gains({x, 0, -1}, Listener{});
        CHECK_NEAR(g.x * g.x + g.y * g.y, 1.0f, 1e-5);  // constant power
    }

    // Mixer: positional mono source at 2x ref distance on the right: L = 0, R = 0.5 * gain.
    Mixer m;
    PlayParams p;
    p.positional = true;
    p.position = {200, 0, 0};
    p.attenuation = a;
    const VoiceId v = m.play(tone(kOutputRate, 1, 1.0f), p);
    glm::vec2 r = render_one(m);
    CHECK_NEAR(r.x, 0.0f, 1e-4);
    CHECK_NEAR(r.y, 0.25f, 1e-3);
    m.set_3d(false);  // AL_NONE: no attenuation, pan kept
    r = render_one(m);
    CHECK_NEAR(r.y, 0.5f, 1e-3);
    m.set_bus_volume(Bus::Sfx, 0.5f);
    r = render_one(m);
    CHECK_NEAR(r.y, 0.25f, 1e-3);
    m.stop(v);
    CHECK(!m.playing(v));

    // Stereo buffers are never spatialised (OpenAL), and resampling keeps duration: 0.1 s at 11025 Hz.
    m.set_bus_volume(Bus::Sfx, 1.0f);
    const VoiceId s = m.play(tone(11025, 2, 0.1f), p);
    r = render_one(m);
    CHECK_NEAR(r.x, 0.5f, 1e-3);
    CHECK_NEAR(r.y, 0.5f, 1e-3);
    std::vector<float> buf(2 * 4410);
    m.mix(buf.data(), 4400);
    CHECK(!m.playing(s));  // 1024 + 4400 > 4410 frames

    // 12-source limit for 3D sound effects; other buses are not limited.
    for (int i = 0; i < Mixer::kMaxSfxVoices; ++i) CHECK(m.play(tone(22050, 1, 1.0f), {}) != 0);
    CHECK(m.play(tone(22050, 1, 1.0f), {}) == 0);
    PlayParams voice;
    voice.bus = Bus::Voice;
    CHECK(m.play(tone(22050, 1, 1.0f), voice) != 0);
    m.stop_all();

    // SE_play_trigger: when the 12 sources are busy, steal the one farthest from the listener (FUN_00493340).
    m.set_listener({});
    VoiceId first_far = 0, last_far = 0;
    for (int i = 0; i < Mixer::kMaxSfxVoices; ++i) {
        PlayParams sp;
        sp.positional = true;
        sp.position = {0, 0, -100.0f * float(1 + i % 6)};  // two at each distance, 100..600
        const VoiceId id = m.play(tone(22050, 1, 1.0f), sp);
        if (i == 5) first_far = id;
        if (i == 11) last_far = id;
    }
    PlayParams steal;
    steal.steal = true;
    const VoiceId stolen_for = m.play(tone(22050, 1, 1.0f), steal);
    CHECK(stolen_for != 0 && m.playing(stolen_for));
    CHECK(m.playing(first_far) && !m.playing(last_far));  // the last of the farthest goes
    CHECK(m.play(tone(22050, 1, 1.0f), {}) == 0);  // plain FUN_004950a0 path still drops
    m.stop_all();

    // Linear fade: half-way through a 0.1 s fade the stream is at half gain, then it stops.
    auto q = std::make_shared<StreamQueue>();
    std::vector<float> ones(2 * kOutputRate, 1.0f);
    q->push(ones.data(), kOutputRate);
    const VoiceId mv = m.play_stream(q, Bus::Music);
    m.fade_out(mv, 0.1f);
    std::vector<float> fade(2 * 4410);
    m.mix(fade.data(), 4410);
    CHECK_NEAR(fade[2 * 2205], 0.5f, 1e-3);
    CHECK(!m.playing(mv));  // faded to 0 at exactly 0.1 s
    CHECK(q->frames_played() == 4410);

    // With the original data: music director rules and an offline render of real game audio.
    const char* dir = std::getenv("JOA_GAME_DIR");
    const joa::formats::Vfs vfs(dir ? dir : ".");
    if (!vfs.exists("Resource/WAVE.RES")) {
        std::fprintf(stderr, "SKIP (data part): no original data\n");
        return test_result();
    }
    AudioSystem audio(vfs, {}, false);
    const auto pump = [&](float seconds, std::vector<float>* keep = nullptr) {
        std::vector<float> chunk(2 * 1024);
        for (int n = int(seconds * kOutputRate / 1024); n > 0; --n) {
            audio.update();
            audio.mixer().mix(chunk.data(), 1024);
            if (keep) keep->insert(keep->end(), chunk.begin(), chunk.end());
        }
    };

    MusicDirector dir_;
    std::uint32_t now = 0;
    dir_.update(audio, MusicMood::Menu, 1, now);
    CHECK(audio.current_music() && audio.current_music()->code == "MAIN_BACK");
    dir_.update(audio, MusicMood::Normal, 1, now += 100);  // leaving the menu: immediate switch
    CHECK(audio.current_music()->type == 1 && !audio.music_fading());
    dir_.update(audio, MusicMood::Battle, 1, now += 100);  // normal -> battle: 8 s fade first
    CHECK(audio.music_fading());
    pump(4.0f);
    dir_.update(audio, MusicMood::Battle, 1, now += 4000);
    CHECK(audio.current_music()->type == 1);  // still fading
    pump(4.2f);
    dir_.update(audio, MusicMood::Battle, 1, now += 4200);
    CHECK(audio.current_music()->type == 2 && !audio.music_fading());
    for (int i = 0; i < 8; ++i) dir_.update(audio, MusicMood::Normal, 1, now += 1000);  // < 8 s calm: keep battle
    CHECK(audio.current_music()->type == 2 && !audio.music_fading());
    dir_.update(audio, MusicMood::Normal, 1, now += 1001);  // 8001 ms calm (FUN_004e6fe0: elapsed > 8000)
    CHECK(audio.music_fading());
    dir_.update(audio, MusicMood::Won, 1, now += 10);  // a running fade always completes first
    CHECK(audio.music_fading() && audio.current_music()->type == 2);
    pump(8.2f);
    dir_.update(audio, MusicMood::Won, 1, now += 8200);
    CHECK(audio.current_music()->code == "SCENARIO_WON");
    dir_.update(audio, MusicMood::GameOver, 1, now += 10);  // won/game over interrupt a playing track at once
    CHECK(audio.current_music()->type == 6 && !audio.music_fading());

    // Offline mix: menu music, a sword swing circling the listener, a UI click, Jean's voice line.
    std::vector<float> out;
    audio.play_music("MAIN_BACK");
    pump(1.5f, &out);
    CHECK(audio.play_voice("S1L1_tactic", "0014") != 0);
    CHECK(audio.play_sound("NO_SUCH_SOUND") == 0);
    for (int i = 0; i < 8; ++i) {
        const float ang = float(i) * 0.785398f;
        CHECK(audio.play_sound("HIT_H", glm::vec3(std::sin(ang), 0, -std::cos(ang)) * 80.0f, i) != 0);
        pump(0.5f, &out);
    }
    CHECK(audio.play_sound("PICKUP") != 0);
    pump(1.0f, &out);
    float peak = 0;
    for (float x : out) peak = std::max(peak, std::fabs(x));
    CHECK(peak > 0.05f && peak < 2.0f);
    if (const char* o = std::getenv("JOA_TEST_OUT")) {
        write_wav(std::string(o) + "/audio_mix.wav", out);
        std::printf("wrote %s/audio_mix.wav (%.1f s, peak %.2f)\n", o, double(out.size() / 2) / kOutputRate, peak);
    }
    return test_result();
}
