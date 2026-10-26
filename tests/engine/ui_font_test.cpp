// en2d font metrics/layout (engine/ui_font.hpp): synthetic grid-font scan + measure rules, and the real
// Goudy_bold_24 widths measured from the original main menu (work/reference/shots/s06_main_menu.png:
// "New Campaign" spans x 154..358 = 205 px, "Load a Campaign" 141..371 = 231, "Options" 205..307 = 103,
// all centred on x = 256 with floor(w/2)). The real-data part exits 77 without the original files.
#include "check.hpp"
#include "engine/ui_font.hpp"
#include "formats/dds.hpp"
#include "formats/vfs_fs.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace joa::engine;

int main() {
    // Synthetic 2-glyph grid font: cells 4x4, pad 1, columns 2, chars '0'..'1'.
    // '0' = opaque 2x3 block at cell (1,1); '1' = single texel at cell (3,0). Atlas 11x6.
    const int w = 11, h = 6;
    std::vector<std::uint8_t> px(w * h * 4, 0);
    auto set = [&](int x, int y) { px[(y * w + x) * 4 + 3] = 255; };
    for (int y = 2; y <= 4; ++y)
        for (int x = 2; x <= 3; ++x) set(x, y);  // cell 0 origin (1,1)
    set(6 + 3, 1);                              // cell 1 origin (6,1)
    const GridFontSpec spec{"synthetic", 4, 4, 2, '0', '1', 1, 1, 0};
    const UiFontMetrics f = make_grid_font(spec, px, w, h);
    const auto* g0 = f.glyph('0');
    const auto* g1 = f.glyph('1');
    CHECK(g0 && g1);
    if (g0 && g1) {
        CHECK(g0->x1 == 2 && g0->y1 == 2 && g0->x2 == 3 && g0->y2 == 4);
        CHECK(g0->width == 2 && g0->height == 3);
        CHECK(g1->x1 == 9 && g1->y1 == 1 && g1->width == 1 && g1->height == 1);
        // y_offset is relative to the topmost glyph top over the font (row 0 for '1').
        CHECK(g0->y_offset == 1 && g1->y_offset == 0);
    }
    CHECK(f.line_height() == 3);        // tallest glyph
    CHECK(f.fin.metric_c == 4);         // rows 0..3 covered
    CHECK(f.space_width() == 1);        // width of '1'
    // advance = width + spacing(1); trailing spacing dropped; space/missing = space width.
    CHECK(f.measure("0") == 2);
    CHECK(f.measure("00") == 5);
    CHECK(f.measure("0 0") == 7);
    CHECK(f.measure("0x") == 4);        // 'x' missing -> space width
    CHECK(f.measure("0\t") == 2 + 1 + 8);
    CHECK(f.measure("00\n0") == 5);     // widest line
    std::vector<GlyphQuad> quads;
    CHECK(layout_line(f, "0 1", quads) == 6);
    CHECK(quads.size() == 2 && quads[1].pos.x == 5 && quads[0].pos.y == 1);

    // Word wrap keeps words whole.
    const auto lines = wrap_text(f, "00 00 00", 13);  // "00 00" measures 13
    CHECK(lines.size() == 2 && lines[0] == "00 00" && lines[1] == "00");
    // Incremental wrap == re-measuring every candidate line (the reference below), any spacing / text / width.
    const auto reference = [](const UiFontMetrics& font, std::string_view text, int max_width) {
        std::vector<std::string> out;
        for (std::size_t start = 0; start <= text.size();) {
            const std::size_t nl = std::min(text.find('\n', start), text.size());
            const std::string_view para = text.substr(start, nl - start);
            std::string line;
            for (std::size_t p = 0; p <= para.size();) {
                const std::size_t sp = std::min(para.find(' ', p), para.size());
                const std::string word(para.substr(p, sp - p));
                std::string cand = line.empty() ? word : line + " " + word;
                if (!line.empty() && font.measure(cand) > max_width) out.push_back(line), line = word;
                else line = cand;
                p = sp + 1;
            }
            out.push_back(line);
            start = nl + 1;
        }
        return out;
    };
    std::uint32_t seed = 1;
    const auto rnd = [&](std::uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % n;
    };
    for (int spacing : {0, 1, 3}) {
        UiFontMetrics fs = f;
        fs.spacing = spacing;
        for (int i = 0; i < 2000; ++i) {
            std::string s;
            for (std::uint32_t n = rnd(40), k = 0; k < n; ++k) s += "0011x  \t\n"[rnd(9)];
            const int width = int(rnd(30));
            CHECK(wrap_text(fs, s, width) == reference(fs, s, width));
        }
    }

    // Real data.
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("FONT/Goudy_bold_24.fin")) {
        std::fprintf(stderr, "SKIP real-font part: no original data\n");
        return g_failures ? 1 : 77;
    }
    joa::formats::Vfs vfs(game);
    auto fin = joa::formats::parse_fin(*vfs.read("FONT/Goudy_bold_24.fin"));
    CHECK(fin.has_value());
    if (fin) {
        const UiFontMetrics goudy = make_fin_font(*fin, 512, 512);
        CHECK(goudy.space_width() == 9);
        CHECK(goudy.measure("New Campaign") == 205);
        CHECK(goudy.measure("Load a Campaign") == 231);
        CHECK(goudy.measure("Options") == 103);
    }
    // Grid font from its atlas: "24pt large text" (41x41 cells, 24 per row, '!'..'z').
    joa::formats::DdsTexture t;
    std::string err;
    const auto bytes = vfs.read("int8x6/24pt large text.dds");
    CHECK(bytes && joa::formats::parse_dds(*bytes, t, err, 1));
    if (bytes && !t.images.empty()) {
        const GridFontSpec* s = nullptr;
        for (const GridFontSpec& g : grid_font_specs())
            if (std::strcmp(g.name, "24pt large text") == 0) s = &g;
        CHECK(s != nullptr);
        if (s) {
            const UiFontMetrics big = make_grid_font(*s, t.images[0].rgba, int(t.width), int(t.height));
            CHECK(big.fin.glyphs.size() == std::size_t(0x7a - 0x21 + 1));
            const auto* a = big.glyph('A');
            CHECK(a && a->width > 10 && a->width <= 41 && a->height > 10 && a->height <= 41);
            // Glyph rect lies inside its 42-px cell (pad 1 + 41): 'A' is char 0x41 -> index 32 -> row 1, col 8.
            if (a) CHECK(a->x1 >= 1 + 8 * 42 && a->x2 < 1 + 9 * 42 && a->y1 >= 1 + 42 && a->y2 < 1 + 2 * 42);
            std::fprintf(stderr, "24pt large text: line %d space %d 'A' %dx%d\n", big.line_height(), big.space_width(),
                         a ? a->width : 0, a ? a->height : 0);
        }
    }
    return test_result();
}
