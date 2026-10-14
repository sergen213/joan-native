// ScriptHost binding the mission-script builtins (SCRIPT_VM.md §8) onto the World and the mission
// layer. Dispatch is a 408-entry table filled by one registration function per slice
// (script_bind_<slice>.cpp); ids without a handler are "unimplemented": call() counts them,
// records an error once per id and returns the neutral value (0 / 0.0 / "").
//
// Every handler reproduces the original executor case: argument reads are raw (BuiltinCall::i/f/s),
// invalid recnos return the case's failure value, and side effects go to World / Mission
// components. UI/camera/sound/dialog builtins do not draw anything: they update MissionUi state
// and push MissionEvents for the engine.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/script_vm.hpp"

namespace joa::game {

class Mission;
struct GameEvent;

using BuiltinFn = Value (*)(Mission&, const BuiltinCall&);
using PollFn = bool (*)(Mission&, const BuiltinCall&);

struct BuiltinTable {
    std::array<BuiltinFn, 408> call{};  // index = builtin id
    std::array<PollFn, 408> poll{};     // only for is_blocking_builtin ids; null = finished
    // Ids deliberately left unimplemented, with the reason (documented in SAVES_AND_CAMPAIGN.md).
    std::map<int, std::string> unimplemented;
};

// One per slice; each fills the ids it owns (ownership table: docs/SAVES_AND_CAMPAIGN.md §Builtins).
void bind_objects(BuiltinTable& t);     // script_bind_objects.cpp: items, gates, walls, boxes (MapObjects)
void bind_places(BuiltinTable& t);      // script_bind_places.cpp: places, facilities, territories, nations
void bind_units(BuiltinTable& t);       // script_bind_units.cpp: unit queries
void bind_unit_state(BuiltinTable& t);  // script_bind_unit_state.cpp: unit state setters
void bind_troops(BuiltinTable& t);      // script_bind_troops.cpp: spawn, teams, orders/tasks, inventory
void bind_mission(BuiltinTable& t);     // script_bind_mission.cpp: objectives, counters, date/weather, save_game
void bind_ui(BuiltinTable& t);          // script_bind_ui.cpp: ui, dialog, camera, sound (MissionUi)

const BuiltinTable& builtin_table();  // all slices bound once

// Mission-slice hook, called by Mission::tick with the World events of the tick (after
// World::tick_units): objective counters the original increments from engine code (citizen /
// female-citizen kills FUN_0064f320, bribes, crates...). Implemented in script_bind_mission.cpp.
void process_world_events(Mission& m, const std::vector<GameEvent>& events);

// Helpers shared by the slices.
float deg_to_rad(float d);
std::string upper(std::string_view s);

class MissionHost final : public IScriptHost {
public:
    explicit MissionHost(Mission& m) : m_(m) {}
    Value call(const BuiltinCall& c) override;
    bool poll(const BuiltinCall& c) override;
    void trace(const BuiltinCall& c, const Value& result, bool vm_builtin) override;

    // Calls that hit an id without a handler: id -> count.
    const std::map<int, std::uint64_t>& unimplemented_calls() const { return missing_; }
    // Optional per-call trace ("t=<tick> file:line KW name(args) = result"), off by default.
    bool keep_log = false;
    std::string log;
    std::uint64_t calls = 0;

private:
    Mission& m_;
    std::map<int, std::uint64_t> missing_;
};

}  // namespace joa::game
