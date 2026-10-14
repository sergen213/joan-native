// Every Save/*.SAV header + section split, each .BMP thumbnail and Save/Player.dat.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/save_sav.hpp"
#include "formats/vfs_fs.hpp"

using namespace joa::formats;
namespace fs = std::filesystem;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "joan.exe")) return 77;
    Vfs vfs(dir);
    auto save_dir = vfs.resolve("SAVE");
    CHECK(save_dir, "Save dir");

    int saves = 0;
    for (const auto& e : fs::directory_iterator(*save_dir)) {
        if (!iequals(e.path().extension().string(), ".sav")) continue;
        const std::string where = e.path().string();
        auto bytes = read_file(e.path());
        std::string err;
        auto s = parse_sav(*bytes, &err);
        CHECK(s, where + ": " + err);
        CHECK(s->sections.size() == SaveGame::kSections && s->body_offset == 189, where);
        std::uint64_t covered = s->body_offset;
        for (const auto& sec : s->sections) covered += 6 + sec.size;
        CHECK(covered + 6 == bytes->size(), where + ": sections cover body");
        const auto t = filetime_to_unix(s->header.saved_utc);
        CHECK(t > 1072915200 && t < 4102444800, where + ": save time 2004..2100");  // FILETIME sanity
        std::printf("%s: \"%s\" mode=%u/%u utc=%lld sections:", e.path().filename().c_str(),
                    s->header.description.c_str(), s->header.game_mode, s->header.game_submode, (long long)t);
        for (const auto& sec : s->sections) std::printf(" %u", sec.size);
        std::printf("\n");

        auto bmp_path = e.path();
        bmp_path.replace_extension(".BMP");
        if (auto bmp = read_file(bmp_path)) {
            auto info = parse_bmp_header(*bmp, &err);
            CHECK(info && info->bpp == 24 && info->width == 512 && info->height == 256, bmp_path.string() + err);
        }
        ++saves;
    }
    CHECK(saves >= 1, "no .SAV files");

    auto player = vfs.read("save\\player.dat");
    CHECK(player, "Player.dat");
    std::string err;
    auto p = parse_player_dat(*player, &err);
    CHECK(p, err);
    CHECK(p->name() == "Player" && p->gold() == 100 && p->entries.empty() && write_player_dat(*p) == *player,
          "Player.dat fields / byte-exact rewrite");

    // Truncated header must fail cleanly.
    auto sav = read_file(*vfs.resolve("save\\Mission1-AutoSave.SAV"));
    CHECK(!parse_sav(std::span(*sav).first(100)), "truncated SAV accepted");
    std::printf("save ok: %d saves\n", saves);
    return 0;
}
