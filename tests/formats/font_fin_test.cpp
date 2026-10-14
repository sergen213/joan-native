// All font/*.fin parse; glyph rects lie inside the matching int8x6/<name>.dds atlas.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/font_fin.hpp"
#include "formats/vfs_bytes.hpp"
#include "formats/vfs_fs.hpp"

using namespace joa::formats;
namespace fs = std::filesystem;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "joan.exe")) return 77;
    Vfs vfs(dir);

    const auto fins = vfs.find_by_extension(".fin");
    CHECK(fins.size() == 19, "expected 19 fonts");
    for (const auto& path : fins) {
        const std::string where = path.string();
        std::string err;
        auto f = parse_fin(*read_file(path), &err);
        CHECK(f, where + ": " + err);
        CHECK(f->first_char == 32 && f->last_char == 255 && f->glyph_count == f->glyphs.size(), where);
        CHECK(!f->face.empty() && f->line_height > 0, where);

        auto dds = vfs.read("int8x6\\" + path.stem().string() + ".dds");
        CHECK(dds, where + ": atlas missing");
        ByteReader r(*dds, 12);
        const auto h = r.u32(), w = r.u32();
        CHECK(r.ok() && (*dds)[0] == 'D' && (*dds)[1] == 'D' && (*dds)[2] == 'S', where + ": atlas header");
        for (unsigned c = f->first_char; c <= f->last_char; ++c) {
            const FinGlyph& g = *f->glyph(static_cast<unsigned char>(c));
            // x2 may equal the atlas width (Goudy_bold_italic_12 'V' overhangs by one column).
            CHECK(g.x1 >= 0 && g.x1 <= g.x2 && std::uint32_t(g.x2) <= w, where + " glyph " + std::to_string(c));
            CHECK(g.y1 >= 0 && g.y1 <= g.y2 && std::uint32_t(g.y2) < h, where + " glyph " + std::to_string(c));
            CHECK(g.width == g.x2 - g.x1 + 1 && g.height == g.y2 - g.y1 + 1, where + " glyph " + std::to_string(c));
        }
    }
    std::printf("fin ok: %zu fonts\n", fins.size());
    return 0;
}
