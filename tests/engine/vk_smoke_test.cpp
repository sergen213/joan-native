// Renders the demo scene with the Vulkan backend under SDL's offscreen driver (surfaceless) with the Khronos
// validation layer on, while resizing the window and churning meshes/textures between frames. Checks the
// read-back pixels and that validation reported zero errors. Exit 77 when no Vulkan 1.3 device exists.
#include "check.hpp"
#include "vk_test_util.hpp"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <set>

using namespace vktest;

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();

    Options o;
    o.renderer = RendererKind::Vulkan;
    o.width = 800;
    o.height = 450;
    o.vk_validation = true;
    o.gpu_timing = true;
    Image img;
    try {
        img = render_demo(o, 8, [](int frame, Platform& p, IRenderer& r) {
            if (frame == 3) SDL_SetWindowSize(p.window(), 640, 360);  // target (+ swapchain if any) recreation
            // Resource churn while frames are in flight: deferred destruction and bindless slot reuse.
            const TextureHandle t = r.create_texture(make_checker_texture(32, 4, {255, 0, 0, 255}, {0, 0, 255, 255}));
            const MeshHandle m = r.create_mesh(make_box_mesh(glm::vec3(1.0f)));
            CHECK(t && m && r.update_mesh(m, make_ground_mesh(1.0f, 1.0f)));
            r.destroy_texture(t);
            r.destroy_mesh(m);
        });
    } catch (const std::exception& e) {
        if (vulkan_missing(e)) {
            std::fprintf(stderr, "SKIP: %s\n", e.what());
            return 77;
        }
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    save(img, "vk_smoke.png");
    std::fprintf(stderr, "validation: %s, errors %u\n", vk_validation_active() ? "active" : "NOT LOADED", vk_validation_errors());
    if (std::getenv("JOA_REQUIRE_VALIDATION")) CHECK(vk_validation_active());
    CHECK(vk_validation_errors() == 0);

    CHECK(img.width == 640 && img.height == 360 && img.rgba.size() == 640u * 360 * 4);
    if (g_failures) return test_result();
    std::set<std::uint32_t> colors;
    for (std::size_t i = 0; i < img.rgba.size(); i += 4)
        colors.insert(std::uint32_t(img.rgba[i]) << 16 | img.rgba[i + 1] << 8 | img.rgba[i + 2]);
    std::fprintf(stderr, "distinct colours: %zu\n", colors.size());
    CHECK(colors.size() > 100);  // lit, textured, fogged geometry, not a clear colour
    auto at = [&](int x, int y) { return img.rgba.data() + (std::size_t(y) * img.width + x) * 4; };
    const unsigned char* sky = at(320, 5);
    CHECK(sky[2] > sky[0] && sky[2] > 150);  // sRGB-encoded fog colour at the top: image is upright
    const unsigned char* ground = at(320, 350);
    CHECK(ground[1] > ground[2]);  // green checker ground at the bottom
    const unsigned char* marker = at(7, 7);  // orange UI marker in the top-left corner
    CHECK(marker[0] > 200 && marker[0] > marker[2] + 100);
    for (std::size_t i = 3; i < img.rgba.size(); i += 4) CHECK(img.rgba[i] == 255);
    return test_result();
}
