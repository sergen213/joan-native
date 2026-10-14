#pragma once
// Dynamic grass, reverse engineered from dyngrass.cpp (joan.exe FUN_00464740 per-frame update,
// FUN_00465a20 patch generator, FUN_00466560 tuft merge, FUN_00487f70 DynGrass.res record parser):
// - The world is cut into 3x3-cell patches (192x192 units). A patch picks the ground texture that
//   covers at least half of its 3x3 cells (cell texture = tex2 when the mean node blend byte of its
//   four corners is < 127.5, else tex1; Level_load_terrain) and looks up item/DynGrass.res
//   "<texture>_M<scenario>" first, then "<texture>".
// - Per record "<item> : count : percent : yaw : tilt" (degrees): with chance percent/100 the patch
//   gets max(1, count * density / 100) tufts at uniformly random positions inside the patch,
//   skipped where |dh/dx| or |dh/dy| >= 0.4 and sunk by 10 units where |dh/dx| >= 0.3.
//   yaw < 0 and tilt == 0: one of 32 yaws; yaw < 0: random yaw, tilt (tilt - 10..-6 deg);
//   yaw >= 0: yaw (-20..-11 deg) [, tilt (-10..-6 deg)]. Tilt is about the tuft's own Y axis.
// - Patches are cached while inside the square camera-eye +-radius window; when more than one is
//   cached, one new patch is generated per frame. A patch whose centre is farther than radius is
//   hidden; beyond 100 units it is alpha blended with alpha 255 - 128 (d - 100) / (radius - 100).
// - radius = CONFIG.DAT grass distance (>= 100), density = CONFIG.DAT grass density percent.
// Not reproduced: the wind sway (vertex blend weight 1 - f (z / zmax)^2 against a gusting offset
// matrix of up to 10 units) and the original's unseeded Misc_random (here hashed per patch).
#include "engine/assets_cache.hpp"
#include "formats/map_grid.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace joa::engine {

class GrassField {
public:
    // scenario: N of map/scenarioN (selects the "_M<N>" DynGrass.res variants).
    GrassField(AssetCache& assets, const formats::TerrainGrid& grid, int scenario);
    ~GrassField();
    GrassField(const GrassField&) = delete;
    GrassField& operator=(const GrassField&) = delete;

    void build(glm::vec3 camera, std::vector<DrawItem>& draws);
    std::size_t tufts() const;                              // in the cached patches
    std::size_t visible_patches() const { return visible_; }  // drawn last build()
    float radius() const { return radius_; }

private:
    struct Kind {
        MeshData mesh;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> submeshes;  // index first, count
        std::vector<Material> materials;
        int count = 0, percent = 100;
        float yaw = -1, tilt = 0;  // radians
    };
    struct Patch {
        MeshHandle mesh;
        glm::vec3 centre{0.0f};  // engine space; vertices are relative to it
        std::vector<DrawItem> draws;
        std::size_t tufts = 0;
    };
    const std::vector<Kind>* kinds_at(int px, int py) const;
    Patch generate(int px, int py) const;

    AssetCache& assets_;
    const formats::TerrainGrid& grid_;
    int scenario_ = 0;
    float radius_ = 2000.0f;
    int density_ = 100;
    bool enabled_ = true;
    std::map<std::string, std::vector<Kind>> kinds_;  // lowercase DynGrass.res code -> records
    std::map<std::pair<int, int>, Patch> patches_;    // key: first cell of the patch (multiples of 3)
    std::size_t visible_ = 0;
};

}  // namespace joa::engine
