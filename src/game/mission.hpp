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
    void tick(const InputFrame& in = {});

    // --- components -------------------------------------------------------------------------
    World& world() { return *world_; }
    Vm& vm() { return *vm_; }
    ScenarioState& scenario() { return vm_->state(); }
    MissionState& state() { return state_; }
    Places& places() { return places_; }
    MapObjects& objects() { return objects_; }
    AiSystem& ai() { return *ai_; }
    NavMesh* nav() { return nav_.get(); }  // null for multi-node worlds
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
    // FUN_0051ff00 + store_generals + lists/gold/time/command points -> profile (game-end CONTINUE).
    void store_profile();

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
    std::unique_ptr<NavMesh> nav_;
    std::unique_ptr<AiSystem> ai_;
    std::unique_ptr<CharPhysics> phys_;  // destroyed before world_
    std::unique_ptr<MissionHost> host_;
    std::unique_ptr<Vm> vm_;
    MissionState state_;
    Places places_;
    MapObjects objects_;
    MissionUi ui_;
    std::uint8_t last_finish_ = 0;
    bool force_objectives_ = false;
};

}  // namespace joa::game
