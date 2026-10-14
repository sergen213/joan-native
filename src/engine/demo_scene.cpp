#include "engine/demo_scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace joa::engine {
namespace {

// Quad centred at `c` facing `n`; u x v == n so the winding is counter-clockwise seen from outside.
void add_face(MeshData& m, glm::vec3 c, glm::vec3 n, glm::vec3 u, glm::vec3 v, glm::vec2 uv_scale) {
    const auto base = std::uint32_t(m.vertices.size());
    const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const glm::vec2 k : corners) {
        Vertex vx;
        vx.position = c + u * k.x + v * k.y;
        vx.normal = glm::normalize(n);
        vx.uv = glm::vec2((k.x + 1.0f) * 0.5f, (1.0f - k.y) * 0.5f) * uv_scale;
        m.vertices.push_back(vx);
    }
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

}  // namespace

MeshData make_box_mesh(glm::vec3 h) {
    MeshData m;
    const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3 n : normals) {
        const glm::vec3 u = n.y != 0.0f ? glm::vec3(1, 0, 0) : glm::cross(glm::vec3(0, 1, 0), n);
        const glm::vec3 v = glm::cross(n, u);
        add_face(m, n * h, n, u * h, v * h, glm::vec2(1.0f));
    }
    return m;
}

// Tiled so per-vertex fog (D3D-style, renderer shaders) does not fog the whole plane from its corners.
MeshData make_ground_mesh(float s, float uv_repeat) {
    MeshData m;
    const int n = std::max(1, int(s / 10.0f));
    const float t = s / float(n);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const glm::vec3 c{-s + t * float(2 * x + 1), 0.0f, -s + t * float(2 * z + 1)};
            add_face(m, c, {0, 1, 0}, {t, 0, 0}, {0, 0, -t}, glm::vec2(uv_repeat / float(n)));
        }
    return m;
}

TextureData make_checker_texture(int size, int cells, glm::u8vec4 a, glm::u8vec4 b) {
    TextureData t{size, size, PixelFormat::Rgba8Srgb, {}, true, true, {}};
    t.pixels.reserve(std::size_t(size) * size * 4);
    const int cell = std::max(1, size / std::max(1, cells));
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const glm::u8vec4 c = ((x / cell + y / cell) & 1) ? b : a;
            t.pixels.insert(t.pixels.end(), {c.r, c.g, c.b, c.a});
        }
    return t;
}

DemoScene::DemoScene(IRenderer& r) : renderer_(r) {
    ground_ = r.create_mesh(make_ground_mesh(200.0f, 100.0f));
    box_ = r.create_mesh(make_box_mesh(glm::vec3(1.0f)));
    checker_ = r.create_texture(make_checker_texture(256, 2, {116, 128, 84, 255}, {96, 110, 70, 255}));
    crate_ = r.create_texture(make_checker_texture(64, 8, {200, 170, 120, 255}, {150, 110, 70, 0}));
}

DemoScene::~DemoScene() {
    renderer_.destroy_mesh(ground_);
    renderer_.destroy_mesh(box_);
    renderer_.destroy_texture(checker_);
    renderer_.destroy_texture(crate_);
}

Lighting DemoScene::lighting() const {
    Lighting l;
    l.sun_direction = {-0.5f, -1.0f, -0.35f};
    // Gamma-space values (renderer.hpp Lighting); fog = sRGB of the linear clear colour.
    l.sun_color = {0.7f, 0.65f, 0.56f};
    l.ambient = {0.42f, 0.44f, 0.42f};
    l.fog_color = {0.678f, 0.767f, 0.879f};
    l.fog_start = 40.0f;
    l.fog_end = 180.0f;
    l.clear_color = {0.42f, 0.55f, 0.75f, 1.0f};
    return l;
}

void DemoScene::build(const World& w, const UiLayout& ui, std::vector<DrawItem>& draws, std::vector<UiQuad>& quads) const {
    draws.clear();
    quads.clear();
    draws.push_back({ground_, {checker_}, glm::mat4(1.0f)});

    // Deterministic scatter of boxes on a jittered grid.
    for (int gz = -4; gz <= 4; ++gz)
        for (int gx = -4; gx <= 4; ++gx) {
            const std::uint32_t h = hash32(std::uint32_t((gx + 64) * 131 + (gz + 64)));
            if ((h & 3) != 0 || (gx == 0 && gz == 0)) continue;
            const float size = 0.5f + float((h >> 8) & 0xff) / 255.0f * 1.5f;
            const glm::vec3 pos{gx * 9.0f + float((h >> 16) & 7) - 3.5f, size, gz * 9.0f + float((h >> 20) & 7) - 3.5f};
            glm::mat4 m = glm::translate(glm::mat4(1.0f), pos);
            m = glm::rotate(m, float((h >> 24) & 0xff) / 255.0f * 6.2831853f, glm::vec3(0, 1, 0));
            m = glm::scale(m, glm::vec3(size));
            Material mat{crate_};
            mat.blend = (h >> 4) % 5 == 0 ? BlendMode::AlphaTest : BlendMode::Opaque;
            mat.two_sided = mat.blend == BlendMode::AlphaTest;
            draws.push_back({box_, mat, m});
        }

    // Player stand-in: body + facing marker, and a translucent marker ring box overhead.
    glm::mat4 body = glm::translate(glm::mat4(1.0f), w.player_position + glm::vec3(0, 0.9f, 0));
    body = glm::rotate(body, w.player_yaw, glm::vec3(0, 1, 0));
    Material red;
    red.tint = {0.8f, 0.1f, 0.08f, 1.0f};
    draws.push_back({box_, red, glm::scale(body, {0.35f, 0.9f, 0.25f})});
    Material white;
    draws.push_back({box_, white, glm::scale(glm::translate(body, {0, 0.5f, -0.35f}), glm::vec3(0.12f))});
    Material glass;
    glass.tint = {0.2f, 0.5f, 1.0f, 0.35f};
    glass.blend = BlendMode::AlphaBlend;
    glass.unlit = true;
    draws.push_back({box_, glass, glm::scale(glm::translate(body, {0, 1.5f, 0}), {0.5f, 0.05f, 0.5f})});

    // UI anchor markers: white = 4:3 safe-area corners, orange = real screen corners.
    for (int a : {0, 2, 6, 8}) {
        const glm::vec4 s = ui_place(ui, Anchor(a), UiSpace::Safe, {8, 8}, {24, 24});
        quads.push_back({{s.x, s.y}, {s.z, s.w}, {0, 0}, {1, 1}, {1, 1, 1, 0.8f}, {}});
        const glm::vec4 e = ui_place(ui, Anchor(a), UiSpace::Screen, {8, 8}, {16, 16});
        quads.push_back({{e.x, e.y}, {e.z, e.w}, {0, 0}, {1, 1}, {1.0f, 0.45f, 0.05f, 0.9f}, {}});
    }
}

}  // namespace joa::engine
