#include "formats/font_fin.hpp"

#include "formats/vfs_bytes.hpp"

namespace joa::formats {

std::optional<FinFont> parse_fin(std::span<const std::uint8_t> bytes, std::string* err) {
    ByteReader r(bytes);
    FinFont f;
    f.face = r.fixed_string(32);
    f.first_char = r.u16();
    f.last_char = r.u16();
    f.metric_a = r.i16();
    f.line_height = r.i16();
    f.metric_c = r.i16();
    f.metric_d = r.i16();
    f.glyph_count = r.u32();
    if (!r.ok() || f.last_char < f.first_char || f.last_char > 255) {
        if (err) *err = "bad FIN header";
        return std::nullopt;
    }
    for (unsigned c = f.first_char; c <= f.last_char; ++c) {
        FinGlyph g;
        g.y_offset = r.i16();
        g.x1 = r.i16();
        g.y1 = r.i16();
        g.x2 = r.i16();
        g.y2 = r.i16();
        g.width = r.i16();
        g.height = r.i16();
        f.glyphs.push_back(g);
    }
    if (!r.at_end()) {
        if (err) *err = r.ok() ? "trailing bytes after FIN glyphs" : "truncated FIN glyph table";
        return std::nullopt;
    }
    return f;
}

}  // namespace joa::formats
