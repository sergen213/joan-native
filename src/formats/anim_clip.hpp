#pragma once
// .AM3 skeleton hierarchy and .AM2 animation (d:\ww\en3d\animdef.cpp in joan.exe).
// Layout + evidence: docs/formats/MODELS_AND_ANIMATION.md.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "formats/model_mesh.hpp"  // Mat3x4

namespace joa::formats {

struct SkeletonNode {
  std::string name;               // char[50] in file, e.g. "Bip01 L Forearm"
  int32_t parent = -1;
  std::vector<uint32_t> children;
  bool present = false;           // false for ids never declared (gaps in the id space)
};
struct Skeleton { std::vector<SkeletonNode> nodes; };  // indexed by node id == AM1 bone == AM2 track

bool parse_skeleton(std::span<const uint8_t> data, Skeleton& out, std::string& err);

struct AnimClip { std::string name; uint32_t flags = 0; float start = 0, end = 0; };  // times in ms
struct AnimKey { float time = 0; Mat3x4 m{}; };      // model-space bone matrix (not parent-relative)
struct AnimEvent { float time = 0; int32_t id = 0; std::string name; uint8_t flag = 0; };
struct AnimExtraTrack {
  std::vector<std::array<float, 4>> keys4;           // 16-byte keys; meaning UNKNOWN (unused in data)
  std::vector<std::array<float, 2>> scalarKeys;      // {time ms, value} - fade/visibility (HYPOTHESIS)
};

struct Animation {
  float start = 0, end = 0;                          // whole timeline, ms
  std::vector<AnimClip> clips;
  std::vector<std::vector<AnimKey>> tracks;          // one per bone, keys sorted by time
  std::vector<AnimExtraTrack> extraTracks;           // trailer tag 1
  std::vector<AnimEvent> events;                     // trailer tag 1 (e.g. "JoanAAAA.txt" effect)
  size_t trailingBytes = 0;
};

bool parse_animation(std::span<const uint8_t> data, Animation& out, std::string& err);

// Bone matrices at time t (ms): per track, linear blend of the bracketing keys, clamped at ends.
// Tracks without keys yield identity.
// ponytail: element-wise matrix lerp (keys are 30 Hz so drift is small); slerp if joints shear.
std::vector<Mat3x4> sample_pose(const Animation& anim, float t);

}  // namespace joa::formats
