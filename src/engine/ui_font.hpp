#pragma once
// en2d bitmap fonts (en2d/font2d.cpp, 0x623e90..0x624dc0). Pure layout code (no renderer) so it is unit-testable.
// Two sources, both end up as a FinFont glyph table over an int8x6/<name>.dds atlas:
//  - FONT\<name>.fin metrics (loader 0x624810, registered with spacing 1, width adjust 0);
//  - "grid" fonts whose metrics are derived from the atlas pixels at load time (0x623e90): fixed cells,
//    each glyph = tight bounding box of the non-transparent texels in its cell (scanner 0x628490).
// Layout rules (measure 0x624f00, draw 0x6255d0): advance = glyph.width + spacing; space and glyphs missing
// from the font (or with width < 1) advance by space_width (font+0x36), TAB by 8x that; glyph top =
// y + y_offset. Measured width drops the trailing spacing. See docs/reverse-engineering/UI_SYSTEM.md.
#include "formats/font_fin.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace joa::engine {

struct UiFontMetrics {
    formats::FinFont fin;   // glyph table; fin.line_height = font+0x30/0x34, fin.metric_d = space width (+0x36)
    int spacing = 1;        // font+0x38, added after every character
    int width_adjust = 0;   // font+0x3a, already subtracted from every glyph width
    int atlas_w = 0, atlas_h = 0;

    // Glyph used for drawing, or nullptr (outside first..last or width < 1, as 0x627e80).
    const formats::FinGlyph* glyph(unsigned char c) const;
    int space_width() const { return fin.metric_d; }
    int line_height() const { return fin.line_height; }
    // Width of one line in pixels (0x624f00 without wrapping): sum of advances minus the last spacing.
    int measure(std::string_view text) const;
};

// Registration parameters of a grid font (Sys_init_subsystems 0x4d6fe0 calls 0x623e90 with these).
struct GridFontSpec {
    const char* name;
    int cell_w, cell_h, columns, first, last, pad, spacing, width_adjust;
};
std::span<const GridFontSpec> grid_font_specs();

// From a .fin file (spacing 1, width adjust 0, as every 0x624810 call in the original).
UiFontMetrics make_fin_font(formats::FinFont fin, int atlas_w, int atlas_h, int spacing = 1);
// From atlas pixels (RGBA8, tightly packed); opaque = alpha != 0.
UiFontMetrics make_grid_font(const GridFontSpec& spec, std::span<const std::uint8_t> rgba, int atlas_w, int atlas_h);

struct GlyphQuad {
    glm::ivec2 pos;   // top-left, pixels relative to the text origin
    glm::ivec2 size;  // glyph width x height
    glm::ivec4 src;   // atlas x1, y1, x2, y2 inclusive
};
// One line laid out at origin (0,0): glyph quads in drawing order. Returns the measured width.
int layout_line(const UiFontMetrics& font, std::string_view text, std::vector<GlyphQuad>& out);

// Greedy word wrap on spaces / '\n' to max_width pixels. ponytail: txtbox2d's own wrapper is not RE'd;
// words longer than the width stay on their own line.
std::vector<std::string> wrap_text(const UiFontMetrics& font, std::string_view text, int max_width);

}  // namespace joa::engine
