// Script-visible UI / camera / dialog / sound runtime (dialog boxes 0x63cdc0, cut-scene controller
// 0xa5d380, attention messages 0xa55a88, letter/title/brief/buy interfaces, minimap 0xa4f3..., kill
// counters 0xa50518). Builtins (script_bind_ui.cpp) update this state and push MissionEvents; the
// engine renders from it and answers through the input methods. Headless runs use `auto_advance`:
// every player-closed interface reports finished after `auto_ms` of game time, dialog boxes finish
// typing after `auto_ms` of visible time and choice dialogs pick `auto_choice`.
// CONTRACT (owned by the ui slice; Mission relies on tick/clock_stopped/save/load_state).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "formats/vfs_bytes.hpp"
#include "game/mission_places.hpp"
#include "game/savegame_container.hpp"

namespace joa::game {

class GameData;
class Mission;

class MissionUi {
public:
    bool auto_advance = true;
    int auto_ms = 2000;
    int auto_choice = 1;
    // Optional engine override for get_key_string (FUNC 364): name of DEFAULTK action `action` in the
    // user's current bindings. Unset: the shipped STD.SET DEFAULTK/KEYDESC defaults (KeyMap_get_name
    // 0x4cb5b0) for the controller's input scheme.
    std::function<std::string(int action)> key_name;

    void tick(Mission& m);
    // True while a cut scene runs (Game+0x744) or the game clock is stopped (DAT_00a500d2 == 0,
    // letter interface): count times / time limit freeze (FUN_004cd5d0 gate).
    bool clock_stopped() const { return game_cut_scene || clock_paused; }
    void save(ByteWriter& w) const;
    bool load_state(formats::ByteReader& r);

    // --- cut-scene controller 0xa5d380 ----------------------------------------------------------
    // status (+0xf): 0 none, 1 starting camera move, 2 running, 3 camera move, 4 ending camera move,
    // 6 black wipe, 7 black screen hold (750 ms), 8 black wipe back.
    struct CutScene {
        std::uint32_t start_ms = 0;   // +0x00
        bool active = false;          // +0x04
        std::uint8_t black_start = 0; // +0x05
        std::uint8_t black_end = 0;   // +0x06 (DAT_00a5d386, set_end_cut_scene_type)
        float speed = 0;              // +0x07 letterbox fade units/s (+128 start, -128 end)
        float fade = 0;               // +0x0b letterbox 0..255
        std::int32_t status = 0;      // +0x0f
        bool camera_move = false;     // +0x13 "cut_scene_play" camera script drives the camera
        float cam[6] = {};            // +0x18.. target x, y, z, distance, angle1, angle2 (deg)
        std::int32_t saved_camera = 0;// +0x30 (camera index restored at the end)
        std::int16_t wipe = 0;        // +0x34 black wipe texture x
        std::uint32_t hold_ms = 0;    // +0x36 black hold start
        bool wipe_flip = false;       // +0x3e
        bool starting = false;        // +0x3f
        bool ending = false;          // +0x40 (DAT_00a5d3c0)
    } cs;
    int cut_scene_status() const { return cs.status; }
    bool cut_scene_skip_flag() const { return cs.starting; }  // byte 0xa5d3bf
    bool game_cut_scene = false;  // Game+0x744 mirror (Game_set_cutscene 0x4725f0)
    // FUN_006cb9e0 (unit camera, pos == nullptr) / FUN_006cad50 (pos camera). Returns 1/0.
    int cut_scene_start(Mission& m, bool no_camera, bool move, int unit, const float* pos, float a, float b, float c,
                        std::uint8_t black);
    // FUN_006cc6a0(move_back, black). Returns 1 (0 when no cut scene is active).
    int cut_scene_end(Mission& m, bool move_back, std::uint8_t black);
    // FUN_006cd720 (unit) / FUN_006ccf00 (pos). Returns 1/0.
    int cut_scene_camera(Mission& m, bool move, int unit, const float* pos, float a, float b, float c);
    bool cut_scene_wait_done() const;  // FUN_0065af50

    // --- camera scripts (camera manager 0xa48cc0, FUN_00428850 / FUN_00428aa0) -------------------
    struct CameraScript {
        bool playing = false;
        std::string name;
        std::uint32_t start_ms = 0;
        float speed = 0;          // FUN_00428a70
        std::int16_t flag5 = 0, flag6 = 0;  // FUN_00654a80 / FUN_00654ab0
    } camera;
    void play_camera_script(Mission& m, std::string name, std::uint32_t start_ms);

    // --- dialog boxes 0x63cdc0 (ids are recnos of the box array; del_all resets them) ------------
    struct DialogChoice {
        std::string text;
        bool enabled = true;  // +0x335
    };
    struct DialogBox {
        int id = 0;
        bool deleted = false;
        int speaker = 0;
        std::string text;            // "_<name>:_ <text>"
        std::string voice_stem;      // calling script file stem; voice = (stem, text number)
        int voice = 0;               // +0x340 (text number of the voice line, 0 = none)
        std::int32_t anim = 0;       // EX.res entry +8 / +0xe (when entry+2)
        std::int16_t anim2 = 0;
        std::int16_t rect[4] = {-1, -1, -1, -1};
        bool done = false;           // +0x334 typed / voice finished
        std::uint32_t shown_ms = 0;  // headless typing clock (visible time)
        bool has_choices = false;    // +0x34f
        std::vector<DialogChoice> choices;
        int choice = 0;              // +0x353 selected choice (1-based), 0 = none
    };
    std::vector<DialogBox> dialogs;
    bool dialog_show_disabled = false;  // manager +6
    DialogBox* dialog(int id);
    int add_dialog(Mission& m, int speaker, std::string text, std::string stem, int voice, std::int32_t anim,
                   std::int16_t anim2, const std::int16_t* rect);
    void del_dialog(Mission& m, int id);  // FUN_0063e1d0
    void del_all_dialogs(Mission& m);     // FUN_0063d150
    bool dialogs_done() const;            // FUN_0063d7b0

    // --- attention messages (array 0xa55a88, FUN_004ad710 / FUN_004ace90) ------------------------
    struct AttentionMessage {
        int recno = 0;
        std::int32_t state = 0;      // +0x04 0 new, 1 slide, 2/3 open, 4 typing/hold, 5 fade out, 6 gone
        bool fast = false;           // +0x08 (icon messages slide 10x faster)
        float y = 414;               // +0x0d
        std::int16_t slot = 0;       // +0x15
        std::int16_t len = 0;        // +0x17
        float typed = 0;             // +0x3b
        std::uint32_t t = 0;         // +0x43
        float f47 = 0, f4b = 0, f4f = 0, f53 = 0, f57 = 255;
        std::string text;            // +0x21
        std::string icon;            // +0x2e portrait / icon name ("M0000", "TMESS" or speaker name)
        int speaker = 0;             // unit recno (attention_call_message_2 with a unit)
        std::string voice_stem;
        int voice = 0;               // +0x19
    };
    std::vector<AttentionMessage> messages;  // live, ascending recno
    int next_message = 1;
    bool messages_paused = false;            // array +0x3a (cut scene)
    std::uint32_t messages_pause_ms = 0;     // array +0x3b
    int add_attention_message(Mission& m, std::string text, std::string icon, int speaker, std::string stem, int voice);
    AttentionMessage* attention_message(int recno);
    void clear_attention_messages();  // FUN_004734a0 (call_gameover_msg)

    // --- interfaces -----------------------------------------------------------------------------
    struct Title {
        bool active = false;
        std::uint32_t start_ms = 0;
        std::int32_t duration = 0, fade_in = 0, fade_out = 0;
        std::string text;  // 30 chars
    } title;
    bool buy_open = false;
    int buy_shop = 0, buy_seller = 0;
    bool brief_open = false;
    bool letter_open = false;
    int letter_id = 0;
    std::uint32_t interface_ms = 0;  // open time of the buy / brief / letter interface
    bool clock_paused = false;       // DAT_00a500d2 == 0 (letter interface)
    bool inventory_open = false;     // unit attribute interface 0xa56398 (engine sets)
    int scenario_part = 0;           // DAT_00a5027e
    bool music_side_env = true;      // DAT_00a501da
    struct KillInterface {
        bool active = false;
        std::int8_t mode = 0;
    } kill[2];

    // --- minimap 0xa4f3.. -----------------------------------------------------------------------
    struct Waypoint {
        std::int8_t type = 0;        // +0x46[slot]
        float pos[3] = {};           // +0xdc
        bool blink = false;          // +0x262
        std::uint32_t blink_ms = 0;  // +0x280
        std::uint8_t flag = 0;       // +0x244
        int follow = 0;              // +0x64 unit recno
    };
    std::array<Waypoint, 30> waypoints{};
    std::map<std::string, std::uint8_t> minimap_icons;  // icon name -> display enable (+0x9f)
    std::int16_t minimap_rotation = 0;  // DAT_00a4f3ac
    std::int16_t minimap_gold = 0;      // minimap +0x11
    struct VisionArea {
        bool enabled = false;
        std::uint8_t shape = 0;  // 0 rect, 1 circle, 2 quadrilateral
        std::int32_t c[8] = {};  // x1,y1,x2,y2(,x3,y3,x4,y4); circle: x,y + radius
        std::int32_t radius = 0;
    };
    std::array<VisionArea, 10> vision{};

    // Node-owned result buffers / dialog ids per call site (get_key_string, add_choice_dialog).
    std::map<std::uint32_t, std::string> site_text;
    std::map<std::uint32_t, int> site_dialog;

    // --- engine input ---------------------------------------------------------------------------
    void answer_dialog(int id, int choice);  // player clicked choice (1-based) of dialog `id`
    void dialog_done(int id);                // typing / voice of dialog `id` finished
    void skip_cut_scene(Mission& m);         // ESC handler FUN_006ca5c0
    void camera_script_done() { camera.playing = false; }
    void close_interface();                  // player closed the buy / brief / letter interface

private:
    void set_game_cut_scene(Mission& m, bool on);
    void tick_cut_scene(Mission& m);
    void tick_messages(Mission& m);
    template <class F> void visit(F& f);
};

// Shop facility stock (f_shop.cpp, Facility::stock: set by shop_set_sell_item_list, restocked hourly, saved with the
// facility). The engine buy screen (i_buy.cpp) and the campaign bot both buy and sell through these, so a sale is
// seen by every later visit. An entry at quantity 0 is sold out (not shown, FUN_006c3230 places quantity != 0 only).
using ShopStock = Facility::Stock;
std::vector<ShopStock> shop_stock(Mission& m, int facility);   // copy of the facility's live stock (empty: no shop)
// 0x6c5ac6: trunc(Price x price factor x (level - MinLevel + 1)).
int shop_price(const GameData& gd, const ShopStock& s);
// FUN_006c5a00 / FUN_006c5f10: buy one entry matching s.item / s.level with quantity != 0 for `u`: gold short -> 8
// (BUY 8), no room in the personal grid (World::buy_item) -> 9 (BUY 9), not stocked -> -1, else 0: the bag gets the
// item's Quantity, MissionState::gold drops by shop_price, a limited entry loses one and a ShowArmoryInterface item
// joins item_hints_b (FUN_00472a90 / FUN_004729c0, no armory window).
int shop_buy(Mission& m, int facility, const ShopStock& s, Unit& u);
// FUN_006c6550 sale: -1 = refused (BUY 10): CanSell 0, or red / blue food (types 0x15 / 0x17 / 0x19) to a non-POTION
// shop, anything else to a non-WEAPON one. Else, for the whole stack, trunc(Price x SellingRatio x quantity / per x
// (level - MinLevel + 1)): a stack dragged into the seller's grid (0x6c7319) has per = the item's Quantity; a
// double-clicked one (0x6c7860) per = its own quantity, i.e. one Quantity's worth whatever the stack holds.
int shop_sell_value(Mission& m, int facility, const ItemInstance& it, bool double_click = false);
// Sells u's bag stack `bag_index` (removed whole, gold += shop_sell_value); false when refused. Nothing is restocked.
bool shop_sell(Mission& m, int facility, Unit& u, int bag_index, bool double_click = false);

}  // namespace joa::game
