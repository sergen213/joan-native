#pragma once
// RTS overlay (Rts_draw 0x4450f0 + CommanderPanel clicks): drag box, RTSICON cursor, party portrait selection.
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"

#include <vector>

namespace joa::engine {

class GameCamera;

// Pixel rects (x, y, w, h) of the HUD parts that take RTS mouse clicks: the minimap and the party portraits.
std::vector<glm::vec4> rts_hud_rects(Ui& ui, const HudModel& m);
// Portrait clicks (RtsPortrait), the drag box and the RTS cursor; call last (instead of Ui::cursor).
void ui_rts(Ui& ui, const HudModel& m, GameCamera& cam, std::vector<UiCommand>& out);

}  // namespace joa::engine
