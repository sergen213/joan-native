// joa-campaign: run campaign missions headless on the real World and print mission events.
//   joa-campaign [--mission N] [--ticks N] [--seed S] [--save-at T]... [--load FILE]
//                [--import FILE.SAV] [--win-at T] [--continue] [--game-dir DIR] [--trace]
// --save-at T   write a native save after tick T (native save dir, or --save-dir DIR)
// --load FILE   resume a native save; --import FILE.SAV start from an original save
// --win-at T    test hook: finish every WIN objective at tick T (normal win-delay path)
// --continue    after a win, continue to the next campaign mission (carried progression)
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "game/campaign_runner.hpp"
#include "game/savegame_import.hpp"

using namespace joa;
using namespace joa::game;

int main(int argc, char** argv) {
    std::string dir = std::getenv("JOA_GAME_DIR") ? std::getenv("JOA_GAME_DIR") : ".";
    std::string load_path, import_path, save_dir;
    long mission = 1, ticks = 9000, win_at = -1;
    std::uint32_t seed = 1;
    std::set<long> save_at;
    bool cont = false, trace = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--mission") mission = std::atol(next());
        else if (a == "--ticks") ticks = std::atol(next());
        else if (a == "--seed") seed = static_cast<std::uint32_t>(std::atol(next()));
        else if (a == "--save-at") save_at.insert(std::atol(next()));
        else if (a == "--save-dir") save_dir = next();
        else if (a == "--load") load_path = next();
        else if (a == "--import") import_path = next();
        else if (a == "--win-at") win_at = std::atol(next());
        else if (a == "--continue") cont = true;
        else if (a == "--game-dir") dir = next();
        else if (a == "--trace") trace = true;
        else {
            std::fprintf(stderr, "usage: joa-campaign [--mission N] [--ticks N] [--seed S] [--save-at T] [--save-dir DIR] "
                                 "[--load FILE] [--import FILE.SAV] [--win-at T] [--continue] [--game-dir DIR] [--trace]\n");
            return 2;
        }
    }
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::fprintf(stderr, "game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    run.seed = seed;
    if (!run.init(&err)) return std::fprintf(stderr, "campaign: %s\n", err.c_str()), 1;

    if (!load_path.empty()) {
        auto bytes = read_file(load_path);
        auto f = bytes ? decode_save(*bytes, &err) : std::nullopt;
        if (!f || !migrate_save(*f, &err) || !run.load(*f, &err)) return std::fprintf(stderr, "load %s: %s\n", load_path.c_str(), err.c_str()), 1;
        std::printf("loaded %s: scenario %d tick %llu\n", load_path.c_str(), run.mission()->scenario_no(),
                    static_cast<unsigned long long>(run.mission()->ticks()));
    } else if (!import_path.empty()) {
        auto bytes = read_file(import_path);
        auto o = bytes ? import_original_save(*bytes, *gd, &err) : std::nullopt;
        if (!o) return std::fprintf(stderr, "import %s: %s\n", import_path.c_str(), err.c_str()), 1;
        std::printf("import: \"%s\" scenario %d step %d stage %d gold %d end conditions %zu heroes %zu\n",
                    o->header.description.c_str(), o->scenario_no, o->step, o->stage, o->gold, o->end.size(), o->heroes.size());
        for (const auto& n : o->notes) std::printf("import note: %s\n", n.c_str());
        if (!run.start(o->scenario_no, &err)) return std::fprintf(stderr, "start: %s\n", err.c_str()), 1;
        apply_original_save(*run.mission(), *o);
    } else if (!run.start(int(mission), &err)) {
        return std::fprintf(stderr, "start: %s\n", err.c_str()), 1;
    }
    run.mission()->host().keep_log = trace;
    std::printf("mission %d (%s) seed %u\n", run.mission()->scenario_no(), run.mission()->info().stem().c_str(), seed);

    auto print_events = [&] {
        for (const auto& e : run.mission()->take_events())
            std::printf("t=%llu %s %d %d %s\n", static_cast<unsigned long long>(e.tick), event_kind_name(e.kind), e.a, e.b,
                        e.text.c_str());
        if (trace) std::fputs(run.mission()->host().log.c_str(), stdout), run.mission()->host().log.clear();
    };
    print_events();
    for (long t = 1; t <= ticks; ++t) {
        Mission& m = *run.mission();
        if (t == win_at) m.finish_objectives();
        m.tick();
        print_events();
        if (save_at.count(t)) {
            const auto path = (save_dir.empty() ? native_save_dir() : std::filesystem::path(save_dir)) /
                              ("joa-campaign-" + m.info().stem() + "-t" + std::to_string(m.ticks()) + ".jsav");
            if (!write_save_atomic(path, encode_save(m.save()), 3, &err)) return std::fprintf(stderr, "save: %s\n", err.c_str()), 1;
            std::printf("t=%llu saved %s hash %016llx\n", static_cast<unsigned long long>(m.ticks()), path.c_str(),
                        static_cast<unsigned long long>(m.hash()));
        }
        if (m.finish() == 1 && cont && t != ticks) {
            if (!run.continue_after_win(&err)) {
                std::printf("t=%ld %s\n", t, err.c_str());
                break;
            }
            print_events();
            std::printf("t=%ld now mission %d (%s)\n", t, run.mission()->scenario_no(), run.mission()->info().stem().c_str());
        }
    }
    Mission& m = *run.mission();
    std::printf("end: scenario %d tick %llu finish %d gold %d hash %016llx builtin calls %llu\n", m.scenario_no(),
                static_cast<unsigned long long>(m.ticks()), m.finish(), m.state().gold,
                static_cast<unsigned long long>(m.hash()), static_cast<unsigned long long>(m.host().calls));
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("unimplemented builtin %d %s: %llu calls\n", id, std::string(formats::script_funcs()[id - 1].name).c_str(),
                    static_cast<unsigned long long>(n));
    for (const auto& e : m.vm().errors()) std::printf("script error: %s\n", e.c_str());
    return 0;
}
