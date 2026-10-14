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
    std::vector<std::uint8_t> save;  // Vm::save at save_at
};

static std::optional<formats::ScenarioStep> step_of(const formats::Vfs& vfs, int n) {
    char name[64];
    std::snprintf(name, sizeof name, "SCENARIO\\scenario_%02d.scr", n);
    auto bytes = vfs.read(name);
    CHECK(bytes, "%s missing", name);
    if (!bytes) return std::nullopt;
    auto def = formats::parse_scenario_def(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    CHECK(def && !def->steps.empty(), "%s parse", name);
    if (!def || def->steps.empty()) return std::nullopt;
    return def->steps[0];
}

// save_at > 0: at that tick save the Vm, load it into a freshly compiled Vm (host state copied,
// keyed by site_id) and finish the run there; the trace must equal an uninterrupted run.
static ScenarioRun run_scenario(const formats::Vfs& vfs, int n, int save_at = 0) {
    auto step = step_of(vfs, n);
    if (!step) return {};
    ScenarioRun out;
    RecordingHost host(1, false), host2(1, false);
    auto vm = std::make_unique<Vm>(vfs_source_loader(vfs), host, 12345);
    CHECK(vm->load_scenario(*step), "scenario %d load", n);
    RecordingHost* h = &host;
    for (int t = 0; t < kTicks; ++t) {
        if (t == save_at && save_at > 0) {
            vm->save(out.save);
            auto vm2 = std::make_unique<Vm>(vfs_source_loader(vfs), host2, 999);
            CHECK(vm2->load_scenario(*step), "scenario %d reload", n);
            host2 = host;
            CHECK(vm2->load(out.save), "scenario %d: load of own save failed", n);
            CHECK(vm2->hash() == vm->hash(), "scenario %d: state hash differs after load", n);
            vm = std::move(vm2);
            h = &host2;
        }
        vm->tick();
    }
    check_errors("scenario " + std::to_string(n), *vm);
    out.hash = h->hash();
    out.calls = h->calls();
    return out;
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
        // Uninterrupted vs saved+reloaded early (inside the opening cut scenes' WAITs) and late.
        const ScenarioRun a = run_scenario(vfs, n), b = run_scenario(vfs, n, 4), c = run_scenario(vfs, n, 1500);
        CHECK(a.calls > 0 && a.hash == b.hash && a.calls == b.calls && a.hash == c.hash && a.calls == c.calls,
              "scenario %d: trace differs across runs or across save/load", n);
        // A save only loads into a Vm compiled from the same scripts, and only whole.
        RecordingHost other_host;
        Vm other(vfs_source_loader(vfs), other_host);
        CHECK(other.load_scenario(*step_of(vfs, n % 8 + 1)) && !other.load(c.save), "scenario %d save loaded into scenario %d", n,
              n % 8 + 1);
        Vm same(vfs_source_loader(vfs), other_host);
        CHECK(same.load_scenario(*step_of(vfs, n)) && !same.load(std::span(c.save).first(c.save.size() - 1)) &&
                  same.load(c.save),
              "scenario %d truncated save accepted / full save rejected", n);
        std::printf("scenario_%02d: %llu calls in %d ticks, trace hash %016llx, save %zu bytes\n", n,
                    static_cast<unsigned long long>(a.calls), kTicks, static_cast<unsigned long long>(a.hash), c.save.size());
    }
    std::printf("%zu .sce + 18 unitAI scripts x %d ticks: %llu builtin calls\n", files, kTicks, static_cast<unsigned long long>(calls));
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
