#include "formats/anim_clip.hpp"

#include <algorithm>

#include "formats/model_reader.hpp"

namespace joa::formats {
namespace {

using detail::ByteReader;

bool fail(std::string& err, std::string msg) { err = std::move(msg); return false; }

constexpr uint32_t kMaxNodeId = 0xFFFF;  // engine has no cap; ours prevents absurd allocations
constexpr Mat3x4 kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

}  // namespace

// AM3 (FUN_005b9af0): u32 count; count * { u32 id; char name[50]; u32 nChildren; u32 child[n] }.
// Node table size = max(count, every id + 1).
bool parse_skeleton(std::span<const uint8_t> data, Skeleton& out, std::string& err) {
  out = Skeleton{};
  ByteReader r{data};
  uint32_t count = r.u32();
  if (!r.ok || !r.fits(count, 58)) return fail(err, "bad node count");
  auto node = [&](uint32_t id) -> SkeletonNode& {
    if (id >= out.nodes.size()) out.nodes.resize(size_t(id) + 1);
    return out.nodes[id];
  };
  out.nodes.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = r.u32();
    std::string name = r.str(50);
    uint32_t nc = r.u32();
    if (!r.ok || id > kMaxNodeId || !r.fits(nc, 4)) return fail(err, "truncated node");
    SkeletonNode& n = node(id);
    n.name = std::move(name);
    n.present = true;
    for (uint32_t c = 0; c < nc; ++c) {
      uint32_t child = r.u32();
      if (child > kMaxNodeId) return fail(err, "child id out of range");
      node(child);
      out.nodes[id].children.push_back(child);  // re-index: node() may have reallocated
    }
  }
  if (!r.ok) return fail(err, "truncated skeleton");
  for (uint32_t id = 0; id < out.nodes.size(); ++id)
    for (uint32_t c : out.nodes[id].children) out.nodes[c].parent = int32_t(id);
  return true;
}

// AM2 (animdef.cpp loader at 0x5b5830 + track reader, trailer FUN_005b5350).
bool parse_animation(std::span<const uint8_t> data, Animation& a, std::string& err) {
  a = Animation{};
  ByteReader r{data};
  uint32_t ntracks = r.u32(), nclips = r.u32();
  a.start = r.f32();
  a.end = r.f32();
  if (!r.ok || !r.fits(nclips, 62) || !r.fits(ntracks, 4)) return fail(err, "bad header");
  a.clips.resize(nclips);
  for (auto& c : a.clips) {
    c.name = r.str(50);
    c.flags = r.u32();
    c.start = r.f32();
    c.end = r.f32();
  }
  a.tracks.resize(ntracks);
  for (auto& t : a.tracks) {
    uint32_t nk = r.u32();
    if (!r.fits(nk, 52)) return fail(err, "truncated track");
    t.resize(nk);
    for (auto& k : t) {
      k.time = r.f32();
      for (float& f : k.m) f = r.f32();
    }
  }
  if (!r.ok) return fail(err, "truncated tracks");

  // Trailer: u32 tag until 0 (EOF reads as 0). Tag 1 = extra tracks + events; others carry no data.
  while (r.left() >= 4) {
    uint32_t tag = r.u32();
    if (tag == 0) break;
    if (tag != 1) continue;
    uint32_t n = r.u32();
    if (!r.fits(n, 8)) return fail(err, "bad extra track count");
    a.extraTracks.resize(n);
    for (auto& x : a.extraTracks) {
      uint32_t n4 = r.u32();
      if (!r.fits(n4, 16)) return fail(err, "truncated extra track");
      x.keys4.resize(n4);
      for (auto& k : x.keys4) for (float& f : k) f = r.f32();
      uint32_t n2 = r.u32();
      if (!r.fits(n2, 8)) return fail(err, "truncated extra track");
      x.scalarKeys.resize(n2);
      for (auto& k : x.scalarKeys) for (float& f : k) f = r.f32();
    }
    uint32_t ne = r.u32();
    if (!r.fits(ne, 59)) return fail(err, "truncated events");
    a.events.resize(ne);
    for (auto& e : a.events) {
      e.time = r.f32();
      e.id = r.i32();
      e.name = r.str(50);
      e.flag = r.u8();
    }
    if (!r.ok) return fail(err, "truncated trailer");
  }
  a.trailingBytes = r.left();
  return true;
}

std::vector<Mat3x4> sample_pose(const Animation& a, float t) {
  std::vector<Mat3x4> out(a.tracks.size(), kIdentity);
  for (size_t i = 0; i < a.tracks.size(); ++i) {
    const auto& keys = a.tracks[i];
    if (keys.empty()) continue;
    auto hi = std::upper_bound(keys.begin(), keys.end(), t, [](float v, const AnimKey& k) { return v < k.time; });
    if (hi == keys.begin()) { out[i] = keys.front().m; continue; }
    if (hi == keys.end()) { out[i] = keys.back().m; continue; }
    const AnimKey& k0 = *(hi - 1);
    const AnimKey& k1 = *hi;
    float span = k1.time - k0.time, w = span > 0 ? (t - k0.time) / span : 0;
    for (size_t j = 0; j < 12; ++j) out[i][j] = k0.m[j] + (k1.m[j] - k0.m[j]) * w;
  }
  return out;
}

}  // namespace joa::formats
