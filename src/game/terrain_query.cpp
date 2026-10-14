#include "game/terrain_query.hpp"

#include <cctype>
#include <cmath>

#include "formats/vfs_fs.hpp"

namespace joa::game {

std::unique_ptr<GriTerrain> GriTerrain::load_scenario(const std::string& game_dir, const std::string& map, std::string* err) {
    // "S1L1" -> map/scenario1/S1L1.gri
    std::string n;
    for (size_t i = 1; i < map.size() && std::isdigit(static_cast<unsigned char>(map[i])); ++i) n += map[i];
    formats::Vfs vfs(game_dir);
    auto bytes = vfs.read("map/scenario" + n + "/" + map + ".gri");
    if (!bytes) {
        if (err) *err = "no map/scenario" + n + "/" + map + ".gri";
        return nullptr;
    }
    auto g = formats::parse_gri(*bytes, err);
    if (!g) return nullptr;
    return std::make_unique<GriTerrain>(std::move(*g));
}

Vec3 GriTerrain::normal(float x, float y) const {
    const float d = formats::TerrainGrid::kCellSize * 0.5f;
    const float dx = height(x + d, y) - height(x - d, y), dy = height(x, y + d) - height(x, y - d);
    Vec3 n{-dx, -dy, 2 * d};
    const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return {n.x / l, n.y / l, n.z / l};
}

bool GriTerrain::walkable(float x, float y) const {
    const int cx = int(std::floor(x / formats::TerrainGrid::kCellSize + 0.5f));
    const int cy = int(std::floor(y / formats::TerrainGrid::kCellSize + 0.5f));
    if (cx < 0 || cy < 0 || cx >= g_.verts_x() || cy >= g_.verts_y()) return false;
    return g_.valid(cx, cy);
}

}  // namespace joa::game
