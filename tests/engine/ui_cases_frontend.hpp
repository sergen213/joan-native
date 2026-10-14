#pragma once
// Main menu / difficulty / credits cases (menu_main.cpp, menu_credits.cpp) vs the original's screenshots.
#include "ui_shot.hpp"

#include <vector>

inline const std::vector<uishot::ShotCase> kFrontEndCases = {
    // Opening sequence finished; the reference cursor sits at the top-left corner (masked out).
    {"main", "s06_main_menu.png", {-1, -1}, {{40, 0, 984, 40}, {0, 40, 1024, 728}}, 0.98},
    // EASY hovered (M_OV + M_LA/M_RA marks, white text); gauntlet cursor at the mouse in both images.
    {"difficulty", "s06_difficulty.png", {268, 262}, {}, 0.98},
    // Credits at t = 42.28 s (the reference file name is desynced; content = credits, bg3, static).
    {"credits", "s04_02_main_menu_again.png", {-1, -1}, {}, 0.98},
};
