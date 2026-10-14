#include "engine/png.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace joa::engine {
namespace {
std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
}  // namespace

bool decode_png(std::span<const std::uint8_t> d, int& w, int& h, std::vector<std::uint8_t>& rgba, std::string& err) {
    const auto fail = [&](const char* m) { return err = m, false; };
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (d.size() < 8 || std::memcmp(d.data(), sig, 8) != 0) return fail("not a PNG file");
    int depth = 0, type = -1;
    std::vector<std::uint8_t> z, plte, trns;
    for (std::size_t p = 8;;) {
        if (p + 12 > d.size()) return fail("truncated PNG");
        const std::uint32_t len = be32(&d[p]);
        if (len > d.size() - p - 12) return fail("truncated PNG");
        const std::uint8_t* c = &d[p + 8];
        const auto is = [&](const char* t) { return std::memcmp(&d[p + 4], t, 4) == 0; };
        if (is("IHDR")) {
            if (len < 13) return fail("bad PNG header");
            w = int(std::min<std::uint32_t>(be32(c), 1u << 15));
            h = int(std::min<std::uint32_t>(be32(c + 4), 1u << 15));
            depth = c[8];
            type = c[9];
            if (c[12]) return fail("interlaced PNG not supported");
        } else if (is("IDAT")) z.insert(z.end(), c, c + len);
        else if (is("PLTE")) plte.assign(c, c + len);
        else if (is("tRNS")) trns.assign(c, c + len);
        else if (is("IEND")) break;
        p += 12 + len;
    }
    static const int channels_of[7] = {1, 0, 3, 1, 2, 0, 4};
    const int channels = type >= 0 && type <= 6 ? channels_of[type] : 0;
    const bool depth_ok = depth == 8 || (depth == 16 && type != 3) || ((depth == 1 || depth == 2 || depth == 4) && (type == 0 || type == 3));
    if (w <= 0 || h <= 0 || !channels || !depth_ok || (w | h) >= 1 << 15) return fail("unsupported PNG format");
    const std::size_t bits = std::size_t(channels) * depth, stride = (std::size_t(w) * bits + 7) / 8, bpp = std::max<std::size_t>(1, bits / 8);
    std::vector<std::uint8_t> raw((stride + 1) * h);
    uLongf n = uLongf(raw.size());
    if (uncompress(raw.data(), &n, z.data(), uLong(z.size())) != Z_OK || n != raw.size()) return fail("corrupt PNG data");
    // Unfilter in place: row y's bytes live at raw[y * (stride + 1) + 1 ..].
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = &raw[std::size_t(y) * (stride + 1) + 1];
        const std::uint8_t* up = y ? row - (stride + 1) : nullptr;
        const int f = row[-1];
        if (f > 4) return fail("bad PNG filter");
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? row[i - bpp] : 0, b = up ? up[i] : 0, c = up && i >= bpp ? up[i - bpp] : 0;
            const int pred = f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, c) : 0;
            row[i] = std::uint8_t(row[i] + pred);
        }
    }
    rgba.assign(std::size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = &raw[std::size_t(y) * (stride + 1) + 1];
        for (int x = 0; x < w; ++x) {
            std::uint8_t* o = &rgba[(std::size_t(y) * w + x) * 4];
            // Sample ch of pixel x, scaled to 8 bits (sub-byte: unpacked and stretched, 16-bit: high byte).
            const auto s = [&](int ch) -> unsigned {
                if (depth == 8) return row[std::size_t(x) * channels + ch];
                if (depth == 16) return row[(std::size_t(x) * channels + ch) * 2];
                const unsigned v = row[std::size_t(x) * depth / 8] >> (8 - depth - int(std::size_t(x) * depth % 8)) & ((1u << depth) - 1);
                return type == 3 ? v : v * 255 / ((1u << depth) - 1);
            };
            if (type == 3) {
                const unsigned i = depth == 8 ? row[x] : s(0);
                if (i * 3 + 2 >= plte.size()) return fail("PNG palette index out of range");
                o[0] = plte[i * 3], o[1] = plte[i * 3 + 1], o[2] = plte[i * 3 + 2];
                o[3] = i < trns.size() ? trns[i] : 255;
            } else if (channels <= 2) {
                o[0] = o[1] = o[2] = std::uint8_t(s(0));
                if (channels == 2) o[3] = std::uint8_t(s(1));
            } else {
                o[0] = std::uint8_t(s(0)), o[1] = std::uint8_t(s(1)), o[2] = std::uint8_t(s(2));
                if (channels == 4) o[3] = std::uint8_t(s(3));
            }
        }
    }
    return true;
}
}  // namespace joa::engine
