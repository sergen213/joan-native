#include "engine/audio_system.hpp"

#include "formats/config_dat.hpp"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace joa::engine {
namespace {

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}
float number(const std::string& s) {
    float v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}
bool fail(std::string* err, std::string m) {
    if (err) *err = std::move(m);
    return false;
}
bool yes(const std::string& s) { return !s.empty() && (s[0] == 'Y' || s[0] == 'y'); }

std::optional<formats::DbfTable> std_table(const formats::Vfs& vfs, std::string_view name, std::string* err) {
    auto path = vfs.resolve("Resource/STD.SET");
    auto set = path ? formats::ResArchive::open(*path, err) : std::nullopt;
    const formats::ResEntry* e = set ? set->find(name) : nullptr;
    if (!e) {
        if (err && err->empty()) *err = "Resource/STD.SET: no table " + std::string(name);
        return std::nullopt;
    }
    return formats::DbfTable::parse(set->data(*e), err);
}

int music_type(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
        case 'N': return 1; case 'B': return 2; case 'M': return 3; case 'E': return 4; case 'S': return 5;
        case 'G': return 6; case 'W': return 7; case 'C': return 8; case '1': return 9; case '0': return 10;
        default: return 0;
    }
}
int env_type(char c) {
    constexpr std::string_view k = "MANRSCIBW";
    const auto i = k.find(char(std::toupper(static_cast<unsigned char>(c))));
    return c && i != std::string_view::npos ? int(i) + 1 : 0;
}

}  // namespace

bool SoundBank::load(const formats::Vfs& vfs, std::string* err) {
    auto t = std_table(vfs, "SOUND", err);
    if (!t) return false;
    const auto f = [&](const char* n) { return t->field_index(n); };
    const int code = f("SOUND_CODE"), wave = f("WAVE_FILE"), randmax = f("WF_RANDMAX"), gain = f("GAIN"),
              minp = f("MIN_PITCH"), maxp = f("MAX_PITCH"), mind = f("MIN_DIST"), maxd = f("MAX_DIST"),
              roll = f("ROLLOFF"), time = f("SOUNDTIME"), loop = f("LOOP_FLAG"), speed = f("SPEED_INDE"),
              db = f("DELAY_BEG"), de = f("DELAY_END"), main = f("MAIN_CHAR");
    if (std::min({code, wave, randmax, gain, minp, maxp, mind, maxd, roll, time, loop, speed, db, de, main}) < 0)
        return fail(err, "SOUND table layout changed");
    defs_.clear();
    for (std::size_t r = 0; r < t->record_count(); ++r) {
        SoundDef d;
        d.code = t->text(r, std::size_t(code));
        d.wave = t->text(r, std::size_t(wave));
        d.randmax = std::max(1, int(number(t->text(r, std::size_t(randmax)))));  // FUN_004893c0 = max(n, 1)
        d.gain = number(t->text(r, std::size_t(gain)));
        d.min_pitch = number(t->text(r, std::size_t(minp)));
        d.max_pitch = number(t->text(r, std::size_t(maxp)));
        d.attenuation = {number(t->text(r, std::size_t(mind))), number(t->text(r, std::size_t(maxd))),
                         number(t->text(r, std::size_t(roll)))};
        d.sound_time = int(number(t->text(r, std::size_t(time))));
        d.loop = yes(t->text(r, std::size_t(loop)));
        d.speed_index = yes(t->text(r, std::size_t(speed)));
        d.delay_beg = int(number(t->text(r, std::size_t(db))));
        d.delay_end = int(number(t->text(r, std::size_t(de))));
        d.main_char = yes(t->text(r, std::size_t(main)));
        defs_.push_back(std::move(d));
    }
    std::sort(defs_.begin(), defs_.end(), [](const SoundDef& a, const SoundDef& b) { return a.code < b.code; });
    cache_.assign(defs_.size(), nullptr);
    auto path = vfs.resolve("Resource/WAVE.RES");
    if (!path) return fail(err, "Resource/WAVE.RES missing");
    waves_path_ = *path;
    waves_.reset();
    return true;
}

const formats::ResArchive* SoundBank::waves() const {
    if (!waves_ && !waves_path_.empty()) waves_ = formats::ResArchive::open(waves_path_);
    return waves_ ? &*waves_ : nullptr;
}

int SoundBank::find(std::string_view code) const {
    const std::string key = upper(code);
    const auto it = std::lower_bound(defs_.begin(), defs_.end(), key, [](const SoundDef& d, const std::string& k) { return d.code < k; });
    return it != defs_.end() && it->code == key ? int(it - defs_.begin()) : -1;
}

int SoundBank::variant(int index, int seed) const {
    if (index < 0 || index >= int(defs_.size())) return -1;
    const int n = defs_[std::size_t(index)].randmax;
    return n > 1 ? std::min(int(defs_.size()) - 1, index + std::abs(seed) % n) : index;
}

std::shared_ptr<const Pcm> SoundBank::pcm(int index) const {
    const formats::ResArchive* w = waves();
    if (index < 0 || index >= int(defs_.size()) || !w) return nullptr;
    auto& slot = cache_[std::size_t(index)];
    if (!slot)
        if (const formats::ResEntry* e = w->find(defs_[std::size_t(index)].wave))
            if (auto p = decode_wav(w->data(*e))) slot = std::make_shared<const Pcm>(std::move(*p));
    return slot;
}

bool MusicTable::load(const formats::Vfs& vfs, std::string* err) {
    auto t = std_table(vfs, "MUSIC", err);
    if (!t) return false;
    const int code = t->field_index("CODE"), file = t->field_index("FILENAME"), type = t->field_index("TYPE"),
              smin = t->field_index("STARTMIN"), emin = t->field_index("ENDMIN"), ssce = t->field_index("STARTSCE"),
              esce = t->field_index("ENDSCE"), et = t->field_index("E_TYPE");
    if (std::min({code, file, type, smin, emin, ssce, esce, et}) < 0) return fail(err, "MUSIC table layout changed");
    tracks_.clear();
    for (std::size_t r = 0; r < t->record_count(); ++r) {
        const auto s = [&](int i) { return t->text(r, std::size_t(i)); };
        tracks_.push_back({s(code), s(file), music_type(s(type).c_str()[0]), env_type(s(et).c_str()[0]), number(s(smin)),
                           number(s(emin)), int(number(s(ssce))), int(number(s(esce)))});
    }
    return true;
}

const MusicTrack* MusicTable::find(std::string_view key) const {
    for (const auto& t : tracks_)
        if (formats::iequals(t.code, key)) return &t;
    for (const auto& t : tracks_)
        if (formats::iequals(t.file, key)) return &t;
    return nullptr;
}

const MusicTrack* MusicTable::pick(int type, int env, int scenario, std::minstd_rand& rng) const {
    std::vector<const MusicTrack*> pool;
    for (const auto& t : tracks_)
        if (t.type == type && (env == 0 || t.env_type == env) &&
            (type == 5 || scenario == 0 || (t.start_sce <= scenario && scenario <= t.end_sce)))
            pool.push_back(&t);
    return pool.empty() ? nullptr : pool[rng() % pool.size()];
}

const MusicTrack* MusicTable::by_time(int type, float m) const {
    for (const auto& t : tracks_) {
        if (t.type != type) continue;
        const bool hit = t.start_min <= t.end_min ? (t.start_min <= m && m < t.end_min) : (m >= t.start_min || m < t.end_min);
        if (hit) return &t;
    }
    return nullptr;
}

AudioSettings AudioSettings::from_config(const formats::GameConfig& c) {
    AudioSettings s;
    s.music = std::clamp(c.music_volume / 100.0f, 0.0f, 1.0f);
    s.env = std::clamp(c.sfx_volume / 100.0f, 0.0f, 1.0f);      // 0x3C Environmental
    s.sfx = std::clamp(c.speech_volume / 100.0f, 0.0f, 1.0f);   // 0x41 Sound Volume
    s.voice = std::clamp(c.volume_3e / 100.0f, 0.0f, 1.0f);     // 0x3E HYPOTHESIS
    s.three_d = c.sound_3d != 0;
    s.enabled = c.flag_40 != 0;
    return s;
}

AudioSystem::AudioSystem(formats::Vfs vfs, const AudioSettings& s, bool open_device) : vfs_(std::move(vfs)) {
    std::string err;
    if (!bank_.load(vfs_, &err)) std::fprintf(stderr, "audio: sound bank: %s\n", err.c_str());
    err.clear();
    if (!music_.load(vfs_, &err)) std::fprintf(stderr, "audio: music table: %s\n", err.c_str());
    apply(s);
    if (!open_device) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio: SDL audio unavailable (%s); running silent\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kOutputRate};
    device_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioSystem::callback, this);
    if (!device_) {
        std::fprintf(stderr, "audio: no playback device (%s); running silent\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    SDL_ResumeAudioStreamDevice(device_);
}

AudioSystem::~AudioSystem() {
    if (device_) {
        SDL_DestroyAudioStream(device_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void AudioSystem::callback(void* self, SDL_AudioStream* s, int additional, int) {
    auto& a = *static_cast<AudioSystem*>(self);
    const std::size_t frames = std::size_t(std::max(additional, 0)) / (2 * sizeof(float));
    if (!frames) return;
    a.device_buf_.resize(frames * 2);
    a.mixer_.mix(a.device_buf_.data(), frames);
    SDL_PutAudioStreamData(s, a.device_buf_.data(), int(frames * 2 * sizeof(float)));
}

void AudioSystem::apply(const AudioSettings& s) {
    mixer_.set_master(s.master);
    mixer_.set_bus_volume(Bus::Music, s.music);
    mixer_.set_bus_volume(Bus::Env, s.env);
    mixer_.set_bus_volume(Bus::Sfx, s.sfx);
    mixer_.set_bus_volume(Bus::Voice, s.voice);
    mixer_.set_3d(s.three_d);
    enabled_ = s.enabled;
}

VoiceId AudioSystem::play_sound(std::string_view code, std::optional<glm::vec3> pos, int seed, bool steal) {
    const int i = bank_.variant(bank_.find(code), seed);
    if (!enabled_ || i < 0) return 0;
    const SoundDef& d = bank_.defs()[std::size_t(i)];
    PlayParams p;
    p.gain = d.gain;
    p.pitch = d.min_pitch;  // MIN/MAX_PITCH are 1.0 in every shipped record
    p.loop = d.loop;
    p.positional = pos.has_value();
    p.position = pos.value_or(glm::vec3(0.0f));
    p.attenuation = d.attenuation;
    p.steal = steal;
    return mixer_.play(bank_.pcm(i), p);
}

VoiceId AudioSystem::play_voice(std::string_view pack, std::string_view id) {
    if (!enabled_) return 0;
    auto it = voice_packs_.find(pack);
    if (it == voice_packs_.end()) {
        const auto path = vfs_.resolve("Resource/" + std::string(pack) + ".RES");
        it = voice_packs_.emplace(std::string(pack), path ? formats::ResArchive::open(*path) : std::nullopt).first;
    }
    const formats::ResEntry* e = it->second ? it->second->find(id) : nullptr;
    auto pcm = e ? decode_wav(it->second->data(*e)) : std::nullopt;
    if (!pcm) return 0;
    PlayParams p;
    p.bus = Bus::Voice;
    return mixer_.play(std::make_shared<const Pcm>(std::move(*pcm)), p);
}

bool AudioSystem::start(Channel& c, std::string_view key, Bus bus, bool loop) {
    mixer_.stop(c.voice);
    c = {};
    c.track = music_.find(key);
    const auto path = vfs_.resolve("music/" + (c.track ? c.track->file : std::string(key)));
    std::string err;
    c.file = path ? AudioFileStream::open(*path, &err) : nullptr;
    if (!c.file) {
        std::fprintf(stderr, "audio: cannot open music %.*s %s\n", int(key.size()), key.data(), err.c_str());
        c.track = nullptr;
        return false;
    }
    c.loop = loop;
    c.queue = std::make_shared<StreamQueue>();
    refill(c);
    c.voice = mixer_.play_stream(c.queue, bus);
    return true;
}

void AudioSystem::refill(Channel& c) {
    if (!c.file) return;
    std::vector<float> buf;
    bool rewound_empty = false;  // a looping file that decodes nothing would otherwise rewind forever
    while (c.queue->queued() < std::size_t(kOutputRate)) {  // keep ~1 s queued
        buf.clear();
        if (c.file->read(buf, 4096)) {
            c.queue->push(buf.data(), buf.size() / 2);
            rewound_empty = false;
        } else if (!c.loop || rewound_empty || !c.file->rewind()) {  // FUN_0059b1c0 restarts a finished looping track
            c.queue->finish();
            c.file.reset();
            return;
        } else {
            rewound_empty = true;
        }
    }
}

bool AudioSystem::play_music(std::string_view key, bool loop) {
    music_fading_ = false;
    return start(channels_[0], key, Bus::Music, loop);
}
void AudioSystem::stop_music() {
    mixer_.stop(channels_[0].voice);
    channels_[0] = {};
    music_fading_ = false;
}
void AudioSystem::fade_out_music(float seconds) {
    if (music_fading_) return;
    music_fading_ = true;
    mixer_.fade_out(channels_[0].voice, seconds);
}
bool AudioSystem::music_playing() const { return channels_[0].voice && mixer_.playing(channels_[0].voice); }

bool AudioSystem::play_env(int slot, std::string_view key, bool loop) {
    return slot >= 0 && slot < kEnvSlots && start(channels_[1 + slot], key, Bus::Env, loop);
}
void AudioSystem::fade_out_env(int slot, float seconds) {
    if (slot >= 0 && slot < kEnvSlots) mixer_.fade_out(channels_[1 + slot].voice, seconds);
}
void AudioSystem::stop_env(int slot) {
    if (slot < 0 || slot >= kEnvSlots) return;
    mixer_.stop(channels_[1 + slot].voice);
    channels_[1 + slot] = {};
}
bool AudioSystem::env_playing(int slot) const {
    return slot >= 0 && slot < kEnvSlots && channels_[1 + slot].voice && mixer_.playing(channels_[1 + slot].voice);
}

void AudioSystem::update() {
    for (Channel& c : channels_) refill(c);
}

void MusicDirector::update(AudioSystem& a, MusicMood mood, int scenario, std::uint32_t now) {
    if (mood != MusicMood::Normal) calm_since_ = 0;
    const MusicTrack* t = a.music_table().pick(int(mood), 0, scenario, rng);
    if (!t) return;
    const MusicTrack* cur = a.current_music();
    if (a.music_fading()) {  // state 2: wait for the 8 s fade, then start the new mood's track
        if (!a.music_playing()) a.play_music(t->code);
        return;
    }
    if (cur && cur->type == int(mood) && a.music_playing()) return;  // same mood keeps (and loops) its track
    if (!cur || !a.music_playing() || cur->type == int(MusicMood::Menu) || mood == MusicMood::Menu ||
        mood == MusicMood::Won || mood == MusicMood::GameOver) {
        a.play_music(t->code);  // immediate switch, no fade
        return;
    }
    if (mood == MusicMood::Normal && cur->type == int(MusicMood::Battle)) {
        if (!calm_since_) calm_since_ = now + 1;
        if (now + 1 - calm_since_ > kBattleHoldMs) a.fade_out_music();
    } else {
        a.fade_out_music();
    }
}

}  // namespace joa::engine
