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
    bool handle(const UiCommand&, game::Mission&);  // HoldResume, MinimapClick (RTS move order); true = consumed
    // An overlay owns the screen (cut scene, title, win message / game-end menu, game-over box): FUN_006f9b60
    // draws no HUD then, so the app skips hud_game().
    bool blocking(game::Mission&) const;

    bool voice_active = false;      // set by the app from GameAudio
    bool show_names = true;         // unit name bars (F11 toggles)
    bool auto_advance = false;      // reset(): MissionUi::auto_advance (headless tests set it)
    const std::vector<int>* rts_selection = nullptr;  // GameCamera::rts_selection() for minimap move orders

private:
    void letterbox(Ui&, game::Mission&);
    void dialog_box(Ui&, game::Mission&);
    void title_screen(Ui&, game::Mission&);
    void win_message(Ui&, game::Mission&, std::vector<UiCommand>&);
    void game_end_menu(Ui&, game::Mission&, HudModel&, std::vector<UiCommand>&);
    void game_over(Ui&, game::Mission&, std::vector<UiCommand>&);

    const formats::Vfs& vfs_;
    std::vector<int> party_;                   // recnos behind HudModel::party (HoldResume index)
    std::map<int, HudMessage> live_messages_;  // attention recno -> message (logged when it leaves)
    bool brief_shown_ = false;                 // HudModel::show_mission_brief was set for a script brief
    int typed_id_ = 0;                         // dialog box being typed and its reveal (chars, 40/s)
    float typed_ = 0.0f;
    std::string map_icons_key_;                // map dir + scenario of map_icons_
    std::vector<std::pair<std::string, HudMapIcon>> map_icons_;  // <stem>.map records: display-enable key, icon
    float win_time_ = -1.0f, over_time_ = -1.0f;  // seconds since the win message / game-over box opened, -1 = closed
    bool end_menu_ = false;                    // win message "Next Mission" clicked: game-end menu open
    int load_mission_ = -1, load_stage_ = 0;
    float load_time_ = 0.0f, load_shown_ = 0.0f;  // seconds in the current stage, displayed bar percent
};

// Speaker portrait (MESSAGE record) of an attention message / dialog box: unit class (FUN_004ab330 /
// FUN_00639b40) when `unit` != 0, else the script's speaker name (FUN_004aa3c0); "M0000" / "TMESS" pass through.
std::string message_portrait(game::Mission&, int unit, std::string_view name);

// Native saves (<native_save_dir>/*.jsav, META description/mission/date) and the original install's Save/*.SAV
// (imported read-only), newest first. SaveSlot::file = full path (UiCommand Load/DeleteSave text).
std::vector<SaveSlot> list_saves(const formats::Vfs&);

}  // namespace joa::engine
