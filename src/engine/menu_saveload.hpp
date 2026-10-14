#pragma once
// Load/Save screens (interfac/savemenu, FUN_0074cb10 init .. FUN_00753440) plus the en2d text helpers shared
// with the in-game menu (menu_ingame.cpp). RE notes: docs/reverse-engineering/UI_SYSTEM.md.
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"
#include "formats/vfs_fs.hpp"

#include <string_view>
#include <vector>

namespace joa::engine {

// Save/*.SAV, newest first (GameFile_enum_saves 0x4770a0 sorted by comparator 0x477bd0: saved FILETIME
// descending). date/time formatted as the info panel shows them (DATE|1 "October 13, 2026", "8:43 PM").
std::vector<SaveSlot> scan_saves(const formats::Vfs& vfs);

// font::text_in_rect 0x626130: centred horizontally (clamped to x1) and vertically on the line height in
// the inclusive rect (x1, y1)-(x2, y2); RGB = colour, alpha = glyph alpha (TextOp::Diffuse).
void ui_text_in_rect(Ui& ui, const UiFont* f, std::string_view s, glm::ivec4 r, glm::vec4 color);
// en2d widget slide (move-to 0x61e020, per-frame update 0x61f450): straight line, v0 px/s with acceleration
// a px/s^2 (menus: 3500 / -1500), starting `delay` s after t = 0; distance travelled at t, clamped to `dist`.
float ui_slide(float t, float delay, float dist, float v0 = 3500.0f, float a = -1500.0f);
// Seconds a slide needs to travel d px (inverse of ui_slide without delay).
float ui_slide_time(float d, float v0 = 3500.0f, float a = -1500.0f);
// en2d alpha fade (0x61f1b0 / 0x61f980): 0..1 at `rate` per second after `delay` s.
float ui_fade(float t, float delay, float rate);

// Text-only menu button (savemenu 0x754220, in-game BACK 0x6f5200): white label in the rect raised 1 px;
// hover/pressed adds COMMELE2 "mark" at both sides (y1 + 8), pressed draws the label black.
// Returns true when clicked (never when alpha < 1). r = inclusive x1, y1, x2, y2.
bool ui_text_button(Ui& ui, const UiFont* f, std::string_view s, glm::ivec4 r, float alpha = 1.0f);
// COMMELE2 LONGBARs at (top_x, 180 + lc) and (bottom_x, 712 + lc) with the screen title centred in the top
// bar (Goudy_bold_18 white, 0x74e314); lc = -48 in game (DAT_00a500c2 == 9).
void menu_heading_bars(Ui& ui, int lc, std::string_view heading, float top_x = 0.0f, float bottom_x = 0.0f);

// INGAME_B backdrop split by en2d layer (FUN_006f21c0): back = woodtex/sphere/ring/left/right, front =
// titlebar, "In-Game Menu" title, top, bottom — the front layer covers the menu page drawn in between.
// t = seconds since the slide-in started (FUN_006f24d0), e = seconds since the slide-out started
// (FUN_006f26e0, < 0 = none). menu_ingame_backdrop(ui, m, heading) = back + heading bars + front.
void ingame_backdrop_back(Ui& ui, float t = 1e9f, float e = -1.0f);
void ingame_backdrop_front(Ui& ui, float t = 1e9f, float e = -1.0f);
// Slide-in clock: model.time while MenuModel::ingame_backdrop_closed, else settled.
float ingame_backdrop_time(const MenuModel& m);
constexpr float kIngameBackdropIn = 0.3f, kIngameBackdropOut = 0.3f;  // durations of the two slides
// G_POPUP message box (FUN_006d3cb0): no title, body wrapped in Goudy14(22), buttons OK | CANCEL
// (G_POPUP 1/2, b_ot/b_ov/b_dw), centred on the 4:3 canvas. Enter = OK, Escape = CANCEL.
// Returns 1 = OK, 2 = CANCEL, 0 = still open. Defined in menu_ingame.cpp.
int ui_confirm_popup(Ui& ui, std::string_view body);
}  // namespace joa::engine
