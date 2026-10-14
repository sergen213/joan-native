// DDS decoder: synthetic known-answer blocks + every .dds in the install (all mips, all faces).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "formats/dds.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

static std::vector<uint8_t> slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

static std::vector<uint8_t> header(uint32_t w, uint32_t h, uint32_t pfFlags, const char* fourcc, uint32_t bits,
                                   uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  std::vector<uint8_t> d(128, 0);
  auto put = [&](size_t off, uint32_t v) { std::memcpy(&d[off], &v, 4); };
  std::memcpy(d.data(), "DDS ", 4);
  put(4, 124); put(12, h); put(16, w); put(28, 1); put(76, 32); put(80, pfFlags);
  if (fourcc) std::memcpy(&d[84], fourcc, 4);
  put(88, bits); put(92, r); put(96, g); put(100, b); put(104, a);
  return d;
}

int main() {
  std::string err;
  {  // DXT1 4x4: c0 = red (0xF800), c1 = blue (0x001F), indices 0,1,2,3 on row 0.
    auto d = header(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0);
    d.insert(d.end(), {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0, 0, 0});
    DdsTexture t;
    CHECK(parse_dds(d, t, err), "%s", err.c_str());
    const uint8_t want[16] = {255, 0, 0, 255, 0, 0, 255, 255, 170, 0, 85, 255, 85, 0, 170, 255};
    CHECK(t.images.size() == 1 && std::memcmp(t.images[0].rgba.data(), want, 16) == 0, "DXT1 decode");
  }
  {  // DXT1 punch-through: c0 <= c1 -> index 3 is transparent black.
    auto d = header(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0);
    d.insert(d.end(), {0x1F, 0x00, 0x00, 0xF8, 0x03, 0, 0, 0});
    DdsTexture t;
    CHECK(parse_dds(d, t, err) && t.images[0].rgba[3] == 0 && t.images[0].rgba[7] == 255, "DXT1 alpha");
  }
  {  // A8R8G8B8 1x1 + A4R4G4B4 1x1.
    auto d = header(1, 1, 0x41, nullptr, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000);
    d.insert(d.end(), {0x30, 0x20, 0x10, 0x80});  // B G R A
    DdsTexture t;
    CHECK(parse_dds(d, t, err) && (t.images[0].rgba == std::vector<uint8_t>{0x10, 0x20, 0x30, 0x80}), "A8R8G8B8");
    d = header(1, 1, 0x41, nullptr, 16, 0xF00, 0xF0, 0xF, 0xF000);
    d.insert(d.end(), {0x21, 0xF3});  // A=F R=3 G=2 B=1
    CHECK(parse_dds(d, t, err) && (t.images[0].rgba == std::vector<uint8_t>{0x33, 0x22, 0x11, 0xFF}), "A4R4G4B4");
    // Shipped int8x6 A4R4G4B4 headers declare R mask 0xF000 (= alpha); D3DX reads red from 0x0F00.
    auto bad = header(1, 1, 0x41, nullptr, 16, 0xF000, 0xF0, 0xF, 0xF000);
    bad.insert(bad.end(), {0x21, 0x83});  // A=8 R=3 G=2 B=1
    CHECK(parse_dds(bad, t, err) && (t.images[0].rgba == std::vector<uint8_t>{0x33, 0x22, 0x11, 0x88}), "A4R4G4B4 bad R mask");
    d.pop_back();
    CHECK(!parse_dds(d, t, err), "truncated accepted");
  }

  const char* env = std::getenv("JOA_GAME_DIR");
  if (!env || !fs::exists(fs::path(env) / "texture")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }
  const fs::path root = env;
  std::map<std::string, size_t> formats;
  size_t files = 0, cubes = 0, images = 0;
  for (const char* dir : {"texture", "int8x6", "item", "unit", "model", "map", "Resource", "Save"}) {
    if (!fs::exists(root / dir)) continue;
    for (auto& e : fs::recursive_directory_iterator(root / dir)) {
      std::string ext = e.path().extension().string();
      if (ext != ".dds" && ext != ".DDS") continue;
      const std::string rel = fs::relative(e.path(), root).string();
      DdsTexture t;
      bool ok = parse_dds(slurp(e.path()), t, err);
      CHECK(ok, "%s: %s", rel.c_str(), err.c_str());
      if (!ok) continue;
      ++files;
      ++formats[t.format];
      cubes += t.cubemap;
      images += t.images.size();
      CHECK(t.images.size() == size_t(t.mipCount) * (t.cubemap ? 6 : 1), "%s: image count", rel.c_str());
      for (auto& im : t.images) CHECK(im.rgba.size() == size_t(im.width) * im.height * 4, "%s: image size", rel.c_str());
    }
  }
  std::printf("DDS %zu files, %zu cubemaps, %zu decoded images;", files, cubes, images);
  for (auto& [f, n] : formats) std::printf(" %s=%zu", f.c_str(), n);
  std::printf("\n");
  CHECK(files > 4000, "too few DDS files");
  return failures ? 1 : 0;
}
