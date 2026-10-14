// Closed-form en2d widget motion used by the menu slide-ins/outs (menu_saveload.hpp): move-to 0x61e020 /
// 0x61f450 and alpha fade 0x61f1b0.
#include "check.hpp"
#include "engine/menu_saveload.hpp"

using namespace joa::engine;

int main() {
    CHECK_NEAR(ui_slide(0.0f, 0.0f, 600), 0.0f, 1e-4);
    CHECK_NEAR(ui_slide(0.05f, 0.1f, 600), 0.0f, 1e-4);                 // still in its delay
    CHECK_NEAR(ui_slide(0.1f, 0.0f, 600), 3500 * 0.1f - 750 * 0.01f, 1e-2);  // v0 t + a t^2 / 2
    CHECK_NEAR(ui_slide(5.0f, 0.0f, 600), 600.0f, 1e-4);                // clamped at the target
    CHECK_NEAR(ui_slide(ui_slide_time(600), 0.0f, 1e4f), 600.0f, 0.5);  // inverse
    CHECK_NEAR(ui_slide(0.1f, 0.0f, 512, 5120, 0), 512.0f, 1e-3);       // constant speed (backdrop halves)
    CHECK_NEAR(ui_slide_time(64, 320, 0), 0.2f, 1e-5);
    CHECK_NEAR(ui_fade(0.15f, 0.1f, 10), 0.5f, 1e-5);
    CHECK_NEAR(ui_fade(1.0f, 0.0f, 6), 1.0f, 1e-6);
    CHECK_NEAR(ui_fade(0.0f, 0.2f, 6), 0.0f, 1e-6);
    return test_result();
}
