// No game data needed: mmap-backed ResArchive (span lifetime, empty files), the Vfs directory cache
// (files created, deleted or re-cased after the first scan) and corrupt-count rejection in the
// .pfd / .gri parsers before any large allocation.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "formats/map_grid.hpp"
#include "formats/pfd_mesh.hpp"
#include "formats/res_archive.hpp"
#include "formats/vfs_fs.hpp"

using namespace joa::formats;
namespace fs = std::filesystem;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

namespace {
using Bytes = std::vector<std::uint8_t>;
void put32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i))); }
void put_name(Bytes& b, const char* s, std::size_t n) {
    const std::size_t at = b.size();
    b.resize(at + n, 0);
    std::memcpy(b.data() + at, s, std::strlen(s));
}
void write(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
std::string str(std::span<const std::uint8_t> s) { return std::string(reinterpret_cast<const char*>(s.data()), s.size()); }

// Name34 container {"Alpha": "hello", "beta": "xyz"}.
Bytes name34() {
    Bytes b;
    put32(b, 2);
    const std::uint32_t base = 4 + 3 * 38;
    put_name(b, "Alpha", 34); put32(b, base);
    put_name(b, "beta", 34); put32(b, base + 5);
    put_name(b, "", 34); put32(b, base + 8);
    for (char c : std::string("helloxyz")) b.push_back(std::uint8_t(c));
    return b;
}
}  // namespace

int main() {
    std::string tmpl = (fs::temp_directory_path() / "joa-formats-XXXXXX").string();
    CHECK(::mkdtemp(tmpl.data()), "mkdtemp");
    const fs::path tmp(tmpl);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{tmp};

    // ---- ResArchive: mapped file, same result as parse(); copies share the bytes.
    {
        const Bytes b = name34();
        write(tmp / "a.res", b);
        std::string err;
        auto mapped = ResArchive::open(tmp / "a.res", &err);
        auto parsed = ResArchive::parse(b, &err);
        CHECK(mapped && parsed, err);
        CHECK(mapped->format() == ResIndexFormat::Name34 && mapped->entries().size() == 2, "index");
        CHECK(mapped->file_size() == b.size() && mapped->end_offset() == b.size(), "size");
        CHECK(mapped->text("ALPHA") == "hello" && str(mapped->data(mapped->entries()[1])) == "xyz", "data");
        CHECK(parsed->text("alpha") == "hello" && parsed->text("BETA") == "xyz", "parse");
        std::span<const std::uint8_t> kept;
        std::optional<ResArchive> copy;
        {
            auto local = ResArchive::open(tmp / "a.res");
            CHECK(local, "reopen");
            copy = *local;
            std::optional<ResArchive> moved = std::move(local);
            kept = moved->data(moved->entries()[0]);
        }  // both originals gone; the copy keeps the mapping alive
        CHECK(str(copy->data(copy->entries()[0])) == "hello", "copy outlives original");
        CHECK(copy->data(copy->entries()[0]).data() == kept.data(), "copy shares the mapping");

        write(tmp / "empty.res", {});
        err.clear();
        CHECK(!ResArchive::open(tmp / "empty.res", &err) && err == "not a resource index container", err);
        err.clear();
        CHECK(!ResArchive::open(tmp / "missing.res", &err) && err == "cannot read file", err);
        err.clear();
        CHECK(!ResArchive::open(tmp, &err) && err == "cannot read file", err);
        Bytes truncated = b;
        truncated.pop_back();
        write(tmp / "short.res", truncated);
        CHECK(!ResArchive::open(tmp / "short.res"), "end offset past EOF");
    }

    // ---- Vfs cache: misses rescan, vanished hits re-resolve, exact case wins.
    {
        const fs::path root = tmp / "game";
        write(root / "Resource/STD.SET", {1});
        Vfs vfs(root);
        auto p = vfs.resolve("RESOURCE\\std.set");
        CHECK(p && *p == root / "Resource/STD.SET", "case-insensitive");
        CHECK(vfs.resolve("resource/STD.SET") == p, "cached");
        CHECK(!vfs.resolve("Save\\slot1.sav") && !vfs.resolve("Resource\\nope"), "absent");
        CHECK(!vfs.resolve("..\\x") && !vfs.resolve("C:\\x") && !vfs.resolve("/x"), "rejected");
        CHECK(vfs.resolve("") == root && vfs.resolve("./") == root, "root");

        write(root / "save/SLOT1.SAV", {2});   // created after root and its listing were cached
        const Vfs copy = vfs;                  // copies share the cache
        CHECK(copy.resolve("Save\\slot1.sav") == root / "save/SLOT1.SAV", "new file found");
        CHECK(vfs.read("SAVE/Slot1.Sav") == Bytes{2}, "read");

        fs::remove(root / "save/SLOT1.SAV");
        CHECK(!vfs.resolve("Save\\slot1.sav") && !vfs.exists("save/SLOT1.SAV"), "deleted file gone");
        write(root / "save/slot1.sav", {3});
        CHECK(vfs.resolve("SAVE\\SLOT1.SAV") == root / "save/slot1.sav", "re-cased file");

        fs::rename(root / "save", root / "Saves_tmp");
        fs::rename(root / "Saves_tmp", root / "SAVE");
        CHECK(vfs.resolve("save\\slot1.sav") == root / "SAVE/slot1.sav", "renamed directory");

        write(root / "Dir/x", {4});
        write(root / "dir/x", {5});
        CHECK(vfs.resolve("dir/x") == root / "dir/x" && vfs.resolve("Dir/X") == root / "Dir/x", "exact case wins");
        CHECK(vfs.resolve("Resource/STD.SET/x") == std::nullopt, "file used as directory");

        // Shared cache under concurrent lookups (run under -DJOA_SANITIZE for the race checks).
        std::vector<std::thread> threads;
        int bad = 0;
        std::vector<int> fails(4, 0);
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&, t] {
                for (int i = 0; i < 200; ++i)
                    if (copy.resolve(i % 2 ? "resource/std.set" : "save/SLOT1.sav") == std::nullopt) ++fails[std::size_t(t)];
            });
        for (auto& th : threads) th.join();
        for (int f : fails) bad += f;
        CHECK(bad == 0, "concurrent resolve");
    }

    // ---- PFD: counts the remaining bytes cannot hold are refused up front; a tiny valid file parses.
    {
        Bytes huge_polys;
        put32(huge_polys, 0); put32(huge_polys, 0); put32(huge_polys, 1u << 22);
        std::string err;
        CHECK(!parse_pfd(huge_polys, &err) && err == "bad poly count", err);
        Bytes huge_points;
        put32(huge_points, 0); put32(huge_points, 1u << 22);
        CHECK(!parse_pfd(huge_points, &err) && err == "bad point count", err);
        Bytes huge_grid;
        for (std::uint32_t v : {0u, 0u, 0u, 0u, 1u, 0u, 0u, 4096u, 4096u, 1u, 1u}) put32(huge_grid, v);
        CHECK(!parse_pfd(huge_grid, &err) && err == "truncated grid", err);

        Bytes ok;
        put32(ok, 0);                                    // models
        put32(ok, 3);                                    // points
        for (int i = 0; i < 9; ++i) put32(ok, std::uint32_t(i));
        put32(ok, 1);                                    // polys
        put32(ok, 0); for (int i = 0; i < 6; ++i) put32(ok, 0); put32(ok, 0); put32(ok, 3);
        put32(ok, 1); put32(ok, 2); put32(ok, 3);
        put32(ok, 0);                                    // links
        for (std::uint32_t v : {1u, 0u, 0u, 1u, 1u, 2u, 2u}) put32(ok, v);  // grid present, header
        put32(ok, std::uint32_t(-5));                    // h[0]
        put32(ok, 0);                                    // sub-grids
        err.clear();
        auto m = parse_pfd(ok, &err);
        CHECK(m && m->points.size() == 3 && m->polys.size() == 1 && m->polys[0].verts == std::vector<std::int32_t>({0, 1, 2}), err);
        CHECK(m->grid && m->grid->h == std::vector<std::int16_t>{-5}, "grid");
    }

    // ---- GRI: a 16-byte header claiming 4097^2 nodes is refused before allocating; the tightest valid file parses.
    {
        Bytes huge;
        for (std::uint32_t v : {4096u, 4096u, 4u, 0u, 0u}) put32(huge, v);
        std::string err;
        CHECK(!parse_gri(huge, &err) && err == std::string("bad GRI node plane"), err);
        Bytes ok;
        for (std::uint32_t v : {1u, 1u, 4u, 0u, 0u}) put32(ok, v);   // 2x2 nodes, no sectors/textures
        for (int plane = 0; plane < 16; ++plane)
            for (std::uint8_t c : {0xFF, 4, plane == 7 ? 9 : 0, 0xFF, 0}) ok.push_back(c);  // FF 04 v, end
        err.clear();
        auto g = parse_gri(ok, &err);
        CHECK(g && g->nodes.size() == 4 && g->height(1, 1) == 9 && g->details.empty(), err);
        ok.resize(ok.size() - 1);
        CHECK(!parse_gri(ok, &err), "truncated plane");
    }

    std::printf("formats synthetic ok\n");
    return 0;
}
