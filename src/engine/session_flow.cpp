#include "engine/session_flow.hpp"

#include "engine/hud_mapobj.hpp"
#include "engine/menu_options_store.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace joa::engine {

const char* flow_state_name(FlowState s) {
    switch (s) {
        case FlowState::Menu: return "menu";
        case FlowState::Loading: return "loading";
        case FlowState::Playing: return "playing";
        case FlowState::Won: return "won";
        case FlowState::Lost: return "lost";
        case FlowState::Ending: return "ending";
        case FlowState::Quit: return "quit";
    }
    return "?";
}

bool GameFlow::paused() const {
    return state_ != FlowState::Playing || menu_.screen != MenuScreen::None || hud_.show_mission_brief ||
           hud_.show_message_log || hud_.show_quickload_prompt || hud_.show_character || hud_.armory_item ||
           hud_.recruit_screen || hud_.workshop_screen ||  // RecruitWorkshop: 0x6c0310 / 0x6ed3e0 set game speed 0
           !hud_.rts_join_prompt.empty() ||                // RtsHero: CHARMOVE|1 popup (Rts_cmd_assign pauses the game)
           (s_.mission() && s_.mission()->ui().buy_open && !s_.mission()->ui().auto_advance);  // FUN_006f7d80 / i_buy: speed 0
}

void GameFlow::new_game(int mission, int difficulty) {
    menu_.options.difficulty = difficulty;
    loading_mission_ = mission;
    pending_ = [this, mission, difficulty](std::string* err) { return s_.new_game(mission, difficulty, err); };
    state_ = FlowState::Loading;
}

void GameFlow::load(const std::string& path) {
    const auto meta = Session::read_meta(path);
    loading_mission_ = meta ? meta->mission : 0;
    pending_ = [this, path](std::string* err) { return s_.load(path, err); };
    state_ = FlowState::Loading;
}

bool GameFlow::run_pending() {
    if (!pending_) return false;
    std::string e;
    const bool ok = std::exchange(pending_, {})(&e);
    if (ok) {
        enter_mission();
    } else if (e == "campaign complete") {  // CONTINUE after scenario 8: the ending, then the credits
        state_ = FlowState::Ending;
        menu_.in_game = false;
        menu_.screen = MenuScreen::Credits;
        menu_.time = 0;
    } else {
        error = e;
        std::fprintf(stderr, "joan-native: %s\n", e.c_str());
        if (s_.mission() && generation_ == s_.generation()) state_ = FlowState::Playing;  // failed load: keep playing
        else to_menu();
    }
    return true;
}

void GameFlow::enter_mission() {
    state_ = FlowState::Playing;
    generation_ = s_.generation();
    menu_.screen = MenuScreen::None;
    menu_.previous = MenuScreen::InGame;
    menu_.in_game = true;
    menu_.ingame_backdrop_closed = true;
    hud_.show_mission_brief = hud_.show_message_log = hud_.show_quickload_prompt = hud_.show_objective = false;
    hud_.mission = s_.mission()->scenario_no();
}

void GameFlow::to_menu() {
    state_ = FlowState::Menu;
    const GameOptions options = menu_.options;
    auto bindings = std::move(menu_.bindings);
    menu_ = MenuModel{};
    menu_.options = options;
    menu_.bindings = std::move(bindings);
    hud_ = HudModel{};
}

void GameFlow::open(MenuScreen screen) {
    menu_.previous = MenuScreen::None;  // BACK returns straight to the game (F7/F8/F9 shortcuts)
    menu_.screen = screen;
    menu_.time = 0;
    menu_.options_tab = OptionsTab::Root;
    menu_.options_prev = -1;
    menu_.save_selected = -1, menu_.save_state = 0, menu_.save_page = 0, menu_.save_exit_time = -1.0f;
    if ((screen == MenuScreen::InGame || screen == MenuScreen::Save) && capture_world) shot_ = capture_world();
    listed_ = screen;
    if (screen == MenuScreen::Load || screen == MenuScreen::Save) rescan_saves();
}

void GameFlow::rescan_saves() {
    if (list_saves) menu_.saves = list_saves();
}

// Save New ("" file): <native dir>/Save_<YYYYmmdd-HHMMSS>.jsav; overwrite = the selected slot's file (native only).
std::string GameFlow::save_path_for(const UiCommand& c) const {
    const std::filesystem::path dir = game::native_save_dir();
    if (!c.text.empty()) {
        std::filesystem::path p(c.text);
        if (p.is_relative()) p = dir / p;
        if (p.extension() == ".jsav" && p.parent_path() == dir) return p.string();
    }
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "Save_%Y%m%d-%H%M%S", std::localtime(&t));
    std::filesystem::path p = dir / (std::string(stamp) + ".jsav");
    for (int i = 2; std::filesystem::exists(p); ++i) p = dir / (std::string(stamp) + "-" + std::to_string(i) + ".jsav");
    return p.string();
}

void GameFlow::command(const UiCommand& c) {
    using T = UiCommand::Type;
    switch (c.type) {
        case T::NewGame: new_game(1, c.value); break;
        case T::Load: load(c.text); break;
        case T::Save: {  // FUN_0074f230 state 2 + Return: GameFile_save, BMP (0x4731e0), list refreshed at page 1
            std::string e;
            if (!s_.save(save_path_for(c), c.text2, &e, &shot_)) error = e, std::fprintf(stderr, "joan-native: save: %s\n", e.c_str());
            rescan_saves();
            menu_.save_page = 0, menu_.save_selected = -1, menu_.save_state = 0;
            break;
        }
        case T::DeleteSave: {
            std::filesystem::path p(c.text);
            std::error_code ec;
            if (p.parent_path() == game::native_save_dir())  // never original saves
                std::filesystem::remove(p, ec), std::filesystem::remove(p.replace_extension(".bmp"), ec);
            rescan_saves();
            menu_.save_selected = -1;
            break;
        }
        case T::OptionsChanged:
            if (persist_options && !save_options(options_ini_path(), menu_.options, menu_.bindings))
                std::fprintf(stderr, "joan-native: cannot write %s\n", options_ini_path().c_str());
            if (options_changed) options_changed();
            break;
        case T::Resume: menu_.screen = MenuScreen::None; break;
        case T::RestartMission:
            if (s_.mission()) {
                loading_mission_ = s_.mission()->scenario_no();
                pending_ = [this](std::string* err) { return s_.retry(err); };
                state_ = FlowState::Loading;
            }
            break;
        case T::ContinueCampaign:
            if (state_ == FlowState::Won) {
                loading_mission_ = s_.mission()->scenario_no() + 1;
                pending_ = [this](std::string* err) { return s_.continue_after_win(err); };
                state_ = FlowState::Loading;
            }
            break;
        case T::QuitToMain: to_menu(); break;
        case T::Quit: state_ = FlowState::Quit; break;
        case T::MissionBrief: menu_.screen = MenuScreen::None, hud_.show_mission_brief = true, hud_.brief_page = 0; break;
        case T::MessageLog: menu_.screen = MenuScreen::None, hud_.show_message_log = true; break;
        case T::QuickLoad:
            hud_.show_quickload_prompt = false;
            load(Session::quick_save_path().string());
            break;
        case T::QuickLoadCancel: hud_.show_quickload_prompt = false; break;
        case T::LoadMenu: open(MenuScreen::Load); break;
        case T::HoldResume:
        case T::MinimapClick:  // UiBridge::handle
        case T::RtsPortrait: break;  // Client -> GameCamera
    }
}

void GameFlow::action(int id) {
    if (state_ != FlowState::Playing || !s_.mission()) return;
    game::Mission& m = *s_.mission();
    // Character window (0x6f7780 / 0x6f5a70): Esc closes it, F1 toggles it, another menu key switches to that screen.
    if (hud_.show_character && (id == 34 || id == 35 || id == 36 || (id >= 37 && id <= 41))) {
        hud_.show_character = false;
        m.ui().inventory_open = false;
        if (id <= 36) return;
    }
    const bool idle = menu_.screen == MenuScreen::None && !hud_.show_mission_brief && !hud_.show_message_log &&
                      !hud_.show_quickload_prompt && !hud_.show_character && !m.ui().buy_open && !hud_.armory_item &&
                      !hud_.recruit_screen && !hud_.workshop_screen &&  // RecruitWorkshop
                      hud_.rts_join_prompt.empty();                     // RtsHero: CHARMOVE|1 popup
    switch (id) {
        case 35:  // View Character Details (F1 / pad 7)
        case 36:  // Combo Window (unbound): same window (0x6f5a70 maps both to interface 2)
            if (idle && !m.ui().cs.active && !m.ui().game_cut_scene) hud_.show_character = true;
            break;
        case 40:  // Mission Brief (F12) toggles; closing a script-opened brief ends its WAIT (show_scenario_brief)
            if (hud_.show_mission_brief) {
                hud_.show_mission_brief = false;
                if (m.ui().brief_open) m.ui().close_interface();
            } else if (idle) {
                command({UiCommand::Type::MissionBrief});
            }
            break;
        case 41:  // Message Log (M) toggles
            if (hud_.show_message_log) hud_.show_message_log = false;
            else if (idle) hud_.show_message_log = true;
            break;
        case 34:  // Main Game Menu (Esc): skips a running cut scene first (ESC handler FUN_006ca5c0)
            if (!idle) break;
            if (m.ui().cs.active || m.ui().game_cut_scene) m.ui().skip_cut_scene(m);
            else open(MenuScreen::InGame), menu_.previous = MenuScreen::InGame, menu_.ingame_exit = -1;
            break;
        case 37: if (idle) open(MenuScreen::Save); break;
        case 38: if (idle) open(MenuScreen::Load); break;
        case 39: if (idle) open(MenuScreen::Options); break;
        case 23: if (idle) s_.toggle_rts(), hud_.rts_mode = s_.in_rts(); break;
        case 45: if (idle) hud_.minimap_zoom ^= 1; break;
        case 46: if (idle) hud_objective_toggle(hud_); break;  // HudGaps mapobj: fade in / out, pinned (0x42e603)
        case 53:  // Quick Save (F5)
            if (idle) {
                std::string e;
                if (capture_world) shot_ = capture_world();
                if (!s_.save(Session::quick_save_path(), "Quick Save", &e, &shot_)) error = e, std::fprintf(stderr, "joan-native: quick save: %s\n", e.c_str());
            }
            break;
        case 54:  // Quick Load (F6): QSAVE prompt "Load the last Quick Save game now?"
            if (idle && std::filesystem::exists(Session::quick_save_path())) hud_.show_quickload_prompt = true;
            break;
        default: break;
    }
}

void GameFlow::update() {
    if (menu_.screen != listed_ && (menu_.screen == MenuScreen::Load || menu_.screen == MenuScreen::Save))
        rescan_saves();  // opened by a menu (main menu LOAD, in-game LOAD/SAVE): the folder may have changed
    listed_ = menu_.screen;
    if (state_ == FlowState::Ending && menu_.screen == MenuScreen::Main) to_menu();
    if (state_ != FlowState::Playing || !s_.mission()) return;
    const game::Mission& m = *s_.mission();
    if (m.finish() == 1) state_ = FlowState::Won;
    else if (m.finish() == 2) state_ = FlowState::Lost;
}

}  // namespace joa::engine
