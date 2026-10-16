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
#include "engine/water_waves.hpp"
#include "engine/world_scene.hpp"
#include "game/mission.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
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
    // Engine-space water surface height over engine_pos (WaterField::surface_height of every node), nullopt if dry.
    std::optional<float> water_height(glm::vec3 engine_pos) const;
    // Interactive water (world_view_water.cpp): the wave field after the last frame() (FrameData::waves via data()).
    const WaveField& waves() const { return waves_; }
    AssetCache& assets() { return *assets_; }  // RtsFinish: GameCamera picks map items by their model bounds
    // Stage lighting with the fog/clear colour of the current view direction (refreshed by frame()).
    Lighting lighting() const { return lighting_; }
    // CONFIG.DAT 0x9C shadow mode (GameOptions::shadow): 0 none, 1 ShadowModel blobs only, 2 a stencil
    // volume for the player's hero only, 3 volumes for every unit (Scene::draw_shadow actor loop 0x5f1cc0).
    int shadow_mode = 3;
    // RTS highlight (Unit 0x519250): units drawn with their select*.SMO ring (world_view_rings.cpp); null = none.
    const std::vector<int>* selection = nullptr;

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
    // Body mesh game path of a unit's actor (unit.res Model / ModelName at its model level), empty if absent.
    std::string actor_model(int unit_id) const;
    struct ItemStats {
        int shots = 0, drops = 0;      // projectiles / floor items drawn by the last frame()
        int missing = 0;               // item models that failed to load (cumulative)
    };
    const ItemStats& item_stats() const { return item_stats_; }
    // Mesh game path drawn for a projectile / floor item id (World item id > 0, -MapObjects recno for map
    // items with a floor marker), empty if none.
    std::string shot_model(int projectile_id) const;
    std::string drop_model(int item_id) const;
    // Model matrix of a projectile at its latest tick position (identity if none).
    glm::mat4 shot_frame(int projectile_id) const;
    // Stage clip a map item (MapObjects recno) is drawn playing, empty while its static .stg object stands.
    std::string map_item_clip(int recno) const;

private:
    struct Actor {
        std::unique_ptr<Character> character;  // null: look missing (not retried)
        std::string anim_file, clip;           // current clip
        glm::vec3 p0{0.0f}, p1{0.0f};          // original world units
        float yaw0 = 0, yaw1 = 0;              // degrees
        float t0 = 0, t1 = 0;                  // clip ms; t1 may exceed the length after a loop wrap
        bool wrap = false;                     // t0 -> t1 crosses the loop end
        std::array<int, 3> items{-1, -1, -1};  // attached item ids (melee x2, ranged)
        int level = -1;                        // Unit model level of `character` (+0x3c5)
        bool own_anim = false;                 // level with its own animation group: holds its idle
        std::string model;                     // body mesh game path
        const game::ClipInfo* root = nullptr;  // root-motion clip being drawn (Bip01 x/y stripped), else null
        float speed = 0;                       // horizontal units/s over the last tick
        // Interactive water: when the standing unit last bobbed (time_s_) and the weapon tips of the last frame.
        float bob_t = -1e9f;
        struct Tip {
            glm::vec3 p{0.0f}, mark{0.0f};  // engine position; where it last kicked the water
            float at = -1e9f;               // time_s_ of that kick
        };
        std::vector<Tip> tips;
    };
    // Projectile (ThrowItem): the ammo item's level-2 mesh along the velocity, interpolated.
    struct Shot {
        const Model* model = nullptr;
        std::string path;
        glm::vec3 p0{0.0f}, p1{0.0f};  // original world units
        glm::vec3 dir{1.0f, 0.0f, 0.0f};
        int item = 0;
    };
    // World item on the floor (Unit_dropItem): level-0 mesh, AM1 pickup models play their clip.
    struct Drop {
        std::unique_ptr<Character> anim;
        const Model* model = nullptr;
        std::string path;
        glm::vec3 pos{0.0f};
        float yaw = 0;  // radians, the dropping unit's heading / map item HD
        int item = 0;   // item.res id (map items: a reused recno with another item is a new drop)
    };
    // Map item drawn by a WorldScene .stg object (the scene draws every CreateObject statically). Deleted
    // (modify_item 0x4afb20: dead -> FUN_0049f370) -> the object is hidden. A stage drop whose stage AnimName
    // runs on the shown model (stage_down: box "break" 0x48a1f0, gate/obstacle stage clips) plays that clip on
    // the object's AM1 in its place, holding the last frame; a box opened by interact fades out
    // (process 0x48ba70: actor+0x30 -0.5 over 2 s) before its deletion.
    struct MapActor {
        int recno = 0, res = 0, stage = 0;
        std::size_t object = 0;  // WorldScene::objects() index
        std::unique_ptr<Character> anim;
        std::string clip;
        int start = 0;  // game ms the clip started
        float t0 = 0, t1 = 0, fade = 1;
    };
    void sync_map_items(const game::World& world);
    void build_map_items(float alpha, std::vector<DrawItem>& draws);
    void sync_items(const game::World& world, const game::Snapshot& prev, const game::Snapshot& cur);
    void build_items(float alpha, std::vector<DrawItem>& draws);
    const Model* item_model(const std::string& path);
    void spawn_item_emitter(int item, int slot, const game::Vec3& at);
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
    void build_rings(std::vector<DrawItem>& draws);  // world_view_rings.cpp, after frames_ is filled
    // Interactive water (world_view_water.cpp). wet: open water at engine x, z (a wave field cell, not a wall);
    // ripple: a wave kick at engine `p` (x/z) of the given strength (0.15 bob .. 1 splash); splash: one there when p
    // lies over water within `reach` of its surface, with the jumpintowatersplash_s spray for strength 1;
    // splash_segment: one where the original-space segment a -> b goes down through a surface; wade: a posed unit's
    // body (an obstacle, a wake when moving) and weapons.
    bool wet(glm::vec2 p) const;
    void ripple(glm::vec3 p, float strength);
    void splash(glm::vec3 p, float strength, float reach);
    void splash_segment(const glm::vec3& a, const glm::vec3& b, float strength);
    void wade(int id, Actor& a, const glm::mat4& t, float dt);

    formats::Vfs vfs_;
    std::unique_ptr<AssetCache> assets_;
    std::unique_ptr<WorldScene> scene_;
    std::vector<Node> nodes_;
    std::unique_ptr<SkyDome> sky_;
    std::unique_ptr<ParticleSystem> effects_;
    UnitDb db_;
    const game::MapObjects* objects_;                // mission map items (box drops, script add_item)
    std::map<std::string, ScriptEmitters> scripts_;  // lower-case animscr Code
    const game::World* world_ = nullptr;             // last sync()
    std::unordered_map<int, Actor> actors_;
    std::unordered_map<int, glm::mat4> frames_;      // unit model matrices of the last frame()
    int player_ = 0;
    float time_s_ = 0;  // rendered time, drives sky drift and water
    Lighting lighting_;
    UnitStats unit_stats_;
    ShadowStats shadow_stats_;
    std::unordered_map<int, Shot> shots_;
    std::unordered_map<int, Drop> drops_;
    std::unordered_map<int, game::Vec3> gone_;      // map drops removed by the last sync (PICKUP emitter spot)
    std::unordered_map<std::string, const Model*> item_models_;  // null = failed
    ItemStats item_stats_;
    std::vector<MapActor> map_actors_;
    WaveField waves_{[this](glm::vec2 p) { return wet(p); }};
};

}  // namespace joa::engine
