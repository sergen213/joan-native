#pragma once
// Game flow state machine (front end -> loading -> mission -> end screens -> next mission / menu), without
// rendering or devices: the client loop and the headless flow test drive it with UiCommands (menu/HUD
// clicks), DEFAULTK actions pressed this frame and ticks. docs/PLAYING.md "Flow".
#include "engine/session.hpp"
#include "engine/ui_model.hpp"

#include <functional>
#include <string>

namespace joa::engine {

enum class FlowState : std::uint8_t {
    Menu,     // front end (main menu, difficulty, load, options, credits)
    Loading,  // loading screen shown; the pending start/load runs after one presented frame
    Playing,  // mission running (paused() while a menu / brief / prompt is open)
    Won,      // finish 1: mission won screen + game-end menu (CONTINUE)
    Lost,     // finish 2: game-over box (RETRY / LOAD / QUIT TO MAIN)
    Ending,   // campaign complete: credits, then the main menu
    Quit,
};
const char* flow_state_name(FlowState s);

class GameFlow {
public:
    GameFlow(Session& s, MenuModel& menu, HudModel& hud) : s_(s), menu_(menu), hud_(hud) {}

    FlowState state() const { return state_; }
    // The world sim is stopped: menus, mission brief / message log, quick-load prompt, end screens.
    bool paused() const;
    std::string error;  // last failed start/load/save (also printed to stderr)

    // Pending mission start (Loading): mission number of the loading screen and the loader.
    int loading_mission() const { return loading_mission_; }
    // Runs a pending start/load once the loading screen has been presented. Returns true if it ran.
    bool run_pending();

    // Direct entries (CLI shortcuts and tests); each goes through Loading.
    void new_game(int mission, int difficulty);
    void load(const std::string& path);

    // Menu / HUD / end-screen commands (everything the screens emit).
    void command(const UiCommand& c);
    // DEFAULTK action ids pressed this frame (menu keys, F5/F6 quick save/load, F2 RTS, Esc ...).
    void action(int id);
    // After the frame's ticks: mission finish -> Won / Lost, campaign end.
    void update();

    // Effects the client applies (options persisted by the flow itself).
    std::function<void()> options_changed;  // re-read MenuModel options/bindings (audio volumes, input map)
    std::function<std::vector<SaveSlot>()> list_saves;  // Load/Save screens (UiBridge list_saves)
    bool persist_options = true;            // write options.ini on OptionsChanged (tests: false)
    // SaveCapture: world frame without UI as a save thumbnail (save_thumbnail). Taken when the in-game menu or the
    // Save screen opens (Esc / F7: the menus pause the world, so it equals the original's render at save time) and
    // at F5; Save / quick save write it as <save>.bmp. Unset (tests) = saves without thumbnail.
    std::function<Image()> capture_world;

private:
    void enter_mission();
    void to_menu();
    void open(MenuScreen screen);
    std::string save_path_for(const UiCommand& c) const;
    void rescan_saves();

    Session& s_;
    MenuModel& menu_;
    HudModel& hud_;
    FlowState state_ = FlowState::Menu;
    std::function<bool(std::string*)> pending_;
    int loading_mission_ = 0;
    int generation_ = -1;
    Image shot_;                               // last capture_world() result
    MenuScreen listed_ = MenuScreen::None;     // screen of the last update(): Load/Save re-scan when they open
};

}  // namespace joa::engine
