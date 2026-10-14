// Builtin coverage: every builtin id called by a shipped script (script/scenario/*.sce) is
// implemented by the VM or has a MissionHost handler; prints the coverage numbers.
#include <cstdio>
#include <cstdlib>
#include <set>

#include "formats/script_ast.hpp"
#include "formats/script_funcs.hpp"
#include "formats/vfs_fs.hpp"
#include "game/script_bind.hpp"

using namespace joa;
using namespace joa::game;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("script/scenario/S1L1.sce")) return 77;
    const formats::Vfs vfs(dir);
    std::set<int> used;
    for (const auto& p : vfs.find_by_extension(".sce")) {
        auto bytes = formats::read_file(p);
        auto s = bytes ? formats::parse_script(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size())) : std::nullopt;
        if (!s) return std::printf("FAIL parse %s\n", p.c_str()), 1;
        formats::for_each_call(*s, [&](const formats::ScriptExpr& e) {
            if (e.text == "SCRIPT" || e.args.empty()) return;
            if (const auto* f = formats::find_script_func(e.args[0].text)) used.insert(f->id);
        });
    }
    const BuiltinTable& t = builtin_table();
    int fails = 0, vm = 0, vm_all = 0, host_used = 0, bound_used = 0, bound_all = 0, unimpl = 0;
    for (int id = 1; id <= 407; ++id) {
        const bool in_vm = is_vm_builtin(std::uint16_t(id));
        const bool bound = t.call[id] != nullptr;
        bound_all += bound;
        vm_all += in_vm;
        if (!in_vm && !bound) {
            ++unimpl;
            auto it = t.unimplemented.find(id);
            std::printf("unimplemented %d %s%s: %s\n", id, std::string(formats::script_funcs()[id - 1].name).c_str(),
                        used.count(id) ? " (USED)" : "", it == t.unimplemented.end() ? "NO REASON GIVEN" : it->second.c_str());
            if (it == t.unimplemented.end() || used.count(id)) ++fails;
        }
        if (!used.count(id)) continue;
        if (in_vm) ++vm;
        else ++host_used, bound_used += bound;
    }
    std::printf("builtins used by shipped scripts: %zu (%d VM, %d host); host handlers for used ids: %d/%d; "
                "handlers total %d + %d VM of 407; unimplemented %d\n",
                used.size(), vm, host_used, bound_used, host_used, bound_all, vm_all, unimpl);
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
