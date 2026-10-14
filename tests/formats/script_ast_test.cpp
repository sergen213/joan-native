// Parses every scenario script (script/scenario/*.sce) and every unitAI.res script body,
// checks round-trip formatting and that every FUNC/DIALOG/ACTION call resolves in the
// 407-entry function table with an argument count the executor reads.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

#include "formats/res_archive.hpp"
#include "formats/script_ast.hpp"
#include "formats/script_funcs.hpp"
#include "formats/vfs_fs.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

static size_t calls = 0;
static std::set<std::string> used;

static void check_script(const std::string& label, std::string_view text) {
    std::string err;
    auto s = parse_script(text, &err);
    CHECK(s, "%s: %s", label.c_str(), err.c_str());
    if (!s) return;
    std::string once = format_script(*s);
    auto again = parse_script(once, &err);
    CHECK(again && format_script(*again) == once, "%s: format round-trip", label.c_str());
    for_each_call(*s, [&](const ScriptExpr& c) {
        if (c.text == "SCRIPT") {
            CHECK(c.args.size() == 1 && c.args[0].kind == ScriptExpr::Kind::String, "%s:%d SCRIPT(path)", label.c_str(), c.line);
            return;
        }
        ++calls;
        const ScriptFunc* f = find_script_func(c.args[0].text);
        CHECK(f, "%s:%d unknown function %s", label.c_str(), c.line, c.args[0].text.c_str());
        if (!f) return;
        used.insert(std::string(f->name));
        // DIALOG/ACTION functions are only implemented by their own executor; plain FUNC
        // functions also work through ACTION (stop_curr_task is called both ways).
        if (f->executor == ScriptExecutor::Dialog) CHECK(c.text == "DIALOG", "%s:%d %s via %s", label.c_str(), c.line, c.args[0].text.c_str(), c.text.c_str());
        if (f->executor == ScriptExecutor::Action) CHECK(c.text == "ACTION", "%s:%d %s via %s", label.c_str(), c.line, c.args[0].text.c_str(), c.text.c_str());
        if (f->name != "format_string")
            CHECK(c.args.size() - 1 <= f->arity, "%s:%d %s: %zu args > arity %u", label.c_str(), c.line,
                  c.args[0].text.c_str(), c.args.size() - 1, unsigned(f->arity));
    });
}

int main() {
    // Function table invariants.
    CHECK(script_funcs().size() == 407, "table size %zu", script_funcs().size());
    std::set<int> ids;
    for (const auto& f : script_funcs()) {
        ids.insert(f.id);
        CHECK(f.sig.size() == f.arity || f.name == "format_string", "%.*s sig/arity", int(f.name.size()), f.name.data());
    }
    CHECK(ids.size() == 407 && *ids.begin() == 1 && *ids.rbegin() == 407, "ids 1..407");
    CHECK(find_script_func("GET_SCENARIO_STAGE") && find_script_func("get_scenario_stage")->id == 287, "case-insensitive lookup");

    // Grammar on synthetic input: precedence and else-if chains.
    {
        auto s = parse_script("\"t\"\n{\n INT a\n a = 1 + 2 * 3 ^ 2 - 4\n if( a == 1 || a > 2 && a != 3 ) { exit } else if( a ) { WAIT { FUNC(\"pause_script\", 1) } } else { a = -5 }\n}", nullptr);
        CHECK(s && s->body.size() == 3, "synthetic parse");
        if (s) {
            CHECK(format_expr(s->body[1].expr) == "( 1 + ( 2 * ( 3 ^ 2 ) ) ) - 4", "precedence: %s", format_expr(s->body[1].expr).c_str());
            CHECK(format_expr(s->body[2].expr) == "( a == 1 ) || ( ( a > 2 ) && ( a != 3 ) )", "logic: %s", format_expr(s->body[2].expr).c_str());
            CHECK(s->body[2].else_body.size() == 1 && s->body[2].else_body[0].kind == ScriptStmt::Kind::If, "else if");
        }
        CHECK(!parse_script("\"t\" { GOTO x }"), "unsupported keyword rejected");
        CHECK(!parse_script("\"t\" { FUNC(1) }"), "FUNC needs a name");
    }

    const char* env = std::getenv("JOA_GAME_DIR");
    if (!env || !fs::exists(fs::path(env) / "script" / "scenario")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }
    const fs::path root = env;

    size_t files = 0;
    for (const auto& e : fs::directory_iterator(root / "script" / "scenario")) {
        if (e.path().extension() != ".sce") continue;
        auto bytes = read_file(e.path());
        CHECK(bytes, "read %s", e.path().c_str());
        if (!bytes) continue;
        check_script(e.path().filename().string(), std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        ++files;
    }
    CHECK(files == 35, "expected 35 .sce files, got %zu", files);

    std::string err;
    auto ai = ResArchive::open(root / "script" / "unit" / "unitAI.res", &err);
    CHECK(ai && ai->entries().size() == 18, "unitAI.res: %s", err.c_str());
    if (ai)
        for (const auto& en : ai->entries()) {
            auto text = ai->text(en.name);
            check_script("unitAI.res:" + en.name, text.value_or(""));
            auto s = parse_script(text.value_or(""));
            // Every shipped unit AI script exits immediately; the old logic is commented out.
            CHECK(s && !s->body.empty() && s->body[0].kind == ScriptStmt::Kind::Exit, "%s does not start with exit", en.name.c_str());
        }

    std::printf("scripts: %zu .sce, %zu calls, %zu distinct functions used of %zu\n", files, calls, used.size(), script_funcs().size());
    return failures ? 1 : 0;
}
