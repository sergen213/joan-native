// joa-model-dump: summarize .SMO/.AM1/.AM2/.AM3/.DDS files; export OBJ (meshes) or PNG (DDS).
//   joa-model-dump <file> [--obj out.obj] [--anim clip.am2] [--time ms] [--png out.png]
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "formats/anim_clip.hpp"
#include "formats/dds.hpp"
#include "formats/model_mesh.hpp"

using namespace joa::formats;

namespace {

std::vector<uint8_t> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

std::string ext_of(const std::string& p) {
  auto dot = p.rfind('.');
  std::string e = dot == std::string::npos ? "" : p.substr(dot + 1);
  for (char& c : e) c = char(std::tolower(uint8_t(c)));
  return e;
}

// Minimal PNG writer: RGBA8, zlib "stored" blocks (no compression).
bool write_png(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
  static uint32_t crcTab[256];
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTab[n] = c;
  }
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (w * 4 + 1));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  size_t o = 0;
  do {  // raw is never empty (h >= 1)
    size_t n = std::min<size_t>(65535, raw.size() - o);
    z.push_back(o + n == raw.size());
    z.insert(z.end(), {uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8)});
    z.insert(z.end(), raw.begin() + o, raw.begin() + o + n);
    o += n;
  } while (o < raw.size());
  uint32_t adler = b << 16 | a;
  z.insert(z.end(), {uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler)});
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
    uint32_t n = uint32_t(d.size());
    out.insert(out.end(), {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), d.begin(), d.end());
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = start; i < out.size(); ++i) c = crcTab[(c ^ out[i]) & 0xFF] ^ (c >> 8);
    c ^= 0xFFFFFFFFu;
    out.insert(out.end(), {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)});
  };
  chunk("IHDR", {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16),
                 uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0});
  chunk("IDAT", z);
  chunk("IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return bool(f);
}

int dump_mesh(const std::string& path, bool skinned, const std::string& obj, const std::string& animPath, float t) {
  Mesh m;
  std::string err;
  if (!parse_mesh(slurp(path), skinned, m, err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
  std::vector<Mat3x4> pose;
  if (skinned && !animPath.empty()) {
    Animation a;
    if (!parse_animation(slurp(animPath), a, err)) { std::fprintf(stderr, "%s: %s\n", animPath.c_str(), err.c_str()); return 1; }
    pose = sample_pose(a, t);
  }
  std::printf("%s mesh: bones=%u headerField=%u sphere=(%g %g %g r=%g) submeshes=%zu textures=%zu morphFrames=%zu constantNormals=%d trailing=%zu\n",
              skinned ? "AM1 skinned" : "SMO static", m.boneCount, m.headerField, m.sphere[0], m.sphere[1], m.sphere[2],
              m.sphere[3], m.submeshes.size(), m.textures.size(), m.morphFrames.size(), m.constantNormals, m.trailingBytes);
  for (size_t i = 0; i < m.textures.size(); ++i)
    std::printf("  texture[%zu] \"%s\" embedded=%zu\n", i, m.textures[i].name.c_str(), m.textures[i].embedded.size());
  std::vector<std::vector<MeshVertex>> verts;
  for (size_t i = 0; i < m.submeshes.size(); ++i) {
    const Submesh& s = m.submeshes[i];
    verts.push_back(skinned ? skin_vertices(s, pose) : s.vertices);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (auto& v : verts.back())
      for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v.pos[k]); hi[k] = std::max(hi[k], v.pos[k]); }
    std::printf("  sub[%zu] verts=%zu strip=%zu tris=%zu influences=%zu tex=%d blend=%d detail=%d spec=%d flag=%u",
                i, s.vertices.size(), s.strip.size(), strip_to_triangles(s.strip).size() / 3, s.influences.size(), s.texture,
                s.blendMode, s.detailTexture, s.specularMode, s.tag2Flag);
    if (!s.vertices.empty() && (!skinned || !pose.empty()))
      std::printf(" bbox=(%.2f %.2f %.2f)-(%.2f %.2f %.2f)", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    std::printf("\n    colors(ARGB):");
    for (auto& c : s.colors) std::printf(" (%.3g %.3g %.3g %.3g)", c.a, c.r, c.g, c.b);
    std::printf("\n");
  }
  if (obj.empty()) return 0;
  if (skinned && pose.empty()) std::fprintf(stderr, "warning: AM1 without --anim: positions use identity bones\n");
  std::string mtl = obj.substr(0, obj.rfind('.')) + ".mtl";
  std::ofstream o(obj), mo(mtl);
  o << "mtllib " << mtl.substr(mtl.find_last_of('/') + 1) << "\n";
  for (size_t i = 0; i < m.textures.size(); ++i) mo << "newmtl tex" << i << "\nmap_Kd " << m.textures[i].name << "\n";
  size_t base = 1;
  for (size_t i = 0; i < m.submeshes.size(); ++i) {
    o << "o sub" << i << "\n";
    if (m.submeshes[i].texture >= 0) o << "usemtl tex" << m.submeshes[i].texture << "\n";
    for (auto& v : verts[i]) o << "v " << v.pos[0] << ' ' << v.pos[1] << ' ' << v.pos[2] << "\n";
    for (auto& v : verts[i]) o << "vt " << v.uv[0] << ' ' << 1 - v.uv[1] << "\n";  // OBJ v is bottom-up
    for (auto& v : verts[i]) o << "vn " << v.normal[0] << ' ' << v.normal[1] << ' ' << v.normal[2] << "\n";
    auto tri = strip_to_triangles(m.submeshes[i].strip);
    for (size_t k = 0; k < tri.size(); k += 3) {
      o << 'f';
      for (int j = 0; j < 3; ++j) { size_t x = base + tri[k + j]; o << ' ' << x << '/' << x << '/' << x; }
      o << "\n";
    }
    base += verts[i].size();
  }
  std::printf("wrote %s\n", obj.c_str());
  return o ? 0 : 1;
}

int dump_skeleton(const std::string& path) {
  Skeleton s;
  std::string err;
  if (!parse_skeleton(slurp(path), s, err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
  std::printf("AM3 skeleton: %zu nodes\n", s.nodes.size());
  for (size_t i = 0; i < s.nodes.size(); ++i)
    std::printf("  [%zu] parent=%d children=%zu \"%s\"%s\n", i, s.nodes[i].parent, s.nodes[i].children.size(),
                s.nodes[i].name.c_str(), s.nodes[i].present ? "" : " (undeclared)");
  return 0;
}

int dump_anim(const std::string& path) {
  Animation a;
  std::string err;
  if (!parse_animation(slurp(path), a, err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
  size_t keys = 0;
  for (auto& t : a.tracks) keys += t.size();
  std::printf("AM2 animation: time %g..%g ms, tracks=%zu keys=%zu clips=%zu extraTracks=%zu events=%zu trailing=%zu\n",
              a.start, a.end, a.tracks.size(), keys, a.clips.size(), a.extraTracks.size(), a.events.size(), a.trailingBytes);
  for (auto& c : a.clips) std::printf("  clip \"%s\" %g..%g flags=%u\n", c.name.c_str(), c.start, c.end, c.flags);
  for (auto& e : a.events) std::printf("  event t=%g id=%d \"%s\" flag=%u\n", e.time, e.id, e.name.c_str(), e.flag);
  return 0;
}

int dump_dds(const std::string& path, const std::string& png) {
  DdsTexture t;
  std::string err;
  if (!parse_dds(slurp(path), t, err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
  std::printf("DDS %s %ux%u mips=%u cubemap=%d images=%zu\n", t.format.c_str(), t.width, t.height, t.mipCount, t.cubemap,
              t.images.size());
  if (png.empty()) return 0;
  if (!write_png(png, t.images[0].width, t.images[0].height, t.images[0].rgba)) return 1;
  std::printf("wrote %s\n", png.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string file, obj, png, anim;
  float t = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    bool more = i + 1 < argc;
    if (a == "--obj" && more) obj = argv[++i];
    else if (a == "--png" && more) png = argv[++i];
    else if (a == "--anim" && more) anim = argv[++i];
    else if (a == "--time" && more) t = std::strtof(argv[++i], nullptr);
    else if (file.empty() && a[0] != '-') file = a;
    else { file.clear(); break; }
  }
  if (file.empty()) {
    std::fprintf(stderr, "usage: %s <file.smo|am1|am2|am3|dds> [--obj out.obj] [--anim clip.am2] [--time ms] [--png out.png]\n", argv[0]);
    return 2;
  }
  std::string e = ext_of(file);
  if (e == "smo" || e == "am1") return dump_mesh(file, e == "am1", obj, anim, t);
  if (e == "am3") return dump_skeleton(file);
  if (e == "am2") return dump_anim(file);
  if (e == "dds") return dump_dds(file, png);
  std::fprintf(stderr, "unknown extension .%s\n", e.c_str());
  return 2;
}
