#pragma once
// Options menu cases (menu_options.cpp) vs the original: s04_07_options_ingame (in-game root) and the
// tools/reference/capture_ui.py shots ui_options_* (opened from the main menu, sandbox CONFIG.DAT). Mouse =
// where the original's gauntlet sits in each shot (its hover states are reproduced, the cursor is drawn by both).
#include "ui_shot.hpp"

#include <vector>

inline const std::vector<uishot::ShotCase> kOptionsCases = {
    {"ingame-options", "s04_07_options_ingame.png", {-1, -1}, {}, 0.99},
    {"options", "ui_options_root.png", {253, 407}, {}, 0.97},
    {"video", "ui_options_video.png", {507, 338}, {}, 0.95},
    {"audio", "ui_options_audio.png", {507, 416}, {}, 0.97},
    {"input", "ui_options_input.png", {507, 507}, {}, 0.97},
    {"game", "ui_options_game.png", {510, 585}, {}, 0.97},
    {"remap", "ui_options_remap_0.png", {285, 722}, {}, 0.97},
};
