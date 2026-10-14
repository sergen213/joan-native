// Terrain queries the gameplay simulation needs (height / normal / walkability), independent of
// rendering. GriTerrain samples the scenario .GRI grid (formats/map_grid: 64-unit cells, bilinear
// height = what placed units sit on, CONFIRMED in CONTAINERS_WORLD_SAVES.md §4.1). Walkability is
// the GRI node "valid" byte (HYPOTHESIS: the original also uses PFD nav data, owned by AiTroops,
// whose implementation can replace this one through the same interface).
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "formats/map_grid.hpp"
#include "game/unit_state.hpp"

namespace joa::game {

class ITerrainQuery {
public:
    virtual ~ITerrainQuery() = default;
    virtual float height(float x, float y) const = 0;
    virtual Vec3 normal(float x, float y) const = 0;   // unit length, z up
    virtual bool walkable(float x, float y) const = 0;
};

class FlatTerrain final : public ITerrainQuery {
public:
    explicit FlatTerrain(float z = 0) : z_(z) {}
    float height(float, float) const override { return z_; }
    Vec3 normal(float, float) const override { return {0, 0, 1}; }
    bool walkable(float, float) const override { return true; }
private:
    float z_;
};

class GriTerrain final : public ITerrainQuery {
public:
    // game_dir + "map/scenarioN/<map>.gri" for a scenario code such as "S1L1".
    static std::unique_ptr<GriTerrain> load_scenario(const std::string& game_dir, const std::string& map, std::string* err = nullptr);
    explicit GriTerrain(formats::TerrainGrid g) : g_(std::move(g)) {}
    float height(float x, float y) const override { return g_.height_at(x, y); }
    Vec3 normal(float x, float y) const override;
    bool walkable(float x, float y) const override;
    const formats::TerrainGrid& grid() const { return g_; }
private:
    formats::TerrainGrid g_;
};

}  // namespace joa::game
