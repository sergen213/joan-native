// Import of original joan.exe saves (Save/*.SAV). The body is a raw memory dump, so only the parts
// with a confirmed layout are decoded (docs/SAVES_AND_CAMPAIGN.md "Original save import"):
//   header                  description, FILETIMEs, scenario number (+0x19a)
//   section 4 (FUN_00484780) Game object 0x745: scenario no / step, finish, gold, play time,
//                           win delay/start, end conditions (+ their texts), item-hint list counts;
//                           Scenario 0x289 + step records 0x1b2: bit flags, stage, time limit,
//                           mode, scenario name
//   section 13 unit records  0x432-byte Unit dumps: hero (rank > 3) progression fields, and the records after
//                           each dump up to its inventory (equipment, bag items with grid cells)
// Script-tree state is not in the file (the original recompiles the script on load and resumes
// from the bit flags / stage), so an import restarts the mission script with those restored.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "formats/save_sav.hpp"
#include "game/mission_state.hpp"
#include "game/unit_state.hpp"

namespace joa::game {

class GameData;
class Mission;

struct ImportedHero {
    std::string code;  // unit class code (from the type id at +0x3b)
    Unit unit;         // name, rank, nation, pos, yaw, level, xp, points, hp/ep, stats, morale, inventory...
    bool inventory = false;   // unit.inv decoded from the records after the dump
};

struct OriginalSave {
    formats::SaveHeader header;
    int scenario_no = 0, step = 0;
    std::uint8_t finish = 0;
    std::int32_t gold = 0, win_delay_ms = 0;
    std::uint32_t win_start_ms = 0, play_ms = 0;
    std::uint32_t count_time[3] = {}, passed_ms = 0;
    std::uint32_t bits[4] = {};
    std::int32_t stage = 0, time_limit = -1, time_remaining = -1, mode = 0;
    std::string scenario_name;
    std::vector<EndCondition> end;
    std::vector<ImportedHero> heroes;
    std::vector<std::string> notes;  // what could not be decoded
};

std::optional<OriginalSave> import_original_save(std::span<const std::uint8_t> bytes, const GameData& gd,
                                                 std::string* err = nullptr);

// Applies an import to a mission freshly started for o.scenario_no: ScenarioState bit flags /
// stage / mode / time limit / count times, MissionState end conditions / gold / play time / win
// delay, and the progression of every hero whose class is present in the World.
void apply_original_save(Mission& m, const OriginalSave& o);

}  // namespace joa::game
