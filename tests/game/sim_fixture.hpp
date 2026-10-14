// Shared helpers for the gameplay simulation tests (game data from JOA_GAME_DIR; exit 77 = skip).
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "game/world_sim.hpp"

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)
#define NEAR(a, b, eps) (std::fabs(double(a) - double(b)) <= (eps))

inline std::string game_dir() {
    const char* env = std::getenv("JOA_GAME_DIR");
    return env ? env : "";
}

// Loads GameData or exits 77 when the original data is not available.
inline std::unique_ptr<joa::game::GameData> load_data_or_skip() {
    const std::string dir = game_dir();
    if (dir.empty() || !std::filesystem::exists(std::filesystem::path(dir) / "unit" / "unit.res")) {
        std::puts("SKIP: JOA_GAME_DIR has no game data");
        std::exit(77);
    }
    std::string err;
    auto gd = joa::game::GameData::load(dir, &err);
    if (!gd) {
        std::fprintf(stderr, "GameData::load: %s\n", err.c_str());
        std::exit(1);
    }
    return gd;
}

inline int finish() {
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("ok");
    return failures ? 1 : 0;
}
