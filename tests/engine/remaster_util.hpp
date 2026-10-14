#pragma once
// Shared by gl_remaster / vk_remaster: image statistics and S1L1 captures through the real client path.
#include "check.hpp"
#include "engine/app.hpp"
#include "formats/vfs_fs.hpp"
#include "png_read.hpp"
#include "vk_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>

namespace rmtest {
using namespace vktest;

struct Diff {
    double mad = 0, psnr = 99;
    std::size_t outliers = 0;  // pixels with a channel off by > 32
};

inline Diff diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    Diff d;
    if (a.size() != b.size() || a.empty()) return {255, 0, a.size()};
    double sum = 0, sq = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const int x = std::abs(int(a[i + c]) - int(b[i + c]));
            sum += x;
            sq += double(x) * x;
            worst = std::max(worst, x);
        }
        d.outliers += worst > 32;
    }
    const double n = double(a.size() / 4 * 3);
    d.mad = sum / n;
    d.psnr = sq > 0 ? 10.0 * std::log10(255.0 * 255.0 / (sq / n)) : 99.0;
    return d;
}

inline double mean_luma(const std::vector<std::uint8_t>& rgba) {
    double s = 0;
    for (std::size_t i = 0; i < rgba.size(); i += 4) s += 0.299 * rgba[i] + 0.587 * rgba[i + 1] + 0.114 * rgba[i + 2];
    return s / double(std::max<std::size_t>(rgba.size() / 4, 1));
}

inline std::size_t distinct_colours(const std::vector<std::uint8_t>& rgba) {
    std::set<std::uint32_t> c;
    for (std::size_t i = 0; i < rgba.size(); i += 16) c.insert(std::uint32_t(rgba[i]) << 16 | rgba[i + 1] << 8 | rgba[i + 2]);
    return c.size();
}

inline Options demo_options(RendererKind kind, GraphicsPreset preset) {
    Options o;
    o.width = 640;
    o.height = 360;
    o.renderer = kind;
    o.vk_validation = kind == RendererKind::Vulkan;
    o.graphics.preset = preset;
    return o;
}

inline const char* game_dir() {
    const char* game = std::getenv("JOA_GAME_DIR");
    return game && joa::formats::Vfs(game).exists("map/scenario1/S1L1.gri") ? game : nullptr;
}

// S1L1 start view through joan-native's own path (run()), read back as RGBA8.
inline std::vector<std::uint8_t> s1l1(RendererKind kind, const char* preset, const char* name, int frames = 8) {
    Options o;
    const std::string shot = out_path(name);
    std::vector<std::string> args = {"joan-native", "--game-dir", game_dir(), "--scenario", "S1L1", "--width", "640", "--height", "360",
                                     "--screenshot", shot, "--frames", std::to_string(frames)};
    if (kind == RendererKind::Vulkan) args.insert(args.end(), {"--renderer", "vulkan", "--vk-validation"});
    if (preset) args.insert(args.end(), {"--preset", preset});
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    CHECK(parse_options(int(argv.size()), argv.data(), o).empty());
    CHECK(run(o) == 0);
    Png img;
    if (!read_png(shot, img)) return {};
    return std::vector<std::uint8_t>(img.rgba.begin(), img.rgba.end());
}

}  // namespace rmtest
