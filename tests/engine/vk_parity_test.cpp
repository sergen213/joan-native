// Renders the same demo frame (all material modes: opaque, alpha test + two-sided, alpha blend + unlit,
// additive, UI quads, fog, sun light) with the OpenGL and Vulkan backends at 640x360 and compares them.
// Thresholds: PSNR >= 35 dB and mean absolute difference <= 1.0 (8-bit units, RGB). RX 7900 XTX
// radeonsi vs RADV measured 60.6 dB / 0.005 (5 pixels off by >32, polygon edges). Rasterisation
// and mip/filter rounding differ slightly between drivers; gross errors score far lower (a y-flipped
// frame: 15.9 dB, a gamma-2.2 error: 13.6 dB). Exit 77 when no Vulkan 1.3 device or no GL context is available.
#include "check.hpp"
#include "vk_test_util.hpp"

#include <SDL3/SDL_init.h>

#include <cmath>
#include <cstdlib>

using namespace vktest;

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();

    Options o;
    o.width = 640;
    o.height = 360;
    Image gl, vk;
    try {
        gl = render_demo(o, 3);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "SKIP: OpenGL unavailable (%s)\n", e.what());
        return 77;
    }
    o.renderer = RendererKind::Vulkan;
    o.vk_validation = true;
    try {
        vk = render_demo(o, 3);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", vulkan_missing(e) ? "SKIP" : "FAIL", e.what());
        return vulkan_missing(e) ? 77 : 1;
    }
    save(gl, "vk_parity_gl.png");
    save(vk, "vk_parity_vk.png");
    CHECK(gl.width == vk.width && gl.height == vk.height && gl.rgba.size() == vk.rgba.size() && !gl.rgba.empty());
    if (g_failures) return test_result();

    double sum = 0, sq = 0;
    std::size_t outliers = 0;
    Image diff{gl.width, gl.height, std::vector<std::uint8_t>(gl.rgba.size(), 255)};
    for (std::size_t i = 0; i < gl.rgba.size(); i += 4) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(int(gl.rgba[i + c]) - int(vk.rgba[i + c]));
            sum += d;
            sq += double(d) * d;
            worst = std::max(worst, d);
            diff.rgba[i + c] = std::uint8_t(std::min(255, d * 8));
        }
        outliers += worst > 32;
    }
    save(diff, "vk_parity_diff.png");
    const double n = double(gl.rgba.size() / 4 * 3);
    const double mad = sum / n, mse = sq / n;
    const double psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
    std::fprintf(stderr, "parity GL vs Vulkan: mean abs diff %.3f, PSNR %.2f dB, pixels off by >32: %zu (%.3f%%)\n", mad, psnr,
                 outliers, 100.0 * double(outliers) / double(gl.rgba.size() / 4));
    CHECK(psnr >= 35.0);
    CHECK(mad <= 1.0);
    CHECK(vk_validation_errors() == 0);
    if (std::getenv("JOA_REQUIRE_VALIDATION")) CHECK(vk_validation_active());
    return test_result();
}
