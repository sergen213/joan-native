// Decodes both original movies (frame count, fps, size, soundtrack length) and compares intro.wmv
// frames with the reference captures of the original (1024x768, movie letterboxed to 1024x576).
// Exit 77 (skip) without the original data.
#include "check.hpp"
#include "engine/video_player.hpp"
#include "formats/vfs_fs.hpp"

#include <cstdlib>
#include <optional>

using namespace joa::engine;

namespace {

// Mean absolute RGB difference between a movie frame and the letterboxed area of a 1024x768 capture.
double capture_diff(const VideoFrame& f, const VideoFrame& ref) {
    const glm::vec4 box = letterbox({0, 0, float(ref.width), float(ref.height)}, f.width, f.height);
    double sum = 0;
    std::size_t n = 0;
    for (int y = 0; y < f.height; y += 2)
        for (int x = 0; x < f.width; x += 2) {
            const int rx = int(box.x + (float(x) + 0.5f) * box.z / float(f.width));
            const int ry = int(box.y + (float(y) + 0.5f) * box.w / float(f.height));
            const std::uint8_t* a = &f.rgba[(std::size_t(y) * f.width + x) * 4];
            const std::uint8_t* b = &ref.rgba[(std::size_t(ry) * ref.width + rx) * 4];
            for (int c = 0; c < 3; ++c) sum += std::abs(int(a[c]) - int(b[c]));
            n += 3;
        }
    return sum / double(n);
}

std::optional<VideoFrame> load_image(const std::string& path) {  // PNG through FFmpeg's image demuxer
    auto d = VideoDecoder::open(path);
    VideoFrame f;
    if (!d || !d->next(f)) return std::nullopt;
    return f;
}

}  // namespace

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    const joa::formats::Vfs vfs(dir ? dir : ".");
    if (!vfs.exists("movie/intro.wmv")) {
        std::fprintf(stderr, "SKIP: no original data at %s\n", dir ? dir : ".");
        return 77;
    }

    struct Expect { const char* file; int w, h; double seconds; };
    for (const Expect e : {Expect{"movie/enlight.wmv", 512, 288, 13.73}, Expect{"movie/intro.wmv", 800, 450, 108.5}}) {
        std::string err;
        auto d = VideoDecoder::open(*vfs.resolve(e.file), &err);
        CHECK(d != nullptr);
        if (!d) continue;
        CHECK(d->width() == e.w && d->height() == e.h && d->has_audio());
        CHECK_NEAR(d->fps(), 30.0, 0.01);
        CHECK_NEAR(d->duration(), e.seconds, 0.05);
        VideoFrame f;
        int frames = 0;
        double last = -1;
        bool monotonic = true;
        while (d->next(f)) {
            monotonic = monotonic && f.pts > last;
            last = f.pts;
            ++frames;
        }
        const double audio = double(d->audio()->queued()) / kOutputRate;
        std::printf("%s: %dx%d, %d frames, %.3f fps, last pts %.3f s, audio %.3f s\n", e.file, d->width(), d->height(), frames,
                    d->fps(), last, audio);
        CHECK(monotonic);
        // Variable frame rate: the encoder dropped frames (intro: 3170 coded frames for 108 s at 30 fps).
        CHECK(frames > int(e.seconds * 30.0 * 0.95) && frames <= int(e.seconds * 30.0) + 1);
        CHECK_NEAR(last, e.seconds, 0.6);
        CHECK_NEAR(audio, last, 0.25);
    }

    // Reference captures (work/reference/shots, launcher wall clock): "20s" shows intro t = 2.667 s,
    // "42s" shows intro t = 22.733 s (frames 80 and 682, best match over every frame).
    const std::string shots = std::string(dir ? dir : ".") + "/work/reference/shots/";
    auto ref20 = load_image(shots + "s01_intro_movie_20s.png");
    auto ref42 = load_image(shots + "s01_intro_movie_42s.png");
    if (!ref20 || !ref42) {
        std::fprintf(stderr, "SKIP (reference part): no reference captures in %s\n", shots.c_str());
        return test_result();
    }
    auto d = VideoDecoder::open(*vfs.resolve("movie/intro.wmv"));
    for (const auto& [t, ref] : {std::pair{80.0 / 30.0, &*ref20}, std::pair{682.0 / 30.0, &*ref42}}) {
        VideoFrame f;
        CHECK(d->seek(t) && d->next(f));
        CHECK_NEAR(f.pts, t, 0.02);
        const double diff = capture_diff(f, *ref);
        std::printf("intro t=%.3f s vs reference: mean abs diff %.2f / 255\n", f.pts, diff);
        CHECK(diff < 6.0);
        // A frame one second off must be clearly worse: the comparison is time-sensitive.
        VideoFrame g;
        CHECK(d->seek(t + 1.0) && d->next(g));
        CHECK(capture_diff(g, *ref) > diff + 3.0);
        // Black letterbox bars in the capture, as the native player draws them.
        CHECK(ref->rgba[(10 * 1024 + 512) * 4] < 8 && ref->rgba[(760 * 1024 + 512) * 4] < 8);
    }
    CHECK(letterbox({0, 0, 1024, 768}, 800, 450) == glm::vec4(0, 96, 1024, 576));
    const glm::vec4 wide = letterbox({0, 0, 2560, 1080}, 800, 450);
    CHECK(glm::all(glm::lessThan(glm::abs(wide - glm::vec4(320, 0, 1920, 1080)), glm::vec4(0.01f))));
    return test_result();
}
