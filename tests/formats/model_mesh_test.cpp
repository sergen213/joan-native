// Parses every .SMO/.AM1 in the install; checks bounds, indices, bones, weights, texture resolution.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "formats/anim_clip.hpp"
#include "formats/model_mesh.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

static std::vector<uint8_t> slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
static std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(uint8_t(c))); return s; }

int main() {
  // Unit check: engine strip decoding (degenerates skipped, odd positions swapped).
  {
    const uint16_t s[] = {0, 0, 1, 2, 3, 3, 4};
    auto t = strip_to_triangles(s);
    CHECK((t == std::vector<uint32_t>{1, 0, 2, 1, 2, 3}), "strip decode got %zu idx", t.size());
  }

  const char* env = std::getenv("JOA_GAME_DIR");
  if (!env || !fs::exists(fs::path(env) / "model")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }
  const fs::path root = env;

  // Texture resolution as joan.exe FUN_00608e10: base name (extension stripped), case-insensitive,
  // among .dds/.tga/.bmp under TEXTURE\ (+ITEM,UNIT,TERRAIN,PARTICLE,MISC), INT8X6\ (+ITEM), SAVE\.
  std::set<std::string> texNames;
  for (const char* d : {"texture", "int8x6", "Save"})
    if (fs::exists(root / d))
      for (auto& e : fs::recursive_directory_iterator(root / d)) {
        std::string ext = lower(e.path().extension().string());
        if (ext == ".dds" || ext == ".tga" || ext == ".bmp") texNames.insert(lower(e.path().stem().string()));
      }
  // Referenced by shipped models but absent from the GOG install (verified with find -iname).
  const std::set<std::string> knownMissing = {"pinealpha", "meung01de2", "monastery", "sky_night01", "sky_night02",
                                              "sky_night03", "sky_night04", "phillip", "footstep", "footstepalp",
                                              "panda_nor", "snowtree_high", "grass", "gateroof", "lc_road1"};

  size_t files[2] = {}, subs = 0, verts = 0, tris = 0, texRefs = 0, unresolved = 0, am3Checked = 0;
  for (const char* d : {"model", "item", "unit", "anim", "map"}) {
    if (!fs::exists(root / d)) continue;
    for (auto& e : fs::recursive_directory_iterator(root / d)) {
      std::string ext = lower(e.path().extension().string());
      if (ext != ".smo" && ext != ".am1") continue;
      const bool skinned = ext == ".am1";
      const std::string rel = fs::relative(e.path(), root).string();
      Mesh m;
      std::string err;
      bool ok = parse_mesh(slurp(e.path()), skinned, m, err);
      CHECK(ok, "%s: %s", rel.c_str(), err.c_str());
      if (!ok) continue;
      ++files[skinned];
      CHECK(m.trailingBytes <= 1, "%s: %zu trailing bytes", rel.c_str(), m.trailingBytes);  // grass03.SMO has 1
      for (auto& s : m.submeshes) {
        ++subs;
        verts += s.vertices.size();
        auto t = strip_to_triangles(s.strip);
        tris += t.size() / 3;
        for (uint32_t i : t) CHECK(i < s.vertices.size(), "%s: index %u >= %zu", rel.c_str(), i, s.vertices.size());
        // Positions must be sane; UVs only finite (firebasket.SMO ships 6 vertices with UV ~3e21).
        for (auto& v : s.vertices) {
          bool ok = std::isfinite(v.uv[0]) && std::isfinite(v.uv[1]);
          for (float f : v.pos) ok = ok && std::isfinite(f) && std::fabs(f) < 1e5f;
          if (!ok) { CHECK(false, "%s: bad vertex (%g %g %g)", rel.c_str(), v.pos[0], v.pos[1], v.pos[2]); break; }
        }
        if (skinned) {
          CHECK(s.skinOffsets.size() == s.vertices.size() + 1, "%s: skin offsets", rel.c_str());
          for (size_t i = 0; i + 1 < s.skinOffsets.size(); ++i) {
            float w = 0;
            for (uint32_t j = s.skinOffsets[i]; j < s.skinOffsets[i + 1]; ++j) {
              const auto& in = s.influences[j];
              CHECK(in.bone < m.boneCount, "%s: bone", rel.c_str());
              CHECK(std::isfinite(in.pos[0] + in.pos[1] + in.pos[2]) && std::fabs(in.pos[0]) + std::fabs(in.pos[1]) + std::fabs(in.pos[2]) < 1e5f,
                    "%s: influence pos", rel.c_str());
              w += in.weight;
            }
            CHECK(std::fabs(w - 1.f) < 0.01f, "%s: vertex %zu weight sum %g", rel.c_str(), i, w);
          }
        }
      }
      for (auto& t : m.textures) {
        ++texRefs;
        std::string base = lower(t.name.substr(0, t.name.rfind('.')));
        if (!t.embedded.empty() || texNames.count(base)) continue;
        ++unresolved;
        CHECK(knownMissing.count(base), "%s: texture \"%s\" not found", rel.c_str(), t.name.c_str());
      }
      // AM1 bones index the sibling AM3 skeleton (same stem). item/anim/fd_meat.AM1 declares 2 bones,
      // its AM3 only 1 node: original data quirk, bone ids still fit the table the engine allocates.
      if (skinned) {
        for (const char* sx : {".AM3", ".am3"}) {
          fs::path sk = e.path();
          sk.replace_extension(sx);
          if (!fs::exists(sk)) continue;
          Skeleton skel;
          CHECK(parse_skeleton(slurp(sk), skel, err), "%s: %s", sk.c_str(), err.c_str());
          ++am3Checked;
          CHECK(skel.nodes.size() == m.boneCount || rel.find("fd_meat") != std::string::npos, "%s: %u bones vs AM3 %zu nodes",
                rel.c_str(), m.boneCount, skel.nodes.size());
          break;
        }
      }
    }
  }
  std::printf("SMO %zu, AM1 %zu files; %zu submeshes, %zu vertices, %zu triangles; %zu texture refs (%zu known-missing); %zu AM1/AM3 pairs\n",
              files[0], files[1], subs, verts, tris, texRefs, unresolved, am3Checked);
  CHECK(files[0] > 1000 && files[1] > 500, "too few files found");

  // Joan posed with her own animation at t=0 must be a standing human of sane height (Z up).
  {
    Mesh m;
    Animation a;
    std::string err;
    CHECK(parse_mesh(slurp(root / "unit/Joan_of_Arc.AM1"), true, m, err), "%s", err.c_str());
    CHECK(parse_animation(slurp(root / "anim/joan_of_arc.am2"), a, err), "%s", err.c_str());
    auto pose = sample_pose(a, 0);
    float lo = 1e9f, hi = -1e9f;
    for (auto& s : m.submeshes)
      for (auto& v : skin_vertices(s, pose)) { lo = std::min(lo, v.pos[2]); hi = std::max(hi, v.pos[2]); }
    std::printf("Joan posed height: %.2f..%.2f\n", lo, hi);
    CHECK(lo > -5 && lo < 5 && hi > 50 && hi < 120, "Joan height %g..%g", lo, hi);
  }
  return failures ? 1 : 0;
}
