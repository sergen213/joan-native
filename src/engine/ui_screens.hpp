#pragma once
// Screen entry points (immediate mode: each call draws the screen into the Ui frame and reacts to this
// frame's input). Menus run on the 4:3 canvas (Anchor::Center); the HUD anchors to screen edges.
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"

#include <string_view>
#include <vector>

namespace joa::engine {

// MenuModel::screen dispatch (MenuScreen::None draws nothing). Advances model.time by ui.dt().
void ui_menu(Ui& ui, MenuModel& model, std::vector<UiCommand>& out);
void ui_hud(Ui& ui, HudModel& model, std::vector<UiCommand>& out);

// Individual screens (menu_main.cpp, menu_options.cpp, menu_saveload.cpp, menu_credits.cpp, hud_game.cpp).
void menu_main(Ui& ui, MenuModel& model, std::vector<UiCommand>& out);
void menu_difficulty(Ui& ui, MenuModel& model, std::vector<UiCommand>& out);
void menu_credits(Ui& ui, MenuModel& model, std::vector<UiCommand>& out);
void menu_options(Ui& ui, MenuModel& model, std::vector<UiCommand>& out);   // OptionsTab pages
void menu_remap(Ui& ui, MenuModel& model, std::vector<UiCommand>& out);
void menu_saveload(Ui& ui, MenuModel& model, std::vector<UiCommand>& out);  // Load and Save
void menu_ingame(Ui& ui, MenuModel& model, std::vector<UiCommand>& out);
void hud_game(Ui& ui, HudModel& model, std::vector<UiCommand>& out);
// Minimap px per world unit at the current zoom (0 without minimap tiles): the FUN_00717a90 "inside the minimap
// view" test of the party portraits and Ctrl_switch_commander.
glm::vec2 hud_minimap_scale(Ui& ui, const HudModel& model);

// Shared backdrops (owners: menu_main.cpp, menu_ingame.cpp). Front-end = main-menu art (stone ring, crowd,
// logo bar) behind Main/Difficulty and Load/Options opened from the main menu. In-game = INGAME_B wood ring +
// IGAMEMNU "In-Game Menu" top bar behind every menu opened during a mission; `heading` is the section title
// bar text ("SAVE A GAME", "OPTIONS", ...; empty = none).
void menu_frontend_backdrop(Ui& ui, const MenuModel& model);
void menu_ingame_backdrop(Ui& ui, const MenuModel& model, std::string_view heading);
// '_'-markup text (hud_game.cpp; '_' toggles the red highlight, 0x626320 with markup on): greedy word wrap in
// r.z, line gap 2, only the last `max_lines` lines (scrolled up like FUN_004d8c10). Returns the line count.
int ui_markup_block(Ui& ui, const UiFont* f, std::string_view s, glm::vec4 r, glm::vec4 c, int max_lines = 99);
int ui_markup_lines(const UiFontMetrics& m, std::string_view s, int width);  // line count of ui_markup_block

// --ui-screen names: main, difficulty, credits, options (root list), video, audio, input, game, remap, load,
// save, ingame, ingame-options, hud, msglog, brief, brief-background, brief-objectives, brief-townships,
// brief-townships-2, brief-commanders, brief-commanders-2, brief-battleground, brief-battleground-2d, brief-strategy,
// brief-siege, quickload, objective, attention. Fills sample models matching the reference screenshots' state.
// Returns false for an unknown name.
bool ui_sample_state(std::string_view name, MenuModel& menu, HudModel& hud, bool& show_hud);
std::vector<std::string_view> ui_screen_names();

}  // namespace joa::engine
