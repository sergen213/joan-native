#include "engine/ui_font.hpp"

#include <algorithm>
#include <array>

namespace joa::engine {

const formats::FinGlyph* UiFontMetrics::glyph(unsigned char c) const {
    const formats::FinGlyph* g = fin.glyph(c);
    return g && g->width >= 1 ? g : nullptr;
}

int UiFontMetrics::measure(std::string_view text) const {
    int line = 0, widest = 0;
    for (unsigned char c : text) {
        if (c == '\n') {
            widest = std::max(widest, line - spacing);
            line = 0;
            continue;
        }
        const formats::FinGlyph* g = c == ' ' ? nullptr : glyph(c);
        line += g ? g->width : (c == '\t' ? 8 * space_width() : space_width());
        line += spacing;
    }
    return std::max(widest, text.empty() ? 0 : line - spacing);
}

std::span<const GridFontSpec> grid_font_specs() {
    // Sys_init_subsystems 0x4d6fe0 (decomp ~line 200984): name, cell w/h, columns, first, last, pad, spacing, adjust.
    static constexpr std::array<GridFontSpec, 30> kSpecs{{
        {"testfont", 16, 16, 37, 0x21, 0x7e, 1, 1, 0},
        {"font_verdana_10", 12, 12, 39, 0x21, 0xff, 1, 1, 0},
        {"font_verdana_10_bold", 12, 12, 39, 0x21, 0xff, 1, 1, 0},
        {"font_verdana_12", 14, 14, 34, 0x21, 0xff, 1, 1, 0},
        {"font_verdana_12_bold", 14, 14, 34, 0x21, 0xff, 1, 1, 0},
        {"verdana_10_regular", 12, 12, 39, 0x21, 0x7e, 1, 1, 0},
        {"verdana_12_regular_black_outline", 14, 14, 34, 0x21, 0x7e, 1, 1, 0},
        {"font_perrygothic_10_gold", 20, 20, 37, 0x21, 0xff, 1, 0, 0},
        {"font_verdana_10_gold", 20, 20, 37, 0x21, 0xff, 1, 1, 0},
        {"24pt large text", 41, 41, 24, 0x21, 0x7a, 1, 1, 0},
        {"18pt attribute numbers out", 23, 23, 25, 0x22, 0x39, 1, 1, 0},
        {"18pt attribute numbers over", 23, 23, 25, 0x22, 0x39, 1, 1, 0},
        {"18pt dark numbers", 23, 23, 25, 0x22, 0x39, 1, 1, 0},
        {"18pt light numbers", 23, 23, 25, 0x22, 0x39, 1, 1, 0},
        {"24pt large numbers gold", 31, 31, 25, 0x21, 0x39, 1, 1, 0},
        {"24pt large numbers light", 31, 31, 25, 0x21, 0x39, 1, 1, 0},
        {"Goudy12(18) inner emboss", 17, 17, 14, 0x21, 0xff, 1, 0, 0},
        {"Goudy12(18) black", 17, 17, 14, 0x21, 0xff, 1, 0, 0},
        {"Goudy12(18) yellow", 17, 17, 14, 0x21, 0xff, 1, 0, 0},
        {"verdana_12_gold_black_outline", 14, 14, 34, 0x21, 0x7e, 1, 1, 0},
        {"Celtic 10pt English", 17, 17, 28, 0x21, 0x7a, 1, 0, 0},
        {"Celtic 10pt French", 17, 17, 28, 0x21, 0x7a, 1, 0, 0},
        {"Celtic 18pt Black", 31, 31, 32, 0x21, 0x7a, 1, 0, 0},
        {"Verdana 12 English", 14, 14, 34, 0x21, 0xff, 1, 0, 0},
        {"Verdana 12 french", 14, 14, 34, 0x21, 0xff, 1, 0, 0},
        {"Goudy18(27) white", 26, 26, 18, 0x21, 0xff, 1, 0, 0},
        {"verdana12_reg_outline", 16, 16, 60, 0x21, 0xff, 1, 1, 0},
        {"Goudy14(22) white", 26, 26, 18, 0x21, 0xff, 1, 0, 0},
        {"Goudy12 Bold Black", 17, 17, 14, 0x21, 0xff, 1, 0, 0},
        {"Goudy12(18) target unit", 17, 17, 14, 0x21, 0xff, 1, 0, 0},
    }};
    return kSpecs;
}

namespace {
void apply_width_adjust(UiFontMetrics& f) {
    if (f.width_adjust == 0) return;
    for (formats::FinGlyph& g : f.fin.glyphs) g.width = std::int16_t(g.width - f.width_adjust);
}
}  // namespace

UiFontMetrics make_fin_font(formats::FinFont fin, int atlas_w, int atlas_h, int spacing) {
    UiFontMetrics f;
    f.fin = std::move(fin);
    f.spacing = spacing;
    f.atlas_w = atlas_w;
    f.atlas_h = atlas_h;
    return f;
}

UiFontMetrics make_grid_font(const GridFontSpec& s, std::span<const std::uint8_t> rgba, int w, int h) {
    UiFontMetrics f;
    f.atlas_w = w;
    f.atlas_h = h;
    f.spacing = s.spacing;
    f.width_adjust = s.width_adjust;
    f.fin.first_char = std::uint16_t(s.first);
    f.fin.last_char = std::uint16_t(s.last);
    f.fin.glyph_count = std::uint32_t(s.last - s.first + 1);
    auto opaque = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && rgba.size() >= std::size_t(w) * h * 4 && rgba[(std::size_t(y) * w + x) * 4 + 3] != 0;
    };
    int cx = s.pad, cy = s.pad, col = 0;
    int min_top = s.cell_h - 1, max_bottom = 0;
    for (int c = s.first; c <= s.last; ++c) {
        // 0x628490: first/last opaque row, then first/last opaque column inside those rows; an empty
        // cell collapses to the 1x1 bottom-right texel (same fall-through as the original loops).
        int x0 = 0, y0 = 0, x1 = s.cell_w - 1, y1 = s.cell_h - 1;
        auto row_has = [&](int y) { for (int x = x0; x <= x1; ++x) if (opaque(cx + x, cy + y)) return true; return false; };
        auto col_has = [&](int x) { for (int y = y0; y <= y1; ++y) if (opaque(cx + x, cy + y)) return true; return false; };
        int y = y0;
        while (y <= y1 && !row_has(y)) ++y;
        y0 = y > y1 ? y1 : y;
        y = y1;
        while (y >= y0 && !row_has(y)) --y;
        y1 = y < y0 ? y0 : y;
        int x = x0;
        while (x <= x1 && !col_has(x)) ++x;
        x0 = x > x1 ? x1 : x;
        x = x1;
        while (x >= x0 && !col_has(x)) --x;
        x1 = x < x0 ? x0 : x;

        formats::FinGlyph g;
        g.y_offset = std::int16_t(y0);
        g.x1 = std::int16_t(cx + x0);
        g.y1 = std::int16_t(cy + y0);
        g.x2 = std::int16_t(cx + x1);
        g.y2 = std::int16_t(cy + y1);
        g.width = std::int16_t(x1 - x0 + 1);
        g.height = std::int16_t(y1 - y0 + 1);
        f.fin.metric_a = std::max<std::int16_t>(f.fin.metric_a, g.width);
        f.fin.line_height = std::max<std::int16_t>(f.fin.line_height, g.height);
        min_top = std::min(min_top, y0);
        max_bottom = std::max(max_bottom, y1);
        f.fin.glyphs.push_back(g);

        cx += s.pad + s.cell_w;
        if (w < cx + s.cell_w || ++col == s.columns) {
            cx = s.pad;
            cy += s.pad + s.cell_h;
            col = 0;
        }
    }
    f.fin.metric_c = std::int16_t(max_bottom - min_top + 1);
    for (formats::FinGlyph& g : f.fin.glyphs) g.y_offset = std::int16_t(g.y_offset - min_top);
    // font+0x36 = width of '1' (taken before the width adjust, as in 0x623e90).
    if (const formats::FinGlyph* one = f.fin.glyph('1')) f.fin.metric_d = one->width;
    apply_width_adjust(f);
    return f;
}

int layout_line(const UiFontMetrics& font, std::string_view text, std::vector<GlyphQuad>& out) {
    int x = 0;
    for (unsigned char c : text) {
        const formats::FinGlyph* g = c == ' ' ? nullptr : font.glyph(c);
        if (!g) {
            x += c == '\t' ? 8 * font.space_width() : font.space_width();
        } else {
            out.push_back({{x, g->y_offset}, {g->x2 - g->x1 + 1, g->height}, {g->x1, g->y1, g->x2, g->y2}});
            x += g->width;
        }
        x += font.spacing;
    }
    return text.empty() ? 0 : x - font.spacing;
}

std::vector<std::string> wrap_text(const UiFontMetrics& font, std::string_view text, int max_width) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = std::min(text.find('\n', start), text.size());
        const std::string_view para = text.substr(start, nl - start);
        // measure() of a line without '\n' is additive (no kerning): sum of advances (each + spacing) minus one spacing,
        // so the candidate "line word" measures line_adv + space_adv + word_adv - spacing (adv = measure + spacing).
        const auto adv = [&](std::string_view s) { return s.empty() ? 0 : font.measure(s) + font.spacing; };
        const int space_adv = adv(" ");
        std::string line;
        int line_adv = 0;
        std::size_t p = 0;
        while (p <= para.size()) {
            const std::size_t sp = std::min(para.find(' ', p), para.size());
            const std::string_view word = para.substr(p, sp - p);
            const int word_adv = adv(word);
            if (line.empty()) {
                line = word, line_adv = word_adv;
            } else if (line_adv + space_adv + word_adv - font.spacing > max_width) {
                lines.push_back(std::move(line));
                line = word, line_adv = word_adv;
            } else {
                line += ' ', line += word, line_adv += space_adv + word_adv;
            }
            p = sp + 1;
        }
        lines.push_back(std::move(line));
        start = nl + 1;
    }
    return lines;
}

}  // namespace joa::engine
