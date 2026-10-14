// Renders S1L1 through the real client path (--scenario) and checks the captured image:
// not uniform, and the terrain fills the lower part of the frame (not the clear colour).
// Exit 77 without the original data or a video driver.
#include "check.hpp"
#include "engine/app.hpp"
#include "formats/vfs_fs.hpp"
#include "png_read.hpp"

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
    const std::string path = std::string(out_dir ? out_dir : ".") + "/gl_world_render.png";

    Options o;
    o.width = 640;
    o.height = 360;
    o.game_dir = game;
    o.scenario = "S1L1";
    o.screenshot = path;
    o.frames = 3;
    CHECK(run(o) == 0);
    Png img;
    CHECK(read_png(path, img));
    CHECK(img.w == 640 && img.h == 360);
    if (g_failures) return test_result();

    // Many distinct colours overall.
    std::set<std::uint32_t> colours;
    for (int y = 0; y < img.h; y += 4)
        for (int x = 0; x < img.w; x += 4) {
            const unsigned char* p = img.at(x, y);
            colours.insert(std::uint32_t(p[0]) << 16 | p[1] << 8 | p[2]);
        }
    std::fprintf(stderr, "distinct colours (4px grid): %zu\n", colours.size());
    CHECK(colours.size() > 500);

    // Terrain: the bottom quarter is textured ground, i.e. mostly differs from the top-left pixel
    // (sky/clear) and varies from pixel to pixel.
    const unsigned char* sky = img.at(2, 2);
    int differ = 0, total = 0;
    std::set<std::uint32_t> ground;
    for (int y = img.h * 3 / 4; y < img.h; y += 2)
        for (int x = 0; x < img.w; x += 2, ++total) {
            const unsigned char* p = img.at(x, y);
            differ += std::abs(p[0] - sky[0]) + std::abs(p[1] - sky[1]) + std::abs(p[2] - sky[2]) > 12;
            ground.insert(std::uint32_t(p[0]) << 16 | p[1] << 8 | p[2]);
        }
    std::fprintf(stderr, "bottom quarter: %d/%d differ from sky, %zu colours\n", differ, total, ground.size());
    CHECK(differ * 10 > total * 8);
    CHECK(ground.size() > 200);
    return test_result();
}
