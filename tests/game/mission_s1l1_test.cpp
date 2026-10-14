// S1L1 on the real World for 9000 ticks (5 min of game time): every builtin the scripts call has
// a handler, no script errors, deterministic (same hash twice).
#include <cstdio>
#include <cstdlib>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    std::uint64_t hashes[2] = {};
    int fails = 0;
    for (auto& h : hashes) {
        CampaignRunner run(*gd, vfs);
        if (!run.init(&err) || !run.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;
        Mission& m = *run.mission();
        m.autosave = false;
        for (int t = 0; t < 9000; ++t) m.tick();
        h = m.hash();
        std::printf("S1L1 9000 ticks: finish %d stage %d calls %llu hash %016llx\n", m.finish(), m.scenario().stage,
                    static_cast<unsigned long long>(m.host().calls), static_cast<unsigned long long>(h));
        for (const auto& [id, n] : m.host().unimplemented_calls()) {
            std::printf("FAIL unimplemented builtin %d %s (%llu calls)\n", id,
                        std::string(formats::script_funcs()[id - 1].name).c_str(), static_cast<unsigned long long>(n));
            ++fails;
        }
        for (const auto& e : m.vm().errors()) std::printf("FAIL script error: %s\n", e.c_str()), ++fails;
        if (m.host().calls == 0) std::printf("FAIL no builtin calls\n"), ++fails;
    }
    if (hashes[0] != hashes[1]) std::printf("FAIL nondeterministic\n"), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
