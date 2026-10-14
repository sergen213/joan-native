// Runs every shipped script against RecordingHost: each script/scenario/*.sce as a main script
// and every unitAI.res body for 3000 ticks, then all 8 campaign scenarios the way the game
// loads them (main + _des once). No compile or runtime errors allowed; traces deterministic.
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "formats/res_archive.hpp"
#include "formats/scenario_def.hpp"
#include "game/script_host.hpp"
#include "game/script_vm.hpp"

namespace fs = std::filesystem;
using namespace joa;
using namespace joa::game;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

constexpr int kTicks = 3000;

static void check_errors(const std::string& label, const Vm& vm) {
    CHECK(vm.errors().empty(), "%s: %zu error(s), first: %s", label.c_str(), vm.errors().size(),
          vm.errors().empty() ? "" : vm.errors()[0].c_str());
}

struct ScenarioRun {
    std::uint64_t hash = 0, calls = 0;
};

static ScenarioRun run_scenario(const formats::Vfs& vfs, int n) {
    char name[64];
    std::snprintf(name, sizeof name, "SCENARIO\\scenario_%02d.scr", n);
    auto bytes = vfs.read(name);
    CHECK(bytes, "%s missing", name);
    if (!bytes) return {};
    auto def = formats::parse_scenario_def(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    CHECK(def && !def->steps.empty(), "%s parse", name);
    if (!def) return {};
    RecordingHost host(1, false);
    Vm vm(vfs_source_loader(vfs), host, 12345);
    CHECK(vm.load_scenario(def->steps[0]), "%s load", name);
    for (int t = 0; t < kTicks; ++t) vm.tick();
    check_errors(name, vm);
    return {host.hash(), host.calls()};
}

int main() {
    const char* env = std::getenv("JOA_GAME_DIR");
    if (!env || !fs::exists(fs::path(env) / "script" / "scenario")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }
    const formats::Vfs vfs(env);

    size_t files = 0;
    std::uint64_t calls = 0;
    for (const auto& e : fs::directory_iterator(fs::path(env) / "script" / "scenario")) {
        if (e.path().extension() != ".sce") continue;
        const std::string rel = "script\\scenario\\" + e.path().filename().string();
        RecordingHost host(1, false);
        Vm vm(vfs_source_loader(vfs), host);
        CHECK(vm.load_main(rel), "%s compile", rel.c_str());
        for (int t = 0; t < kTicks; ++t) vm.tick();
        check_errors(rel, vm);
        calls += host.calls();
        ++files;
    }
    CHECK(files == 35, "expected 35 .sce files, got %zu", files);

    std::string err;
    auto ai = formats::ResArchive::open(fs::path(env) / "script" / "unit" / "unitAI.res", &err);
    CHECK(ai && ai->entries().size() == 18, "unitAI.res: %s", err.c_str());
    if (ai)
        for (const auto& en : ai->entries()) {
            RecordingHost host;
            Vm vm(vfs_source_loader(vfs), host);
            CHECK(vm.load_main_text("unitAI.res:" + en.name, ai->text(en.name).value_or("")), "unitAI %s", en.name.c_str());
            for (int t = 0; t < kTicks; ++t) CHECK(vm.tick() == -1, "unitAI %s must exit at once", en.name.c_str());
            check_errors(en.name, vm);
        }

    for (int n = 1; n <= 8; ++n) {
        const ScenarioRun a = run_scenario(vfs, n), b = run_scenario(vfs, n);
        CHECK(a.calls > 0 && a.hash == b.hash && a.calls == b.calls, "scenario %d not deterministic", n);
        std::printf("scenario_%02d: %llu calls in %d ticks, trace hash %016llx\n", n, static_cast<unsigned long long>(a.calls),
                    kTicks, static_cast<unsigned long long>(a.hash));
    }
    std::printf("%zu .sce + 18 unitAI scripts x %d ticks: %llu builtin calls\n", files, kTicks, static_cast<unsigned long long>(calls));
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
