// Ui cache lookups (ui_core.cpp transparent key views) without the original data: t_interf.txr "SECTION|n" keys stay
// case-sensitive with std::to_string digits, missing textures / fonts / sprites are cached case-insensitively.
#include "check.hpp"
#include "engine/ui_core.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace joa::engine;

namespace {
struct NullRenderer final : IRenderer {
    const char* name() const override { return "null"; }
    MeshHandle create_mesh(const MeshData&) override { return {1}; }
    void destroy_mesh(MeshHandle) override {}
    TextureHandle create_texture(const TextureData&) override { return {1}; }
    void destroy_texture(TextureHandle) override {}
    void begin_frame(glm::ivec2) override {}
    void submit(const FrameData&) override {}
    void end_frame() override {}
    Image read_back() override { return {}; }
};
}  // namespace

int main() {
    const char* out_dir = std::getenv("JOA_TEST_OUT");
    const std::filesystem::path root = std::filesystem::path(out_dir ? out_dir : ".") / "ui_lookup_data";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Resource");
    {
        std::ofstream f(root / "Resource" / "t_interf.txr", std::ios::binary);
        f << "1|MAINMENU|1|New Campaign\r\n2|MAINMENU|-12|negative\n3|mainmenu|1|lower\n4|MAINMENU|1|second\n"
             "5|A|B|1|ambiguous\n6|SECTION|2147483647|max\n";
    }
    joa::formats::Vfs vfs(root);
    NullRenderer r;
    Ui ui(vfs, r);
    CHECK(ui.text("MAINMENU", 1) == "New Campaign");  // first entry wins, '\r' stripped
    CHECK(ui.text("mainmenu", 1) == "lower");         // sections are case-sensitive
    CHECK(ui.text("MAINMENU", -12) == "negative");
    CHECK(ui.text("MAINMENU", 2).empty());
    CHECK(ui.text("MAINMENU", 12).empty());
    CHECK(ui.text("SECTION", 2147483647) == "max");
    CHECK(ui.text("A", 1).empty());  // "A|B|1|..." parses as section A, index "B"

    // Missing assets: one cache entry per lowercase name, any case finds it (no crash, same null result).
    CHECK(!ui.texture("NoSuch"));
    CHECK(ui.texture_size("NOSUCH") == glm::ivec2(0));
    CHECK(ui.texture_size("never_loaded") == glm::ivec2(0));
    CHECK(ui.font("No Font") == nullptr && ui.font("NO FONT") == nullptr);
    CHECK(ui.sprite("Set", "Name") == nullptr && ui.sprite("SET", "name") == nullptr);
    CHECK(!ui.has_sprite("set", "name"));
    return g_failures ? 1 : 0;
}
