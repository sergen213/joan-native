// joa-install hd-import (import_hd_pack): synthetic pack, no game data.
#include "install/install.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace joa::install;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static void put(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("joa_hd_import_" + std::to_string(getpid()));
    fs::remove_all(tmp);
    const Paths paths{tmp / "data/joan-native/game", tmp / "config", tmp / "state", tmp / "data/applications"};
    put(paths.current() / "texture/item/Same.dds", "original");
    put(paths.current() / "texture/item/changed.dds", "original");
    const fs::path src = tmp / "HDUpdate";
    put(src / "TEXTURE/item/same.dds", "original");      // identical (case differs): skipped
    put(src / "TEXTURE/item/changed.dds", "hd pixels");  // copied
    put(src / "TEXTURE/unit/New.PNG", "png");            // copied
    put(src / "TEXTURE/unit/readme.txt", "x");           // not an image
    put(src / "int8x6/menu.dds", "1080p hack");          // UI: never imported
    put(src / "joan.exe", "exe");

    CHECK(paths.hd_root() == tmp / "data/joan-native/hd");
    const HdImport r = import_hd_pack(src, "hd-update", paths);
    CHECK(r.pack == paths.hd_root() / "hd-update");
    CHECK(r.copied == 2 && r.identical == 1);
    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(r.pack)) files += e.is_regular_file();
    CHECK(files == 2);
    CHECK(fs::exists(r.pack / "texture/item/changed.dds") && fs::exists(r.pack / "texture/unit/New.PNG"));
    CHECK(!fs::exists(paths.hd_root() / ".hd-update.staging"));

    bool threw = false;
    try { import_hd_pack(src, "hd-update", paths); } catch (const std::exception&) { threw = true; }
    CHECK(threw);  // existing pack is never overwritten
    threw = false;
    try { import_hd_pack(tmp / "config", "other", paths); } catch (const std::exception&) { threw = true; }
    CHECK(threw && !fs::exists(paths.hd_root() / "other"));  // no texture/ dir
    fs::remove_all(tmp);
    return 0;
}
