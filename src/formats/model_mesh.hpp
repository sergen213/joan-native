#pragma once
// .SMO (static mesh) and .AM1 (skinned mesh) parser, file version 500.
// Layout + evidence: docs/formats/MODELS_AND_ANIMATION.md (joan.exe FUN_005e0530 / FUN_005dc420).
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace joa::formats {

// 3x4 row-major [R|t]: p' = (m0 m1 m2)·p + m3, (m4 m5 m6)·p + m7, (m8 m9 m10)·p + m11.
using Mat3x4 = std::array<float, 12>;

struct MeshColor { float a = 0, r = 0, g = 0, b = 0; };  // file float order A,R,G,B
struct MeshVertex { float pos[3]{}, normal[3]{}, uv[2]{}; };
// AM1 influence: pos/normal are in bone space; world = boneMatrix * pos, weighted by weight.
struct SkinInfluence { uint32_t bone = 0; float weight = 0; float pos[3]{}, normal[3]{}; };
struct MeshGeometry { std::vector<uint16_t> strip; std::vector<MeshVertex> vertices; };

struct Submesh {
  // SMO: 4 colours (c0 -> engine material +4, overwritten by a constant DAT_00a69cd0 after load;
  // c1 -> +0; c2 specular; c3 emissive). AM1: c0..c2 only, c3 stays zero.
  MeshColor colors[4];
  float sphere[4]{};               // bounding sphere x,y,z,r (HIGH CONFIDENCE)
  int32_t texture = -1;            // index into Mesh::textures, -1 = none
  std::vector<uint16_t> strip;     // raw triangle strip (count+2 entries), see strip_to_triangles
  std::vector<MeshVertex> vertices;  // AM1: only uv is filled; use skin_vertices() for pos/normal
  std::vector<uint32_t> skinOffsets;      // AM1: size vertices+1, ranges into influences
  std::vector<SkinInfluence> influences;  // AM1
  // (The AM1 per-vertex u32 list the engine seeks past is skipped here too; see doc.)
  // Trailer tag 1 / tag 2 values per submesh (-1 = unset).
  int32_t blendMode = -1, detailTexture = -1;
  int32_t specularMode = -1, tag2Unused = -1;
  uint8_t tag2Flag = 0;
};

struct MeshTexture { std::string name; std::vector<uint8_t> embedded; };  // name max 19 chars

struct Mesh {
  bool skinned = false;
  uint32_t boneCount = 0;          // AM1 only
  uint32_t headerField = 0;        // SMO: morph frame count; AM1: read but unused by the engine
  float sphere[4]{};
  std::vector<Submesh> submeshes;
  std::vector<MeshTexture> textures;
  std::vector<std::vector<MeshGeometry>> morphFrames;  // SMO only: [frame][submesh]
  bool constantNormals = false;    // trailer tag 3: engine replaces all normals with a constant
  size_t trailingBytes = 0;
};

// skinned=false for .SMO, true for .AM1. Every read is bounds-checked; returns false + err on
// malformed input, out-of-range strip/texture/bone indices.
bool parse_mesh(std::span<const uint8_t> data, bool skinned, Mesh& out, std::string& err);

// Triangle list from a strip exactly as the engine builds it: degenerate triangles dropped,
// odd strip positions swap the first two indices.
std::vector<uint32_t> strip_to_triangles(std::span<const uint16_t> strip);

// Poses an AM1 submesh: out[i].pos/normal = sum(weight * boneMatrix[bone] * influence); uv copied.
// Bones missing from boneMatrices count as identity.
std::vector<MeshVertex> skin_vertices(const Submesh& sub, std::span<const Mat3x4> boneMatrices);
// Same, into `out` (its capacity reused: per-frame skinning without allocations).
void skin_vertices(const Submesh& sub, std::span<const Mat3x4> boneMatrices, std::vector<MeshVertex>& out);

}  // namespace joa::formats
