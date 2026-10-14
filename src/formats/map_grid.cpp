#include "formats/map_grid.hpp"

#include <algorithm>

#include "formats/vfs_bytes.hpp"

namespace joa::formats {

// FUN_0059dc10 with escape 0xFF (push 0xff at 0x4f7286): literal bytes, or
// FF n v = n copies of v, FF 00 = end of stream. Returns false on overflow/truncation.
static bool read_rle(ByteReader& r, std::vector<std::uint8_t>& out, std::size_t expect) {
    out.clear();
    while (r.ok()) {
        const std::uint8_t c = r.u8();
        if (c != 0xFF) {
            out.push_back(c);
        } else {
            const std::uint8_t n = r.u8();
            if (n == 0) return r.ok() && out.size() == expect;
            const std::uint8_t v = r.u8();
            out.insert(out.end(), n, v);
        }
        if (out.size() > expect) return false;
    }
    return false;
}

std::optional<TerrainGrid> parse_gri(std::span<const std::uint8_t> bytes, std::string* err) {
    auto fail = [&](const char* m) -> std::optional<TerrainGrid> {
        if (err) *err = m;
        return std::nullopt;
    };
    ByteReader r(bytes);
    TerrainGrid g;
    g.cells_x = r.i32();
    g.cells_y = r.i32();
    g.header_c = r.i32();
    const std::int32_t nsec = r.i32();
    if (!r.ok() || g.cells_x <= 0 || g.cells_y <= 0 || g.cells_x > 4096 || g.cells_y > 4096 || nsec < 0 ||
        std::uint64_t(nsec) * 32 > r.remaining())
        return fail("bad GRI header");
    for (int i = 0; i < nsec; ++i) g.sector_names.push_back(r.fixed_string(32));
    const std::int32_t ntex = r.i32();
    if (!r.ok() || ntex < 0 || std::uint64_t(ntex) * 64 > r.remaining()) return fail("bad GRI texture table");
    for (int i = 0; i < ntex; ++i) {
        GriTexture t;
        t.name = r.fixed_string(32);
        t.alt = r.fixed_string(32);
        g.textures.push_back(std::move(t));
    }

    const std::size_t n = std::size_t(g.verts_x()) * g.verts_y();
    g.nodes.resize(n);
    std::vector<std::uint8_t> plane;
    for (int b = 0; b < 16; ++b) {
        if (!read_rle(r, plane, n)) return fail("bad GRI node plane");
        for (std::size_t i = 0; i < n; ++i) g.nodes[i][b] = plane[i];
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t level = g.nodes[i][15];
        if (level == 0) continue;
        if (level > 2) return fail("bad GRI detail level");
        const std::size_t m = level == 1 ? 9 : 25;
        GriDetail d{std::uint32_t(i), level, std::vector<std::uint8_t>(m * 5)};
        for (std::size_t p = 0; p < 5; ++p) {
            if (!read_rle(r, plane, m)) return fail("bad GRI detail plane");
            for (std::size_t k = 0; k < m; ++k) d.samples[k * 5 + p] = plane[k];
        }
        g.details.push_back(std::move(d));
    }
    if (!r.at_end()) return fail("trailing bytes after GRI planes");
    return g;
}

float TerrainGrid::height_at(float wx, float wy) const {
    const float fx = std::clamp(wx / kCellSize, 0.0f, float(cells_x));
    const float fy = std::clamp(wy / kCellSize, 0.0f, float(cells_y));
    const int x0 = std::min(int(fx), cells_x - 1), y0 = std::min(int(fy), cells_y - 1);
    const float tx = fx - x0, ty = fy - y0;
    const float h00 = height(x0, y0), h10 = height(x0 + 1, y0);
    const float h01 = height(x0, y0 + 1), h11 = height(x0 + 1, y0 + 1);
    return (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
}

}  // namespace joa::formats
