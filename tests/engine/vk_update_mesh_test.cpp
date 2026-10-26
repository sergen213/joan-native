// Vulkan update_mesh streaming (persistent per-mesh host-visible regions): one mesh rewritten every frame with frames in
// flight, through a static -> dynamic switch, vertex-count growth/shrink, index content and count changes and several
// updates in one frame. Each read-back frame must show exactly the last update's quad (no stale region or indices),
// with zero validation errors; then the same churn under the remaster TAA path (previous-pose vertex binding). Also
// update_texture rewriting one texture every frame.
// Exit 77 when no Vulkan 1.3 device exists.
#include "check.hpp"
#include "vk_test_util.hpp"

#include <SDL3/SDL_init.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace vktest;

namespace {
constexpr int kSize = 64, kCells = 4, kCell = kSize / kCells;

// Unit quad filling grid cell `cell` (row-major from the top-left), `extra` unused vertices appended, index variant
// 0: two triangles, 1: the same triangles in another order, 2: plus a degenerate triangle.
MeshData quad(int cell, int extra, int variant) {
    const float x0 = -1.0f + 2.0f * float(cell % kCells) / kCells, x1 = x0 + 2.0f / kCells;
    const float y1 = 1.0f - 2.0f * float(cell / kCells) / kCells, y0 = y1 - 2.0f / kCells;
    MeshData m;
    for (const glm::vec2 p : {glm::vec2(x0, y0), glm::vec2(x1, y0), glm::vec2(x1, y1), glm::vec2(x0, y1)}) {
        Vertex v;
        v.position = {p, 0.0f};
        m.vertices.push_back(v);
    }
    for (int i = 0; i < extra; ++i) m.vertices.push_back(m.vertices[i % 4]);
    m.indices = variant == 1 ? std::vector<std::uint32_t>{2, 3, 0, 0, 1, 2} : std::vector<std::uint32_t>{0, 1, 2, 2, 3, 0};
    if (variant == 2) m.indices.insert(m.indices.end(), {1, 1, 1});
    return m;
}

int red_pixels(const Image& img, int cell, bool& centre_red) {
    int n = 0;
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) n += img.rgba[i] > 128 && img.rgba[i + 1] < 64;
    const int x = (cell % kCells) * kCell + kCell / 2, y = (cell / kCells) * kCell + kCell / 2;
    const std::uint8_t* p = img.rgba.data() + (std::size_t(y) * img.width + x) * 4;
    centre_red = p[0] > 128 && p[1] < 64 && p[2] < 64;
    return n;
}

void run(Platform& platform, IRenderer& r, bool remaster) {
    const MeshHandle mesh = r.create_mesh(quad(15, 0, 0));
    Material red;
    red.unlit = true;
    red.fog = false;
    red.two_sided = true;
    red.tint = {1.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 40; ++i) {
        const int cell = (i * 5) % 16;
        // Growth (x1.5 regions), shrink, and one large jump that forces a regrow.
        const int extra = i == 20 ? 600 : (i % 5) * 9;
        if (i % 4 == 1) CHECK(r.update_mesh(mesh, quad((cell + 7) % 16, extra + 40, 2)));  // replaced in the same frame
        CHECK(r.update_mesh(mesh, quad(cell, extra, i % 3)));
        DrawItem d{mesh, red};
        d.object = 1;
        FrameData f;
        f.view.projection = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
        f.view.viewport = {0, 0, kSize, kSize};
        f.lighting.fog_end = 0.0f;
        f.draws = std::span<const DrawItem>(&d, 1);
        r.begin_frame(platform.pixel_size());
        r.submit(f);
        if (i % 3 == 2) {  // other frames stay in flight
            bool centre = false;
            const int n = red_pixels(r.read_back(), cell, centre);
            if (!centre || (!remaster && n != kCell * kCell))
                std::fprintf(stderr, "frame %d (%s): cell %d centre %s, %d red pixels\n", i, remaster ? "remaster" : "original", cell,
                             centre ? "red" : "NOT red", n);
            CHECK(centre);
            if (!remaster) CHECK(n == kCell * kCell);  // exactly the current quad: no stale region or index range
        }
        r.end_frame();
    }
    r.destroy_mesh(mesh);
}
// update_texture (movie frames): one texture rewritten every frame, alternating red / blue, frames in flight; the read-back
// frames show the latest contents. A size change is refused (the caller recreates).
void run_texture(Platform& platform, IRenderer& r) {
    const auto solid = [](int size, glm::u8vec4 c) { return make_checker_texture(size, 1, c, c); };
    const TextureHandle tex = r.create_texture(solid(16, {255, 0, 0, 255}));
    CHECK(!r.update_texture(tex, solid(32, {0, 0, 255, 255})));
    for (int i = 0; i < 12; ++i) {
        const bool blue = i % 2;
        TextureData t = solid(16, blue ? glm::u8vec4(0, 0, 255, 255) : glm::u8vec4(255, 0, 0, 255));
        t.mipmaps = true;
        CHECK(r.update_texture(tex, t));
        UiQuad q;
        q.size = {float(kSize), float(kSize)};
        q.texture = tex;
        FrameData f;
        f.view.viewport = {0, 0, kSize, kSize};
        f.ui = std::span<const UiQuad>(&q, 1);
        r.begin_frame(platform.pixel_size());
        r.submit(f);
        if (i % 3 == 2) {
            const Image img = r.read_back();
            const std::uint8_t* p = img.rgba.data() + (std::size_t(kSize / 2) * img.width + kSize / 2) * 4;
            CHECK(blue ? p[2] > 200 && p[0] < 50 : p[0] > 200 && p[2] < 50);
        }
        r.end_frame();
    }
    r.destroy_texture(tex);
}
}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();

    Options o;
    o.renderer = RendererKind::Vulkan;
    o.width = kSize;
    o.height = kSize;
    o.vk_validation = true;
    try {
        Platform platform(o, true);
        std::unique_ptr<IRenderer> r = create_vk_renderer(platform.window(), o);
        run(platform, *r, false);
        run_texture(platform, *r);
        GraphicsConfig g;
        g.preset = GraphicsPreset::Ultra;
        g.apply("graphics.upscale", "none");  // FSR 3 needs hardware this test does not ask for; TAA with motion vectors stays
        g.apply("graphics.aa", "taa");
        r->set_graphics(g.resolve());
        run(platform, *r, true);
    } catch (const std::exception& e) {
        if (vulkan_missing(e)) {
            std::fprintf(stderr, "SKIP: %s\n", e.what());
            return 77;
        }
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "validation: %s, errors %u\n", vk_validation_active() ? "active" : "NOT LOADED", vk_validation_errors());
    if (std::getenv("JOA_REQUIRE_VALIDATION")) CHECK(vk_validation_active());
    CHECK(vk_validation_errors() == 0);
    return test_result();
}
