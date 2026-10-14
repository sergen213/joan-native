#pragma once
// HudGaps: interact indicator (GMSG box FUN_006d23b0) and target HUD (TUI set, head of FUN_006f9b60).
// HudModel::interact / ::target are filled by GameUiBridge (hud_effects.cpp); RE notes in
// docs/reverse-engineering/UI_SYSTEM.md "Interact indicator" / "Target HUD".
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace joa::engine {

// FUN_005a6660-style positional format: %N s / %N d (N 1-based), %N?{a|b} (a when argument N is non-zero),
// %Nj{a|b|...} (alternative number N, none when out of range), %Np{one|many} (one for 0 / 1),
// %Nq{zero|one|many}, %% -> %. Missing arguments expand to "".
std::string ui_format(std::string_view fmt, const std::vector<std::string>& args);
// Indicator text of the GMSG box ("" = no box) and the bribe line under the minimap ("" = none).
std::string interact_text(Ui& ui, const HudModel::Interact& in);
std::string bribe_text(Ui& ui, const HudModel::Interact& in);

void hud_interact(Ui& ui, const HudModel& m);  // GMSG box (384,100) + bribe line (820,220)-(1000,320)
void hud_target(Ui& ui, const HudModel& m);    // TUI portrait / sword HP bar / siege base, top left

}  // namespace joa::engine
