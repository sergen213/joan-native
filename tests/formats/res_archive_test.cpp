// Every *.RES / *.SET / Resource/RESOURCE in the install must be recognised by exactly the
// parser documented for it; index containers must cover their file byte-for-byte.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "formats/res_archive.hpp"
#include "formats/vfs_fs.hpp"

using namespace joa::formats;
namespace fs = std::filesystem;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "joan.exe")) return 77;
    Vfs vfs(dir);

    auto files = vfs.find_by_extension(".res");
    for (const auto& f : vfs.find_by_extension(".set")) files.push_back(f);
    files.push_back(*vfs.resolve("Resource\\RESOURCE"));

    int archives = 0, entries = 0, dbf_tables = 0, other = 0, foreign = 0, empty = 0;
    for (const auto& path : files) {
        const std::string name = lower(path.filename().string());
        const std::string where = path.string();
        auto bytes = read_file(path);
        CHECK(bytes, where);
        if (bytes->empty()) { ++empty; continue; }  // ANIM_AM3.RES, anim_am2.res ship empty

        // Compiled scenario scripts (script/scenario/*EX.res) belong to the script parser.
        if (path.parent_path().filename() == "scenario" && name.ends_with("ex.res")) { ++foreign; continue; }

        std::string err;
        if (auto a = ResArchive::parse(*bytes, &err)) {
            ++archives;
            CHECK(a->end_offset() == bytes->size(), where);
            entries += int(a->entries().size());
            CHECK(a->find(a->entries().front().name) == &a->entries().front(), where);
            const bool is_set = name.ends_with(".set") || name == "resource";
            CHECK(!is_set || a->format() == ResIndexFormat::Name9, where);
            if (is_set) {
                for (const auto& e : a->entries()) {
                    auto t = DbfTable::parse(a->data(e), &err);
                    CHECK(t, where + ":" + e.name + " " + err);
                    ++dbf_tables;
                }
            }
            continue;
        }
        ++other;
        if (name == "cursor.res") {
            auto t = DbfTable::parse(*bytes, &err);
            CHECK(t && t->record_count() == 2, err);
            auto bmps = parse_bitmap_db(*read_file(path.parent_path() / "I_CURSOR.RES"), &err);
            CHECK(bmps, err);
            std::set<std::uint32_t> offsets;
            for (const auto& b : *bmps) offsets.insert(b.offset);
            const int ptr = t->field_index("HBITMAPPTR");  // field names carry a leading 'H' in this file
            CHECK(ptr >= 0, "BITMAPPTR field");
            for (std::size_t r = 0; r < t->record_count(); ++r) {
                auto raw = t->raw(r, std::size_t(ptr));
                const std::uint32_t off = raw[0] | raw[1] << 8 | raw[2] << 16 | std::uint32_t(raw[3]) << 24;
                CHECK(offsets.count(off), "CURSOR.RES BITMAPPTR -> I_CURSOR.RES record");
            }
        } else if (name == "i_cursor.res") {
            auto bmps = parse_bitmap_db(*bytes, &err);
            CHECK(bmps && bmps->size() == 2 && (*bmps)[0].width == 32 && (*bmps)[0].height == 32, err);
        } else if (name == "pal_std.res") {
            auto p = parse_palette(*bytes, &err);
            CHECK(p && p->header0 == 776, err);
        } else if (name == "help.res") {
            CHECK(std::string(bytes->begin(), bytes->begin() + 7) == "CompCol", where);  // plain text
        } else {
            CHECK(false, "unclassified resource file " + where);
        }
    }

    // Spot checks of named lookups used by the engine.
    auto gameplay = ResArchive::open(*vfs.resolve("resource\\gameplay.res"));
    CHECK(gameplay && gameplay->format() == ResIndexFormat::Name9, "GAMEPLAY.RES");
    CHECK(gameplay->text("charmove") && gameplay->text("CHARMOVE")->size() == 502, "GAMEPLAY.RES CHARMOVE");
    auto wave = ResArchive::open(*vfs.resolve("Resource\\WAVE.RES"));
    CHECK(wave && wave->entries().size() == 503, "WAVE.RES count");
    for (const auto& e : wave->entries()) {
        auto d = wave->data(e);
        CHECK(d.size() > 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F', "WAVE.RES entry " + e.name);
    }

    std::printf("res ok: %zu files, %d index archives (%d entries), %d DBF tables in sets, %d other, %d empty, %d left to script parser\n",
                files.size(), archives, entries, dbf_tables, other, empty, foreign);
    return 0;
}
