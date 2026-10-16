#pragma once
// gl_/vk_dev_overlay tests (JOA_DEV_OVERLAY builds only): renders the demo scene plain, with the Dear ImGui overlay
// and with one full-screen white triangle scissored to a small rect, all through FrameData::ui_vertices / ui_triangles.
// Checks the overlay window pixels, the scissor (position, GL y-flip) and that the rest of the frame is untouched.
#include "check.hpp"
#include "engine/dev_overlay.hpp"
#include "vk_test_util.hpp"

#include <SDL3/SDL_init.h>

#include <algorithm>
#include <utility>

namespace vktest {

enum class Extra { None, Overlay, ClippedTriangle };

inline Image render_overlay_frames(IRenderer& r, Platform& platform, DemoScene& scene, DevOverlay& overlay, Extra extra) {
    World world;
    FollowCamera camera;
    camera.smoothing = 0.0f;
    camera.update(world.player_position, 0.0f);
    std::vector<DrawItem> draws;
    std::vector<UiQuad> quads;
    Image img;
    for (int i = 0; i < 3; ++i) {  // ImGui sizes an auto-resize window on its first frame and shows it from the second
        const glm::ivec2 size = platform.pixel_size();
        const glm::ivec4 vp = fit_viewport(size, 0.0f);
        draws.clear();
        quads.clear();
        scene.build(world, ui_layout(vp), draws, quads);
        FrameData f;
        f.view.view = camera.view();
        f.view.projection = perspective_hor_plus(glm::radians(kOriginalVerticalFovDeg), float(vp.z) / float(vp.w), 0.5f, 500.0f);
        f.view.camera_position = camera.eye;
        f.view.viewport = vp;
        f.lighting = scene.lighting();
        f.draws = draws;
        f.ui = quads;
        const UiVertex tri[3] = {{{0.0f, 0.0f}}, {{2.0f * float(size.x), 0.0f}}, {{0.0f, 2.0f * float(size.y)}}};
        const UiTriangles clipped{0, 3, {100, 200, 200, 300}, {}};
        overlay.visible = extra == Extra::Overlay;
        overlay.frame(DevStats{}, size, 1.0f / 60.0f, f);
        if (extra == Extra::ClippedTriangle) f.ui_vertices = tri, f.ui_triangles = {&clipped, 1};
        r.begin_frame(size);
        r.submit(f);
        if (i == 2) img = r.read_back();
        r.end_frame();
    }
    return img;
}

inline int run_dev_overlay_test(RendererKind kind, const char* png) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    Options o;
    o.renderer = kind;
    o.width = 800;
    o.height = 450;
    o.vk_validation = kind == RendererKind::Vulkan;
    Image plain, shown, clipped;
    try {
        Platform platform(o, true);
        std::unique_ptr<IRenderer> r = kind == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
        DemoScene scene(*r);
        DevOverlay overlay(*r, GraphicsSettings{});
        plain = render_overlay_frames(*r, platform, scene, overlay, Extra::None);
        shown = render_overlay_frames(*r, platform, scene, overlay, Extra::Overlay);
        clipped = render_overlay_frames(*r, platform, scene, overlay, Extra::ClippedTriangle);
    } catch (const std::exception& e) {
        if (vulkan_missing(e)) {
            std::fprintf(stderr, "SKIP: %s\n", e.what());
            return 77;
        }
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    save(shown, png);
    CHECK(plain.width == 800 && plain.height == 450 && shown.width == 800 && clipped.width == 800);
    if (g_failures) return test_result();
    auto at = [](const Image& img, int x, int y) { return img.rgba.data() + (std::size_t(y) * img.width + x) * 4; };
    auto same = [&](const Image& a, const Image& b, int x, int y) { return std::equal(at(a, x, y), at(a, x, y) + 4, at(b, x, y)); };

    // Overlay: Dark-style WindowBg (0.18 grey = 46) at SetNextWindowBgAlpha 0.85, blended in gamma space like the rest
    // of the UI; white text inside; everything right of / below the window untouched.
    const unsigned char* scene = at(plain, 300, 250);
    const unsigned char* body = at(shown, 300, 250);
    std::fprintf(stderr, "scene %d,%d,%d  overlay body %d,%d,%d\n", scene[0], scene[1], scene[2], body[0], body[1], body[2]);
    for (int c = 0; c < 3; ++c) CHECK_NEAR(body[c], 0.85 * 46.0 + 0.15 * scene[c], 3.0);
    int text = 0, changed = 0;
    for (int y = 10; y < 200; ++y)
        for (int x = 10; x < 300; ++x) {
            const unsigned char* p = at(shown, x, y);
            text += p[0] > 200 && p[1] > 200 && p[2] > 200;
            changed += !same(plain, shown, x, y);
        }
    std::fprintf(stderr, "overlay: %d text pixels, %d changed pixels in the top-left 290x190\n", text, changed);
    CHECK(text > 200);
    CHECK(changed > 20000);
    for (int y = 300; y < 450; y += 7)
        for (int x = 500; x < 800; x += 7) CHECK(same(plain, shown, x, y));

    // Scissor: the full-screen white triangle only lands in x 100..199, y 200..299 (top-left origin).
    const unsigned char* in = at(clipped, 150, 250);
    CHECK(in[0] == 255 && in[1] == 255 && in[2] == 255);
    CHECK(at(clipped, 100, 200)[0] == 255 && at(clipped, 199, 299)[0] == 255);
    for (auto [x, y] : {std::pair{99, 250}, {200, 250}, {150, 199}, {150, 300}, {150, 100}, {150, 350}, {400, 225}})
        CHECK(same(plain, clipped, x, y));

    if (kind == RendererKind::Vulkan) {
        std::fprintf(stderr, "validation: %s, errors %u\n", vk_validation_active() ? "active" : "NOT LOADED", vk_validation_errors());
        if (std::getenv("JOA_REQUIRE_VALIDATION")) CHECK(vk_validation_active());
        CHECK(vk_validation_errors() == 0);
    }
    return test_result();
}

}  // namespace vktest
