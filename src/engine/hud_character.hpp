#pragma once
// Character details window (unit attribute interface 0xa56398, interfac i_unitat.cpp / i_invent.cpp / i_attcomb.cpp):
// left attribute panel (draw FUN_006f6280, input FUN_006f7780), inventory page (draw FUN_006d6740, input
// FUN_006d8ac0) and attack-combo page (draw FUN_006ef9e0, input FUN_006f0a90); plus the shop window
// start_buy_interface opens (i_buy.cpp: init FUN_006c2120, draw FUN_006c4070, buy FUN_006c5a00, commit FUN_006c3540;
// hud_shop.cpp). Every change goes through the World / Mission functions. RE notes and gaps:
// docs/reverse-engineering/UI_SYSTEM.md "Character details window".
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"
#include "game/mission.hpp"

#include <map>
#include <vector>

namespace joa::engine {

class CharacterWindow {
public:
    // HudModel::show_character (F1 / pad, GameFlow) opens the window for the player; the window mirrors it into
    // MissionUi::inventory_open (script close_inventory_interface closes it). MissionUi::buy_open shows the shop.
    // Draw before the HUD (the original's attention message sits above the window). The sim is paused while
    // either is open (FUN_006f7d80 sets the game speed to 0: GameFlow::paused).
    void draw(Ui&, game::Mission&, HudModel&);
    void reset() { *this = {}; }
    // Player pickups of a ShowArmoryInterface item not in the hint list (0x49da30 tail: 0x472a90 / 0x4729c0, then
    // 0x6bbc90 + 0x6ba4b0) open the ARMORY window: HudModel::armory_item.
    void on_world_events(const std::vector<game::GameEvent>&, game::Mission&, HudModel&);

    bool combo_page = false;   // +0x21 == 0: attack-combo page (WP button) instead of the inventory (CB button)
    int unit = 0;              // +0x6af shown unit (party portrait clicks switch heroes)

    // Bag item placement on the inventory grids: each bag item's own cell (ItemInstance bag/col/row = the original's
    // +0x15/+0x16, saved with the game); unplaced items go first-fit in bag order. grid 0 personal 6x8, 1 enhancers 4x1.
    struct Cell { int item = 0, level = 0, grid = -1, col = 0, row = 0; };
    const std::vector<Cell>& cells() const { return cells_; }   // per bag index of `unit` (last drawn frame)

private:
    struct Held {              // item on the pointer (+0x11 source, +0x12 item)
        int src = -1;          // 0..3 melee0 / melee1 / ranged / ammo, 10 bag
        int bag = -1;
        int item = 0, level = 0;
        glm::vec2 at{0.0f};
        bool dragging = false;
    };
    struct Stock { int item = 0, level = 0, quantity = -1; Cell cell; };   // quantity -1 = no limit

    void left_panel(Ui&, game::Mission&, game::Unit&);
    void inventory(Ui&, game::Mission&, game::Unit&);
    void combos(Ui&, game::Mission&, game::Unit&);
    void layout(const game::GameData&, game::Unit&);
    bool drop(game::Mission&, game::Unit&, glm::vec2 p);
    void shop(Ui&, game::Mission&);
    void armory(Ui&, game::Mission&, HudModel&);
    float sell_ratio_ = 0.5f;   // sellitem.res SellingRatio of the open shop
    void tip(std::string title, std::string body) { tip_title_ = std::move(title), tip_body_ = std::move(body); }
    void draw_tip(Ui&);   // help box FUN_006b8db0 at the pointer

    bool open_ = false;
    std::vector<Cell> cells_;              // per bag index of `unit`
    int cells_unit_ = 0;
    Held held_;
    glm::vec2 rest_mouse_{-1.0f};
    float rest_ = 0.0f, blink_ = 0.0f;     // pointer rest time (tooltips after 250 ms), CB button blink clock
    int combo_scroll_ = 0, new_scroll_ = 0, combo_rows_ = 0;   // combo_rows_: learned combos (PadWindows LB / RB scroll)
    std::string tip_title_, tip_body_;     // tooltip of this frame, drawn last
    glm::vec2 tip_at_{-1.0f};              // PadWindows: its top-left with the pad (-1: below the focused rect)
    // Shop: the original edits copies (gold +0x57, bag, stock +0x5b, FUN_006c3230) and commits them on every close
    // (Esc / the button, FUN_006c3540), so buying straight into the World is equivalent.
    int shop_facility_ = 0;
    std::vector<Stock> stock_;
    std::map<int, std::vector<int>> stock_left_;   // facility recno -> quantities left (window lifetime)
    std::string shop_msg_;                         // BUY 8 / BUY 9 message box
};

// Shared with hud_shop.cpp.
std::string item_icon(const game::ItemDef& d);                          // ITEM\<code> (Saint-Catherine special case)
std::string item_description(const game::ItemDef& d, int level);        // Description with %Nd values (0x5a4c30)
bool place_first_fit(const game::GameData&, std::vector<CharacterWindow::Cell>&, int grid, CharacterWindow::Cell& c);
glm::ivec2 grid_size(int grid);   // 0 personal 6x8, 1 enhancers 4x1, 2 troop 6x4, 3 shop stock 6x13

}  // namespace joa::engine
