#include "formats/model_mesh.hpp"

#include <algorithm>
#include <utility>

#include "formats/model_reader.hpp"

namespace joa::formats {
namespace {

using detail::ByteReader;

constexpr uint32_t kVersion = 500;

bool fail(std::string& err, std::string msg) { err = std::move(msg); return false; }

void read_color(ByteReader& r, MeshColor& c) { c.a = r.f32(); c.r = r.f32(); c.g = r.f32(); c.b = r.f32(); }

// Strip: i32 n, then (n+2) u16 when n > 0 (engine: FUN_005e0530 reads 2 + n shorts).
bool read_strip(ByteReader& r, int32_t n, std::vector<uint16_t>& strip) {
  if (n <= 0) return r.ok;
  if (!r.fits(uint64_t(n) + 2, 2)) return false;
  strip.resize(size_t(n) + 2);
  return r.read(strip.data(), strip.size() * 2);
}

// SMO vertex record: normal[3], pos[3], uv[2] (32 bytes).
bool read_static_vertices(ByteReader& r, int32_t n, std::vector<MeshVertex>& v) {
  if (n < 0 || !r.fits(uint64_t(n), 32)) return false;
  v.resize(size_t(n));
  for (auto& x : v) {
    for (float& f : x.normal) f = r.f32();
    for (float& f : x.pos) f = r.f32();
    for (float& f : x.uv) f = r.f32();
  }
  return r.ok;
}

bool check_strip(const std::vector<uint16_t>& strip, size_t nv) {
  for (uint16_t i : strip) if (i >= nv) return false;
  return true;
}

}  // namespace

bool parse_mesh(std::span<const uint8_t> data, bool skinned, Mesh& m, std::string& err) {
  m = Mesh{};
  m.skinned = skinned;
  ByteReader r{data};
  uint32_t version = r.u32();
  if (!r.ok || version != kVersion) return fail(err, "unsupported version " + std::to_string(version));
  if (skinned) m.boneCount = r.u32();
  int32_t nsub = r.i32(), ntex = r.i32();
  m.headerField = r.u32();
  for (float& f : m.sphere) f = r.f32();
  if (!r.ok || nsub < 0 || ntex < 0) return fail(err, "bad header");
  // Smallest possible submesh record is 76 bytes (AM1) - reject impossible counts early.
  if (!r.fits(uint64_t(nsub), 76) || !r.fits(uint64_t(ntex), 24)) return fail(err, "counts exceed file size");

  m.submeshes.resize(size_t(nsub));
  for (auto& s : m.submeshes) {
    int32_t nstrip = r.i32(), nv = r.i32();
    for (int c = 0; c < (skinned ? 3 : 4); ++c) read_color(r, s.colors[c]);
    for (float& f : s.sphere) f = r.f32();
    s.texture = r.i32();
    if (!read_strip(r, nstrip, s.strip)) return fail(err, "truncated strip");
    if (!skinned) {
      if (!read_static_vertices(r, nv, s.vertices)) return fail(err, "truncated vertices");
    } else {
      // AM1 vertex: uv[2], u32 nRefs, u32 nInfl, nRefs*u32 (skipped), nInfl*{u32 bone, f32 w,
      // f32 pos[3], f32 normal[3]} (engine FUN_005dc420 -> SkinVertex::add_bone).
      if (nv < 0 || !r.fits(uint64_t(nv), 16)) return fail(err, "bad vertex count");
      s.vertices.resize(size_t(nv));
      s.skinOffsets.reserve(size_t(nv) + 1);
      for (auto& v : s.vertices) {
        s.skinOffsets.push_back(uint32_t(s.influences.size()));
        v.uv[0] = r.f32();
        v.uv[1] = r.f32();
        uint32_t nrefs = r.u32(), ninf = r.u32();
        if (!r.fits(nrefs, 4) || !r.skip(size_t(nrefs) * 4) || !r.fits(ninf, 32)) return fail(err, "truncated skin vertex");
        for (uint32_t k = 0; k < ninf; ++k) {
          SkinInfluence in;
          in.bone = r.u32();
          in.weight = r.f32();
          for (float& f : in.pos) f = r.f32();
          for (float& f : in.normal) f = r.f32();
          if (in.bone >= m.boneCount) return fail(err, "bone index out of range");
          s.influences.push_back(in);
        }
      }
      s.skinOffsets.push_back(uint32_t(s.influences.size()));
      if (!r.ok) return fail(err, "truncated skin vertices");
    }
    if (!check_strip(s.strip, s.vertices.size())) return fail(err, "strip index out of range");
    if (s.texture < -1 || s.texture >= ntex) return fail(err, "texture index out of range");
  }

  // Texture table: char name[20], i32 embeddedSize, embedded bytes (D3DX in-memory image).
  m.textures.resize(size_t(ntex));
  for (auto& t : m.textures) {
    t.name = r.str(20);
    int32_t sz = r.i32();
    if (sz > 0) {
      if (!r.has(size_t(sz))) return fail(err, "truncated embedded texture");
      t.embedded.assign(data.begin() + r.o, data.begin() + r.o + sz);
      r.o += size_t(sz);
    }
  }
  if (!r.ok) return fail(err, "truncated texture table");

  // SMO morph frames: per frame, per submesh: i32 nstrip, i32 nv, strip, nv static vertices.
  if (!skinned && m.headerField) {
    if (!r.fits(m.headerField, std::max<uint64_t>(8, uint64_t(nsub) * 8))) return fail(err, "bad morph frame count");
    m.morphFrames.resize(m.headerField);
    for (auto& frame : m.morphFrames) {
      frame.resize(size_t(nsub));
      for (auto& g : frame) {
        int32_t nstrip = r.i32(), nv = r.i32();
        if (!read_strip(r, nstrip, g.strip) || !read_static_vertices(r, nv, g.vertices) ||
            !check_strip(g.strip, g.vertices.size()))
          return fail(err, "bad morph frame");
      }
    }
  }

  // Tagged trailer: i32 tag until a tag outside 1..3 (normally 0). The engine's reads return 0 at
  // EOF, so a file ending exactly at a tag boundary is valid (model/ripple.AM1).
  for (;;) {
    if (r.left() < 4) break;
    int32_t tag = r.i32();
    if (tag == 1) {
      for (auto& s : m.submeshes) { s.blendMode = r.i32(); s.detailTexture = r.i32(); }
    } else if (tag == 2) {
      for (auto& s : m.submeshes) { s.specularMode = r.i32(); s.tag2Unused = r.i32(); s.tag2Flag = r.u8(); }
    } else if (tag == 3) {
      m.constantNormals = r.u32() != 0;
    } else {
      break;
    }
    if (!r.ok) return fail(err, "truncated trailer tag " + std::to_string(tag));
  }
  for (auto& s : m.submeshes)
    if (s.detailTexture < -1 || s.detailTexture >= ntex) return fail(err, "detail texture index out of range");
  m.trailingBytes = r.left();
  return true;
}

std::vector<uint32_t> strip_to_triangles(std::span<const uint16_t> s) {
  std::vector<uint32_t> out;
  if (s.size() < 3) return out;
  out.reserve((s.size() - 2) * 3);
  for (size_t k = 2; k < s.size(); ++k) {
    uint32_t a = s[k - 2], b = s[k - 1], c = s[k];
    if (a == b || b == c || a == c) continue;
    if (k & 1) std::swap(a, b);  // parity flips every strip position, degenerates included
    out.insert(out.end(), {a, b, c});
  }
  return out;
}

void skin_vertices(const Submesh& sub, std::span<const Mat3x4> bones, std::vector<MeshVertex>& out) {
  out.assign(sub.vertices.begin(), sub.vertices.end());
  if (sub.skinOffsets.size() != sub.vertices.size() + 1) return;
  for (size_t i = 0; i < out.size(); ++i) {
    MeshVertex& v = out[i];
    for (int k = 0; k < 3; ++k) v.pos[k] = v.normal[k] = 0;
    for (uint32_t j = sub.skinOffsets[i]; j < sub.skinOffsets[i + 1]; ++j) {
      const SkinInfluence& in = sub.influences[j];
      static constexpr Mat3x4 kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      const Mat3x4& m = in.bone < bones.size() ? bones[in.bone] : kIdentity;
      for (int k = 0; k < 3; ++k) {
        const float* row = &m[size_t(k) * 4];
        v.pos[k] += in.weight * (row[0] * in.pos[0] + row[1] * in.pos[1] + row[2] * in.pos[2] + row[3]);
        v.normal[k] += in.weight * (row[0] * in.normal[0] + row[1] * in.normal[1] + row[2] * in.normal[2]);
      }
    }
  }
}

std::vector<MeshVertex> skin_vertices(const Submesh& sub, std::span<const Mat3x4> bones) {
  std::vector<MeshVertex> out;
  skin_vertices(sub, bones, out);
  return out;
}

}  // namespace joa::formats
