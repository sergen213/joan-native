#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace joa::engine {
// Writes 8-bit RGBA (top row first) as a PNG via zlib. Returns false on I/O failure.
bool write_png(const std::string& path, int width, int height, std::span<const std::uint8_t> rgba);
}
