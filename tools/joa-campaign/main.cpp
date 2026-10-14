// joa-campaign: run campaign missions headless on the real World and print mission events.
//   joa-campaign [--mission N] [--ticks N] [--seed S] [--save-at T]... [--load FILE]
//                [--import FILE.SAV] [--win-at T] [--continue] [--game-dir DIR] [--trace]
//                [--autopilot] [--retries N]
// --save-at T   write a native save after tick T (native save dir, or --save-dir DIR)
// --load FILE   resume a native save; --import FILE.SAV start from an original save
// --win-at T    test hook: finish every WIN objective at tick T (normal win-delay path)
// --continue    after a win, continue to the next campaign mission (carried progression)
// --autopilot   diagnostic: the campaign bot (game/autopilot_player) plays; --ticks is the per-mission
//               budget, a loss is retried up to --retries times (default 2); one report line per mission
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "game/autopilot_player.hpp"
#include "game/campaign_runner.hpp"
#include "game/savegame_import.hpp"

using namespace joa;
using namespace joa::game;

int main(int argc, char** argv) {
    std::string dir = std::getenv("JOA_GAME_DIR") ? std::getenv("JOA_GAME_DIR") : ".";
    std::string load_path, import_path, save_dir;
    long mission = 1, ticks = 9000, win_at = -1, retries = 2, difficulty = 1;
    std::uint32_t seed = 1;
    std::set<long> save_at;
    bool cont = false, trace = false, autopilot = false;
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
        else if (a == "--autopilot") autopilot = true;
        else if (a == "--retries") retries = std::atol(next());
        else if (a == "--difficulty") difficulty = std::atol(next());  // new-game profile: 0 easy .. 3 very hard
        else {
            std::fprintf(stderr, "usage: joa-campaign [--mission N] [--ticks N] [--seed S] [--save-at T] [--save-dir DIR] "
                                 "[--load FILE] [--import FILE.SAV] [--win-at T] [--continue] [--game-dir DIR] [--trace] "
                                 "[--autopilot] [--retries N] [--difficulty D]\n");
            return 2;
        }
    }
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::fprintf(stderr, "game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    run.seed = seed;
    run.profile.difficulty = int(difficulty);
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

    if (autopilot) {
        for (;;) {
            const MissionReport r = play_mission(run, int(ticks), int(retries));
            for (const auto& l : r.log) std::printf("  %s\n", l.c_str());
            std::printf("%d %s %s ticks %d attempts %d deaths %d kills %d dealt %ld taken %ld objectives %d/%d hash %016llx%s%s\n", r.no,
                        r.stem.c_str(), r.result == 1 ? "WON" : r.result == 2 ? "LOST" : "STUCK", r.ticks, r.attempts, r.deaths, r.kills,
                        r.dealt, r.taken, r.objectives_done, r.objectives_total, static_cast<unsigned long long>(r.hash), r.stuck.empty() ? "" : " | ",
                        r.stuck.c_str());
            std::fflush(stdout);
            if (r.result != 1 || !cont) break;
            if (!run.continue_after_win(&err)) {
                std::printf("%s\n", err.c_str());
                break;
            }
            if (!save_dir.empty()) {  // mission-start checkpoint: resume the chain with --load
                const auto path = std::filesystem::path(save_dir) / ("bot-" + run.mission()->info().stem() + ".jsav");
                if (!write_save_atomic(path, encode_save(run.save()), 3, &err)) return std::fprintf(stderr, "save: %s\n", err.c_str()), 1;
                std::printf("saved %s\n", path.c_str());
            }
        }
        return 0;
    }

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
