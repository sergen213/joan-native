#pragma once
// Audio decoding: RIFF/PCM WAVE entries of the RES sound packs, and FFmpeg-decoded WMA (music/*.wma,
// movie soundtracks). Everything ends up as interleaved float samples. docs/AUDIO_AND_VIDEO.md.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVPacket;
struct AVStream;
struct SwrContext;
struct AVFrame;
struct AVFormatContext;

namespace joa::engine {

constexpr int kOutputRate = 44100;  // mixer/device rate; the WMA tracks and most WAVE entries use it

struct Pcm {
    int rate = 0;
    int channels = 0;            // 1 or 2
    std::vector<float> samples;  // interleaved, [-1, 1]
    std::size_t frames() const { return channels ? samples.size() / std::size_t(channels) : 0; }
    double seconds() const { return rate ? double(frames()) / rate : 0.0; }
};

// RIFF WAVE with WAVE_FORMAT_PCM (8-bit unsigned or 16-bit signed, mono/stereo, any rate).
// Unknown chunks (cue, LIST) are skipped. Every entry of WAVE.RES, A_WAVE*.RES, S*L1_*.RES fits.
std::optional<Pcm> decode_wav(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

// Decodes one FFmpeg audio stream to interleaved float stereo at kOutputRate.
class FfAudioDecoder {
public:
    FfAudioDecoder() = default;
    ~FfAudioDecoder();
    FfAudioDecoder(const FfAudioDecoder&) = delete;
    FfAudioDecoder& operator=(const FfAudioDecoder&) = delete;

    bool open(const AVStream* stream, std::string* err = nullptr);
    // Sends `packet` (nullptr = drain) and appends every decoded frame to `out`. False on decode error.
    bool decode(const AVPacket* packet, std::vector<float>& out);
    void flush();  // after a seek

private:
    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFrame* frame_ = nullptr;
};

// Pull-based decoder for one audio file (music/env tracks): demux + decode on demand.
class AudioFileStream {
public:
    static std::unique_ptr<AudioFileStream> open(const std::filesystem::path& path, std::string* err = nullptr);
    ~AudioFileStream();

    // Appends up to `frames` stereo frames to `out`; returns frames appended (0 = end of file).
    std::size_t read(std::vector<float>& out, std::size_t frames);
    bool rewind();
    double duration() const { return duration_; }

private:
    AudioFileStream() = default;
    AVFormatContext* fmt_ = nullptr;
    AVPacket* packet_ = nullptr;
    int stream_ = -1;
    FfAudioDecoder dec_;
    std::vector<float> pending_;
    std::size_t pending_pos_ = 0;
    bool eof_ = false;
    double duration_ = 0;
};

// Whole-file decode (tests, short env loops).
std::optional<Pcm> decode_audio_file(const std::filesystem::path& path, std::string* err = nullptr);

}  // namespace joa::engine
