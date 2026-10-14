#include "engine/ui_screens.hpp"

#include <algorithm>
#include <array>

namespace joa::engine {

void ui_menu(Ui& ui, MenuModel& m, std::vector<UiCommand>& out) {
    const MenuScreen before = m.screen;
    ui.frame(Anchor::Center);
    switch (m.screen) {
        case MenuScreen::None: break;
        case MenuScreen::Main: menu_main(ui, m, out); break;
        case MenuScreen::Difficulty: menu_difficulty(ui, m, out); break;
        case MenuScreen::Credits: menu_credits(ui, m, out); break;
        case MenuScreen::Options: menu_options(ui, m, out); break;
        case MenuScreen::Remap: menu_remap(ui, m, out); break;
        case MenuScreen::Load:
        case MenuScreen::Save: menu_saveload(ui, m, out); break;
        case MenuScreen::InGame: menu_ingame(ui, m, out); break;
    }
    m.time = m.screen == before ? m.time + ui.dt() : 0.0f;
}

void ui_hud(Ui& ui, HudModel& model, std::vector<UiCommand>& out) { hud_game(ui, model, out); }

namespace {
constexpr std::array<std::string_view, 29> kNames{"main",  "difficulty", "credits", "options", "video",  "audio",
                                                  "input", "game",       "remap",   "load",    "save",   "ingame",
                                                  "ingame-options", "hud", "msglog", "brief", "brief-background",
                                                  "brief-objectives", "brief-townships", "brief-townships-2",
                                                  "brief-commanders", "brief-commanders-2", "brief-battleground",
                                                  "brief-battleground-2d", "brief-strategy", "brief-siege", "quickload",
                                                  "objective", "attention"};

void sample_hud(HudModel& h) {
    // State of work/reference/shots/s04_04_mission1_hud.png (mission 1 start, Normal). Player position and the
    // minimap icons are the world points that reproduce the reference minimap (work/ui/notes_HudGame.md):
    // camps from map/scenario1/S1L1.map, enemy dots measured.
    h.scenario = "S1L1";
    h.map_size = {16384.0f, 16384.0f};
    h.player = {"Joan of Arc", "Joan1", 1.0f, 1.0f, 60, 60, 30, 30, 1, 0.0f};
    HudUnit joan = h.player, jean = {"Jean de Metz", "jeanc1", 1.0f, 1.0f, 60, 60, 30, 30, 1, 0.0f};
    joan.portrait = "Joanc2";  // JOAN_ARMOR_LV1
    h.party = {joan, jean};
    jean.screen = {488.0f, 266.0f};
    jean.on_screen = true;
    h.name_bars = {jean};
    h.player_world = {14971.0f, 12698.0f};
    h.player_heading = -0.4f;
    h.minimap_zoom = 1;
    h.map_icons = {{{8116, 726}, "Fcastle"}, {{12000, 14000}, "Fcamp"}, {{12000, 14000}, "e_camp"}, {{14725, 12944}, "!metz_M"}};
    // Enemy groups (red unit dots, measured): 5 units 1 minimap px (41 world units) apart give the 5x5 blobs.
    for (glm::vec2 g : {glm::vec2{11428, 13742}, {12657, 13435}, {11919, 10936}, {11469, 9204}, {13160, 8806}})
        for (glm::vec2 d : {glm::vec2{0, 0}, {-41, 0}, {41, 0}, {0, -41}, {0, 41}})
            h.map_icons.push_back({g + d, "", HudMapIcon::Dot::Red});
    h.objectives = {"Safely reach and enter Orl\xe9" "ans.", "Eliminate all enemies along the path."};
    h.objectives_main = 1;
    h.mission_time = h.total_time = 56;
    h.mission = 1;
    // Mission-brief runtime lists at mission 1 start (S1L1_des.sce enabled end conditions; commanders in unit-id
    // order, Jean de Metz loaded after Joan; both teams empty).
    h.brief_objectives = {{0, 0, "Safely reach and enter Orl\xe9" "ans.", "Orl\xe9" "ans", 500, 1000, false},
                          {2, 0, "Eliminate all enemies along the path.", "", 500, 500, false}};
    h.brief_commanders = {{"Jean de Metz", 0, false, {}}, {"Joan of Arc", 0, false, {}}};
    h.message_log = {
        {"_Jean de Metz:_ We must make haste and reach Orl\xe9" "ans safely - the Duke of Orl\xe9" "ans is expecting us. "
         "Perchance we will meet with our enemies, however if we are protective of each other we shall both survive "
         "the journey.", "metz1"},
        {"_Jean de Metz:_ Your sword is at your disposal when the need arises (_Left mouse button_ for Light attack, "
         "_Right mouse button_ for Heavy attack, and _Q_ to defend). Let's move along, shall we?", "metz1"},
        {"Press _W_ and _S_ to move forward or back. To move sideways, press _A_ to go left, and _D_ to go right. "
         "Spin the mouse wheel to zoom the camera view in or out.", "tmess"},
        {"You can also perform combo attacks, by pressing the Light (A) attack (_Left mouse button_) several times, "
         "followed by a Heavy (B) attack (_Right mouse button_). Try out the AAAB attack when engaging the enemy.", "tmess"}};
    for (HudMessage& msg : h.message_log) msg.age = 1e6f;
}
}  // namespace

std::vector<std::string_view> ui_screen_names() { return {kNames.begin(), kNames.end()}; }

bool ui_sample_state(std::string_view name, MenuModel& menu, HudModel& hud, bool& show_hud) {
    menu = MenuModel{};
    hud = HudModel{};
    show_hud = false;
    menu.time = 30.0f;  // opening animations finished
    if (name == "main") menu.screen = MenuScreen::Main;
    else if (name == "difficulty") menu.screen = MenuScreen::Difficulty;
    // credits at t = 42.28 s = work/reference/shots/s04_02_main_menu_again.png (file name desynced; content is the
    // credits); 4 captured frames at 1/30 s add 0.1 s.
    else if (name == "credits") menu.screen = MenuScreen::Credits, menu.time = 42.1833f;
    else if (name == "options") menu.screen = MenuScreen::Options;
    else if (name == "video") menu.screen = MenuScreen::Options, menu.options_tab = OptionsTab::Video;
    else if (name == "audio") menu.screen = MenuScreen::Options, menu.options_tab = OptionsTab::Audio;
    else if (name == "input") menu.screen = MenuScreen::Options, menu.options_tab = OptionsTab::Input;
    else if (name == "game") menu.screen = MenuScreen::Options, menu.options_tab = OptionsTab::Game;
    else if (name == "remap") menu.screen = MenuScreen::Remap, menu.options_tab = OptionsTab::Input;
    else if (name == "load") {
        menu.screen = MenuScreen::Load;
        menu.saves = {{"Mission1-AutoSave", "Mission 1 Auto Save", 1, "", ""}};
    } else if (name == "save") {
        menu.screen = MenuScreen::Save;
        menu.in_game = true;
        menu.previous = MenuScreen::InGame;
        menu.saves = {{"Mission-QuickSave", "Mission QuickSave", 1, "", ""}, {"Mission1-AutoSave", "Mission 1 Auto Save", 1, "", ""}};
    } else if (name == "ingame") {
        menu.screen = MenuScreen::InGame;
        menu.in_game = true;
    } else if (name == "ingame-options") {
        menu.screen = MenuScreen::Options;
        menu.in_game = true;
        menu.previous = MenuScreen::InGame;
    } else if (std::find(kNames.begin() + 13, kNames.end(), name) != kNames.end()) {  // HUD samples
        menu.screen = MenuScreen::None;
        menu.in_game = true;
        show_hud = true;
        sample_hud(hud);
        hud.show_message_log = name == "msglog";
        hud.show_mission_brief = name.starts_with("brief");
        // brief-<topic>: work/reference/shots/ui_brief_<topic>.png (tools/reference/capture_ui.py brief); the -2
        // states = second row hovered (townships: camera on Orleans) / clicked (commanders). Strategy and siege are
        // disabled in mission 1 (chapter < 5): their samples enable them and pick topic 1 / the cannon.
        constexpr std::array<std::string_view, 10> kBrief{"brief-background",   "brief-objectives",      "brief-townships",
                                                          "brief-townships-2",  "brief-commanders",      "brief-commanders-2",
                                                          "brief-battleground", "brief-battleground-2d", "brief-strategy",
                                                          "brief-siege"};
        constexpr std::array<int, 10> kBriefPage{1, 2, 3, 3, 4, 4, 5, 5, 8, 9};
        for (std::size_t i = 0; i < kBrief.size(); ++i)
            if (name == kBrief[i]) hud.brief_page = kBriefPage[i], hud.brief_time = 30.0f;
        if (hud.brief_page == 1) hud.brief_scroll = -4;
        if (name.ends_with("-2")) hud.brief_select = 1, hud.brief_focus = 1.0f;
        hud.brief_2d = name == "brief-battleground-2d";
        if (name == "brief-townships-2") hud.brief_pan = glm::vec2(8196.0f, 750.0f) / 16384.0f * 200.0f - hud.brief_map_centre;
        if (name == "brief-strategy" || name == "brief-siege") hud.brief_strategy = hud.brief_siege = true;
        if (name == "brief-strategy") hud.brief_select = 0;
        if (name == "brief-siege") hud.brief_select = 3, hud.brief_weapons = {true, true, true, true, true, true};
        hud.show_quickload_prompt = name == "quickload";
        hud.show_objective = name == "objective";
        if (name == "attention") hud.messages = {hud.message_log.back()};  // s04_05_after_moves
    } else {
        return false;
    }
    if (menu.in_game && menu.screen != MenuScreen::InGame && menu.screen != MenuScreen::None) menu.previous = MenuScreen::InGame;
    return true;
}

}  // namespace joa::engine
