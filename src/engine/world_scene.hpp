#pragma once
// Original scenario world: GRI terrain, .stg objects, .day/.lgt/.sca lighting and fog, start pose.
// Evidence and open questions: docs/GRAPHICS_ROADMAP.md "First original scene".
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
    int map_units = 0;        // .uni CreateUnit blocks
    std::string stage;        // .day lighting stage in use
};

struct PlacedObject {
    const Model* model = nullptr;
    glm::mat4 transform{1.0f};
    glm::vec3 center{0.0f};  // world bounding sphere
    float radius = 0.0f;
    std::uint8_t light_count = 0;  // lights of its scene sector (DrawItem::lights)
    std::array<std::uint16_t, kMaxDrawLights> lights{};
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

class WorldScene {
public:
    // Throws std::runtime_error when the scenario or its .gri is missing or malformed.
    // clock_minutes picks the lighting stage of the world .day cycle (minutes after midnight).
    // HYPOTHESIS: missions start at noon; matches the original S1L1 start capture
    // (work/reference/shots/s04_04_mission1_hud.png: white daylight, blue-grey haze).
    WorldScene(AssetCache& assets, std::string_view scenario, float clock_minutes = 720.0f);
    ~WorldScene();
    WorldScene(const WorldScene&) = delete;
    WorldScene& operator=(const WorldScene&) = delete;

    const WorldStats& stats() const { return stats_; }
    const formats::TerrainGrid& grid() const { return grid_; }
    const std::vector<PlacedObject>& objects() const { return objects_; }
    // Stage lighting at the start clock; fog_color for a level view (WorldView recomputes it per frame
    // with fog_color()); point_lights = every scene light (flickering ones change in tick_lights()).
    Lighting lighting() const;
    glm::vec3 fog_color(glm::vec3 view_dir) const { return scattering_fog_color(scattering_, view_dir); }
    // Advances RANDOM_ATTENUATION light flicker by one 30 Hz tick.
    void tick_lights();
    // Appends the lights of the scene sector containing engine-space p (at most `max`, .stg order) to d.
    void sector_lights(glm::vec3 p, int max, DrawItem& d) const;
    // .day CloudTexture1, CloudTexture2 (e.g. newcloud02, newcloud01); empty without a .day.
    const std::vector<std::string>& cloud_textures() const { return clouds_; }

    // Bilinear terrain height (engine Y) under an engine-space point.
    float ground_height(glm::vec3 p) const { return grid_.height_at(p.x, -p.z); }
    // Joan's scripted start (script/scenario/<name>.sce set_unit_pos / set_unit_dir), else the .pla
    // StartPointPlace, else the map centre. Heading: degrees CCW from original +X (HIGH: Joan 115 and
    // Jean 295 face each other in S1L1, as in the original capture); returned in radians.
    glm::vec3 start_position() const { return start_; }
    float start_yaw() const { return start_yaw_; }
    struct StartUnit {
        std::string code;  // unit.res record, e.g. JOAN_OF_ARC, ENGLISH_ARCHER (attach_items.hpp UnitDb)
        glm::vec3 position{0.0f};  // engine space, on the terrain
        float heading = 0.0f;      // radians CCW from original +X
    };
    // Units the scenario script places at start (FUNC load_unit/add_unit + set_unit_pos/dir), then
    // the map's .uni armies.
    const std::vector<StartUnit>& units() const { return units_; }

    // Appends the visible draws (terrain, objects within the frustum).
    void build(const glm::mat4& view_proj, glm::vec3 camera_pos, std::vector<DrawItem>& draws) const;

private:
    void load_terrain(const ScenarioPaths& p, const formats::BlockText& stg);
    void load_objects(const formats::BlockText& stg);
    void load_lighting(const ScenarioPaths& p, float clock_minutes);
    void load_lights(const formats::BlockText& stg);
    void load_start(const ScenarioPaths& p);
    int sector_at(glm::vec3 p) const;
    void add_light(PointLight light, int sector);

    AssetCache& assets_;
    formats::TerrainGrid grid_;
    WorldStats stats_;
    MeshHandle terrain_mesh_;
    TextureHandle lightmap_;
    std::vector<DrawItem> terrain_draws_;  // model matrix identity
    std::vector<PlacedObject> objects_;
    Lighting lighting_;
    Scattering scattering_;
    std::vector<PointLight> lights_;
    struct Flicker {
        std::vector<std::uint16_t> lights;  // one .stg Group (a light copied into several sectors)
        float min = 0, max = 0;             // ATT1 range (ATT1 * (1 -/+ ModifyRange))
    };
    std::vector<Flicker> flicker_;
    std::uint32_t flicker_rng_ = 0x2545f491u;
    struct Sector {
        glm::vec2 min{0.0f}, max{0.0f};  // original X/Y
        std::vector<std::uint16_t> lights;
    };
    std::vector<Sector> sectors_;
    glm::vec3 start_{0.0f};
    float start_yaw_ = 0.0f;
    std::vector<StartUnit> units_;
    std::vector<std::string> clouds_;
};

}  // namespace joa::engine
