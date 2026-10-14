#pragma once
// Plain UI state. Screens read these structs, change only menu-navigation fields and emit UiCommands;
// the game layer owns all real logic (starting missions, saving, applying options).
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace joa::engine {

enum class MenuScreen : std::uint8_t {
    None,        // in game, no menu (HUD only)
    Main,        // MENU1024 main menu
    Difficulty,  // new-campaign difficulty picker (MAINMENU 5..9)
    Load,        // "LOAD A SAVED GAME" (SAVEMENU)
    Save,        // "SAVE A GAME" (in game only)
    Options,     // OPTMENU, tab in MenuModel::options_tab
    Remap,       // options -> REMAP KEYS
    Credits,
    InGame,      // in-game menu (IGAMEMNU)
};

enum class OptionsTab : std::uint8_t { Root, Video, Audio, Input, Game };  // Root = the 4-button list

// In-game options, mirrors CONFIG.DAT fields (docs/formats/CONTAINERS_WORLD_SAVES.md section 6);
// persisted as $XDG_CONFIG_HOME/joan-native/options.ini (docs/reverse-engineering/UI_SYSTEM.md).
struct GameOptions {
    // Video
    int shadow = 3;               // 0x9C 0..3 (No/Low/Medium/High Shadow)
    float brightness = -0.2f;     // 0xA8 -1..1
    float contrast = 1.1f;        // 0xAC
    float gamma = 1.0f;           // 0xB0
    int draw_distance = 20;       // 0x98 slider 0..20 (far = slider*0x3c00/20 + 0x401)
    bool dynamic_grass = true;    // 0x9F
    int grass_distance = 2000;    // 0xA0 multiple of 20
    int grass_density = 100;      // 0xA4 percent
    // Audio (0..100)
    int music_volume = 77;        // 0x3A
    int sound_volume = 100;       // 0x41 "Sound Volume" (3D sfx + UI), FUN_007413e0 (AudioVideo RE)
    int environment_volume = 39;  // 0x3C "Environmental Volume"
    bool sound_3d = true;         // 0x43
    // Input
    int input_method = 1;         // 0x6B scheme {0,3,1}: 0 Keyboard, 1 Keyboard & Mouse, 2 Gamepad/Joystick
    float mouse_sensitivity = 0;  // 0x6F -1..1 (slider-10)/10
    bool invert_mouse = false;    // 0x73
    float riding_camera_angle = 90.0f;  // 0x76 degrees
    // Game
    int difficulty = 1;           // 0x0A 0 Easy, 1 Normal, 2 Hard, 3 Very Hard
    int riding_mode = 0;          // 0x74 Mode A / Mode B (shown on the Input page)
    bool melee_auto_target = true; // 0x75 == 0
};

struct KeyBindingRow {
    int action = 0;               // STD.SET DEFAULTK record number (1-based, remapkey.cpp action id)
    std::string label;            // KEY_ACTION text (raw cp1252)
    bool header = false;          // section row (ids 1,11,22,33,42,47,52)
    // Raw codes as the remap entry +0x24/+0x28/+0x22/+0x2c/+0x23: K_KEY, J_KEY (DIK), J_JOYPAD (pad button+1),
    // FPS_KEY (DIK), FPS_MOUSE (1 L, 2 R, 3 M). Unbound = 0xffff (keys) / 0xff (buttons). def = DEFAULTK.
    std::array<int, 5> code{}, def{};
};

struct SaveSlot {
    std::string file;             // full path (list_saves): native <dir>/*.jsav or original Save/<name>.SAV (+ Save/<name>.BMP)
    std::string description;      // player-entered text (raw cp1252)
    int mission = 0;              // 1..8, 0 = free mission
    std::string date, time;       // as shown
};

struct MenuModel {
    MenuScreen screen = MenuScreen::Main;
    MenuScreen previous = MenuScreen::Main;  // where BACK returns (options/load from main vs in-game)
    bool in_game = false;
    OptionsTab options_tab = OptionsTab::Root;
    GameOptions options;
    std::vector<KeyBindingRow> bindings;
    int remap_category = 0;       // OPTMENU 40..44/50/56 category combo, 0..6
    std::vector<KeyBindingRow> remap_backup;  // bindings when the remap page opened (CANCEL restores)
    int remap_selected = -1;      // waiting for a key when >= 0
    int options_prev = -1;        // page shown before options_tab (OptionsTab, 5 = remap, -1 = menu just opened):
                                  // it slides out while options_tab slides in (time = model.time, FUN_0073cc00)
    float slider_hold_ms = 0, slider_repeat_ms = 0;  // held slider arrow: hold time / repeat accumulator (0x760df0)
    std::vector<SaveSlot> saves;
    int save_page = 0;            // 0-based, 10 slots per page
    int save_selected = -1;       // index into saves, -1 = none
    int save_state = 0;           // 0 browse, 1 delete confirm, 2 description entry (FUN_0074f230, obj+0x4b04)
    float save_click_time = -1.0f;  // MenuModel::time of the last slot click (double click loads)
    std::string save_description; // text field on the Save screen (raw cp1252, max 63 chars)
    int ingame_confirm = 0;       // in-game popup: 0 none, 1 quit to Windows, 2 quit to main, 3 restart
    float save_exit_time = -1.0f; // Load/Save slide-out start (model.time), -1 none (FUN_0074dbc0)
    int ingame_exit = -1;         // in-game menu slide-out: clicked item 0..7, 8 = BACK/Escape (resume), -1 none
    float ingame_exit_time = 0;   // model.time of that click (FUN_006f38a0, then backdrop FUN_006f26e0 on resume)
    bool ingame_backdrop_closed = true;  // INGAME_B not up: the next in-game page slides it in (FUN_006f24d0)
    int main_entry = 0;           // how Main reopens (MenuFrontEnd): 0 full opening sequence, 1 back from Load/Options
                                  // (Joan + stripe fade in, then buttons), 2 back from Difficulty (buttons slide in)
    int frontend_exit = -1;       // Main/Difficulty exit animation in progress (MenuFrontEnd): clicked button index, -1 none
    float frontend_exit_time = 0; // model.time of that click
    float time = 0.0f;            // seconds since the current screen opened (animations, credits scroll)
};

// Something the game layer must do. Menu-internal navigation never produces commands.
struct UiCommand {
    enum class Type : std::uint8_t {
        NewGame,         // value = difficulty 0..3
        Load,            // text = save file
        Save,            // text = save file ("" = new), text2 = description
        DeleteSave,      // text = save file
        OptionsChanged,  // MenuModel::options / bindings changed (apply + persist)
        Resume,          // close the in-game menu
        RestartMission,
        QuitToMain,
        Quit,            // quit to the desktop
        MissionBrief,    // in-game menu shortcuts
        MessageLog,
        HoldResume,      // HUD party button: value = party index (toggle hold / resume)
        MinimapClick,    // HUD minimap: world = clicked world x/y, value = 0 left / 1 right button
        QuickLoad,       // quick-load prompt OK
        QuickLoadCancel, // quick-load prompt Cancel
        ContinueCampaign,// mission won: game-end menu CONTINUE (next campaign mission, FUN_00720ca0)
        LoadMenu,        // game-over / game-end menu LOAD GAME: open the Load screen (load menu 0x74da40)
    } type;
    int value = 0;
    std::string text, text2;
    glm::vec2 world{0.0f};
};

struct HudUnit {                  // player panel / party portrait / unit name bar
    std::string name;             // raw cp1252
    std::string portrait;         // MAINGAME sprite: panel "Joan1", party "Joanc2"/"jeanc1" ("" = none)
    float hp = 1.0f, energy = 1.0f;   // 0..1
    int hp_value = 0, hp_max = 0, energy_value = 0, energy_max = 0;
    int level = 1;
    float xp = 0.0f;              // 0..1 progress to the next level
    int ammo = 0;                 // arrows (panel field under the level shield)
    int units = 0;                // troops under command
    bool selected = false;
    bool holding = false;         // party: HOLD button instead of RESUME (unit ordered to hold)
    bool hostile = false;         // name bar colour / bar texture (bar_english vs bar_french)
    glm::vec2 screen{0.0f};       // name bar: projected point above the head, window pixels (bar centre, text above)
    float bar_scale = 1.0f;       // name bar billboard: (camera distance in s04_04_mission1_hud) / (camera-to-unit distance)
    bool on_screen = false;
};

struct HudMapIcon {
    glm::vec2 world{0.0f};        // original world x/y (y up)
    std::string sprite;           // MAPICON record ("Fcamp", "!enemy", "!metz_M", ...)
    enum class Dot : std::uint8_t { None, Red, Green, Blue, White } dot = Dot::None;  // unit dot instead of a sprite
};

struct HudMessage {
    std::string text;             // raw cp1252; '_' toggles the red highlight ("_Jean de Metz:_ We must ...")
    std::string icon;             // MESSAGE record: speaker portrait ("metz1") or "tmess" / "m0000"
    int mission = 1;              // message log: "Mission %1d" section
    float age = 0.0f;             // seconds since posted (on-screen typewriter reveal)
};

struct BriefObjective {           // end-condition record (Game+0x30b + 0x1a n) that is enabled and has a description
    int type = 0;                 // 0 main (win), 2 side goal, 3 imperative (1 = lose: never listed)
    int finish = 0;               // 0 pending, 1 passed, 2 failed
    std::string text;             // description incl. any "(Time left ...)" suffix, raw cp1252
    std::string territory;        // territory link ("" = none): focus circle, picture and camera pan on hover
    int gold = 0, exp = 0;        // rewards ("REWARDS:" panel on hover)
    bool awarded = false;         // "(rewarded)" instead of the plain number
};

struct BriefTown {                // territory record (map/scenarioN/*.ter CreateTerritory)
    std::string name;             // TerritoryName, raw cp1252
    std::string picture;          // ScrName: OBJMENU 200x150 sprite ("s1_2"), "" = none
    int type = 0;                 // camp 0, town 1, city 2, castle 3, church 4, forest 5
    int side = 2;                 // owner: 0 French (player), 1 English / Mercenary, 2 other
    glm::vec2 world{0.0f};        // CoordinateX/Y (original world units, 16384 = map width)
};

struct BriefSupport {             // supporting unit of a commander (team +0x33), one entry per unit, grouped by type
    std::string type;             // unit.res code ("FRENCH_ARCHER") -> OBJMENU icon / type name
    int level = 1;
    float hp = 1.0f;              // 0..1
};

struct BriefCommander {           // commander list entry (scenario +0x235: units with class > 3), list order
    std::string name;             // raw cp1252
    int nation = 0;               // 0 French, 1 English, 2 Mercenary (other nations are never listed)
    bool hostile = false;         // empty team reads "Unknown" instead of "None"
    std::vector<BriefSupport> team;
};

struct BriefWeapon {              // siege weapon (unit.res record + des/<weapon>.txt), Siege Weapon page
    std::string name;             // unit.res Name ("Cannon"), shown upper-cased
    std::string text;             // des/<weapon>.txt, raw cp1252
    int attack = 0, defense = 0, mobility = 0;  // rating pips 0..5
    int crew = 0;                 // AttachPointCount (Light Infantry needed)
};

struct HudModel {
    HudUnit player;               // bottom-left panel
    std::vector<HudUnit> party;   // bottom-right portraits (up to 8, the player included)
    std::vector<HudUnit> name_bars;  // over-unit names/HP bars
    std::string scenario = "S1L1";   // minimap: INTERFAC.SET table <scenario>W, tiles z<zoom>r<row>c<col>
    glm::vec2 map_size{16384.0f};    // world extent covered by the minimap tiles (World +0x54/+0x58)
    glm::vec2 player_world{0.0f};
    float player_heading = 0.0f;  // radians, 0 = +x, counter-clockwise (world)
    int minimap_zoom = 1;         // 0 = whole map in 200 px, 1 = 2x2 tiles (default view)
    std::vector<HudMapIcon> map_icons;
    std::vector<HudMessage> messages;      // live on-screen attention messages, newest last (first one shown)
    std::vector<HudMessage> message_log;   // full log (message log window)
    int message_log_page = 0;              // left page index (even)
    bool show_message_log = false;
    bool show_mission_brief = false;
    bool show_objective = false;
    std::vector<std::string> objectives;   // objective panel lines, raw cp1252
    int objectives_main = 0;               // first n lines are main objectives (white separator line below them)
    int mission_time = 0, total_time = 0;  // seconds (mission brief corner boxes)
    bool brief_strategy = false, brief_siege = false;  // "Strategic Command" / "Siege Weapon" topics enabled (chapter >= 5)
    // Mission-brief pages (hud_brief.cpp). brief_page = objmenu +0x80: 0 topic menu, 1 Background, 2 Objectives,
    // 3 Townships, 4 Commanders, 5 Battleground, 8 Strategic Command, 9 Siege Weapon. File-backed fields are
    // loaded by brief_content() when a page opens and brief_mission != mission.
    int mission = 0;                       // campaign scenario number 1..8 (scenario/scenario_%02d.scr), 0 = none
    int brief_mission = 0;                 // mission whose file-backed brief content is loaded
    int brief_page = 0;
    float brief_time = 0.0f;               // seconds since the page opened (fade-ins)
    int brief_scroll = 0;                  // Background: first line (starts at -4 = 4 blank lines); lists: first row
    int brief_select = -1;                 // selected/hovered list entry (-1 = none)
    float brief_focus = 0.0f;              // detail panel fade 0..1 (SCR_FH / IMG_F, 6/s)
    glm::vec2 brief_pan{0.0f};             // 3D-map camera target - map centre (map units; hover pans 15 per frame)
    bool brief_2d = false;                 // Battleground: 2D minimap view instead of the 3D map (toggle)
    bool brief_help = false;               // Siege Weapon: SIEGE WEAPONS HELP popup open
    std::string brief_doc;                 // Strategic Command: des/strategic_<n>.txt of the selected topic
    // Game-filled runtime lists (objmenu reads them from the live game state).
    std::vector<BriefObjective> brief_objectives;
    std::vector<BriefCommander> brief_commanders;
    std::array<bool, 6> brief_weapons{};   // ram, crossbow, bombard, cannon, fauconneau, trebuchet present in the unit array
    int brief_rotation = 0;                // set_minimap_rotation (DAT_00a4f3ac, 0..3): 3D-map yaw = 90 - 90 r degrees
    // File-backed (brief_content).
    std::string brief_text;                // Background text (<ObjectiveFileName>.obj), raw cp1252, '\n' paragraphs
    std::string brief_map;                 // 3D-map scene (script/scenario/<ObjectiveMapFileName>.bm)
    glm::vec2 brief_map_centre{100.0f};    // centre of the scene's CreateMapActor boxes (map units)
    std::vector<BriefTown> brief_towns;    // territories of the mission map (owner may change at run time)
    std::array<BriefWeapon, 6> brief_weapon_info{};
    bool rts_mode = false;                // party: hold/resume buttons also on the player's portrait
    bool show_quickload_prompt = false;   // QSAVE "Load the last Quick Save game now?"
};

}  // namespace joa::engine
