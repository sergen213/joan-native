// Credits (CREDITS set). RE: start FUN_006b56b0, tick FUN_006b5900, draw FUN_006b5e60 (state 0xf, entered from the
// main menu via FUN_004d7e70(0xf); leaves with FUN_004d7e70(3) = main menu). Notes: work/ui/notes_MenuFrontEnd.md.
#include "engine/ui_screens.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace joa::engine {

void menu_credits(Ui& ui, MenuModel& m, std::vector<UiCommand>&) {
    const float t = m.time;
    // Background stages (fade widget +0x32, alpha +0x43): stage 0 = bg1 fades in from black at 4/s together with the
    // border; stage k = 1..3 shows bgk while bg(k+1) waits 13000 ms then fades in at 0.33099/s (0x3ea978d5); then the
    // stage advances. Stage 4 = bg4 until the last strip stops.
    constexpr float kIn = 0.25f, kPeriod = 13.0f + 1.0f / 0.33099f;
    int stage = 0;
    float a = std::min(t * 4.0f, 1.0f);  // alpha of the incoming background
    if (t >= kIn) {
        stage = std::min(1 + int((t - kIn) / kPeriod), 4);
        a = stage < 4 ? std::clamp((t - kIn - float(stage - 1) * kPeriod - 13.0f) * 0.33099f, 0.0f, 1.0f) : 0.0f;
    }
    const auto bg = [](int k, const char* suffix = "") { return "bg" + std::to_string(k) + suffix; };

    ui.fill({0, 0, 1024, 768}, {0, 0, 0, 1});
    if (stage == 0) ui.sprite("CREDITS", "bg1", {0, 0}, {1, 1, 1, a});
    else ui.sprite("CREDITS", bg(stage), {0, 0});
    if (stage >= 1 && stage < 4) ui.sprite("CREDITS", bg(stage + 1), {0, 0}, {1, 1, 1, a});

    // Text strips t1..t5 (510 wide, x = (1024-510)/2) stacked from y = 768, all moving up at 60 px/s
    // (FUN_0061e020(.., 60.0)) until each one's bottom reaches y = 136 (0x88); drawn only while moving. They only
    // tick from stage 1 on (FUN_006b5900), so the scroll starts after the 0.25 s fade-in.
    float y0 = 768.0f;
    bool scrolling = false;
    for (const char* name : {"t1", "t2", "t3", "t4", "t5"}) {
        const UiSprite* s = ui.sprite("CREDITS", name);
        if (!s) continue;
        const float y = std::floor(y0 - 60.0f * std::max(t - kIn, 0.0f)), stop = 136.0f - s->size.y;
        if (y > stop) ui.sprite(s, {std::floor((1024.0f - s->size.x) / 2.0f), y}), scrolling = true;
        y0 += s->size.y;
    }

    // Foreground half of the background (top/bottom bands hiding the strips). The original blends bgk_mask into
    // bg(k+1)_mask with a second texture stage; ponytail: two alpha draws, exact at both ends of the cross-fade.
    if (stage >= 1) ui.sprite("CREDITS", bg(stage, "_mask"), {0, 0}, {1, 1, 1, stage < 4 ? 1.0f - a : 1.0f});
    if (stage >= 1 && stage < 4) ui.sprite("CREDITS", bg(stage + 1, "_mask"), {0, 0}, {1, 1, 1, a});

    const glm::vec4 c{1, 1, 1, stage == 0 ? a : 1.0f};
    ui.sprite("CREDITS", "border", {0, 0}, c);
    ui.sprite("CREDITS", "logo", {372, 24}, c);
    ui.sprite("CREDITS", "bar", {316, 112}, c);
    if (const UiFont* f = ui.font("goudy_bold_14"))  // centred in (316,112)-(707,135), FUN_00626130
        ui.text(f, ui.text("CREDITS", 1), {float(316 + (392 - f->metrics.measure(ui.text("CREDITS", 1))) / 2),
                                            float(112 + (24 - f->metrics.line_height()) / 2)},
                c, TextAlign::Left, 1.0f, TextOp::Diffuse);

    // Ends (FUN_006b5900 stage 4 -> 6) when the last strip stops; Escape or a click leaves early.
    if ((t > kIn && !scrolling) || ui.key(SDL_SCANCODE_ESCAPE) || ui.input().mouse_released) {
        m.screen = MenuScreen::Main;
        m.main_entry = 0;  // the main menu is rebuilt: full opening sequence
    }
}

}  // namespace joa::engine
