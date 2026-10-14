#pragma once
// MGEXP HUD: EXP gain counter (queue add FUN_006fe520, draw FUN_006fe660), level-up banner (show FUN_00703680,
// draw FUN_006ffed0) and the combo-hit HUD (ComboHUD_show 0x7036f0 / ComboHUD_update 0x702740); state
// HudModel::exp. RE notes: docs/reverse-engineering/UI_SYSTEM.md "EXP counter / level-up / combo HUD".
#include "engine/ui_model.hpp"

#include <vector>

namespace joa::game {
struct GameEvent;
class Mission;
}  // namespace joa::game

namespace joa::engine {

class Ui;

void hud_exp_add(HudModel::Exp&, int xp);         // FUN_006fe520
void hud_exp_level_up(HudModel::Exp&, int level); // FUN_00703680
void hud_exp_combo(HudModel::Exp&, int hits);     // ComboHUD_show
// World events of a sim tick: the player's XpGain / LevelUp, combo hit count (GameUiBridge::on_world_events).
void hud_exp_events(const std::vector<game::GameEvent>&, game::Mission&, HudModel&);
// Draw + advance (hud_game, after the player panel as in FUN_006f9b60).
void hud_exp(Ui&, HudModel&);

}  // namespace joa::engine
