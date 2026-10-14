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

// Sharpness: variance of the 4-neighbour Laplacian of the luma (interior pixels of a w-wide RGBA8 image).
inline double laplacian_variance(const std::vector<std::uint8_t>& rgba, int w) {
    const int h = int(rgba.size() / 4) / std::max(w, 1);
    const auto y = [&](int x, int yy) {
        const std::size_t i = (std::size_t(yy) * std::size_t(w) + std::size_t(x)) * 4;
        return 0.299 * rgba[i] + 0.587 * rgba[i + 1] + 0.114 * rgba[i + 2];
    };
    double s = 0, sq = 0, n = 0;
    for (int yy = 1; yy + 1 < h; ++yy)
        for (int x = 1; x + 1 < w; ++x) {
            const double l = y(x - 1, yy) + y(x + 1, yy) + y(x, yy - 1) + y(x, yy + 1) - 4.0 * y(x, yy);
            s += l, sq += l * l, n += 1;
        }
    return n > 0 ? sq / n - (s / n) * (s / n) : 0.0;
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

// Hair stand-in: ONE draw of blend state 1 (AlphaBlend + depth_write, as Joan's head/hair submeshes) holding, in
// index order, a near red quad, a far green quad behind it, then a nearer blue strip at vertex alpha 0.25. Like a
// ponytail whose far-side layers come later in the index buffer. Returns the frame and the pixels to check.
struct LayerShot {
    Image img;
    glm::ivec2 centre, strip;  // red quad over the green one; blue alpha-0.25 strip over red
};
inline LayerShot render_layers(RendererKind kind, GraphicsPreset preset) {
    Options o = demo_options(kind, preset);
    for (const char* k : {"raytracing.reflections", "raytracing.shadows", "raytracing.gi"}) o.graphics.apply(k, "false");
    Platform platform(o, true);
    std::unique_ptr<IRenderer> r = kind == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
    if (preset != GraphicsPreset::Original) r->set_graphics(o.graphics.resolve());
    MeshData m;
    const auto quad = [&](float x0, float x1, float y0, float y1, float z, glm::vec4 c) {
        const auto base = std::uint32_t(m.vertices.size());
        for (glm::vec2 p : {glm::vec2(x0, y0), glm::vec2(x1, y0), glm::vec2(x1, y1), glm::vec2(x0, y1)}) {
            Vertex v;
            v.position = {p, z};
            v.normal = {0, 0, 1};
            v.color = c;
            m.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    };
    quad(-1, 1, -1, 1, -4, {1, 0, 0, 1});            // near layer
    quad(-3, 3, -3, 3, -8, {0, 1, 0, 1});            // far layer, drawn later
    quad(-0.5f, 0, -0.25f, 0.25f, -3, {0, 0, 1, 0.25f});  // thin strand fringe below the alpha reference
    const MeshHandle mesh = r->create_mesh(m);
    Material mat;
    mat.blend = BlendMode::AlphaBlend;
    mat.depth_write = true;
    mat.unlit = true;
    mat.fog = false;
    mat.two_sided = true;
    FrameData f;
    f.view.projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.5f, 100.0f);
    f.view.viewport = {0, 0, platform.pixel_size()};
    const DrawItem draw{mesh, mat, glm::mat4(1.0f)};
    f.draws = {&draw, 1};
    LayerShot s;
    for (int i = 0; i < 4; ++i) {
        r->begin_frame(platform.pixel_size());
        r->submit(f);
        if (i == 3) s.img = r->read_back();
        r->end_frame();
    }
    r->destroy_mesh(mesh);
    const auto px = [&](glm::vec3 p) {
        const glm::vec4 c = f.view.projection * glm::vec4(p, 1.0f);
        return glm::ivec2(int((c.x / c.w * 0.5f + 0.5f) * float(s.img.width)), int((0.5f - c.y / c.w * 0.5f) * float(s.img.height)));
    };
    s.centre = px({0.4f, 0.4f, -4});
    s.strip = px({-0.25f, 0, -3});
    return s;
}

// Near layer wins over the later far layer (no see-through) and the alpha-0.25 fringe leaves no blue halo.
inline void check_layers(const LayerShot& s, const char* what) {
    const auto at = [&](glm::ivec2 p) {
        const std::size_t i = (std::size_t(p.y) * std::size_t(s.img.width) + std::size_t(p.x)) * 4;
        return glm::ivec3(s.img.rgba[i], s.img.rgba[i + 1], s.img.rgba[i + 2]);
    };
    const glm::ivec3 c = at(s.centre), b = at(s.strip);
    std::fprintf(stderr, "%s layers: centre %d,%d,%d strip %d,%d,%d\n", what, c.r, c.g, c.b, b.r, b.g, b.b);
    CHECK(c.r > 150 && c.g < 40 && c.b < 40);
    CHECK(std::abs(b.r - c.r) < 8 && std::abs(b.g - c.g) < 8 && std::abs(b.b - c.b) < 8);
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
