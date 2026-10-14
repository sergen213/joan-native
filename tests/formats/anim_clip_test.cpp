// Parses every .AM3 skeleton and .AM2 animation in the install and checks structural invariants.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "formats/anim_clip.hpp"

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
  // Unit checks on synthetic data: truncation is rejected, sampling interpolates.
  {
    Skeleton s;
    std::string err;
    const uint8_t bad[] = {5, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!parse_skeleton(bad, s, err), "truncated AM3 accepted");
    Animation a;
    a.tracks = {{{0, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}, {10, {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0}}}};
    CHECK(std::fabs(sample_pose(a, 5)[0][3] - 5) < 1e-5f && sample_pose(a, 99)[0][3] == 10, "sample_pose");
  }

  const char* env = std::getenv("JOA_GAME_DIR");
  if (!env || !fs::exists(fs::path(env) / "anim")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }
  const fs::path root = env;

  size_t nAm3 = 0, nodes = 0, nAm2 = 0, tracks = 0, keys = 0, clips = 0, events = 0, paired = 0;
  for (const char* d : {"model", "item", "unit", "anim", "map"}) {
    if (!fs::exists(root / d)) continue;
    for (auto& e : fs::recursive_directory_iterator(root / d)) {
      std::string ext = lower(e.path().extension().string());
      const std::string rel = fs::relative(e.path(), root).string();
      std::string err;
      if (ext == ".am3") {
        Skeleton s;
        bool ok = parse_skeleton(slurp(e.path()), s, err);
        CHECK(ok, "%s: %s", rel.c_str(), err.c_str());
        if (!ok) continue;
        ++nAm3;
        nodes += s.nodes.size();
        size_t roots = 0;
        for (auto& n : s.nodes) {
          CHECK(n.present, "%s: undeclared node id", rel.c_str());
          roots += n.parent < 0;
        }
        CHECK(roots >= 1 || s.nodes.empty(), "%s: no root", rel.c_str());
      } else if (ext == ".am2") {
        Animation a;
        bool ok = parse_animation(slurp(e.path()), a, err);
        CHECK(ok, "%s: %s", rel.c_str(), err.c_str());
        if (!ok) continue;
        ++nAm2;
        CHECK(a.trailingBytes == 0, "%s: %zu trailing bytes", rel.c_str(), a.trailingBytes);
        tracks += a.tracks.size();
        clips += a.clips.size();
        events += a.events.size();
        for (auto& c : a.clips) CHECK(c.start <= c.end, "%s: clip %s reversed", rel.c_str(), c.name.c_str());
        for (auto& t : a.tracks) {
          keys += t.size();
          for (size_t i = 0; i < t.size(); ++i) {
            CHECK(i == 0 || t[i - 1].time <= t[i].time, "%s: key times not sorted", rel.c_str());
            for (float f : t[i].m)
              if (!(std::isfinite(f) && std::fabs(f) < 1e5f)) { CHECK(false, "%s: bad matrix value %g", rel.c_str(), f); break; }
          }
        }
        // Tracks are indexed by the sibling AM3 node ids.
        for (const char* sx : {".am3", ".AM3"}) {
          fs::path sk = e.path();
          sk.replace_extension(sx);
          if (!fs::exists(sk)) continue;
          Skeleton s;
          if (parse_skeleton(slurp(sk), s, err)) {
            ++paired;
            CHECK(s.nodes.size() == a.tracks.size(), "%s: %zu tracks vs %zu nodes", rel.c_str(), a.tracks.size(), s.nodes.size());
          }
          break;
        }
      }
    }
  }
  std::printf("AM3 %zu files / %zu nodes; AM2 %zu files / %zu tracks / %zu keys / %zu clips / %zu events; %zu AM2-AM3 pairs\n",
              nAm3, nodes, nAm2, tracks, keys, clips, events, paired);
  CHECK(nAm3 > 900 && nAm2 > 300, "too few files found");
  return failures ? 1 : 0;
}
