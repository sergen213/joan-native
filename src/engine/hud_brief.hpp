#pragma once
// Mission-brief topic pages (interfac/objmenu.cpp + objmenub.cpp + objmenuw.cpp: init FUN_0071d2d0, page switch
// FUN_00721a60, draw FUN_0071f690, input 0x720ca0). The topic menu itself is hud_game.cpp; it sets
// HudModel::brief_page and calls hud_brief while a page is open. RE notes: work/ui/notes_MissionBrief.md.
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"
#include "formats/vfs_fs.hpp"

namespace joa::engine {

// Draws the open page (HudModel::brief_page) and handles its input; BACK / Escape return to the topic menu.
void hud_brief(Ui& ui, HudModel& m);

// File-backed page content for campaign scenario `mission` (1..8), as the original reads it: scenario/
// scenario_%02d.scr -> ObjectiveFileName (Background text, FUN_007246f0), ObjectiveMapFileName (*_map.bm 3D
// map actor -> model texture), the S<n>_SCR town pictures and des/strategic_<n>.txt topics. Runtime parts
// (objectives, townships, commanders, siege weapons) are the game layer's to fill.
void brief_content(const formats::Vfs& vfs, int mission, HudModel& m);

}  // namespace joa::engine
