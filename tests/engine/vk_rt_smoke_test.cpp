// S1L1 through joan-native's own path (run()), Ultra, validation on (docs/RAYTRACING.md): all three ray-traced
// effects together render validation clean; each effect alone changes the image where expected relative to
// the Ultra raster frame: reflections on Joan's armour (start view, right half), contact shadows on units and
// grass (start view, lower half; under the courtyard houses RT agrees with the shadow map), GI in the covered
// alley under the castle walkway (wall view, left half).
// Exit 77 without game data, Vulkan 1.3 or a ray-query device.
#include "remaster_util.hpp"

#include <SDL3/SDL_init.h>

using namespace rmtest;

namespace {

std::vector<std::uint8_t> shot(const char* name, const char* effects, const char* camera, bool validation) {
    const std::string out = out_path(name);
    std::vector<std::string> args = {"joan-native", "--game-dir", game_dir(), "--scenario", "S1L1", "--width", "640", "--height", "360",
                                     "--renderer", "vulkan", "--preset", "ultra", "--screenshot", out, "--frames", "24"};
    if (validation) args.push_back("--vk-validation");
    if (camera) args.insert(args.end(), {"--camera", camera});
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    Options o;
    CHECK(parse_options(int(argv.size()), argv.data(), o).empty());
    for (const char* k : {"reflections", "shadows", "gi"})
        o.graphics.apply(std::string("raytracing.") + k, std::string(effects).find(k) != std::string::npos ? "true" : "false");
    // Fixed exposure: auto exposure rescales the whole frame per effect set and would mask the local differences.
    CHECK(o.graphics.apply("graphics.auto_exposure", "false").empty());
    CHECK(run(o) == 0);
    Png img;
    if (!read_png(out, img)) return {};
    return {img.rgba.begin(), img.rgba.end()};
}

// Mean absolute RGB difference inside [x0,x1) x [y0,y1) (fractions of 640x360).
double region(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, double x0, double y0, double x1, double y1) {
    if (a.size() != b.size() || a.size() != 640u * 360u * 4u) return 0.0;
    double s = 0;
    std::size_t n = 0;
    for (int y = int(y0 * 360); y < int(y1 * 360); ++y)
        for (int x = int(x0 * 640); x < int(x1 * 640); ++x, ++n)
            for (int c = 0; c < 3; ++c) s += std::abs(int(a[(std::size_t(y) * 640 + x) * 4 + c]) - int(b[(std::size_t(y) * 640 + x) * 4 + c]));
    return n ? s / double(n * 3) : 0.0;
}

// Mean RGB inside the same fractional rectangle.
double mean(const std::vector<std::uint8_t>& a, double x0, double y0, double x1, double y1) {
    if (a.size() != 640u * 360u * 4u) return 0.0;
    double s = 0;
    std::size_t n = 0;
    for (int y = int(y0 * 360); y < int(y1 * 360); ++y)
        for (int x = int(x0 * 640); x < int(x1 * 640); ++x, ++n)
            for (int c = 0; c < 3; ++c) s += a[(std::size_t(y) * 640 + x) * 4 + c];
    return n ? s / double(n * 3) : 0.0;
}

}  // namespace

int main() {
    if (!game_dir()) {
        std::fputs("SKIP: no game data (JOA_GAME_DIR)\n", stderr);
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    std::vector<std::uint8_t> all;
    try {
        all = shot("vk_rt_s1l1_all.png", "reflections,shadows,gi", nullptr, true);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", vulkan_missing(e) ? "SKIP" : "FAIL", e.what());
        return vulkan_missing(e) ? 77 : 1;
    }
    if (!vk_ray_tracing_available()) {
        std::fputs("SKIP: no ray-query device\n", stderr);
        return 77;
    }
    CHECK(vk_validation_errors() == 0);
    if (std::getenv("JOA_REQUIRE_VALIDATION")) CHECK(vk_validation_active());
    const auto raster = shot("vk_rt_s1l1_raster.png", "", nullptr, false);
    CHECK(distinct_colours(all) > 1000);
    std::fprintf(stderr, "S1L1 mean luma raster %.1f, all RT %.1f\n", mean_luma(raster), mean_luma(all));
    CHECK(std::fabs(mean_luma(all) - mean_luma(raster)) < 0.15 * mean_luma(raster));

    // Reflections: Joan's armour (right half below the sky) vs the tree line / sky (top-left).
    const auto refl = shot("vk_rt_s1l1_reflections.png", "reflections", nullptr, false);
    const double armour = region(raster, refl, 0.6, 0.2, 0.9, 0.95), trees = region(raster, refl, 0.0, 0.0, 0.4, 0.4);
    std::fprintf(stderr, "reflections: armour %.3f, trees/sky %.3f\n", armour, trees);
    CHECK(armour > 0.3 && armour > 4.0 * trees);

    // Shadows: contact/self shadows on the units and the grass (start view, lower half); the sky does not change.
    // Under the courtyard houses (`--camera 8600,1000,1500,270,-20`) RT and the 4096 shadow map agree within
    // MAD 0.04, so that view only proves agreement (logged), not change.
    const auto shadow = shot("vk_rt_s1l1_shadows.png", "shadows", nullptr, false);
    const double contact = region(raster, shadow, 0.3, 0.55, 1.0, 1.0), sky2 = region(raster, shadow, 0.0, 0.0, 0.4, 0.3);
    std::fprintf(stderr, "shadows: units/grass %.3f, trees/sky %.3f\n", contact, sky2);
    CHECK(contact > 0.3 && contact > 4.0 * sky2);
    const char* yard = "8600,1000,1500,270,-20";
    const double under_houses = region(shot("vk_rt_s1l1_yard_raster.png", "", yard, false), shot("vk_rt_s1l1_yard_shadows.png", "shadows", yard, false),
                                       0.0, 0.35, 1.0, 1.0);
    std::fprintf(stderr, "shadows: courtyard RT vs shadow map %.3f\n", under_houses);
    CHECK(under_houses < 2.0);  // same shadows, different penumbrae

    // GI: the alley under the castle walkway (left half) darkens / bounces; the sky (top-left corner) does not change.
    const char* wall = "8600,1000,1500,0,-20";
    const auto wall_raster = shot("vk_rt_s1l1_wall_raster.png", "", wall, false);
    const auto wall_gi = shot("vk_rt_s1l1_wall_gi.png", "gi", wall, false);
    const double alley = region(wall_raster, wall_gi, 0.05, 0.2, 0.5, 1.0), sky3 = region(wall_raster, wall_gi, 0.0, 0.0, 0.03, 0.15);
    std::fprintf(stderr, "gi: covered alley %.3f, sky %.3f\n", alley, sky3);
    CHECK(alley > 0.3 && alley > 4.0 * sky3);
    return test_result();
}
