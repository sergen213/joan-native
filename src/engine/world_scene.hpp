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
    int water = 0;
    std::string stage;        // .day lighting stage in use
};

struct PlacedObject {
    const Model* model = nullptr;
    glm::mat4 transform{1.0f};
    glm::vec3 center{0.0f};  // world bounding sphere
    float radius = 0.0f;
};

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
    Lighting lighting() const { return lighting_; }

    // Bilinear terrain height (engine Y) under an engine-space point.
    float ground_height(glm::vec3 p) const { return grid_.height_at(p.x, -p.z); }
    // Joan's scripted start (script/scenario/<name>.sce set_unit_pos / set_unit_dir), else the .pla
    // StartPointPlace, else the map centre. Heading: degrees CCW from original +X (HIGH: Joan 115 and
    // Jean 295 face each other in S1L1, as in the original capture); returned in radians.
    glm::vec3 start_position() const { return start_; }
    float start_yaw() const { return start_yaw_; }
    struct ScriptedUnit {
        std::string code;  // unit code, e.g. JOAN_OF_ARC -> unit/joan_of_arc.AM1, anim/joan_of_arc.am2
        glm::vec3 position{0.0f};  // engine space, on the terrain
        float heading = 0.0f;      // radians CCW from original +X
    };
    // Units the scenario script places at start (FUNC load_unit/add_unit + set_unit_pos/dir).
    const std::vector<ScriptedUnit>& scripted_units() const { return units_; }

    // Appends the visible draws (sky, terrain, water, objects within the frustum).
    void build(const glm::mat4& view_proj, glm::vec3 camera_pos, std::vector<DrawItem>& draws) const;

private:
    void load_terrain(const ScenarioPaths& p, const formats::BlockText& stg);
    void load_objects(const formats::BlockText& stg);
    void load_lighting(const ScenarioPaths& p, float clock_minutes);
    void load_start(const ScenarioPaths& p);

    AssetCache& assets_;
    formats::TerrainGrid grid_;
    WorldStats stats_;
    MeshHandle terrain_mesh_, water_mesh_;
    TextureHandle lightmap_;
    std::vector<DrawItem> terrain_draws_;  // model matrix identity
    std::vector<DrawItem> water_draws_;
    std::vector<PlacedObject> objects_;
    Lighting lighting_;
    glm::vec3 start_{0.0f};
    float start_yaw_ = 0.0f;
    std::vector<ScriptedUnit> units_;
};

}  // namespace joa::engine
