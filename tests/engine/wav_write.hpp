#pragma once
// 16-bit PCM WAVE of an interleaved float stereo mixer render (audio tests' listening artefacts).
#include "engine/audio_decode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

inline void write_wav(const std::string& path, const std::vector<float>& stereo) {
    std::ofstream f(path, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t bytes = std::uint32_t(stereo.size() * 2);
    const std::uint32_t rate = joa::engine::kOutputRate;
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(rate);
    u32(rate * 4); u16(4); u16(16); f.write("data", 4); u32(bytes);
    for (float s : stereo) u16(std::uint16_t(std::int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f))));
}
