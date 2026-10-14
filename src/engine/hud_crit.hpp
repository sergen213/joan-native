#pragma once
// HudGaps crit: critical-attack HUD (Crit_drawHUD 0x45fae0, HUD object 0xa4f820; sets CRITICAL / CRITI_K /
// CRITI_F) and the kill interface counter panel (FUN_006df180, object 0xa50518; set KILL). RE notes:
// docs/reverse-engineering/UI_SYSTEM.md "Critical-attack HUD".
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"

#include <vector>

namespace joa::game {
class Mission;
struct GameEvent;
}  // namespace joa::game

namespace joa::engine {

// Draw (hud_game, after the player panel as FUN_006f9b60 calls Crit_drawHUD). Advances HudModel::crit
// banner_ms with ui.dt() (the original times the banner on real ms DAT_00a500ba).
void hud_crit(Ui& ui, HudModel& m);
// GameUiBridge::fill: sequence keys/progress from World::crit, kill panels from MissionUi::kill + counters.
void hud_crit_fill(game::Mission& m, HudModel& hud);
// GameUiBridge::on_world_events: Ev::Crit CRITSEQ / CRITATT / CRITKILL -> banner state.
void hud_crit_events(const std::vector<game::GameEvent>& events, HudModel& hud);

}  // namespace joa::engine
