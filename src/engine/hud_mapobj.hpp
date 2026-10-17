#pragma once
// HudGaps mapobj: minimap vision mask / fog of war (FUN_0070ec50 builds a 200x200 byte mask per frame, FUN_0070e500
// draws it as a black 0x80 texture over the view; FUN_0070fbe0 draws unit dots only where the mask is set) and the
// objective panel (HUD object 0xa4d618: list mutators FUN_007078c0 / FUN_00707400 / FUN_007076c0 / FUN_00707960 /
// FUN_00707b20 / FUN_007072c0, show FUN_00707ce0, O key in PlayerCtrl_process 0x42e603, draw FUN_00704810).
// RE notes: docs/reverse-engineering/UI_SYSTEM.md "Minimap vision" / "Objective panel".
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace joa::game {
class Mission;
struct MissionEvent;
}  // namespace joa::game

namespace joa::engine {

// ---- minimap vision ----
// GameUiBridge::fill (before the unit dots): HudModel::minimap_vision / minimap_fog from the live mission.
void hud_mapobj_fill(game::Mission& m, HudModel& hud);
// FUN_0070fbe0 mask test for a unit dot at world `p` (world-space form of the mask; true when no mask is built).
bool hud_vision_visible(const HudModel& hud, glm::vec2 p);
// 200x200 mask in view pixels (row-major, 1 = visible) for view pixel = ((x - origin.x) sx, (origin.y - y) sy).
using VisionMask = std::array<std::uint8_t, 200 * 200>;
void hud_vision_mask(const HudModel& hud, glm::vec2 origin, glm::vec2 scale, VisionMask& mask);
// set_minimap_rotation (minimap +0x2b; FUN_007141a0 tiles, FUN_00716010 icons, FUN_0070fbe0 dots, FUN_00718540
// waypoints, Minimap_screen_to_world 0x717700): the view turns rotation x 90 degrees counter-clockwise about its
// centre (W-0xd3 + 99.5, 110), so rotation 1 shows world +x up. Inverse = hud_minimap_turn(4 - rotation, p).
glm::vec2 hud_minimap_turn(int rotation, glm::vec2 p);
// minimap(): the fog over the view at (W-0xd3, 10) (before FRAME), then the waypoints (after FRAME).
// screen = turn((x, map_h - y) * ppw + off), as hud_game's minimap.
void hud_minimap_fog(Ui& ui, const HudModel& m, glm::vec2 off, glm::vec2 ppw);
void hud_minimap_waypoints(Ui& ui, const HudModel& m, glm::vec2 off, glm::vec2 ppw);

// ---- objective panel ----
void hud_mapobj_reset(game::Mission& m, HudModel& hud);                             // GameUiBridge::reset
void hud_mapobj_event(const game::MissionEvent& e, game::Mission& m, HudModel& hud);  // Objective* MissionEvents
void hud_objective_show(HudModel& hud);    // FUN_00707ce0 (script show_objective, list changes)
void hud_objective_toggle(HudModel& hud);  // O key (action 0x2e)
// Panel alpha 0..255 after advancing the fade by `ms` (state machine of FUN_00704810; hides the panel at 0).
int hud_objective_alpha(HudModel& hud, float ms);
// "%1d" / "%1p{one|many}" (t_basic style) with args[0..1] for MAINGAME 5 / 6.
std::string hud_format_count(std::string_view fmt, int a, int b);
// Draw (hud_game, when show_objective): advances the panel and line clocks with ui.dt().
void hud_objectives(Ui& ui, HudModel& m);

}  // namespace joa::engine
