#pragma once
// PNG reader for the GL capture tests (only the subset engine/png.cpp writes).
#include <zlib.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct Png {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    const unsigned char* at(int x, int y) const { return rgba.data() + (std::size_t(y) * w + x) * 4; }
};

static std::uint32_t be32(const unsigned char* p) { return std::uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Reads the subset produced by write_png: RGBA8, non-interlaced, filter 0 rows.
static bool read_png(const std::string& path, Png& out) {
    std::ifstream f(path, std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 8 || d[1] != 'P' || d[2] != 'N' || d[3] != 'G') return false;
    std::vector<unsigned char> z;
    for (std::size_t p = 8; p + 12 <= d.size();) {
        const std::uint32_t len = be32(&d[p]);
        const std::string type(reinterpret_cast<const char*>(&d[p + 4]), 4);
        if (p + 12 + len > d.size()) return false;
        if (crc32(0, &d[p + 4], len + 4) != be32(&d[p + 8 + len])) return false;
        if (type == "IHDR") {
            out.w = int(be32(&d[p + 8]));
            out.h = int(be32(&d[p + 12]));
            if (d[p + 16] != 8 || d[p + 17] != 6) return false;
        } else if (type == "IDAT") {
            z.insert(z.end(), d.begin() + long(p + 8), d.begin() + long(p + 8 + len));
        }
        p += 12 + len;
    }
    const std::size_t stride = std::size_t(out.w) * 4 + 1;
    std::vector<unsigned char> raw(stride * out.h);
    uLongf n = uLongf(raw.size());
    if (uncompress(raw.data(), &n, z.data(), uLong(z.size())) != Z_OK || n != raw.size()) return false;
    for (int y = 0; y < out.h; ++y) {
        if (raw[y * stride] != 0) return false;
        out.rgba.insert(out.rgba.end(), raw.begin() + long(y * stride + 1), raw.begin() + long((y + 1) * stride));
    }
    return true;
}

