// Case-insensitive VFS against the original install (read-only).
#include <cstdio>
#include <cstdlib>

#include "formats/vfs_fs.hpp"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !std::filesystem::exists(std::filesystem::path(dir) / "joan.exe")) return 77;
    joa::formats::Vfs vfs(dir);

    auto p = vfs.resolve("RESOURCE\\std.set");
    CHECK(p && p->filename() == "STD.SET");
    p = vfs.resolve("Map\\SCENARIO1\\s1l1.GRI");
    CHECK(p && p->filename() == "S1L1.gri" && p->parent_path().filename() == "scenario1");
    p = vfs.resolve("map/scenario1/");
    CHECK(p && std::filesystem::is_directory(*p));
    CHECK(vfs.resolve("font\\GOUDY_10.FIN"));
    CHECK(vfs.resolve("Int8x6\\goudy_10.DDS"));

    CHECK(!vfs.resolve("Resource\\nope.res"));
    CHECK(!vfs.resolve("..\\joan.exe"));
    CHECK(!vfs.resolve("Resource\\..\\joan.exe"));
    CHECK(!vfs.resolve("C:\\joan.exe"));
    CHECK(!vfs.resolve("\\joan.exe"));

    auto bytes = vfs.read("config.dat");
    CHECK(bytes && bytes->size() == 0xB05);
    CHECK(!vfs.read("map\\scenario1"));  // directory, not a file

    const auto gris = vfs.find_by_extension(".GRI");
    CHECK(gris.size() == 32);
    std::printf("vfs ok: %zu .gri files\n", gris.size());
    return 0;
}
