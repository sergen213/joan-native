// font/*.fin : glyph metrics for a pre-rendered atlas texture int8x6/<same name>.dds.
// Loader in joan.exe: FUN_00624810 (en2d/font2d.cpp). See docs/formats/CONTAINERS_WORLD_SAVES.md.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace joa::formats {

struct FinGlyph {
    std::int16_t y_offset = 0;        // vertical offset of the glyph top inside the line
    std::int16_t x1 = 0, y1 = 0;      // inclusive atlas rect
    std::int16_t x2 = 0, y2 = 0;
    std::int16_t width = 0;           // advance; engine subtracts its per-font spacing argument from this
    std::int16_t height = 0;          // y2 - y1 + 1
};

struct FinFont {
    std::string face;                 // Windows face name the atlas was rendered from, e.g. "GoudyOlSt BT"
    std::uint16_t first_char = 0, last_char = 0;  // 32..255 (cp1252)
    std::int16_t metric_a = 0;        // font+0x2e; tracks point size (14 for _10, 74 for _48)
    std::int16_t line_height = 0;     // font+0x30, copied to +0x34 and scaled by FUN_00624eb0
    std::int16_t metric_c = 0;        // font+0x32
    std::int16_t metric_d = 0;        // font+0x36, scaled by FUN_00624e60
    std::uint32_t glyph_count = 0;    // read and ignored by the engine; equals last-first+1
    std::vector<FinGlyph> glyphs;     // glyphs[c - first_char]

    const FinGlyph* glyph(unsigned char c) const {
        return (c < first_char || c > last_char) ? nullptr : &glyphs[c - first_char];
    }
};

std::optional<FinFont> parse_fin(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

}  // namespace joa::formats
