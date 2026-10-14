// joa-script-run: run a mission's scripts headless against RecordingHost and print the
// builtin call trace.
//   joa-script-run <scenario> [--ticks N] [--seed S] [--poll-delay N] [--game-dir DIR]
// <scenario>: campaign index 1..8 or scenario/scenario_NN.scr (loads main + _des like the game),
// or a script stem/path (S1L1, script\scenario\S1L1_tactic.sce) run alone as the main script.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/scenario_def.hpp"
#include "game/script_host.hpp"
#include "game/script_vm.hpp"

using namespace joa;

int main(int argc, char** argv) {
    std::string target, dir = std::getenv("JOA_GAME_DIR") ? std::getenv("JOA_GAME_DIR") : ".";
    long ticks = 300, seed = 0, delay = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--ticks") ticks = std::atol(next());
        else if (a == "--seed") seed = std::atol(next());
        else if (a == "--poll-delay") delay = std::atol(next());
        else if (a == "--game-dir") dir = next();
        else target = a;
    }
    if (target.empty()) {
        std::fprintf(stderr, "usage: joa-script-run <1..8|scenario_NN.scr|script> [--ticks N] [--seed S] [--poll-delay N] [--game-dir DIR]\n");
        return 2;
    }
    const formats::Vfs vfs(dir);
    game::RecordingHost host(static_cast<int>(delay));
    game::Vm vm(game::vfs_source_loader(vfs), host, static_cast<std::uint32_t>(seed));

    std::string scr = target;
    if (target.find_first_not_of("0123456789") == std::string::npos) scr = "SCENARIO\\scenario_0" + target + ".scr";
    bool ok;
    if (scr.size() > 4 && formats::iequals(scr.substr(scr.size() - 4), ".scr")) {
        auto bytes = vfs.read(scr);
        std::string err;
        auto def = bytes ? formats::parse_scenario_def(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err)
                         : std::nullopt;
        if (!def || def->steps.empty()) {
            std::fprintf(stderr, "cannot load %s %s\n", scr.c_str(), err.c_str());
            return 1;
        }
        ok = vm.load_scenario(def->steps[0]);
    } else {
        std::string path = target;
        if (path.find_first_of("\\/") == std::string::npos) path = "SCRIPT\\SCENARIO\\" + path;
        if (path.find('.') == std::string::npos) path += ".SCE";
        ok = vm.load_main(path);
    }
    for (long t = 0; ok && t < ticks; ++t) vm.tick();
    std::fputs(host.log().c_str(), stdout);
    for (const auto& e : vm.errors()) std::fprintf(stderr, "error: %s\n", e.c_str());
    std::fprintf(stderr, "%llu ticks, %llu calls, trace hash %016llx\n", static_cast<unsigned long long>(vm.ticks()),
                 static_cast<unsigned long long>(host.calls()), static_cast<unsigned long long>(host.hash()));
    return ok && vm.errors().empty() ? 0 : 1;
}
