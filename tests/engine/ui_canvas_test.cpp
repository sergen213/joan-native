// UI canvas transform and hit testing (engine/ui_core.hpp) without a GPU: menus on the centred 4:3
// canvas, HUD frames anchored to the real screen edges, high-DPI scale, mouse mapped back through the
// same transform, and button release semantics.
#include "check.hpp"
#include "engine/ui_core.hpp"

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

bool near(glm::vec2 a, glm::vec2 b) { return glm::all(glm::lessThan(glm::abs(a - b), glm::vec2(1e-3f))); }
}  // namespace

int main() {
    NullRenderer r;
    joa::formats::Vfs vfs(".");
    Ui ui(vfs, r);
    std::vector<UiQuad> out;
    const float s = 1080.0f / 768.0f;

    // 21:9 2520x1080: 4:3 canvas 1440x1080 centred at x = 540.
    ui.begin({0, 0, 2520, 1080}, out, {}, 0.0f);
    CHECK(near(ui.to_pixels({0, 0}), {540, 0}));
    CHECK(near(ui.to_pixels({1024, 768}), {1980, 1080}));
    ui.frame(Anchor::BottomRight);
    CHECK(near(ui.to_pixels({1024, 768}), {2520, 1080}));
    CHECK(near(ui.to_pixels({900, 700}), {2520 - 124 * s, 1080 - 68 * s}));
    ui.frame(Anchor::TopLeft);
    CHECK(near(ui.to_pixels({10, 10}), {10 * s, 10 * s}));
    ui.frame(Anchor::TopRight);
    CHECK(near(ui.to_pixels({1000, 20}), {2520 - 24 * s, 20 * s}));
    CHECK(near(ui.to_design(ui.to_pixels({1000, 20})), {1000, 20}));

    // Quads are pixel-snapped.
    ui.frame(Anchor::Center);
    ui.fill({100, 50, 20, 10}, kUiWhite);
    CHECK(out.size() == 1 && out[0].position.x == std::round(540 + 100 * s) && out[0].size.y == std::round(60 * s) - std::round(50 * s));

    // Hit testing through the transform; buttons fire on release.
    UiInput in;
    in.mouse = {540 + 105 * s, 55 * s};
    in.mouse_released = true;
    ui.begin({0, 0, 2520, 1080}, out, in, 0.0f);
    CHECK(near(ui.mouse(), {105, 55}));
    CHECK(ui.hover({100, 50, 20, 10}));
    CHECK(ui.clicked({100, 50, 20, 10}));
    CHECK(!ui.hover({126, 50, 20, 10}));
    CHECK(!ui.pressed({100, 50, 20, 10}));

    // High-DPI 4:3 (2048x1536): scale 2, no bars; letterboxed portrait 1024x1024 keeps scale 4/3 width.
    ui.begin({0, 0, 2048, 1536}, out, {}, 0.0f);
    CHECK(near(ui.to_pixels({512, 384}), {1024, 768}));
    CHECK(ui.layout().scale == 2.0f);
    ui.begin({0, 0, 1024, 1024}, out, {}, 0.0f);
    CHECK(near(ui.to_pixels({0, 0}), {0, 128}));
    ui.frame(Anchor::Bottom);
    CHECK(near(ui.to_pixels({512, 768}), {512, 1024}));
    return test_result();
}
