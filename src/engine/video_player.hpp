#pragma once
// WMV movie playback (movie/enlight.wmv, movie/intro.wmv: ASF, WMV3 video 30 fps + WMA2 stereo).
// Original: WMF 9 synchronous reader, uti3d\video.cpp (Video_open_wmf 0x684650). docs/AUDIO_AND_VIDEO.md.
#include "engine/audio_mixer.hpp"

#include <glm/glm.hpp>

#include <deque>
#include <filesystem>
#include <memory>
#include <string>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace joa::engine {

class IRenderer;
class Platform;

struct VideoFrame {
    int width = 0, height = 0;
    double pts = 0;                  // seconds
    std::vector<std::uint8_t> rgba;  // sRGB RGBA8, top row first
};

class VideoDecoder {
public:
    static std::unique_ptr<VideoDecoder> open(const std::filesystem::path& path, std::string* err = nullptr);
    ~VideoDecoder();

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }
    double duration() const { return duration_; }
    bool has_audio() const { return audio_index_ >= 0; }
    // Decoded soundtrack (stereo float at kOutputRate). Its frames_played() is the playback clock.
    std::shared_ptr<StreamQueue> audio() const { return audio_q_; }

    // Decodes up to the next video frame (pushing interleaved audio packets to audio()).
    // False at end of file.
    bool next(VideoFrame& out);
    // Positions so that next() returns the frame on screen at `seconds` (largest pts <= seconds).
    // Audio before that point is dropped.
    bool seek(double seconds);

private:
    VideoDecoder() = default;
    bool receive(VideoFrame& out);
    bool decode_next(VideoFrame& out);
    std::deque<VideoFrame> ready_;  // frames decoded ahead by seek()
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* video_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    FfAudioDecoder audio_dec_;
    std::shared_ptr<StreamQueue> audio_q_ = std::make_shared<StreamQueue>();
    std::vector<float> audio_buf_;
    int video_index_ = -1, audio_index_ = -1;
    int width_ = 0, height_ = 0;
    double fps_ = 0, duration_ = 0, time_base_ = 0;
    bool eof_ = false, audio_on_ = true;
};

// Largest rectangle of aspect w:h centred in `area` (x, y, w, h pixels): letter/pillarbox.
glm::vec4 letterbox(glm::vec4 area, int w, int h);

struct MovieOptions {
    double start = 0;        // seconds
    std::string screenshot;  // non-empty: render the first frame at `start` to this PNG and return
};

// Plays a movie fullscreen (aspect-correct, black bars) until it ends or Esc / left click
// (one press skips the current movie, like the original). Audio goes to `mixer` on Bus::Movie
// and drives the clock; without a mixer the wall clock does. Returns false if it cannot open/write.
bool play_movie(Platform& platform, IRenderer& renderer, Mixer* mixer, const std::filesystem::path& path,
                const MovieOptions& options = {});

}  // namespace joa::engine
