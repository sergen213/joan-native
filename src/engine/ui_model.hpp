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
    // SaveCapture: description text box (en2d txtbox2d, FUN_0062dd10): caret +0x87 and selection anchor +0x8d
    // (byte indices, kept across entries like the original object), caret blink origin (model.time, 0x62d730).
    int save_caret = 0, save_anchor = 0;
    float save_blink = 0;
    bool save_drag = false;       // mouse selection drag in the text box (+0x84)
    // HudGaps: options close slide (FUN_0073cc00 case 6) running since model.time 0; the screen switches to
    // `previous` when it ends (FUN_0073db40), input ignored meanwhile.
    bool options_closing = false;
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
        // RtsComplete:
        RtsPortrait,     // RTS party portrait click: value = party index (Rts_select_commander by recno)
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
    // Player panel bars (HUD +0x20/+0x24/+0x28, chased per tick by FUN_006fd8b0); -1 = hp / energy / xp. Texts use the real values.
    float hp_bar = -1.0f, energy_bar = -1.0f, xp_bar = -1.0f;
    int ammo = 0;                 // arrows (panel field under the level shield)
    int units = 0;                // troops under command
    bool selected = false;
    bool holding = false;         // party: HOLD button instead of RESUME (unit ordered to hold)
    bool hostile = false;         // name bar colour / bar texture (bar_english vs bar_french)
    glm::vec2 screen{0.0f};       // name bar: projected point above the head, window pixels (bar centre, text above)
    float bar_scale = 1.0f;       // name bar billboard: (camera distance in s04_04_mission1_hud) / (camera-to-unit distance)
    bool on_screen = false;
    bool workshop_bar = false;    // name_bars entry: workshop build bar (bar_workshop fill = hp, no label; 0x46c540)
    // HudGaps party effects (FUN_006f9b60 party loop):
    int hit_ms = -1;              // ms since the last hit (DICON/DSWORD/CHIT/MHIT flash, 1000 ms); -1 = none
    bool danger = false;          // hp / max < GAMEPLAY.RES {DANGER} INDANGER: DICON blink + DSWORD loop
    int morale = -1;              // average morale of leader + troops (FUN_006ffb40): morale*6/100 CMORAL pips; -1 = no troops
    // CSELECT (+ MSELECT with troops) blink: the unit of the open character window (+0x6ae/+0x6af); else in RTS mode
    // CSELECT on the ViewCtrl +0x1238 commander (Rts_select_commander).
    bool commander_selected = false, troops_selected = false;
    // Drawn at 0x80ffffff (half alpha) when +0x2f (control) / task-locked (+0x2e1) or outside the minimap view (0x717a90).
    bool busy = false;
    glm::vec2 world{0.0f};        // original world x/y (minimap view test)
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
    bool show_objective = false;           // objective panel drawn (+0x14d); state and lines: objective_* (mapobj)
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
    std::string rts_join_prompt;          // RtsHero: G_POPUP CHARMOVE|1 "team over capacity" body; "" = closed (pauses)
    bool show_quickload_prompt = false;   // QSAVE "Load the last Quick Save game now?"
    // CharDetails:
    bool show_character = false;          // character details window (F1 / action 35, 36; CharacterWindow)
    int armory_item = 0, armory_level = 0;  // ARMORY "YOU HAVE ACQUIRED THE" window (show_armory pickup), 0 = closed
    // HudGaps: interact indicator FUN_006d23b0 (GMSG box + bribe line), target HUD (TUI set, FUN_006f9b60 head).
    struct Interact {
        int line = -1;            // GMSG line: 2 talk, 4 prisoner, 6 barrack, 7 siege house, 8 wood, 9 iron,
                                  // 10 resource without Alencon, 11 mount, 12 recruit; 0 = item Description; 1 = RTS mode:
                                  // t_interf RTS 1 crew text of the hovered siege weapon; -1 = no box
        std::string name;         // talk target name / siege weapon class name (line 1), raw cp1252
        std::string item_text;    // item Description (line 0)
        int item_arg = 0;         // line 1: crew slots (class AttachPointCount +0x1ad)
        int bribe_line = 0;       // 3 = offer, 5 = not enough gold, 0 = none (text under the minimap)
        std::string bribe_name;
        int bribe_cost = 0, gold = 0;
        std::string key;          // KeyMap_get_name(scheme, 0x11): the interact key
    } interact;
    struct Target {
        bool active = false;      // +0x33 != 0 (cleared 3000 ms after the last set)
        bool unit = true;         // +0x37: unit (else a structure item)
        bool siege = false;       // unit class +0x118 not NORMAL/MAIN: SBASE layout
        std::string icon;         // TUI sprite: portrait / siege icon (FUN_00703880) or structure icon; "" = none
        std::string text;         // "<class name> (<level>)" (units only)
        int hp = 0, hp_max = 1;   // displayed HP (+0x2c, set to the real HP on a new target, then chases it by <= 40 per tick) / max
    } target;
    int clock_ms = 0;             // game clock (World ms = DAT_00a500ba): party hit / danger blinks
    float ui_ms = 0.0f;           // real-time clock DAT_00a500b2 (hud_game adds ui.dt): selection blink
    float select_ms = 0.0f;       // its value when the character window opened / switched unit (DAT_00a4d75f)
    bool sim_paused = false;      // GameFlow::paused(): game speed 0, DAT_00a500ba stands (EXP / crit HUD timers stop)
    // HudGaps crit: critical-attack HUD Crit_drawHUD 0x45fae0 (object 0xa4f820), kill interface FUN_006df180 (0xa50518).
    struct Crit {
        bool enabled = false;     // +0x38 set_critical_attack_enabled (nothing is drawn while clear)
        std::string keys;         // +0x1f combo of the shown sequence: 'A' Light, 'B' Heavy; "" = none (+0x18 clear)
        int progress = 0;         // +0x1d keys entered (lit icons)
        int banner = 0;           // 1 CA (+0x19, CRITATT), 2 CK (+0x1b, CRITKILL), 3 CF (+0x1a, never set); 0 none
        float banner_ms = 0.0f;   // game ms since CRITATT (+0x2f vs DAT_00a500ba), advanced by hud_crit unless sim_paused
        int fade_in1 = 200, fade_in2 = 200, glow_in[3] = {200, 200, 200};  // GAMEPLAY.RES {CRITICAL}
    } crit;
    struct KillPanel {
        bool active = false;      // start_/end_kill_interface(_2): +8 / +9
        int mode = 0;             // +0x12 / +0x13: 0 icon, 1 ficon, 2 hicon, 3 ticon (time left), 4 icon, 5 gold
        std::string text;         // "count/limit" or "m:s"
    } kill_panel[2];
    // HudGaps exp: MGEXP EXP counter FUN_006fe520/FUN_006fe660, level-up FUN_00703680/FUN_006ffed0, ComboHUD 0x7036f0/
    // 0x702740 (HUD object fields; hud_exp.cpp). Times: ms of `now` (game clock DAT_00a500ba), advanced by hud_exp with
    // ui.dt() unless sim_paused; the roll steps once per drawn frame of the exe's 30 Hz loop (real time).
    struct Exp {
        struct Entry {
            int value = 0;    // +0x3c gained EXP
            int digits = 0;   // +0x60 decimal digits of value
            int roll = 0;     // +0x69 rolling value shown (0 -> value, lowest digit first)
            int state = 0;    // +0x8d 0 roll, 1 flash, 2 hold 1500 ms, 3 flash, 4 fade/fly out 300 ms
            float t = -1.0f;  // +0x96 state timer (-1 = unset, the exe's 0)
        };
        float now = 0.0f;
        float roll_acc = 0.0f;     // ms toward the next roll step
        std::vector<Entry> queue;  // +0xbe count, at most 9 (the first entry is drawn)
        float slide_t = 0.0f;      // +0xba EXPTEXT slide in / out start
        int level_state = 0;       // +0xc2 0 off, 1..9 (LEVELUP0..3, LEVEL1..3 + digits)
        int level = 0;             // +0xc3 the player's new level
        float level_t = 0.0f;      // +0xc4
        int combo_state = 0;       // +0xcd 0 off, 1 fade in, 2, 3 hold (count shown), 4, 5 fade out
        int combo = 0;             // +0xce count shown
        float combo_t = 0.0f;      // +0xd0
        int combo_hits = 0;        // DAT_00a5ba62 = World::combo_hits after the last tick
    } exp;
    // HudGaps mapobj: minimap vision mask FUN_0070ec50 (minimap 0xa4f3.. +0x478, hud_mapobj.cpp) and the objective
    // panel (HUD object 0xa4d618 +0x14d..+0x299, FUN_00704810). Panel/line times: ms since the exe's stamps
    // (+0x153 / +0x1f9 = DAT_00a500ba real ms), advanced by hud_objectives with ui.dt().
    struct VisionArea {
        int shape = 0;             // 0 rectangle (p[0], p[1] corners), 1 circle (p[0], r), 2 quadrilateral p[0..3]
        glm::vec2 p[4]{};          // original world x/y (y up)
        float r = 0.0f;
        bool round = false;        // unit circle: centre rounded (ftol(x + 0.5)) like the exe
    };
    bool minimap_fog = false;      // mask built (minimap on, player alive): black 0x80 outside `minimap_vision`
    std::vector<VisionArea> minimap_vision;  // friendly units (radius +0x3c1), allied territories, script areas
    std::vector<HudMapIcon> minimap_waypoints;  // MAPICON wp_1..wp_7 (FUN_00718540): drawn over the fog and frame
    struct ObjectiveLine {
        int recno = 0;             // +0x299 end condition (1-based)
        int status = -1;           // +0x1a7: -1 none, 0 COMPLETED! (gold), 1 FAILED! (red), 2 New! (green)
        int reveal = 0;            // +0x157 prefix start (1 = whole prefix, 0 = no prefix)
        float reveal_ms = 0.0f;    // ms since +0x1f9
        // GameUiBridge::fill, every frame, from the end condition:
        int type = 0;              // 0 main, 2 side (others are not drawn)
        bool enabled = true;       // +0x0f
        std::string des;           // raw cp1252
        int suffix = 0;            // para1: 1 "(Time left ...)" MAINGAME 5, 2 "(n/m rescued)" MAINGAME 6
        int a = 0, b = 0;          // its numbers (minutes left / rescue count, limit)
    };
    std::vector<ObjectiveLine> objective_list;  // +0x299 list order (new lines inserted first)
    int objective_fade = 0;        // +0x14f: 0 hidden, 1 fade in 2 s, 2 fade out 3 s, 3 shown (12 s unless pinned)
    float objective_fade_ms = 0.0f;  // ms since +0x153
    bool objective_pinned = false; // +0x14e: opened with O (no 12 s auto-hide)
    bool objective_paused = false; // cut scene: +0x153 follows the clock (FUN_006fd8b0), the panel stands still
    // RecruitWorkshop: barracks (0x6c0310) / siege workshop (0x6ed3e0) screen of this facility recno, 0 = closed
    // (MissionEvent::Interact "recruit_screen" / "workshop_screen"; FacilityWindow; the sim is paused while open).
    int recruit_screen = 0, workshop_screen = 0;
};

}  // namespace joa::engine
