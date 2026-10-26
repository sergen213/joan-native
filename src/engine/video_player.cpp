#include "engine/video_player.hpp"

#include "engine/platform.hpp"
#include "engine/png.hpp"
#include "engine/renderer.hpp"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>

#include <cstdio>
#include <deque>
#include <optional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace joa::engine {
namespace {
bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}
}  // namespace

std::unique_ptr<VideoDecoder> VideoDecoder::open(const std::filesystem::path& path, std::string* err) {
    std::unique_ptr<VideoDecoder> d(new VideoDecoder);
    if (avformat_open_input(&d->fmt_, path.c_str(), nullptr, nullptr) < 0) return fail(err, "cannot open " + path.string()), nullptr;
    if (avformat_find_stream_info(d->fmt_, nullptr) < 0) return fail(err, "no stream info"), nullptr;
    d->video_index_ = av_find_best_stream(d->fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->video_index_ < 0) return fail(err, "no video stream"), nullptr;
    const AVStream* vs = d->fmt_->streams[d->video_index_];
    const AVCodec* codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) return fail(err, std::string("no decoder for ") + avcodec_get_name(vs->codecpar->codec_id)), nullptr;
    d->video_ = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(d->video_, vs->codecpar) < 0 || avcodec_open2(d->video_, codec, nullptr) < 0)
        return fail(err, "cannot open video decoder"), nullptr;
    d->width_ = d->video_->width;
    d->height_ = d->video_->height;
    d->time_base_ = av_q2d(vs->time_base);
    d->fps_ = vs->avg_frame_rate.num ? av_q2d(vs->avg_frame_rate) : av_q2d(vs->r_frame_rate);
    d->duration_ = d->fmt_->duration > 0 ? double(d->fmt_->duration) / AV_TIME_BASE : 0.0;
    d->audio_index_ = av_find_best_stream(d->fmt_, AVMEDIA_TYPE_AUDIO, -1, d->video_index_, nullptr, 0);
    if (d->audio_index_ >= 0 && !d->audio_dec_.open(d->fmt_->streams[d->audio_index_], err)) return nullptr;
    d->frame_ = av_frame_alloc();
    d->packet_ = av_packet_alloc();
    return d;
}

VideoDecoder::~VideoDecoder() {
    sws_freeContext(sws_);
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&video_);
    avformat_close_input(&fmt_);
}

bool VideoDecoder::receive(VideoFrame& out) {
    while (avcodec_receive_frame(video_, frame_) == 0) {
        const double pts = frame_->best_effort_timestamp == AV_NOPTS_VALUE ? 0.0 : double(frame_->best_effort_timestamp) * time_base_;
        sws_ = sws_getCachedContext(sws_, frame_->width, frame_->height, AVPixelFormat(frame_->format), frame_->width,
                                    frame_->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        out.width = frame_->width;
        out.height = frame_->height;
        out.pts = pts;
        out.rgba.resize(std::size_t(out.width) * out.height * 4);
        std::uint8_t* dst[1] = {out.rgba.data()};
        const int stride[1] = {out.width * 4};
        sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, stride);
        av_frame_unref(frame_);
        return true;
    }
    return false;
}

bool VideoDecoder::next(VideoFrame& out) {
    if (ready_.empty()) return decode_next(out);
    out = std::move(ready_.front());
    ready_.pop_front();
    return true;
}

bool VideoDecoder::decode_next(VideoFrame& out) {
    for (;;) {
        if (receive(out)) return true;
        if (eof_) return false;
        if (av_read_frame(fmt_, packet_) < 0) {
            eof_ = true;
            avcodec_send_packet(video_, nullptr);
            if (audio_index_ >= 0) {
                audio_buf_.clear();
                audio_dec_.decode(nullptr, audio_buf_);
                audio_q_->push(audio_buf_.data(), audio_buf_.size() / 2);
                audio_q_->finish();
            }
            continue;
        }
        if (packet_->stream_index == video_index_) {
            avcodec_send_packet(video_, packet_);
        } else if (packet_->stream_index == audio_index_ && audio_on_) {
            audio_buf_.clear();
            audio_dec_.decode(packet_, audio_buf_);
            audio_q_->push(audio_buf_.data(), audio_buf_.size() / 2);
        }
        av_packet_unref(packet_);
    }
}

bool VideoDecoder::seek(double seconds) {
    if (av_seek_frame(fmt_, -1, std::int64_t(seconds * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(video_);
    if (audio_index_ >= 0) audio_dec_.flush();
    audio_q_->clear();
    ready_.clear();
    eof_ = false;
    audio_on_ = false;  // ponytail: audio restarts at the first packet after the target frame (~1 packet of drift)
    std::optional<VideoFrame> prev;
    VideoFrame cur;
    while (decode_next(cur)) {
        if (cur.pts > seconds + 1e-4 && prev) {
            ready_.push_back(std::move(*prev));
            ready_.push_back(std::move(cur));
            prev.reset();
            break;
        }
        prev = std::move(cur);
    }
    if (prev) ready_.push_back(std::move(*prev));
    audio_on_ = true;
    return !ready_.empty();
}

glm::vec4 letterbox(glm::vec4 area, int w, int h) {
    if (w <= 0 || h <= 0 || area.z <= 0 || area.w <= 0) return area;
    const float s = std::min(area.z / float(w), area.w / float(h));
    const glm::vec2 size(float(w) * s, float(h) * s);
    return {area.x + (area.z - size.x) * 0.5f, area.y + (area.w - size.y) * 0.5f, size.x, size.y};
}

bool play_movie(Platform& platform, IRenderer& renderer, Mixer* mixer, const std::filesystem::path& path,
                const MovieOptions& o) {
    std::string err;
    auto dec = VideoDecoder::open(path, &err);
    if (!dec) {
        std::fprintf(stderr, "movie: %s\n", err.c_str());
        return false;
    }
    if (o.start > 0 && !dec->seek(o.start)) std::fprintf(stderr, "movie: seek to %.2f s failed\n", o.start);

    std::deque<VideoFrame> ahead;
    VideoFrame shown;
    TextureHandle tex;
    std::vector<std::vector<std::uint8_t>> spare;  // pixel buffers of shown/skipped frames, refilled by the decoder
    const auto upload = [&](VideoFrame f) {
        TextureData t;
        t.width = f.width;
        t.height = f.height;
        t.pixels = std::move(f.rgba);
        t.mipmaps = false;
        t.repeat = false;
        // The same texture rewritten in place while the size holds (30 Hz movie frames).
        if (!tex || f.width != shown.width || f.height != shown.height || !renderer.update_texture(tex, t)) {
            if (tex) renderer.destroy_texture(tex);
            tex = renderer.create_texture(t);
        }
        spare.push_back(std::move(t.pixels));
        shown.width = f.width;
        shown.height = f.height;
        shown.pts = f.pts;
    };
    const auto draw = [&] {
        const glm::ivec2 size = platform.pixel_size();
        FrameData f;
        f.view.viewport = {0, 0, size.x, size.y};
        f.lighting.clear_color = {0, 0, 0, 1};
        const glm::vec4 r = letterbox({0, 0, float(size.x), float(size.y)}, shown.width, shown.height);
        UiQuad q;
        q.position = {r.x, r.y};
        q.size = {r.z, r.w};
        q.texture = tex;
        f.ui = std::span<const UiQuad>(&q, tex ? 1 : 0);
        renderer.begin_frame(size);
        renderer.submit(f);
    };

    VideoFrame first;
    if (!dec->next(first)) return std::fprintf(stderr, "movie: no frames in %s\n", path.c_str()), false;
    upload(std::move(first));
    if (!o.screenshot.empty()) {
        draw();
        const Image img = renderer.read_back();
        renderer.end_frame();
        renderer.destroy_texture(tex);
        if (!write_png(o.screenshot, img.width, img.height, img.rgba)) return std::fprintf(stderr, "movie: cannot write %s\n", o.screenshot.c_str()), false;
        std::fprintf(stderr, "movie: wrote %s (%dx%d, t=%.3f s)\n", o.screenshot.c_str(), img.width, img.height, shown.pts);
        return true;
    }

    const VoiceId voice = mixer && dec->has_audio() ? mixer->play_stream(dec->audio(), Bus::Movie) : 0;
    const double origin = shown.pts;
    const std::uint64_t t0 = SDL_GetTicksNS();
    bool decoding = true, prev_esc = true, prev_click = true;  // a key held from the previous movie must be released first
    RawInput raw;
    for (;;) {
        if (!platform.pump(raw)) break;
        const bool esc = raw.keys[SDL_SCANCODE_ESCAPE], click = raw.mouse_buttons[SDL_BUTTON_LEFT];
        raw.end_frame();
        if ((esc && !prev_esc) || (click && !prev_click)) break;
        prev_esc = esc;
        prev_click = click;

        // Decode ahead until ~0.3 s of audio is queued (the audio clock must never starve).
        auto audio = dec->audio();
        while (decoding && ahead.size() < 90 && (ahead.size() < 2 || (voice && audio->queued() < kOutputRate * 3 / 10))) {
            VideoFrame f;
            if (!spare.empty()) f.rgba = std::move(spare.back()), spare.pop_back();
            if (!dec->next(f)) decoding = false;
            else ahead.push_back(std::move(f));
        }
        const double clock = origin + (voice ? double(audio->frames_played()) / kOutputRate : double(SDL_GetTicksNS() - t0) * 1e-9);
        while (!ahead.empty() && ahead.front().pts <= clock) {
            if (ahead.size() == 1 || ahead[1].pts > clock) upload(std::move(ahead.front()));
            else spare.push_back(std::move(ahead.front().rgba));
            ahead.pop_front();
        }
        if (!decoding && ahead.empty() && (!voice || !mixer->playing(voice))) break;
        draw();
        renderer.end_frame();
    }
    if (voice) mixer->stop(voice);
    if (tex) renderer.destroy_texture(tex);
    return true;
}

}  // namespace joa::engine
