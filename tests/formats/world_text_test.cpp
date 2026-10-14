// Every text-format world/config file in the install parses with the kv or block parser.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/vfs_fs.hpp"
#include "formats/world_text.hpp"

using namespace joa::formats;
namespace fs = std::filesystem;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

static std::string text_of(const fs::path& p) {
    auto b = read_file(p);
    return b ? std::string(b->begin(), b->end()) : std::string();
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "joan.exe")) return 77;
    Vfs vfs(dir);

    int kv_files = 0, kv_lines = 0;
    for (const char* ext : {".pro", ".lgt", ".sca", ".day"}) {
        for (const auto& p : vfs.find_by_extension(ext)) {
            // item/Object/**/*.pro are "VERSION 1.0" item scripts (script parser), not kv files.
            const auto top = fs::relative(p, vfs.root()).begin()->string();
            if (top != "Resource" && top != "map") continue;
            const auto lines = parse_kv_text(text_of(p));
            CHECK(!lines.empty(), p.string());
            for (const auto& l : lines) CHECK(!l.key.empty(), p.string() + ": line without key: " + l.value);
            ++kv_files;
            kv_lines += int(lines.size());
        }
    }
    // LightNum/StageNum header of light files.
    auto common = parse_kv_text(text_of(*vfs.resolve("map\\common.lgt")));
    CHECK(common.size() > 2 && common[0].key == "LightNum" && common[0].value == "2" && common[1].key == "StageNum",
          "common.lgt header");

    int block_files = 0, blocks = 0, objects = 0;
    for (const char* ext : {".stg", ".pla", ".uni", ".wor", ".ter", ".trk", ".cm", ".pth", ".wav", ".por"}) {
        for (const auto& p : vfs.find_by_extension(ext)) {
            if (ext == std::string(".wav") && p.parent_path().parent_path().filename() != "map") continue;  // audio
            std::string err;
            auto doc = parse_block_text(text_of(p), &err);
            CHECK(doc, p.string() + ": " + err);
            if (ext == std::string(".por")) {  // "Anti-Portal Number: N" followed by N blocks
                CHECK(!doc->top_level.empty() && doc->top_level[0].key == "Anti-Portal Number" &&
                          std::stoul(doc->top_level[0].value) == doc->blocks.size(),
                      p.string());
            } else {
                CHECK(!doc->blocks.empty() || fs::file_size(p) == 0, p.string());  // S6L1w.pth ships empty
            }
            for (const auto& b : doc->blocks) {
                CHECK(b.type.starts_with("Create") || b.type == "Anti-Portal", p.string() + ": block " + b.type);
                if (b.type == "CreateObject") {
                    ++objects;
                    CHECK(b.get("ObjectFile") && b.get("X") && b.get("Y") && b.get("Z"), p.string());
                }
            }
            ++block_files;
            blocks += int(doc->blocks.size());
        }
    }
    auto stg = parse_block_text(text_of(*vfs.resolve("map\\scenario1\\S1L1.stg")));
    CHECK(stg && stg->blocks[0].type == "CreateGrid" && stg->blocks[0].number("X") == 256, "S1L1.stg grid");

    std::printf("text ok: %d kv files (%d lines), %d block files (%d blocks, %d objects)\n", kv_files, kv_lines,
                block_files, blocks, objects);
    return 0;
}
