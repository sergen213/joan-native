// joa-res: inspect / extract Joan of Arc resource containers and export GRI terrain.
//   joa-res list <file>                         RES/SET index or dBase table
//   joa-res extract <file> <outdir>             every entry -> <outdir>/<name>
//   joa-res terrain <file.gri> <out.obj> <out.png>
// Outputs may not land inside the original install except under its work/ directory.
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "formats/map_grid.hpp"
#include "formats/res_archive.hpp"
#include "formats/vfs_fs.hpp"

using namespace joa::formats;
namespace fs = std::filesystem;

static int usage() {
    std::fprintf(stderr,
                 "usage: joa-res list <file>\n"
                 "       joa-res extract <file> <outdir>\n"
                 "       joa-res terrain <file.gri> <out.obj> <out.png>\n");
    return 2;
}

// Install root = nearest ancestor of `input` containing joan.exe (any case).
static std::optional<fs::path> install_root(const fs::path& input) {
    for (fs::path p = fs::absolute(input).parent_path(); !p.empty(); p = p.parent_path()) {
        if (Vfs(p).exists("joan.exe")) return fs::weakly_canonical(p);
        if (p == p.root_path()) break;
    }
    return std::nullopt;
}

static bool inside(const fs::path& child, const fs::path& dir) {
    const auto rel = fs::weakly_canonical(child).lexically_relative(dir);
    return !rel.empty() && *rel.begin() != "..";
}

// Never write into the original game files.
static bool output_allowed(const fs::path& input, const fs::path& out) {
    const auto root = install_root(input);
    if (!root || !inside(out, *root) || inside(out, *root / "work")) return true;
    std::fprintf(stderr, "refusing to write %s inside the original install (use %s/work/...)\n", out.c_str(),
                 root->c_str());
    return false;
}

static bool write_bytes(const fs::path& p, const void* data, std::size_t n) {
    std::ofstream o(p, std::ios::binary);
    return o.write(static_cast<const char*>(data), std::streamsize(n)).good();
}

static std::string safe_name(std::string n) {
    for (auto& c : n)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) c = '_';
    if (n.empty() || n == "." || n == "..") n = "_" + n;
    return n;
}

static int cmd_list(const fs::path& file) {
    auto bytes = read_file(file);
    if (!bytes) return std::fprintf(stderr, "cannot read %s\n", file.c_str()), 1;
    std::string err;
    if (auto a = ResArchive::parse(*bytes, &err)) {
        std::printf("# %s index, %zu entries\n", a->format() == ResIndexFormat::Name9 ? "name9" : "name34",
                    a->entries().size());
        for (const auto& e : a->entries()) std::printf("%-36s %10u %10u\n", e.name.c_str(), e.offset, e.size);
        return 0;
    }
    if (auto t = DbfTable::parse(*bytes, &err)) {
        for (const auto& f : t->fields()) std::printf("%s:%c%u\t", f.name.c_str(), f.type, f.length);
        std::printf("\n");
        for (std::size_t r = 0; r < t->record_count(); ++r) {
            for (std::size_t f = 0; f < t->fields().size(); ++f) std::printf("%s\t", t->text(r, f).c_str());
            std::printf("\n");
        }
        return 0;
    }
    std::fprintf(stderr, "%s: not a resource index or dBase table\n", file.c_str());
    return 1;
}

static int cmd_extract(const fs::path& file, const fs::path& outdir) {
    if (!output_allowed(file, outdir)) return 1;
    std::string err;
    auto a = ResArchive::open(file, &err);
    if (!a) return std::fprintf(stderr, "%s: %s\n", file.c_str(), err.c_str()), 1;
    fs::create_directories(outdir);
    const bool set = a->format() == ResIndexFormat::Name9;
    for (const auto& e : a->entries()) {
        auto d = a->data(e);
        std::string name = safe_name(e.name);
        if (set && DbfTable::parse(d)) name += ".dbf";
        else if (d.size() >= 4 && std::equal(d.begin(), d.begin() + 4, "RIFF")) name += ".wav";
        if (!write_bytes(outdir / name, d.data(), d.size()))
            return std::fprintf(stderr, "write failed: %s\n", (outdir / name).c_str()), 1;
    }
    std::printf("extracted %zu entries to %s\n", a->entries().size(), outdir.c_str());
    return 0;
}

// Minimal PNG writer: 8-bit grayscale, zlib "stored" blocks (no compression).
static bool write_png_gray(const fs::path& p, int w, int h, const std::vector<std::uint8_t>& px) {
    std::array<std::uint32_t, 256> crc_table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int s = 24; s >= 0; s -= 8) v.push_back(std::uint8_t(x >> s));
    };
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        be32(out, std::uint32_t(data.size()));
        const std::size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = start; i < out.size(); ++i) c = crc_table[(c ^ out[i]) & 0xFF] ^ (c >> 8);
        be32(out, c ^ 0xFFFFFFFFu);
    };
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, std::uint32_t(w));
    be32(ihdr, std::uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});  // 8-bit, grayscale
    chunk("IHDR", ihdr);

    std::vector<std::uint8_t> raw;  // filter byte 0 per row
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), px.begin() + std::ptrdiff_t(y) * w, px.begin() + std::ptrdiff_t(y + 1) * w);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    for (std::size_t pos = 0; pos < raw.size();) {  // raw is never empty (h >= 1)
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.insert(z.end(), {std::uint8_t(n), std::uint8_t(n >> 8), std::uint8_t(~n), std::uint8_t(~n >> 8)});
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(pos), raw.begin() + std::ptrdiff_t(pos + n));
        pos += n;
    }
    std::uint32_t a = 1, b = 0;
    for (auto c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    be32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    return write_bytes(p, out.data(), out.size());
}

static int cmd_terrain(const fs::path& gri, const fs::path& obj, const fs::path& png) {
    if (!output_allowed(gri, obj) || !output_allowed(gri, png)) return 1;
    auto bytes = read_file(gri);
    std::string err;
    auto g = bytes ? parse_gri(*bytes, &err) : std::nullopt;
    if (!g) return std::fprintf(stderr, "%s: %s\n", gri.c_str(), err.c_str()), 1;
    const int vx = g->verts_x(), vy = g->verts_y();

    // OBJ in a Y-up frame: (x, height, -y). Faces only where all four corners are valid.
    std::ofstream o(obj);
    o << "# " << gri.filename().string() << " " << g->cells_x << "x" << g->cells_y << " cells, "
      << TerrainGrid::kCellSize << " units/cell; v = (x, height, -y)\n";
    for (int x = 0; x < vx; ++x)
        for (int y = 0; y < vy; ++y)
            o << "v " << x * TerrainGrid::kCellSize << ' ' << g->height(x, y) << ' ' << -y * TerrainGrid::kCellSize
              << "\nvt " << double(x) / g->cells_x << ' ' << double(y) / g->cells_y << '\n';
    auto id = [&](int x, int y) { return x * vy + y + 1; };
    for (int x = 0; x < g->cells_x; ++x)
        for (int y = 0; y < g->cells_y; ++y) {
            if (!g->valid(x, y) || !g->valid(x + 1, y) || !g->valid(x, y + 1) || !g->valid(x + 1, y + 1)) continue;
            const int a = id(x, y), b = id(x + 1, y), c = id(x + 1, y + 1), d = id(x, y + 1);
            o << "f " << a << '/' << a << ' ' << b << '/' << b << ' ' << c << '/' << c << ' ' << d << '/' << d << '\n';
        }
    if (!o) return std::fprintf(stderr, "write failed: %s\n", obj.c_str()), 1;

    // Heightmap PNG, north (max y) at the top, normalised to the map's min..max.
    int lo = 32767, hi = -32768;
    for (int x = 0; x < vx; ++x)
        for (int y = 0; y < vy; ++y) lo = std::min<int>(lo, g->height(x, y)), hi = std::max<int>(hi, g->height(x, y));
    std::vector<std::uint8_t> px(std::size_t(vx) * vy);
    for (int x = 0; x < vx; ++x)
        for (int y = 0; y < vy; ++y)
            px[std::size_t(vy - 1 - y) * vx + x] = std::uint8_t((g->height(x, y) - lo) * 255 / std::max(1, hi - lo));
    if (!write_png_gray(png, vx, vy, px)) return std::fprintf(stderr, "write failed: %s\n", png.c_str()), 1;
    std::printf("%s: %dx%d vertices, height %d..%d -> %s, %s\n", gri.filename().c_str(), vx, vy, lo, hi, obj.c_str(),
                png.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    if (cmd == "list" && argc == 3) return cmd_list(argv[2]);
    if (cmd == "extract" && argc == 4) return cmd_extract(argv[2], argv[3]);
    if (cmd == "terrain" && argc == 5) return cmd_terrain(argv[2], argv[3], argv[4]);
    return usage();
}
