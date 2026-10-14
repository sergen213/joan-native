// Renders S1L1 through the real client path (--scenario) and checks the captured images
// (world_shot.hpp): start view (terrain, sky, blue haze), a burning torch, the castle moat water.
// Exit 77 without the original data or a video driver.
#include "check.hpp"
#include "engine/app.hpp"
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
        CHECK(run(o) == 0);
        return o.screenshot;
    };
    check_world_shot(shot("gl_world_render.png", {}, 3));
    check_torch_shot(shot("gl_world_torch.png", {12037, 13600, 250, 90, -8}, 60));
    check_water_shot(shot("gl_world_water.png", {11000, 3200, 1350, 240, -20}, 10));
    return test_result();
}
