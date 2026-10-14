#include "formats/dds.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "formats/model_reader.hpp"

namespace joa::formats {
namespace {

bool fail(std::string& err, std::string msg) { err = std::move(msg); return false; }

constexpr uint32_t kFourCC = 0x4, kRgb = 0x40, kAlphaPixels = 0x1, kAlphaOnly = 0x2, kLuminance = 0x20000;
constexpr uint32_t kCubemap = 0x200, kCubeFaces = 0xFC00;

void rgb565(uint16_t c, uint8_t* o) {
  o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  o[2] = uint8_t((c & 31) * 255 / 31);
  o[3] = 255;
}

// Colour half of a DXT block -> 16 RGBA pixels. dxt1 enables the 3-colour + transparent mode.
void color_block(const uint8_t* b, bool dxt1, uint8_t px[16][4]) {
  uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  uint8_t pal[4][4];
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  if (c0 > c1 || !dxt1) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
    pal[2][3] = 255;
    std::memset(pal[3], 0, 4);
  }
  uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
  for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

void alpha_dxt5(const uint8_t* b, uint8_t px[16][4]) {
  uint8_t a[8] = {b[0], b[1]};
  if (a[0] > a[1]) {
    for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
}

void decode_dxt(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst) {
  const size_t blockBytes = kind == 1 ? 8 : 16;
  for (uint32_t by = 0; by < (h + 3) / 4; ++by)
    for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx, src += blockBytes) {
      uint8_t px[16][4];
      if (kind == 1) {
        color_block(src, true, px);
      } else {
        color_block(src + 8, false, px);
        if (kind == 3) {
          for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(((src[i / 2] >> (4 * (i & 1))) & 15) * 17);
        } else {
          alpha_dxt5(src, px);
        }
      }
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
          uint32_t X = bx * 4 + x, Y = by * 4 + y;
          if (X < w && Y < h) std::memcpy(dst + (size_t(Y) * w + X) * 4, px[y * 4 + x], 4);
        }
    }
}

// Generic bit-mask channel: value scaled to 0..255; missing mask -> def.
uint8_t channel(uint32_t v, uint32_t mask, uint8_t def) {
  if (!mask) return def;
  int shift = std::countr_zero(mask);
  uint32_t max = mask >> shift;
  return uint8_t(uint64_t((v & mask) >> shift) * 255 / max);
}

}  // namespace

bool parse_dds(std::span<const uint8_t> data, DdsTexture& t, std::string& err, uint32_t maxMips) {
  t = DdsTexture{};
  if (data.size() < 128 || std::memcmp(data.data(), "DDS ", 4) != 0) return fail(err, "not a DDS file");
  detail::ByteReader r{data};
  r.skip(4);
  uint32_t hdr[31];
  r.read(hdr, sizeof hdr);
  // hdr index = (file offset - 4) / 4
  uint32_t height = hdr[2], width = hdr[3], mips = hdr[6];
  uint32_t pfFlags = hdr[19], fourcc = hdr[20], bits = hdr[21];
  uint32_t rm = hdr[22], gm = hdr[23], bm = hdr[24], am = hdr[25], caps2 = hdr[27];
  if (hdr[0] != 124 || width == 0 || height == 0 || width > 16384 || height > 16384) return fail(err, "bad header");
  t.width = width;
  t.height = height;
  uint32_t fullChain = uint32_t(std::bit_width(std::max(width, height)));
  t.mipCount = std::clamp<uint32_t>(mips, 1, fullChain);
  uint32_t faces = 1;
  if (caps2 & kCubemap) {
    t.cubemap = true;
    faces = uint32_t(std::popcount(caps2 & kCubeFaces));
    if (faces == 0) faces = 6;
  }

  int dxt = 0;  // 1, 3 (DXT2/3), 5 (DXT4/5); 0 = uncompressed
  if (pfFlags & kFourCC) {
    char cc[5] = {char(fourcc), char(fourcc >> 8), char(fourcc >> 16), char(fourcc >> 24), 0};
    t.format = cc;
    if (t.format == "DXT1") dxt = 1;
    else if (t.format == "DXT2" || t.format == "DXT3") dxt = 3;
    else if (t.format == "DXT4" || t.format == "DXT5") dxt = 5;
    else return fail(err, "unsupported FourCC " + t.format);
  } else {
    if (!(pfFlags & (kRgb | kLuminance | kAlphaOnly)) || bits == 0 || bits > 32 || bits % 8) return fail(err, "unsupported pixel format");
    t.format = "RGBA" + std::to_string(bits);
  }
  uint32_t decodeMips = maxMips ? std::min(maxMips, t.mipCount) : t.mipCount;

  for (uint32_t f = 0; f < faces; ++f) {
    uint32_t w = width, h = height;
    for (uint32_t m = 0; m < t.mipCount; ++m, w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
      size_t size = dxt ? size_t((w + 3) / 4) * ((h + 3) / 4) * (dxt == 1 ? 8 : 16) : size_t(w) * h * (bits / 8);
      if (!r.has(size)) return fail(err, "truncated surface face " + std::to_string(f) + " mip " + std::to_string(m));
      const uint8_t* src = data.data() + r.o;
      r.o += size;
      if (m >= decodeMips) continue;
      DdsImage img{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
      if (dxt) {
        decode_dxt(src, w, h, dxt, img.rgba.data());
      } else {
        const uint32_t bpp = bits / 8;
        const bool lum = (pfFlags & kLuminance) && !(pfFlags & kRgb);
        const uint32_t amask = (pfFlags & (kAlphaPixels | kAlphaOnly)) ? am : 0;
        for (size_t i = 0; i < size_t(w) * h; ++i) {
          uint32_t v = 0;
          std::memcpy(&v, src + i * bpp, bpp);
          uint8_t* o = &img.rgba[i * 4];
          if (lum) o[0] = o[1] = o[2] = channel(v, rm, 0);
          else if (pfFlags & kRgb) { o[0] = channel(v, rm, 0); o[1] = channel(v, gm, 0); o[2] = channel(v, bm, 0); }
          else o[0] = o[1] = o[2] = 255;  // alpha-only surface
          o[3] = channel(v, amask, 255);
        }
      }
      t.images.push_back(std::move(img));
    }
  }
  if (decodeMips < t.mipCount) t.mipCount = decodeMips;
  return true;
}

}  // namespace joa::formats
