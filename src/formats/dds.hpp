#pragma once
// DirectDraw Surface reader: DXT1/DXT2/DXT3/DXT4/DXT5 and uncompressed bit-mask formats
// (A8R8G8B8, A4R4G4B4, R5G6B5, X8R8G8B8, L8, A8, ...), mip chains and cubemaps, decoded to RGBA8.
// No DX10 header (2004 data never uses it).
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace joa::formats {

struct DdsImage { uint32_t width = 0, height = 0; std::vector<uint8_t> rgba; };  // tightly packed

struct DdsTexture {
  uint32_t width = 0, height = 0, mipCount = 1;
  bool cubemap = false;            // faces in order +X,-X,+Y,-Y,+Z,-Z
  std::string format;              // "DXT1".."DXT5" or "RGBA<bits>"
  std::vector<DdsImage> images;    // face-major: images[face * mipCount + mip]
};

// maxMips limits how many levels are decoded (0 = all). Bounds-checked; returns false + err.
bool parse_dds(std::span<const uint8_t> data, DdsTexture& out, std::string& err, uint32_t maxMips = 0);

}  // namespace joa::formats
