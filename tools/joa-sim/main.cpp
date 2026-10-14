// joa-sim: headless gameplay scenario runner.
//   joa-sim [--game-dir DIR] --scenario S1L1 --duel joan english_light_infantry [--ticks 900] [--seed 0]
//           [--difficulty 1] [--level-a N] [--level-b N] [--quiet]
// Spawns the two units on the scenario terrain (.GRI), drives both with the stand-in DuelPad
// (A = player through the player controller, B = virtual pad), prints the event log, the final
// state and the World hash.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "game/world_sim.hpp"

using namespace joa::game;

static std::string unit_code(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (s == "JOAN") return "JOAN_OF_ARC";
    return s;
}

int main(int argc, char** argv) {
    std::string dir = std::getenv("JOA_GAME_DIR") ? std::getenv("JOA_GAME_DIR") : ".";
    std::string scenario = "S1L1", a_code = "JOAN_OF_ARC", b_code = "ENGLISH_LIGHT_INFANTRY";
    int ticks = 900, difficulty = 1, level_a = 0, level_b = 0;
    unsigned seed = 0;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (!std::strcmp(argv[i], "--game-dir")) dir = next();
        else if (!std::strcmp(argv[i], "--scenario")) scenario = next();
        else if (!std::strcmp(argv[i], "--duel")) a_code = unit_code(next()), b_code = unit_code(next());
        else if (!std::strcmp(argv[i], "--ticks")) ticks = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--seed")) seed = unsigned(std::strtoul(next().c_str(), nullptr, 0));
        else if (!std::strcmp(argv[i], "--difficulty")) difficulty = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--level-a")) level_a = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--level-b")) level_b = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else {
            std::fprintf(stderr, "usage: joa-sim [--game-dir DIR] --scenario S1L1 --duel A B [--ticks N] [--seed N] "
                                 "[--difficulty 0-3] [--level-a N] [--level-b N] [--quiet]\n");
            return 2;
        }
    }
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) { std::fprintf(stderr, "game data: %s\n", err.c_str()); return 1; }
    auto terrain = GriTerrain::load_scenario(dir, scenario, &err);
    if (!terrain) { std::fprintf(stderr, "terrain: %s\n", err.c_str()); return 1; }

    World w(*gd, terrain.get(), seed);
    w.difficulty = difficulty;
    // Placement: an English light-infantry spot of S1L1.uni ("Unit 74"); A stands 150 units west.
    const Vec3 at{8246.0f, 4839.0f, 0.0f};
    const int a = w.spawn_unit(a_code, gd->nation_id("FRENCH"), {at.x - 150.0f, at.y, 0}, 0.0f, level_a);
    const int b = w.spawn_unit(b_code, gd->nation_id("ENGLISH"), at, 180.0f, level_b);
    if (!a || !b) { std::fprintf(stderr, "unknown unit class\n"); return 1; }
    w.set_player(a);
    DuelPad pa, pb;
    pa.target = b, pa.combo = "AAAB", pa.approach = 90;
    pb.target = a, pb.combo = "AA", pb.approach = 70;
    w.attach_pad(b, [&pb](World& ww, const Unit& u) { return pb.next(ww, u); });

    auto print_unit = [&](const Unit& u) {
        std::printf("  #%d %-24s lvl %d HP %d/%d EP %d/%d bal %d pos (%.1f, %.1f, %.1f) yaw %.0f %s%s\n", u.id,
                    gd->units[size_t(u.cls)].code.c_str(), u.level, u.hp, w.max_hp(u), u.ep, w.max_ep(u), u.balance,
                    u.pos.x, u.pos.y, u.pos.z, u.yaw_deg, action_name(u.action), w.dead(u) ? " DEAD" : "");
    };
    std::printf("joa-sim %s duel %s vs %s, seed %u, %d ticks (33 ms)\n", scenario.c_str(), a_code.c_str(), b_code.c_str(), seed, ticks);
    for (const Unit& u : w.units()) print_unit(u);
    int ended = -1;
    for (int t = 0; t < ticks; ++t) {
        const Unit* ua = w.unit(a);
        w.tick(ua ? pa.next(w, *ua) : InputFrame{});
        for (const GameEvent& e : w.take_events()) {
            if (quiet && e.type != Ev::Hit && e.type != Ev::Death && e.type != Ev::LevelUp && e.type != Ev::Knockdown && e.type != Ev::Blocked) continue;
            std::printf("%6d ms  %-14s unit %d", e.time, ev_name(e.type), e.unit);
            if (e.other) std::printf(" other %d", e.other);
            if (e.value) std::printf(" value %d", e.value);
            if (!e.name.empty()) std::printf(" %s", e.name.c_str());
            std::printf("\n");
            if (e.type == Ev::Death && ended < 0) ended = e.time;
        }
    }
    std::printf("final state at %d ms:\n", w.now());
    for (const Unit& u : w.units()) print_unit(u);
    const Unit* ua = w.unit(a);
    const Unit* ub = w.unit(b);
    const char* winner = (!ub || w.dead(*ub)) ? a_code.c_str() : (!ua || w.dead(*ua)) ? b_code.c_str() : "none";
    std::printf("winner: %s%s\nhash: %016llx\n", winner, ended >= 0 ? "" : " (time limit)", static_cast<unsigned long long>(w.hash()));
    return 0;
}
