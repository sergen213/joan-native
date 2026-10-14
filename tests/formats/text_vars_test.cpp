// Resource/GAMEPLAY.RES: every record parses as {SECTION}[KEY]value lines.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "formats/res_archive.hpp"
#include "formats/text_vars.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

int main() {
    {
        auto v = parse_game_vars("// c\n{UNIT}\n[BODY_R]32\t\t//#define X 32\n[HORSE_WD]\t500\n");
        CHECK(v && v->size() == 2 && (*v)[1].value == "500" && (*v)[0].comment == "#define X 32", "synthetic");
        CHECK(!parse_game_vars("BODY_R 32\n"), "garbage rejected");
    }

    const char* env = std::getenv("JOA_GAME_DIR");
    if (!env || !fs::exists(fs::path(env) / "Resource" / "GAMEPLAY.RES")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }

    std::string err;
    auto res = ResArchive::open(fs::path(env) / "Resource" / "GAMEPLAY.RES", &err);
    CHECK(res && res->entries().size() == 6, "GAMEPLAY.RES: %s", err.c_str());
    if (!res) return 1;
    std::vector<GameVar> all;
    for (const auto& e : res->entries()) {
        auto v = parse_game_vars(res->text(e.name).value_or(""), &err);
        CHECK(v && !v->empty(), "%s: %s", e.name.c_str(), err.c_str());
        if (v) all.insert(all.end(), v->begin(), v->end());
    }
    auto eq = [&](const char* s, const char* k, double want) {
        auto got = find_game_var(all, s, k);
        CHECK(got && std::fabs(*got - want) < 1e-9, "%s/%s = %g", s, k, got.value_or(-1));
    };
    eq("UNIT", "STR_DAM", 3.0);
    eq("UNIT", "DEF_RAT", 0.3);
    eq("UNIT", "HP_PER", 4);
    eq("KILL", "EP_GEN", 30);
    eq("GAME", "DIFF_D", 1.5);
    eq("THROWI", "CAN_FC", 120000);
    std::printf("GAMEPLAY.RES: %zu variables\n", all.size());
    return failures ? 1 : 0;
}
