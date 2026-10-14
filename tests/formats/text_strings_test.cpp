// String tables and free text: script/scenario/*EX.res, *.obj, des/*.txt, item_des/*.txt,
// Resource/help.res. Every "[NNNNX]" literal in the .sce scripts must exist in its EX.res
// table when that table exists.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <regex>
#include <string>

#include "formats/text_strings.hpp"
#include "formats/vfs_fs.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

static std::string text_of(const fs::path& p) {
    auto b = read_file(p);
    return b ? std::string(b->begin(), b->end()) : std::string();
}

int main() {
    {
        auto t = parse_tagged_strings("[0001N]\r\nHello\r\n~\r\n[0002N]\r\nTwo\r\nlines\r\n~\r\n");
        CHECK(t && t->size() == 2 && (*t)[0].tag == "0001N" && (*t)[1].text == "Two\nlines", "synthetic EX table");
        CHECK(!split_string_tag("[01N]x") && split_string_tag("[0009N]Arrive")->text == "Arrive", "split tag");
        CHECK(latin1_to_utf8("Orl\xe9" "ans") == "Orl\xc3\xa9" "ans", "latin1");
    }

    const char* env = std::getenv("JOA_GAME_DIR");
    if (!env || !fs::exists(fs::path(env) / "script" / "scenario")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }
    const fs::path root = env, scen = root / "script" / "scenario";

    size_t tables = 0, strings = 0, checked_refs = 0;
    std::map<std::string, std::map<std::string, std::string>> by_script;  // "S1L1_des" -> tag -> text
    for (const auto& e : fs::directory_iterator(scen)) {
        std::string fn = e.path().filename().string();
        if (fn.size() < 7 || fn.substr(fn.size() - 6) != "EX.res") continue;
        std::string err;
        auto t = parse_tagged_strings(text_of(e.path()), &err);
        CHECK(t && !t->empty(), "%s: %s", fn.c_str(), err.c_str());
        if (!t) continue;
        ++tables;
        strings += t->size();
        for (auto& s : *t) by_script[fn.substr(0, fn.size() - 6)][s.tag] = s.text;
    }
    CHECK(tables == 28, "expected 28 EX.res tables, got %zu", tables);

    // Script literals "[NNNNN]text" resolve in "<script>EX.res" (joan.exe loads <name> + "EX.res").
    const std::regex lit("\"\\[(\\d{4}[A-Z])\\]");
    for (const auto& e : fs::directory_iterator(scen)) {
        if (e.path().extension() != ".sce") continue;
        auto it = by_script.find(e.path().stem().string());
        if (it == by_script.end()) continue;
        std::string s = text_of(e.path());
        for (std::sregex_iterator m(s.begin(), s.end(), lit), end; m != end; ++m) {
            ++checked_refs;
            CHECK(it->second.count((*m)[1].str()), "%s: tag %s missing from EX.res", e.path().filename().c_str(), (*m)[1].str().c_str());
        }
    }

    size_t texts = 0;
    for (const char* dir : {"des", "item_des"})
        for (const auto& e : fs::directory_iterator(root / dir)) {
            CHECK(!split_tilde_blocks(text_of(e.path())).empty(), "%s empty", e.path().c_str());
            ++texts;
        }
    for (const auto& e : fs::directory_iterator(scen))
        if (e.path().extension() == ".obj") {
            CHECK(!split_tilde_blocks(text_of(e.path())).empty(), "%s empty", e.path().c_str());
            ++texts;
        }
    CHECK(texts == 25 + 81 + 8, "expected 114 text files, got %zu", texts);
    CHECK(split_tilde_blocks(text_of(root / "Resource" / "help.res")).size() > 1, "help.res blocks");

    std::printf("text: %zu EX.res tables, %zu strings, %zu script tag refs, %zu text files\n", tables, strings, checked_refs, texts);
    return failures ? 1 : 0;
}
