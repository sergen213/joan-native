#include "engine/audio_mixer.hpp"

#include <algorithm>
#include <cmath>

namespace joa::engine {

float distance_gain(float d, const Attenuation& a) {
    if (a.ref <= 0.0f) return 1.0f;
    d = std::clamp(d, a.ref, std::max(a.ref, a.max));
    const float denom = a.ref + a.rolloff * (d - a.ref);
    return denom > 0.0f ? std::min(1.0f, a.ref / denom) : 1.0f;
}

glm::vec2 pan_gains(const glm::vec3& position, const Listener& l) {
    const glm::vec3 to = position - l.position;
    const float len = glm::length(to);
    const glm::vec3 right = glm::cross(l.forward, l.up);
    const float rl = glm::length(right);
    const float x = len > 1e-4f && rl > 1e-6f ? std::clamp(glm::dot(to / len, right / rl), -1.0f, 1.0f) : 0.0f;
    const float a = (x + 1.0f) * 0.25f * 3.14159265f;  // 0 = hard left, pi/2 = hard right
    return {std::cos(a), std::sin(a)};
}

void StreamQueue::push(const float* s, std::size_t frames) {
    std::lock_guard l(m_);
    data_.insert(data_.end(), s, s + frames * 2);
}
std::size_t StreamQueue::queued() const {
    std::lock_guard l(m_);
    return data_.size() / 2;
}
std::uint64_t StreamQueue::frames_played() const {
    std::lock_guard l(m_);
    return played_;
}
bool StreamQueue::drained() const {
    std::lock_guard l(m_);
    return finished_ && data_.empty();
}
std::size_t StreamQueue::pop(float* s, std::size_t frames) {
    std::lock_guard l(m_);
    const std::size_t n = std::min(frames, data_.size() / 2);
    std::copy_n(data_.begin(), n * 2, s);
    data_.erase(data_.begin(), data_.begin() + std::ptrdiff_t(n * 2));
    played_ += n;
    return n;
}
void StreamQueue::clear() {
    std::lock_guard l(m_);
    data_.clear();
    played_ = 0;
    finished_ = false;
}

VoiceId Mixer::play(std::shared_ptr<const Pcm> pcm, const PlayParams& p) {
    if (!pcm || !pcm->frames() || pcm->rate <= 0) return 0;
    std::lock_guard l(m_);
    const auto busy = [](const Voice& v) { return v.p.bus == Bus::Sfx && !v.done; };
    if (p.bus == Bus::Sfx && std::count_if(voices_.begin(), voices_.end(), busy) >= kMaxSfxVoices) {
        if (!p.steal) return 0;  // all sources busy: FUN_004950a0 drops the sound
        // FUN_00493340: the last of the farthest sources (listener-centred voices count as distance 0).
        auto far = voices_.end();
        float best = -1.0f;
        for (auto it = voices_.begin(); it != voices_.end(); ++it)
            if (busy(*it))
                if (const float d = it->p.positional ? glm::length(it->p.position - listener_.position) : 0.0f; d >= best) far = it, best = d;
        voices_.erase(far);
    }
    Voice v;
    v.id = next_++;
    v.pcm = std::move(pcm);
    v.p = p;
    voices_.push_back(std::move(v));
    return voices_.back().id;
}

VoiceId Mixer::play_stream(std::shared_ptr<StreamQueue> stream, Bus bus, float gain) {
    if (!stream) return 0;
    std::lock_guard l(m_);
    Voice v;
    v.id = next_++;
    v.stream = std::move(stream);
    v.p.bus = bus;
    v.p.gain = gain;
    voices_.push_back(std::move(v));
    return voices_.back().id;
}

const Mixer::Voice* Mixer::find(VoiceId v) const {
    for (const Voice& x : voices_)
        if (x.id == v) return &x;
    return nullptr;
}

void Mixer::stop(VoiceId v) {
    std::lock_guard l(m_);
    std::erase_if(voices_, [&](const Voice& x) { return x.id == v; });
}
void Mixer::stop_all() {
    std::lock_guard l(m_);
    voices_.clear();
}
bool Mixer::playing(VoiceId v) const {
    std::lock_guard l(m_);
    const Voice* it = find(v);
    return it && !it->done;
}
void Mixer::set_gain(VoiceId v, float g) {
    std::lock_guard l(m_);
    if (Voice* it = find(v)) it->p.gain = g;
}
void Mixer::set_position(VoiceId v, const glm::vec3& pos) {
    std::lock_guard l(m_);
    if (Voice* it = find(v)) it->p.position = pos;
}
void Mixer::fade_out(VoiceId v, float seconds) {
    std::lock_guard l(m_);
    if (Voice* it = find(v); it && it->fade_step == 0.0f)
        it->fade_step = seconds > 0.0f ? 1.0f / (seconds * kOutputRate) : 1.0f;
}

void Mixer::set_listener(const Listener& li) {
    std::lock_guard l(m_);
    listener_ = li;
}
void Mixer::set_bus_volume(Bus b, float vol) {
    std::lock_guard l(m_);
    bus_[std::size_t(b)] = std::clamp(vol, 0.0f, 1.0f);
}
float Mixer::bus_volume(Bus b) const {
    std::lock_guard l(m_);
    return bus_[std::size_t(b)];
}
void Mixer::set_master(float vol) {
    std::lock_guard l(m_);
    master_ = std::clamp(vol, 0.0f, 1.0f);
}
void Mixer::set_3d(bool on) {
    std::lock_guard l(m_);
    three_d_ = on;
}
std::size_t Mixer::active_voices() const {
    std::lock_guard l(m_);
    return voices_.size();
}

glm::vec2 Mixer::voice_gains(const Voice& v) const {
    const float g = master_ * bus_[std::size_t(v.p.bus)] * v.p.gain;
    // OpenAL never spatialises multi-channel buffers; streams are stereo too.
    if (v.stream || v.pcm->channels == 2) return {g, g};
    if (!v.p.positional) return glm::vec2(g * 0.70710678f);
    const float att = three_d_ ? distance_gain(glm::length(v.p.position - listener_.position), v.p.attenuation) : 1.0f;
    return pan_gains(v.p.position, listener_) * (g * att);
}

void Mixer::mix(float* out, std::size_t frames) {
    std::fill_n(out, frames * 2, 0.0f);
    std::lock_guard l(m_);
    for (Voice& v : voices_) {
        const glm::vec2 g = voice_gains(v);
        std::size_t n = frames;
        if (v.stream) {
            scratch_.resize(frames * 2);
            n = v.stream->pop(scratch_.data(), frames);
            for (std::size_t i = 0; i < n; ++i) {
                const float f = v.fade_step ? std::max(0.0f, v.fade - v.fade_step * float(i)) : 1.0f;
                out[i * 2] += scratch_[i * 2] * g.x * f;
                out[i * 2 + 1] += scratch_[i * 2 + 1] * g.y * f;
            }
            if (n < frames && v.stream->drained()) v.done = true;  // underrun otherwise: silence, keep voice
        } else {
            const Pcm& p = *v.pcm;
            const std::size_t len = p.frames();
            const double step = double(p.rate) / kOutputRate * std::max(v.p.pitch, 0.01f);
            const float* s = p.samples.data();
            for (std::size_t i = 0; i < frames; ++i) {
                if (v.cursor >= double(len)) {
                    if (!v.p.loop) { v.done = true; n = i; break; }
                    v.cursor = std::fmod(v.cursor, double(len));
                }
                const std::size_t i0 = std::size_t(v.cursor);
                const std::size_t i1 = i0 + 1 < len ? i0 + 1 : (v.p.loop ? 0 : i0);
                const float t = float(v.cursor - double(i0));
                const float f = v.fade_step ? std::max(0.0f, v.fade - v.fade_step * float(i)) : 1.0f;
                if (p.channels == 1) {
                    const float x = (s[i0] + (s[i1] - s[i0]) * t) * f;
                    out[i * 2] += x * g.x;
                    out[i * 2 + 1] += x * g.y;
                } else {
                    out[i * 2] += (s[i0 * 2] + (s[i1 * 2] - s[i0 * 2]) * t) * g.x * f;
                    out[i * 2 + 1] += (s[i0 * 2 + 1] + (s[i1 * 2 + 1] - s[i0 * 2 + 1]) * t) * g.y * f;
                }
                v.cursor += step;
            }
        }
        if (v.fade_step) {
            v.fade -= v.fade_step * float(n);
            if (v.fade <= 0.0f) v.done = true;
        }
    }
    std::erase_if(voices_, [](const Voice& v) { return v.done; });
}

}  // namespace joa::engine
