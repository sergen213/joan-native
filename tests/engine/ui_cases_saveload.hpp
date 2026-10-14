#pragma once
// Load / Save / in-game menu cases (menu_saveload.cpp, menu_ingame.cpp) vs the original's screenshots.
#include "ui_shot.hpp"

#include <vector>

inline const std::vector<uishot::ShotCase> kSaveLoadCases = {
    // Load from the main menu; cursor (gauntlet) over slot 1 masked out, slot 1 hover reproduced.
    {"load", "s04_03_load_campaign.png", {262, 320}, {{0, 0, 1024, 330}, {0, 330, 240, 438}, {300, 330, 724, 108}, {0, 438, 1024, 330}}, 0.955},  // 4:3 0.997, 21:9 0.860
    {"save", "s04_07_save_menu.png", {-1, -1}, {}, 0.96},  // 4:3 0.999, 21:9 0.867
    {"ingame", "s04_07_ingame_menu.png", {510, 486}, {}, 0.99},  // 4:3 0.999, 21:9 0.912
};
