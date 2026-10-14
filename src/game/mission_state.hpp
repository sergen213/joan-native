// Mission-level game state that joan.exe keeps in the Game object 0xa58f90 (game.cpp,
// end_con.cpp) and a few UI globals, plus the end-condition / victory engine
// (Game::process FUN_00471620). World state (units, items) lives in World; script-tree state
// in Vm/ScenarioState. Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md Part D §3,
// docs/reverse-engineering/SCRIPT_VM.md §8 (objective builtins 227-276, 365-402).
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "formats/vfs_bytes.hpp"
#include "game/savegame_container.hpp"

namespace joa::game {

// End-condition record (Game+0x30b + (n-1)*0x1a). `has_des` mirrors the original's non-null
// description pointer: records without a description are ignored by the evaluation.
struct EndCondition {
    enum Type : std::int8_t { Win = 0, Lose = 1, Side = 2, Imperative = 3 };
    std::int8_t type = Win;
    bool has_des = false;
    std::string des;             // localised text (+0x01)
    std::string territory_link;  // +0x05, UI only
    std::int16_t finish = 0;     // 0 NORMAL, 1 WIN, 2 LOSE (+0x09)
    std::int16_t para1 = 0, para2 = 0;
    std::int16_t enabled = 1;    // +0x0f
    std::int32_t gold = 0, exp = 0;
    std::uint8_t awarded = 0;    // +0x19
};

// Events the mission layer reports to the engine (UI, sound, camera) and to logs/tests.
struct MissionEvent {
    enum class Kind : std::uint8_t {
        ObjectiveAdd, ObjectiveUpdate, ObjectiveFinish, ObjectiveFail, ObjectiveShow, ObjectiveHide,
        Victory,       // finish became 1 (win message FUN_006e7900)
        Defeat,        // finish became 2
        GameOverMessage, AttentionMessage, Dialog, ChoiceDialog, DialogClosed, Letter, TitleScreen, Brief,
        BuyInterface, ScenarioPart, CutScene, Camera, Sound, Music, Minimap, Ui, Ending, SaveGame, Text,
        Transition,    // campaign advanced to the next scenario
        // Player interact (R) indicator FUN_006d3590 (show 0x6d3450 / hide 0x6d3480), emitted on change:
        // a = 1 shown / 0 hidden, b = target recno (unit, modify item or facility), text = target kind
        // ("talk", "bribe", "prisoner", "mount", "recruit", "item", "recruit_screen", "workshop").
        InteractPrompt,
        // Interact handler result: a = player, b = target, text = "free_prisoner", "talk", "bribe",
        // "bribe_no_gold" (one-shot message), "recruit", "item", "recruit_screen" (0x6c0310),
        // "workshop_screen" (0x6ed3e0).
        Interact,
    };
    Kind kind = Kind::Ui;
    std::uint64_t tick = 0;
    std::int32_t a = 0, b = 0;  // per kind: objective index, unit recno, ids...
    std::string text;           // localised text / name / file
};
const char* event_kind_name(MissionEvent::Kind k);

struct MissionState {
    static constexpr int kMaxEndConditions = 40;  // 0x28, FUN_00471e20

    std::vector<EndCondition> end;   // count = size()
    std::int32_t win_delay_ms = 0;   // Game+0x738 (set_delay_before_win)
    std::uint32_t win_start_ms = 0;  // Game+0x73c, 0 = not pending
    bool cut_scene = false;          // Game+0x744 (blocks win and count times)
    std::int32_t gold = 0;           // DAT_00a596c4
    std::uint32_t play_ms = 0;       // Game+0x740
    std::uint32_t passed_ms = 0;     // Scenario+0x1da
    std::int32_t command_points = 0; // DAT_00a5b9f0 (profile +0x40)

    // Objective counters (DAT_00a591cc..0xa5921c).
    std::int32_t rescue_count = 0, rescue_limit = 0, reputation = 0;
    std::int32_t citizen_killed = 0, fcitizen_killed = 0, enemy_bribed = 0, enemy_bribed_cost = 0;
    std::int32_t gold_crates = 0, food_crates = 0, food_picked = 0, total_food = 0, citizen_saved = 0;
    std::int32_t bribed_enemy_killed = 0, house_burnt = 0;
    std::int32_t kill_citizen_limit = 0, kill_citizen_limit_2 = 0;
    std::uint8_t food_can_be_picked = 0;
    std::uint8_t objective_menu_opened = 0;  // DAT_00a591c7
    std::uint8_t read_letter_once = 0;       // DAT_00a46059
    std::uint8_t bribe_no_gold_shown = 0;    // DAT_00a591b5 (Unit_bribe "not enough gold" message once)
    std::vector<std::int16_t> scenario_generals;  // add/remove_scenario_general, max 31
    std::vector<std::uint16_t> item_hints_a, item_hints_b;  // Game+0x71c / +0x728 (carried in profile)
    std::string scenario_name, scenario_description;
    std::int32_t requested_step = 1;  // change_scenario_step (single-step campaign: always 1)

    std::uint64_t ending = 0;  // show_ending: 1 + ending id once requested
    // save_game(name, desc) (FUNC 315) runs mid-tick; Mission performs the save after the tick.
    std::string save_request_name, save_request_desc;

    // Game clock / date object 0xa47ba8 (date FUN_00495290, TimeOfDay_process 0x476bb0, next day
    // GameClock_new_day 0x4953d0). Game_main initialises it to 2002-01-01, minute 0.
    std::int32_t date_jdn = 2452276;               // +0x08 Julian day number (get_game_date)
    std::int32_t day = 1, month = 1, year = 2002;  // +0x10 / +0x14 / +0x18
    float minute = 0;                              // +0x0c minute of day [0, 1440)
    float min_start = 0;                           // +0x1c minute the clock wraps to at 1440
    float fog_distance = 0;                        // _DAT_00a48cdc (set_fog_distance)
    std::int16_t snow_level = 0;                   // weather obj 0xa50298 +0x20 (set_snow_level)
    float weather_start[2] = {};                   // DAT_00a4839c / DAT_00a483ac: minute an effect started
    std::uint8_t charmove_detect_block = 0;        // DAT_00a5ba64 (0 at Battle_start_scenario)
    // DAT_00a500d2 game-clock-running flag: set_victory_display_flag clears it and forces game
    // speed 0 (FUN_0042f930(0)); the clock and count times stop advancing.
    std::uint8_t clock_running = 1;
    std::uint8_t game_over = 0;                    // game-over box shown (obj+0x9ec, FUN_004af410)
    std::string game_over_text;                    // obj+0x9e8 (strncpy 255)
    std::vector<MissionEvent> events;

    // FUN_00471e20 / FUN_00471ef0.
    int add_end_condition();
    int del_end_condition();
    EndCondition* end_condition(int index1);  // nullptr when out of range (original: no check)

    // End-condition part of Game::process FUN_00471620, run after the script exec and the
    // count-time update. finish is ScenarioState::game_finish. award_exp(exp) must add exp to
    // every live hero unit with rank > 3 (FUN_005229a0(exp,1,0)). Returns true when finish
    // became 1 this tick.
    bool process_end_conditions(std::uint8_t& finish, std::uint32_t now_ms, std::uint64_t tick, int scenario_no,
                                const std::function<void(std::int32_t exp)>& award_exp);

    void emit(MissionEvent::Kind k, std::uint64_t tick, std::int32_t a = 0, std::int32_t b = 0, std::string text = {});

    void save(ByteWriter& w) const;
    bool load(formats::ByteReader& r);
};

// TimeOfDay_process 0x476bb0, once per tick with the game-ms delta: minute += dt * 0.0004
// (24/60/1000); at >= 1440 the minute wraps to min_start and the date advances one day
// (GameClock_new_day 0x4953d0). No-op while !clock_running (game speed 0). Implemented in
// script_bind_mission.cpp.
void advance_clock(MissionState& s, std::uint32_t dt_ms);

}  // namespace joa::game
