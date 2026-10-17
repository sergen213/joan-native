#pragma once
// Live mission -> HudModel (player panel, party, name bars, minimap, objectives, brief lists, messages) and the
// in-game overlays the stock HUD lacks: cut-scene letterbox + dialog box / choices, title screen, loading screen,
// mission-won message + game-end menu and the game-over box (game_ui_bridge_screens.cpp). RE notes and gaps:
// docs/reverse-engineering/UI_SYSTEM.md "Game UI bridge".
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"
#include "formats/vfs_fs.hpp"
#include "game/mission.hpp"

#include <glm/glm.hpp>

#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace joa::engine {

class GameUiBridge {
public:
    explicit GameUiBridge(const formats::Vfs& vfs) : vfs_(vfs) {}

    void reset(game::Mission&, HudModel&);  // new mission / load (copies auto_advance to MissionUi)
    void on_events(const std::vector<game::MissionEvent>&, game::Mission&, HudModel&);
    // HudGaps: World events of the tick -> HUD effect state (hit flashes, XP / level-up / combo, crit HUD);
    // hud_effects.cpp dispatches to the hud_* slices.
    void on_world_events(const std::vector<game::GameEvent>&, game::Mission&, HudModel&);
    void fill(game::Mission&, const game::Snapshot&, const glm::mat4& view_proj, glm::ivec4 viewport, HudModel&);
    // Overlays + their input (Ui frame input). Emits RestartMission / LoadMenu / QuitToMain / ContinueCampaign.
    void draw(Ui&, game::Mission&, HudModel&, std::vector<UiCommand>&);
    // Loading screen LoadingScreen 0x755e30 / draw FUN_00756430: the original stage sequence on its own clock
    // (ui.dt()); a new mission_no restarts it. progress 0..1 = loading done (bar target).
    void draw_loading(Ui&, int mission_no, float progress);
    // Stage of that sequence: 0..3 intro (6 s), 4 bar, 5 waiting for progress 1 (load here), 6 bar fade, 7 fade
    // to black, 8 finished.
    int loading_stage() const { return load_stage_; }
    void restart_loading() { load_mission_ = -1, load_stage_ = 0; }  // next draw_loading starts a new sequence
    bool handle(const UiCommand&, game::Mission&);  // HoldResume; true = consumed (minimap clicks: GameCamera)
    int party_unit(int i) const { return i >= 0 && i < int(party_.size()) ? party_[std::size_t(i)] : 0; }  // recno, 0 none
    // An overlay owns the screen (cut scene, title, win message / game-end menu, game-over box): FUN_006f9b60
    // draws no HUD then, so the app skips hud_game().
    bool blocking(game::Mission&) const;

    bool voice_active = false;      // set by the app from GameAudio
    bool show_names = true;         // unit name bars (F11 toggles)
    bool auto_advance = false;      // reset(): MissionUi::auto_advance (headless tests set it)
    // Set by the app each frame (HudFinish): GameCamera::rts_commander() (ViewCtrl +0x1238, party CSELECT in RTS mode),
    // GameCamera::rts_hover_siege() (DAT_00a5704d, RTS interact prompt), the open character window's unit (0 = closed).
    int rts_commander = 0, rts_siege = 0, char_unit = 0;
    const MenuModel* menu = nullptr;  // HudGaps: key bindings / input method for the interact key name (null = defaults)
    // SMS line (interfac\i_sms.cpp): "You have picked up ..." / "You have been given ...!" / gold-crate counter
    // texts, front = the one sliding in / shown (game_ui_bridge_screens.cpp).
    struct Sms {
        std::string text;
        int type = 0;  // 1 = gold-crate counter (FUN_006e5700 param_3: replaced by the next one)
    };
    const std::deque<Sms>& sms() const { return sms_; }

private:
    void letterbox(Ui&, game::Mission&);
    void dialog_box(Ui&, game::Mission&);
    void title_screen(Ui&, game::Mission&);
    void win_message(Ui&, game::Mission&, std::vector<UiCommand>&);
    void game_end_menu(Ui&, game::Mission&, HudModel&, std::vector<UiCommand>&);
    void game_over(Ui&, game::Mission&, std::vector<UiCommand>&);
    bool sms_event(const game::MissionEvent&);  // Ui "sms_*" / "food_blocked": queued, true = consumed
    void sms_line(Ui&, game::Mission&);         // formats the queued events, slides / draws the front line

    const formats::Vfs& vfs_;
    std::vector<int> party_;                   // recnos behind HudModel::party (HoldResume index)
    std::map<int, HudMessage> live_messages_;  // attention recno -> message (logged when it leaves)
    bool brief_shown_ = false;                 // HudModel::show_mission_brief was set for a script brief
    int typed_id_ = 0;                         // dialog box being typed and its reveal (chars, 40/s)
    float typed_ = 0.0f;
    std::string map_icons_key_;                // map dir + world of map_icons_ / map_size_
    std::vector<std::pair<std::string, HudMapIcon>> map_icons_;  // every level's <node>.map: display-enable key, icon
    glm::vec2 map_size_{16384.0f};             // World +0x54/+0x58 (<world>.wor CreateGrid)
    std::vector<HudMapIcon> unit_dots_;        // minimap unit dots for unit_dots_key_ (snapshot time + vision areas)
    std::uint64_t unit_dots_key_ = 0;
    bool unit_dots_valid_ = false;
    float win_time_ = -1.0f, over_time_ = -1.0f;  // seconds since the win message / game-over box opened, -1 = closed
    bool end_menu_ = false;                    // win message "Next Mission" clicked: game-end menu open
    int load_mission_ = -1, load_stage_ = 0;
    float load_time_ = 0.0f, load_shown_ = 0.0f;  // seconds in the current stage, displayed bar percent
    // HudGaps (hud_effects.cpp):
    void effects_event(const game::MissionEvent&, game::Mission&);       // InteractPrompt / Interact / item_hit
    void post_effects(Ui&, game::Mission&);                              // deferred bribe_no_gold attention message
    void party_effects(game::Mission&, const game::Unit&, HudUnit&);     // hit flash, danger, morale, RTS selection
    void fill_effects(game::Mission&, HudModel&);                        // interact indicator, target HUD, clock
    void hud_tick(game::Mission&);                                       // FUN_006fd8b0: bar chases, target timeout
    std::map<int, int> hit_start_;             // unit recno -> World ms of its last hit (party DICON/CHIT flash)
    bool interact_shown_ = false;              // InteractPrompt a: indicator visible (FUN_006d3590 +8)
    bool bribe_no_gold_ = false;               // Interact "bribe_no_gold": post the MESSAGE 2 attention message
    int target_ = 0, target_time_ = 0;         // target HUD +0x33 (unit / item recno), World ms of the last set (+0x38)
    bool target_unit_ = true;                  // +0x37
    int target_hp_ = 0;                        // +0x2c shown HP
    int bar_unit_ = 0, bar_hp_ = 0, bar_ep_ = 0, bar_exp_ = 0;  // +0x20/+0x24/+0x28 shown player HP / EP / EXP
    int char_unit_ = 0;                        // char_unit of the last fill (selection blink restart)
    std::vector<game::MissionEvent> sms_events_;  // "sms_*" Ui events waiting for the t_interf text (sms_line)
    std::deque<Sms> sms_;
    int sms_state_ = 0;     // 0 slide in, 1 hold 3000 ms, 2 slide out (FUN_006e5280 states 0-2 / 3 / 4-5)
    float sms_time_ = 0.0f; // seconds in the state
};

// Speaker portrait (MESSAGE record) of an attention message / dialog box: unit class (FUN_004ab330 /
// FUN_00639b40) when `unit` != 0, else the script's speaker name (FUN_004aa3c0); "M0000" / "TMESS" pass through.
std::string message_portrait(game::Mission&, int unit, std::string_view name);

// Native saves (<native_save_dir>/*.jsav, META description/mission/date) and the original install's Save/*.SAV
// (imported read-only), newest first. SaveSlot::file = full path (UiCommand Load/DeleteSave text).
std::vector<SaveSlot> list_saves(const formats::Vfs&);

}  // namespace joa::engine
