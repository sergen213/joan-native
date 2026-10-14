#include "engine/png.hpp"

#include <zlib.h>

#include <cstdio>
#include <vector>

namespace joa::engine {
namespace {
void put_be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(std::uint8_t(x >> s));
}
void chunk(std::vector<std::uint8_t>& out, const char type[4], const std::vector<std::uint8_t>& data) {
    put_be32(out, std::uint32_t(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_be32(out, std::uint32_t(crc32(0, out.data() + start, uInt(out.size() - start))));
}
}  // namespace

bool write_png(const std::string& path, int w, int h, std::span<const std::uint8_t> rgba) {
    if (w <= 0 || h <= 0 || rgba.size() < std::size_t(w) * h * 4) return false;
    std::vector<std::uint8_t> raw;  // filter byte 0 + row
    raw.reserve((std::size_t(w) * 4 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const auto* row = rgba.data() + std::size_t(y) * w * 4;
        raw.insert(raw.end(), row, row + std::size_t(w) * 4);
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<std::uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
    z.resize(zlen);

    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, std::uint32_t(w));
    put_be32(ihdr, std::uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit, RGBA, deflate, adaptive filter set, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "sRGB", {0});  // perceptual intent: pixels are sRGB-encoded
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;
}
}  // namespace joa::engine
