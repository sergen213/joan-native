// All map/*/*.GRI decode exactly to EOF; heights agree with the Z of objects placed in the
// matching .stg (objects sit on the terrain), proving the x-major node order and int16 height.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "formats/map_grid.hpp"
#include "formats/vfs_fs.hpp"
#include "formats/world_text.hpp"

using namespace joa::formats;
namespace fs = std::filesystem;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "joan.exe")) return 77;
    Vfs vfs(dir);

    const auto gris = vfs.find_by_extension(".gri");
    CHECK(gris.size() == 32, "expected 32 .gri files");
    int checked_maps = 0;
    for (const auto& path : gris) {
        const std::string where = path.string();
        auto bytes = read_file(path);
        std::string err;
        auto g = parse_gri(*bytes, &err);
        CHECK(g, where + ": " + err);
        CHECK(g->header_c == 4 && !g->sector_names.empty() && !g->textures.empty(), where);
        int valid = 0;
        for (int x = 0; x < g->verts_x(); ++x)
            for (int y = 0; y < g->verts_y(); ++y) {
                if (!g->valid(x, y)) continue;
                ++valid;
                CHECK(g->sector(x, y) <= g->sector_names.size(), where + ": sector id");
                CHECK(g->tex1(x, y) <= g->textures.size() && g->tex2(x, y) <= g->textures.size(), where + ": tex id");
            }
        CHECK(valid > g->verts_x() * g->verts_y() / 2, where + ": mostly valid vertices");

        auto stg_path = path;
        stg_path.replace_extension(".stg");
        auto stg_bytes = read_file(stg_path);
        CHECK(stg_bytes, stg_path.string());
        auto stg = parse_block_text(std::string(stg_bytes->begin(), stg_bytes->end()), &err);
        CHECK(stg && stg->blocks[0].type == "CreateGrid", stg_path.string());
        CHECK(stg->blocks[0].number("X") == g->cells_x && stg->blocks[0].number("Y") == g->cells_y,
              where + ": grid size vs .stg CreateGrid");
        std::size_t sectors = 0;
        std::vector<double> dz;
        for (const auto& b : stg->blocks) {
            if (b.type == "CreateSceneSector") ++sectors;
            if (b.type != "CreateObject") continue;
            const double x = b.number("X"), y = b.number("Y"), z = b.number("Z");
            if (x < 0 || y < 0 || x > g->cells_x * TerrainGrid::kCellSize || y > g->cells_y * TerrainGrid::kCellSize)
                continue;
            dz.push_back(std::fabs(z - g->height_at(float(x), float(y))));
        }
        CHECK(sectors == g->sector_names.size(), where + ": sector count vs .stg");
        if (dz.size() >= 20) {
            // Many objects stand on walls/roofs (S7L1_2 siege map), so require only a quarter on the
            // ground; transposed axes give ~4% on S1L1 versus 82% here.
            const double on_ground =
                double(std::count_if(dz.begin(), dz.end(), [](double d) { return d < 40.0; })) / double(dz.size());
            std::printf("%-12s %3dx%-3d sectors=%-3zu tex=%-2zu objects=%-4zu on-ground=%.0f%%\n",
                        path.filename().c_str(), g->cells_x, g->cells_y, sectors, g->textures.size(), dz.size(),
                        on_ground * 100);
            CHECK(on_ground > 0.25, where + ": objects do not sit on decoded terrain");
            ++checked_maps;
        }
    }
    CHECK(checked_maps >= 25, "height cross-check ran on most maps");
    std::printf("gri ok: %zu files, %d height-checked\n", gris.size(), checked_maps);
    return 0;
}
