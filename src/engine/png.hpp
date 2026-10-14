#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace joa::engine {
// Writes 8-bit RGBA (top row first) as a PNG via zlib. Returns false on I/O failure.
bool write_png(const std::string& path, int width, int height, std::span<const std::uint8_t> rgba);
// Decodes a non-interlaced PNG (grey/RGB/palette/grey-alpha/RGBA, 1-16 bit; 16-bit keeps the high
// byte, palette tRNS honoured) to RGBA8, top row first. Returns false + err on anything else.
bool decode_png(std::span<const std::uint8_t> data, int& width, int& height, std::vector<std::uint8_t>& rgba, std::string& err);
}
