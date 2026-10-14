// Every shipped .pfd parses exactly to EOF; S1L1 matches the counts in GAMEPLAY_AI_TROOPS.md Part C §1.1.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/pfd_mesh.hpp"
#include "formats/vfs_fs.hpp"

using namespace joa::formats;
namespace fs = std::filesystem;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "joan.exe")) return 77;
    Vfs vfs(dir);
    const auto files = vfs.find_by_extension(".pfd");
    CHECK(files.size() == 38, "expected 38 .pfd files, got " + std::to_string(files.size()));
    bool saw_s1l1 = false;
    for (const auto& path : files) {
        const std::string where = path.string();
        auto bytes = read_file(path);
        CHECK(bytes, where);
        std::string err;
        auto m = parse_pfd(*bytes, &err);
        CHECK(m, where + ": " + err);
        CHECK(!m->polys.empty() && m->grid, where);
        const bool world = path.stem().string().back() == 'w';
        if (world) CHECK(m->models.empty() && m->grid->cell_w == 2, where + ": world mesh");
        if (path.stem() == "S1L1") {
            saw_s1l1 = true;
            int walk = 0;
            for (auto& p : m->polys) walk += p.type == 1;
            CHECK(m->models.size() == 7 && m->points.size() == 968 && m->polys.size() == 595, where);
            CHECK(walk == 231 && m->links.size() == 609, where);
            CHECK(m->grid->cols == 257 && m->grid->rows == 257 && m->grid->cell_w == 64, where);
        }
    }
    CHECK(saw_s1l1, "S1L1.pfd");
    return 0;
}
