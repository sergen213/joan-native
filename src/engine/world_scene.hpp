#pragma once
// Original scenario world: GRI terrain, .stg objects, .day/.lgt/.sca lighting and fog. A mission map is
// one or more world nodes (<world>.wor CreateNode, game::world_nodes): each node's .gri/.stg/lightmaps
// use node-local X/Y, placed at the node origin. Evidence: docs/GRAPHICS_ROADMAP.md "First original scene".
#include "engine/assets_cache.hpp"
#include "engine/renderer.hpp"
#include "formats/map_grid.hpp"
#include "formats/world_text.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::engine {

// In-game view: near 4, far 4096 (HIGH: FUN_005634e0 -> 0x613ca0(4.0, far, 16384) with far = 4096,
// the view distance cap); objects beyond the far plane are clipped, the sky stays inside it.
inline constexpr float kWorldNear = 4.0f, kWorldFar = 4096.0f;

// "S1L1" -> {map/scenario1, S1L1}; also accepts a path to the .stg/.gri (with or without extension).
struct ScenarioPaths {
    std::string dir;   // game path of the map directory, e.g. "map/scenario1"
    std::string name;  // map base name, e.g. "S1L1"
};
std::optional<ScenarioPaths> find_scenario(const formats::Vfs& vfs, std::string_view scenario);

struct WorldStats {
    int objects = 0;          // CreateObject blocks
    int placed = 0;           // objects with a loaded model
    int missing_models = 0;   // objects whose model file is absent or unparsable
    std::vector<std::string> missing;  // unique missing model paths
    int terrain_triangles = 0;
    int terrain_groups = 0;   // (ground type 1, ground type 2) draw groups
    int lightmaps = 0;        // sector lightmaps found
    int lights = 0;           // point lights: .stg CreateLight blocks + item .pro HasLight
    std::string stage;        // .day lighting stage in use
};

struct PlacedObject {
    const Model* model = nullptr;
    glm::mat4 transform{1.0f};
    glm::vec3 center{0.0f};  // world bounding sphere
    float radius = 0.0f;
    std::uint8_t light_count = 0;  // lights of its scene sector (DrawItem::lights)
    std::array<std::uint16_t, kMaxDrawLights> lights{};
    // .stg identity, so WorldView can tie the object to its game::MapItem: Path + ObjectFile (ItemRes code),
    // original world position (node origin + X/Y, Z), model / AM2 game paths (AM1 props: the clip file).
    std::string code, path, anim;
    glm::vec3 at{0.0f};
    bool hidden = false;  // map item deleted or drawn animated by WorldView: build() skips it
};

// .sca stage record (map/common.sca): Hoffman-Preetham scattering parameters.
struct Scattering {
    float rayleigh = 0.68f, mie = 0.292f, inscattering = 0.156f, g = 0.32f, distance_modifier = 1.0f;
    glm::vec3 sun_rgb{0.336f, 0.494f, 0.49f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};  // engine space, normalised
};
// Fog and clear colour of the original (View3D_render 0x614760): the inscattered light 2000 units
// along the view direction, quantised to bytes. Gamma space. CONFIRMED formula.
glm::vec3 scattering_fog_color(const Scattering& s, glm::vec3 view_dir);

// One world node: map base name (files <dir>/<name>.gri/.stg) at an origin in original X/Y.
struct MapNode {
    std::string name;
    glm::vec2 origin{0.0f};
};

class WorldScene {
public:
    // Throws std::runtime_error when a node's .gri/.stg is missing or malformed.
    // clock_minutes picks the lighting stage of the world .day cycle (minutes after midnight).
    // HYPOTHESIS: missions start at noon; matches the original S1L1 start capture
    // (work/reference/shots/s04_04_mission1_hud.png: white daylight, blue-grey haze).
    // dir: game path of the map directory ("map/scenario3"); world: .wor/.day stem ("S3L1w").
    WorldScene(AssetCache& assets, const std::string& dir, const std::vector<MapNode>& nodes, const std::string& world,
               float clock_minutes = 720.0f);
    // One map at the origin by scenario name (find_scenario), world .day <name>w.day.
    WorldScene(AssetCache& assets, std::string_view scenario, float clock_minutes = 720.0f);
    ~WorldScene();
    WorldScene(const WorldScene&) = delete;
    WorldScene& operator=(const WorldScene&) = delete;

    struct Node {
        MapNode map;
        formats::TerrainGrid grid;
        WorldStats stats;  // this node's share (stage empty)
        MeshHandle terrain_mesh;
        TextureHandle lightmap;
        std::vector<DrawItem> terrain_draws;  // model = translation to the origin
        glm::vec3 offset() const { return to_engine(map.origin.x, map.origin.y, 0.0f); }
        bool contains(float x, float y) const {  // original world X/Y, edges inclusive (game NodeTerrain)
            const float s = formats::TerrainGrid::kCellSize;
            return x >= map.origin.x && y >= map.origin.y && x <= map.origin.x + grid.cells_x * s && y <= map.origin.y + grid.cells_y * s;
        }
    };
    const std::vector<Node>& nodes() const { return nodes_; }
    const WorldStats& stats() const { return stats_; }  // every node
    const std::vector<PlacedObject>& objects() const { return objects_; }
    void hide(std::size_t object) { objects_[object].hidden = true; }  // permanent (see PlacedObject::hidden)
    // Lighting at the current clock (set_clock); fog_color for a level view (WorldView recomputes it per frame
    // with fog_color()); point_lights = every scene light (flickering ones change in tick_lights()).
    Lighting lighting() const;
    // Day/night: blends the .day stages around `minutes` after midnight (wraps) into lighting() and fog_color().
    void set_clock(float minutes);
    float clock() const { return clock_; }
    glm::vec3 fog_color(glm::vec3 view_dir) const { return scattering_fog_color(scattering_, view_dir); }
    // Advances RANDOM_ATTENUATION light flicker by one 30 Hz tick.
    void tick_lights();
    // Appends the lights of the scene sector containing engine-space p (at most `max`, .stg order) to d.
    void sector_lights(glm::vec3 p, int max, DrawItem& d) const;
    // .day CloudTexture1, CloudTexture2 (e.g. newcloud02, newcloud01); empty without a .day.
    const std::vector<std::string>& cloud_textures() const { return clouds_; }

    // Bilinear terrain height (engine Y) under an engine-space point: the first node containing it, else
    // the first node (same rule as the game's NodeTerrain).
    float ground_height(glm::vec3 p) const;

    // Appends the visible draws (terrain, objects within the frustum).
    void build(const glm::mat4& view_proj, glm::vec3 camera_pos, std::vector<DrawItem>& draws) const;

private:
    void load_terrain(Node& n, const formats::BlockText& stg);
    void load_objects(Node& n, const formats::BlockText& stg);
    void load_lighting(const std::string& dir, const std::string& world, const std::string& map, float clock_minutes);
    void load_lights(Node& n, const formats::BlockText& stg);
    int sector_at(glm::vec3 p) const;
    void add_light(PointLight light, int sector, WorldStats& st);

    AssetCache& assets_;
    std::vector<Node> nodes_;
    WorldStats stats_;
    std::vector<PlacedObject> objects_;
    Lighting lighting_;
    Scattering scattering_;
    struct Stage {  // one .day stage with its .lgt sun/ambient and .sca record
        std::string name;
        float start = 0;                                   // StartTime, minutes after midnight
        glm::vec3 sun{0.6f}, ambient{0.25f};               // gamma space 0..1
        float vdir = 60.0f, hdir = 0.0f;                   // degrees
        Scattering scattering;
    };
    std::vector<Stage> stages_;  // by StartTime
    float clock_ = 720.0f;
    std::vector<PointLight> lights_;
    struct Flicker {
        std::vector<std::uint16_t> lights;  // one .stg Group (a light copied into several sectors)
        float min = 0, max = 0;             // ATT1 range (ATT1 * (1 -/+ ModifyRange))
    };
    std::vector<Flicker> flicker_;
    std::uint32_t flicker_rng_ = 0x2545f491u;
    struct Sector {
        glm::vec2 min{0.0f}, max{0.0f};  // original world X/Y
        std::vector<std::uint16_t> lights;
    };
    std::vector<Sector> sectors_;
    std::vector<std::string> clouds_;
};

}  // namespace joa::engine
