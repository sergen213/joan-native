// Parses the 8 campaign scenario files and checks every referenced script, objective,
// world, unit and objective-map file exists (case-insensitive game paths).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "formats/scenario_def.hpp"
#include "formats/vfs_fs.hpp"
#include "formats/world_text.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

static std::string_view sv(const std::vector<std::uint8_t>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

int main() {
    CHECK(!parse_scenario_def("code : X\nStepNo : 1\nWorldFileName : a\n"), "truncated step rejected");

    const char* env = std::getenv("JOA_GAME_DIR");
    if (!env || !fs::exists(fs::path(env) / "scenario")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }
    Vfs vfs(env);

    for (int n = 1; n <= 8; ++n) {
        char name[32];
        std::snprintf(name, sizeof name, "SCENARIO\\scenario_%02d.scr", n);
        auto bytes = vfs.read(name);
        CHECK(bytes, "missing %s", name);
        if (!bytes) continue;
        std::string err;
        auto def = parse_scenario_def(sv(*bytes), &err);
        CHECK(def && def->steps.size() == 1, "%s: %s", name, err.c_str());
        if (!def) continue;
        for (const auto& s : def->steps) {
            CHECK(vfs.exists("SCRIPT\\SCENARIO\\" + s.script_file + ".sce"), "%s script %s", name, s.script_file.c_str());
            CHECK(vfs.exists("SCRIPT\\SCENARIO\\" + s.description_file + ".sce"), "%s des %s", name, s.description_file.c_str());
            CHECK(vfs.exists("SCRIPT\\SCENARIO\\" + s.objective_file + ".obj"), "%s obj %s", name, s.objective_file.c_str());
            // UnitFileName only exists for scenarios 1-2; later maps ship per world-node unit
            // files "<stem>_N.uni" (e.g. scenario3\S3L1_1.uni), so the field is vestigial there.
            std::string stem = s.unit_file.substr(0, s.unit_file.size() - 4);
            CHECK(vfs.exists("MAP\\" + s.unit_file) || vfs.exists("MAP\\" + stem + "_1.uni"), "%s unit %s", name, s.unit_file.c_str());
            CHECK(vfs.exists("MAP\\" + s.world_file + ".wor"), "%s world %s", name, s.world_file.c_str());
            auto bm = vfs.read("SCRIPT\\SCENARIO\\" + s.objective_map_file + ".bm");
            CHECK(bm, "%s objective map %s", name, s.objective_map_file.c_str());
            if (bm) {
                auto doc = parse_block_text(sv(*bm), &err);
                CHECK(doc && doc->blocks.size() >= 2 && doc->blocks.back().type == "CreateMapActor", "%s .bm: %s", name, err.c_str());
            }
            std::printf("%s %s script=%s world=%s\n", name, def->code.c_str(), s.script_file.c_str(), s.world_file.c_str());
        }
    }
    return failures ? 1 : 0;
}
