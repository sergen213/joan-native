// One running campaign mission: World + script VM + mission layer, ticked in the order of
// Game_process_tick 0x4e5fc0 / Game::process 0x471620 (docs/SAVES_AND_CAMPAIGN.md "Tick order").
//
//   play/passed time += 33 (finish == 0)
//   MapObjects::tick          (modify_item 0x4afb20)
//   World::tick_units         (clock, units, projectiles, Trigger_process)
//   Places::tick              (places, facilities, territories)
//   Vm::tick                  (challenge timers, script exec, count times / time limit)
//   MissionState end conditions + win delay
//   MissionUi::tick, World::tick_physics
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "formats/vfs_fs.hpp"
#include "game/ai_system.hpp"
#include "game/physics_char.hpp"
#include "game/campaign.hpp"
#include "game/interact_player.hpp"
#include "game/mission_objects.hpp"
#include "game/mission_places.hpp"
#include "game/mission_state.hpp"
#include "game/mission_ui.hpp"
#include "game/savegame_container.hpp"
#include "game/script_bind.hpp"
#include "game/script_vm.hpp"
#include "game/world_sim.hpp"

namespace joa::game {

struct ScriptText;

// One node of a mission world (<WorldFileName>.wor CreateNode). Map files of a node (.stg, .uni,
// .pla, .trk, .gri, .pfd) use node-local X/Y; add the node origin for world coordinates.
struct WorldNode {
    std::string name;      // NodeName, e.g. "S3L1_2" (file stem of the node's map files)
    float ox = 0, oy = 0;  // node origin in world units (FUN_0047ad40: rotated box min, X/Y in 32-unit steps)
    float x1 = 0, y1 = 0;  // node box max (Level +0x08); 0 for the implicit node of a world without .wor
};
std::vector<WorldNode> world_nodes(const formats::Vfs& vfs, const std::string& map_dir, const formats::ScenarioStep& step);

class Mission {
public:
    // gd/vfs/profile outlive the mission. The profile is read by load_unit and written by the
    // campaign layer after a win.
    Mission(GameData& gd, const formats::Vfs& vfs, CampaignProfile& profile);
    ~Mission();
    Mission(const Mission&) = delete;
    Mission& operator=(const Mission&) = delete;

    // Battle::start_scenario(no, 1) + Battle::load_world: terrain, World, map objects, places,
    // units (.uni), scripts (main + _des once), profile gold / play time / lists, init pass.
    bool start(const CampaignMission& cm, std::uint32_t seed, std::string* err = nullptr);
    // Battle_start_scenario(no, 1, keep = 1) after scenario 5 (CONTINUE 5 -> 6): Battle_deinit
    // 0x41c780 / Game_deinit 0x471010 keep the world, units, places and objects, and
    // Battle_load_world 0x41c810 skips world creation for scenario 6; only the scripts restart.
    bool continue_in_world(const CampaignMission& cm, std::string* err = nullptr);
    void tick(const InputFrame& in = {});

    // --- components -------------------------------------------------------------------------
    World& world() { return *world_; }
    Vm& vm() { return *vm_; }
    ScenarioState& scenario() { return vm_->state(); }
    MissionState& state() { return state_; }
    Places& places() { return places_; }
    MapObjects& objects() { return objects_; }
    const MapObjects& objects() const { return objects_; }
    AiSystem& ai() { return *ai_; }
    NavWorld* nav() { return nav_.get(); }  // every level's .pfd + the world mesh; null without a .pfd
    MissionUi& ui() { return ui_; }
    MissionHost& host() { return *host_; }
    GameData& data() { return gd_; }
    const formats::Vfs& vfs() const { return vfs_; }
    CampaignProfile& profile() { return profile_; }
    const CampaignMission& info() const { return info_; }
    int scenario_no() const { return info_.no; }
    std::uint64_t ticks() const { return vm_->ticks(); }
    std::uint32_t now() const { return vm_->game_ms(); }
    std::uint8_t finish() const { return vm_->state().game_finish; }
    std::string map_dir() const;  // "MAP\\scenarioN"

    void emit(MissionEvent::Kind k, std::int32_t a = 0, std::int32_t b = 0, std::string text = {}) {
        state_.emit(k, ticks(), a, b, std::move(text));
    }
    std::vector<MissionEvent> take_events() { return std::exchange(state_.events, {}); }

    // Hero restore for load_unit (FUNC 86 -> FUN_004c91c0): 0 unless the profile was written by
    // a CONTINUE and holds the class code. Returns the new unit recno.
    int load_hero(std::string_view code, std::string_view nation);
    // UnitArray_add 0x51dfc0: a unit of class `code` at the origin (nation 0). Once a CONTINUE wrote
    // the profile, a stored hero of that class is restored instead (FUN_004c91c0) with full hp/ep
    // and zeroed troop counters. Used by add_unit (FUNC 85) and the .uni loader.
    int add_unit(std::string_view code);
    // FUN_0051ff00 + store_generals + lists/gold/time/command points -> profile (game-end CONTINUE).
    void store_profile();
    // Hero switch (Ctrl_switch_commander; keys SWITCH_CHARACTER 24 / SWITCH_JOAN..ALENCON 25..28): `which` 0 cycles
    // from the player's hero through Joan, Jean, La Hire, Alencon (0x9f8b10) to the next usable one the minimap shows
    // (FUN_00717a90: |dx|,|dy| x view scale < 100 px; any when the player is dead; scale 0 = no minimap), 1..4 picks
    // that hero. Usable: alive, controllable (+0x2f == 0), not retreated. The new hero's task is parked and its team's
    // tasks dropped, the old hero resumes its parked task; in RTS mode (game_mode 4) both stay with the AI.
    bool switch_hero(int which, float view_sx = 0, float view_sy = 0);

    // --- saves --------------------------------------------------------------------------------
    // Sections: MISN (campaign no, seed, profile), WRLD (World::serialize), VMST (Vm::save),
    // MSTA (MissionState), OBJS, PLCS, UIST, HOST (per-site host state).
    SaveFile save() const;
    bool load(const SaveFile& f, std::string* err = nullptr);  // on a Mission constructed fresh
    std::uint64_t hash() const;  // FNV-1a over every section of save()

    // --- test hooks ---------------------------------------------------------------------------
    // Marks every enabled WIN end condition finished (console "objective_finish" FUN_00455c00
    // per record) now and after every later script pass, so the normal win-delay path runs.
    void finish_objectives();
    // save_game (FUNC 315) writes <native_save_dir>/<name>.jsav when true (tests may disable).
    bool autosave = true;

private:
    GameData& gd_;
    const formats::Vfs& vfs_;
    CampaignProfile& profile_;
    CampaignMission info_;
    std::uint32_t seed_ = 0;
    std::unique_ptr<ITerrainQuery> terrain_;
    std::unique_ptr<World> world_;
    std::unique_ptr<NavWorld> nav_;
    std::unique_ptr<AiSystem> ai_;
    std::unique_ptr<CharPhysics> phys_;  // destroyed before world_
    std::unique_ptr<MissionHost> host_;
    std::unique_ptr<Vm> vm_;
    MissionState state_;
    Places places_;
    MapObjects objects_;
    MissionUi ui_;
    std::uint8_t last_finish_ = 0;
    InteractTarget prompt_;  // last reported interact indicator (derived, not saved)
    bool force_objectives_ = false;
    void restore_profile();  // Battle_load_world: lists, gold, play time, command points
};

}  // namespace joa::game
