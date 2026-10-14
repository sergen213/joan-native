#include "engine/audio_decode.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace joa::engine {
namespace {

bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}

std::uint32_t le32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24; }
std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }

std::string av_error(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

}  // namespace

std::optional<Pcm> decode_wav(std::span<const std::uint8_t> b, std::string* err) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
        return fail(err, "not a RIFF WAVE"), std::nullopt;
    int format = 0, channels = 0, rate = 0, bits = 0;
    std::span<const std::uint8_t> data;
    for (std::size_t p = 12; p + 8 <= b.size();) {
        const std::uint32_t size = le32(&b[p + 4]);
        const std::size_t body = p + 8, len = std::min<std::size_t>(size, b.size() - body);  // tolerate a short last chunk
        if (!std::memcmp(&b[p], "fmt ", 4) && len >= 16) {
            format = le16(&b[body]);
            channels = le16(&b[body + 2]);
            rate = int(le32(&b[body + 4]));
            bits = le16(&b[body + 14]);
        } else if (!std::memcmp(&b[p], "data", 4)) {
            data = b.subspan(body, len);
        }
        p = body + size + (size & 1);
    }
    if (format != 1) return fail(err, "WAVE format " + std::to_string(format) + " is not PCM"), std::nullopt;
    if ((channels != 1 && channels != 2) || rate <= 0 || (bits != 8 && bits != 16))
        return fail(err, "unsupported PCM layout"), std::nullopt;
    Pcm pcm{rate, channels, {}};
    const std::size_t bytes = bits / 8, n = data.size() / bytes / channels * channels;
    pcm.samples.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        pcm.samples[i] = bits == 8 ? (float(data[i]) - 128.0f) / 128.0f
                                   : float(std::int16_t(le16(&data[i * 2]))) / 32768.0f;
    return pcm;
}

FfAudioDecoder::~FfAudioDecoder() {
    av_frame_free(&frame_);
    swr_free(&swr_);
    avcodec_free_context(&codec_);
}

bool FfAudioDecoder::open(const AVStream* st, std::string* err) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return fail(err, std::string("no decoder for ") + avcodec_get_name(st->codecpar->codec_id));
    codec_ = avcodec_alloc_context3(codec);
    if (!codec_ || avcodec_parameters_to_context(codec_, st->codecpar) < 0) return fail(err, "codec context");
    if (const int r = avcodec_open2(codec_, codec, nullptr); r < 0) return fail(err, "avcodec_open2: " + av_error(r));
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (const int r = swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, kOutputRate, &codec_->ch_layout,
                                          codec_->sample_fmt, codec_->sample_rate, 0, nullptr);
        r < 0 || swr_init(swr_) < 0)
        return fail(err, "swresample init");
    frame_ = av_frame_alloc();
    return frame_ != nullptr;
}

bool FfAudioDecoder::decode(const AVPacket* packet, std::vector<float>& out) {
    if (const int r = avcodec_send_packet(codec_, packet); r < 0 && r != AVERROR_EOF) return false;
    for (;;) {
        const int r = avcodec_receive_frame(codec_, frame_);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return false;
        const int cap = swr_get_out_samples(swr_, frame_->nb_samples);
        const std::size_t at = out.size();
        out.resize(at + std::size_t(cap) * 2);
        auto* dst = reinterpret_cast<std::uint8_t*>(out.data() + at);
        const int got = swr_convert(swr_, &dst, cap, const_cast<const std::uint8_t**>(frame_->extended_data), frame_->nb_samples);
        out.resize(at + std::size_t(std::max(got, 0)) * 2);
        av_frame_unref(frame_);
    }
    if (!packet) {  // drain the resampler's tail
        const int cap = swr_get_out_samples(swr_, 0);
        if (cap > 0) {
            const std::size_t at = out.size();
            out.resize(at + std::size_t(cap) * 2);
            auto* dst = reinterpret_cast<std::uint8_t*>(out.data() + at);
            const int got = swr_convert(swr_, &dst, cap, nullptr, 0);
            out.resize(at + std::size_t(std::max(got, 0)) * 2);
        }
    }
    return true;
}

void FfAudioDecoder::flush() {
    avcodec_flush_buffers(codec_);
    swr_init(swr_);  // drops buffered samples
}

std::unique_ptr<AudioFileStream> AudioFileStream::open(const std::filesystem::path& path, std::string* err) {
    std::unique_ptr<AudioFileStream> s(new AudioFileStream);
    if (const int r = avformat_open_input(&s->fmt_, path.c_str(), nullptr, nullptr); r < 0)
        return fail(err, path.string() + ": " + av_error(r)), nullptr;
    if (avformat_find_stream_info(s->fmt_, nullptr) < 0) return fail(err, "no stream info"), nullptr;
    s->stream_ = av_find_best_stream(s->fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (s->stream_ < 0) return fail(err, "no audio stream"), nullptr;
    if (!s->dec_.open(s->fmt_->streams[s->stream_], err)) return nullptr;
    s->packet_ = av_packet_alloc();
    if (s->fmt_->duration > 0) s->duration_ = double(s->fmt_->duration) / AV_TIME_BASE;
    return s;
}

AudioFileStream::~AudioFileStream() {
    av_packet_free(&packet_);
    avformat_close_input(&fmt_);
}

std::size_t AudioFileStream::read(std::vector<float>& out, std::size_t frames) {
    std::size_t done = 0;
    while (done < frames) {
        if (pending_pos_ < pending_.size()) {
            const std::size_t n = std::min(frames - done, (pending_.size() - pending_pos_) / 2);
            out.insert(out.end(), pending_.begin() + std::ptrdiff_t(pending_pos_), pending_.begin() + std::ptrdiff_t(pending_pos_ + n * 2));
            pending_pos_ += n * 2;
            done += n;
            continue;
        }
        if (eof_) break;
        pending_.clear();
        pending_pos_ = 0;
        if (av_read_frame(fmt_, packet_) < 0) {
            eof_ = true;
            dec_.decode(nullptr, pending_);
            continue;
        }
        if (packet_->stream_index == stream_) dec_.decode(packet_, pending_);
        av_packet_unref(packet_);
    }
    return done;
}

bool AudioFileStream::rewind() {
    if (av_seek_frame(fmt_, stream_, 0, AVSEEK_FLAG_BACKWARD) < 0) return false;
    dec_.flush();
    pending_.clear();
    pending_pos_ = 0;
    eof_ = false;
    return true;
}

std::optional<Pcm> decode_audio_file(const std::filesystem::path& path, std::string* err) {
    auto s = AudioFileStream::open(path, err);
    if (!s) return std::nullopt;
    Pcm pcm{kOutputRate, 2, {}};
    while (s->read(pcm.samples, 1 << 16)) {}
    return pcm;
}

}  // namespace joa::engine
