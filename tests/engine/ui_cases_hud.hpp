#pragma once
// HUD and HUD overlays (hud_game.cpp) vs the original's screenshots. The 3D world is not rendered behind the HUD,
// so only the opaque panel parts are scored (bars/text strip, party HP bars + shields, minimap frame top/bottom;
// portrait rings and the minimap map are translucent over the world).
#include "ui_shot.hpp"

#include <vector>

namespace hudcase {
inline const std::vector<uishot::Rect> kPanels = {{100, 696, 166, 62}, {845, 736, 179, 32}, {806, 6, 216, 8}, {806, 212, 216, 8}};
}

inline const std::vector<uishot::ShotCase> kHudCases = {
    {"hud", "s04_04_mission1_hud.png", {-1, -1}, hudcase::kPanels, 0.70, true, hudcase::kPanels},
    {"objective", "s04_06_objective.png", {-1, -1}, {{0, 148, 384, 40}}, 0.53, true, {{0, 148, 384, 40}}},
    {"attention", "s04_05_after_moves.png", {-1, -1}, {{90, 556, 920, 70}}, 0.40, true, {{90, 556, 300, 70}}},
    {"quickload", "s04_07_quickload_prompt.png", {-1, -1}, {{284, 324, 456, 120}}, 0.42, true, hudcase::kPanels},
    {"msglog", "s04_06_message_log.png", {-1, -1}, {}, 0.90},
    {"brief", "s04_06_mission_brief.png", {-1, -1}, {}, 0.95},
};
