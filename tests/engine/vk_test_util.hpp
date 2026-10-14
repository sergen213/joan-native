#pragma once
// Shared by the vk_* tests: renders the demo scene's opening view through a real window + backend.
#include "engine/camera.hpp"
#include "engine/demo_scene.hpp"
#include "engine/platform.hpp"
#include "engine/png.hpp"
#include "engine/renderer_gl.hpp"
#include "engine/renderer_vk.hpp"
#include "engine/sim.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vktest {
using namespace joa::engine;

// Called before each frame's begin_frame (resize the window, churn resources, ...).
using FrameHook = std::function<void(int frame, Platform&, IRenderer&)>;

// Demo scene + one additive box (the demo has no additive material), fixed camera, read back after the last submit.
inline Image render_demo(Options o, int frames, const FrameHook& hook = {}) {
    Platform platform(o, true);
    std::unique_ptr<IRenderer> r =
        o.renderer == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
    Image img;
    {
        DemoScene scene(*r);
        const MeshHandle glow = r->create_mesh(make_box_mesh(glm::vec3(0.6f)));
        World world;
        FollowCamera camera;
        camera.smoothing = 0.0f;
        camera.update(world.player_position, 0.0f);
        std::vector<DrawItem> draws;
        std::vector<UiQuad> quads;
        for (int i = 0; i < frames; ++i) {
            if (hook) hook(i, platform, *r);
            const glm::ivec2 size = platform.pixel_size();
            const glm::ivec4 vp = fit_viewport(size, o.aspect);
            scene.build(world, ui_layout(vp), draws, quads);
            Material add;
            add.blend = BlendMode::Additive;
            add.unlit = true;
            add.tint = {1.0f, 0.45f, 0.1f, 0.6f};
            draws.push_back({glow, add, glm::translate(glm::mat4(1.0f), {2.5f, 1.2f, -4.0f})});
            FrameData f;
            f.view.view = camera.view();
            f.view.projection = perspective_hor_plus(glm::radians(kOriginalVerticalFovDeg), float(vp.z) / float(vp.w), 0.5f, 500.0f);
            f.view.camera_position = camera.eye;
            f.view.viewport = vp;
            f.lighting = scene.lighting();
            f.draws = draws;
            f.ui = quads;
            r->begin_frame(size);
            r->submit(f);
            if (i + 1 == frames) img = r->read_back();
            r->end_frame();
        }
        r->destroy_mesh(glow);
    }
    return img;
}

// Vulkan absent (no loader/device): the caller exits 77. Anything else is a real failure.
inline bool vulkan_missing(const std::exception& e) {
    const std::string m = e.what();
    for (const char* s : {"SDL_Vulkan_LoadLibrary", "Vulkan loader unavailable", "Vulkan 1.3 loader required", "no Vulkan device",
                          "no usable Vulkan 1.3 device"})
        if (m.find(s) != std::string::npos) return true;
    return false;
}

inline std::string out_path(const char* name) {
    const char* dir = std::getenv("JOA_TEST_OUT");
    return std::string(dir ? dir : ".") + "/" + name;
}

inline void save(const Image& img, const char* name) {
    if (!img.rgba.empty() && write_png(out_path(name), img.width, img.height, img.rgba))
        std::fprintf(stderr, "wrote %s\n", out_path(name).c_str());
}

}  // namespace vktest
