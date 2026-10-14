// Campaign scenario definitions: scenario/scenario_NN.scr. The loader (VA 0x4ccfb0) builds
// "SCENARIO\\scenario_" + %02d + ".scr" from the campaign scenario number, reads "code",
// "StepNo" and then six "Key : value" fields per step into 65-byte slots (0x1b2-byte step).
// Objective-map scenes (script/scenario/*_map.bm) are BlockText: see formats/world_text.hpp.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::formats {

struct ScenarioStep {
    std::string world_file;          // WorldFileName, relative to MAP\ (e.g. scenario1\S1L1w)
    std::string unit_file;           // UnitFileName (e.g. scenario1\S1L1.uni)
    std::string script_file;         // ScriptFileName -> script\scenario\<x>.sce
    std::string description_file;    // DescriptionFileName -> <x>.sce (end conditions, names)
    std::string objective_file;      // ObjectiveFileName -> <x>.obj (briefing text)
    std::string objective_map_file;  // ObjectiveMapFileName -> <x>.bm (briefing 3D map scene)
};

struct ScenarioDef {
    std::string code;                 // "code", e.g. SCENARIO_01
    std::vector<ScenarioStep> steps;  // StepNo entries (1 in every shipped file)
};

std::optional<ScenarioDef> parse_scenario_def(std::string_view text, std::string* err = nullptr);

}  // namespace joa::formats
