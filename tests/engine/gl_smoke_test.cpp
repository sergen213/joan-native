// Renders the demo scene through the real client path (window, GL backend, PNG writer) and checks
// the captured pixels. Run by CTest under a private xvfb-run server (or SDL's offscreen driver).
#include "check.hpp"
#include "engine/app.hpp"
#include "png_read.hpp"

#include <SDL3/SDL_init.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace joa::engine;

static bool capture(const std::string& path, float aspect, Png& png) {
    Options o;
    o.width = 640;
    o.height = 360;
    o.aspect = aspect;
    o.screenshot = path;
    o.frames = 5;
    return run(o) == 0 && read_png(path, png);
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    const char* out_dir = std::getenv("JOA_TEST_OUT");
    const std::string dir = out_dir ? out_dir : ".";

    Png img;
    CHECK(capture(dir + "/gl_smoke.png", 0.0f, img));
    CHECK(img.w == 640 && img.h == 360 && img.rgba.size() == 640u * 360 * 4);
    if (g_failures) return test_result();

    std::set<std::uint32_t> colors;
    for (std::size_t i = 0; i < img.rgba.size(); i += 4)
        colors.insert(std::uint32_t(img.rgba[i]) << 16 | img.rgba[i + 1] << 8 | img.rgba[i + 2]);
    std::fprintf(stderr, "distinct colours: %zu\n", colors.size());
    CHECK(colors.size() > 100);  // lit, textured, fogged geometry, not a clear colour

    const unsigned char* sky = img.at(320, 5);
    CHECK(sky[2] > sky[0] && sky[2] > 150);  // fog/sky colour, sRGB-encoded (linear 0.75 -> ~225)
    const unsigned char* ground = img.at(320, 350);
    CHECK(ground[1] > ground[2]);  // green checker ground in front of the camera
    const unsigned char* marker = img.at(7, 7);  // orange screen-corner UI marker
    CHECK(marker[0] > 200 && marker[0] > marker[2] + 100);
    for (std::size_t i = 3; i < img.rgba.size(); i += 4) CHECK(img.rgba[i] == 255);

    // Forced 4:3 on a 16:9 window: pillarboxed, scene in the centre 480 px.
    Png boxed;
    CHECK(capture(dir + "/gl_smoke_4x3.png", 4.0f / 3.0f, boxed));
    if (g_failures) return test_result();
    const unsigned char* bar = boxed.at(40, 180);
    CHECK(bar[0] == 0 && bar[1] == 0 && bar[2] == 0);
    const unsigned char* inside = boxed.at(320, 5);
    CHECK(inside[2] > 150);
    return test_result();
}
