#pragma once
// Client-side view of a running mission: every world node of the map (terrain, objects, lights, water,
// grass, ambient particles), the sky, and one animated Character per game::Snapshot unit, interpolated
// between the last two 33 ms ticks. Gameplay state is read only from the snapshots / World.
#include "engine/attach_items.hpp"
#include "engine/camera.hpp"
#include "engine/character_actor.hpp"
#include "engine/grass_field.hpp"
#include "engine/particles.hpp"
#include "engine/sky_dome.hpp"
#include "engine/water.hpp"
#include "engine/world_scene.hpp"
#include "game/mission.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <unordered_map>

namespace joa::engine {

class WorldView {
public:
    // Loads every world node of the mission map (game::world_nodes) and the world .day lighting.
    // Throws on load failure.
    WorldView(IRenderer& renderer, const formats::Vfs& vfs, const game::Mission& mission,
              std::vector<std::filesystem::path> hd_packs = {});
    ~WorldView();

    // After each tick: spawns an actor per new snapshot unit id, drops actors of units that left it,
    // takes position/yaw/clip/clip time of prev -> cur and re-attaches equipment when it changed.
    void sync(const game::World& world, const game::Snapshot& prev, const game::Snapshot& cur);
    // Attack-script / effect particles of the tick's events (animscr.res emitter slots, .txt effects).
    void on_events(const std::vector<game::GameEvent>& events, const game::Snapshot& cur);
    // Poses units at alpha in [0, 1] between the last two syncs and appends the frame's draws. Fills
    // view.view/camera_position and the in-game depth range of view.projection.
    void frame(float alpha, const CameraPose& pose, float dt, FrameView& view, std::vector<DrawItem>& draws);

    float ground_height(glm::vec3 engine_pos) const { return scene_->ground_height(engine_pos); }
    // Stage lighting with the fog/clear colour of the current view direction (refreshed by frame()).
    Lighting lighting() const { return lighting_; }
    // CONFIG.DAT 0x9C shadow mode (GameOptions::shadow): 0 none, 1 ShadowModel blobs only, 2 a stencil
    // volume for the player's hero only, 3 volumes for every unit (Scene::draw_shadow actor loop 0x5f1cc0).
    int shadow_mode = 3;

    // --- stats / tests ---------------------------------------------------------------------------
    const WorldScene& scene() const { return *scene_; }
    std::size_t node_count() const { return nodes_.size(); }
    const GrassField& grass(std::size_t node = 0) const { return *nodes_[node].grass; }
    const ParticleSystem& particles(std::size_t node = 0) const { return *nodes_[node].particles; }
    const WaterField& water(std::size_t node = 0) const { return *nodes_[node].water; }
    const ParticleSystem& effects() const { return *effects_; }  // event emitters
    struct UnitStats {
        int actors = 0, missing = 0;        // live actors / snapshot units without a loadable look
        int items = 0, items_missing = 0;   // equipment meshes attached / not (cumulative)
    };
    const UnitStats& unit_stats() const { return unit_stats_; }
    const Character* actor(int unit_id) const;  // null if absent (or its look failed to load)
    struct ShadowStats {
        int volumes = 0, models = 0;  // units drawn with a volume / with their ShadowModel, last frame()
    };
    const ShadowStats& shadow_stats() const { return shadow_stats_; }

private:
    struct Actor {
        std::unique_ptr<Character> character;  // null: look missing (not retried)
        std::string anim_file, clip;           // current clip
        glm::vec3 p0{0.0f}, p1{0.0f};          // original world units
        float yaw0 = 0, yaw1 = 0;              // degrees
        float t0 = 0, t1 = 0;                  // clip ms; t1 may exceed the length after a loop wrap
        bool wrap = false;                     // t0 -> t1 crosses the loop end
        std::array<int, 3> items{-1, -1, -1};  // attached item ids (melee x2, ranged)
    };
    void equip(Actor& a, const game::World& world, const game::Unit& u);
    struct Node {
        std::unique_ptr<GrassField> grass;
        std::unique_ptr<ParticleSystem> particles;
        std::unique_ptr<WaterField> water;
        glm::vec3 offset{0.0f};  // engine-space origin
    };
    struct SlotEmitter {  // animscr.res emitter slot (AnimScript_parse 0x419320, record +0x77..)
        std::string name;
        float start = 0, end = 0;  // ms after the trigger
        glm::vec3 offset{0.0f};    // unit frame, original axes
        int lock = 0;
    };
    struct ScriptEmitters {
        bool terrain = false;  // TerrainEmitterFlag: ground-type emitters (not reproduced)
        std::vector<SlotEmitter> slots;
    };
    void spawn_slot(const std::string& script, std::size_t slot, const game::UnitView& at);

    formats::Vfs vfs_;
    std::unique_ptr<AssetCache> assets_;
    std::unique_ptr<WorldScene> scene_;
    std::vector<Node> nodes_;
    std::unique_ptr<SkyDome> sky_;
    std::unique_ptr<ParticleSystem> effects_;
    UnitDb db_;
    std::map<std::string, ScriptEmitters> scripts_;  // lower-case animscr Code
    const game::World* world_ = nullptr;             // last sync()
    std::unordered_map<int, Actor> actors_;
    std::unordered_map<int, glm::mat4> frames_;      // unit model matrices of the last frame()
    int player_ = 0;
    float time_s_ = 0;  // rendered time, drives sky drift and water
    Lighting lighting_;
    UnitStats unit_stats_;
    ShadowStats shadow_stats_;
};

}  // namespace joa::engine
