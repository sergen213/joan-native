// Vulkan twin of gl_world_render: renders S1L1 through the real client path with validation and
// checks the captured images (world_shot.hpp): start view, burning torch, castle moat water.
// Exit 77 without the original data or a video driver.
#include "check.hpp"
#include "engine/app.hpp"
#include "vk_test_util.hpp"
#include "formats/vfs_fs.hpp"
#include "world_shot.hpp"

#include <SDL3/SDL_init.h>

#include <cstdlib>
#include <set>
#include <string>

using namespace joa::engine;

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("map/scenario1/S1L1.gri")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    const char* out_dir = std::getenv("JOA_TEST_OUT");
    const auto shot = [&](const char* name, std::vector<float> pose, int frames) {
        Options o;
        o.width = 640;
        o.height = 360;
        o.game_dir = game;
        o.scenario = "S1L1";
        o.screenshot = std::string(out_dir ? out_dir : ".") + "/" + name;
        o.frames = frames;
        o.camera_pose = std::move(pose);
        o.renderer = RendererKind::Vulkan;
        o.vk_validation = true;
        CHECK(run(o) == 0);
        return o.screenshot;
    };
    try {
        check_world_shot(shot("vk_world_render.png", {}, 3));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", vktest::vulkan_missing(e) ? "SKIP" : "FAIL", e.what());
        return vktest::vulkan_missing(e) ? 77 : 1;
    }
    check_torch_shot(shot("vk_world_torch.png", {12037, 13600, 250, 90, -8}, 60));
    check_water_shot(shot("vk_world_water.png", {11000, 3200, 1350, 240, -20}, 10));
    return test_result();
}
