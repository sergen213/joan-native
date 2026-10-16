#include "engine/sky_dome.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace joa::engine {
namespace {

constexpr int kRings = 12, kSegments = 32;
constexpr float kLowest = -0.2f;  // ring elevations from below the horizon (hides the far-plane edge) to the zenith

// Unit-sphere direction of ring r, segment s.
glm::vec3 dir(int r, int s) {
    const float e = glm::mix(kLowest, glm::half_pi<float>(), float(r) / kRings), a = glm::two_pi<float>() * float(s) / kSegments;
    return {std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)};
}

glm::vec3 srgb(float r, float g, float b) { return glm::pow(glm::vec3(r, g, b) / 255.0f, glm::vec3(2.2f)); }

MeshData sphere(float radius, glm::vec3 offset, auto&& colour) {
    MeshData m;
    for (int r = 0; r <= kRings; ++r)
        for (int s = 0; s <= kSegments; ++s) {
            const glm::vec3 d = dir(r, s);
            Vertex v;
            v.position = d * radius - offset;
            // Clouds: planar projection onto a ceiling, one texture repeat across the dome at 45 degrees.
            const glm::vec2 p = glm::vec2(d.x, d.z) / std::max(d.y, 0.08f);
            v.uv = p * 0.5f;
            v.color = colour(d);
            m.vertices.push_back(v);
        }
    for (int r = 0; r < kRings; ++r)
        for (int s = 0; s < kSegments; ++s) {
            const auto a = std::uint32_t(r * (kSegments + 1) + s), b = a + 1, c = a + kSegments + 1, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

// Cloud draws sort among the blended draws by model translation: lifting it to the dome keeps them
// behind nearer translucent geometry (tree leaves, water).
const glm::vec3 kCloudLift{0.0f, SkyDome::kRadius * 0.9f, 0.0f};

}  // namespace

SkyDome::SkyDome(AssetCache& assets, glm::vec3 fog, const std::vector<std::string>& clouds) : renderer_(assets.renderer()) {
    // Horizon = fog colour; a bright band just above it, blue towards the zenith (capture samples
    // (144,183,219) low and (125,167,210) high).
    const glm::vec3 band = srgb(150, 186, 220), zenith = srgb(118, 160, 206);
    dome_ = renderer_.create_mesh(sphere(kRadius, glm::vec3(0.0f), [&](glm::vec3 d) {
        const glm::vec3 c = glm::mix(glm::mix(fog, band, glm::smoothstep(0.0f, 0.12f, d.y)), zenith, glm::smoothstep(0.15f, 0.8f, d.y));
        return glm::vec4(c, 1.0f);
    }));
    float speed = 0.004f;  // radians per second, layer 2 drifts slower
    for (const std::string& name : clouds) {
        const TextureInfo* t = assets.texture(name);
        if (!t) continue;
        // Fades out towards the horizon so the planar projection never streaks.
        const MeshHandle m = renderer_.create_mesh(sphere(kRadius * 0.97f, kCloudLift, [](glm::vec3 d) {
            return glm::vec4(1.0f, 1.0f, 1.0f, glm::smoothstep(0.05f, 0.35f, d.y));
        }));
        layers_.push_back({m, t->handle, glm::vec2(speed, 0.0f)});
        speed *= 0.6f;
    }
}

SkyDome::~SkyDome() {
    renderer_.destroy_mesh(dome_);
    for (const Layer& l : layers_) renderer_.destroy_mesh(l.mesh);
}

void SkyDome::build(glm::vec3 camera, float time_s, glm::vec3 tint, std::vector<DrawItem>& draws) const {
    Material m;
    m.unlit = true;
    m.fog = false;
    m.receive_shadow = false;  // drawn before Scene_render enables the stencil (0x5ebac0)
    m.two_sided = true;
    m.tint = glm::vec4(tint, 1.0f);
    draws.push_back({dome_, m, glm::translate(glm::mat4(1.0f), camera)});
    m.blend = BlendMode::AlphaBlend;
    m.tint = glm::vec4(glm::vec3(1.25f, 1.25f, 1.3f) * tint, 0.9f);  // the cloud texels are grey; lifted towards white wisps
    for (const Layer& l : layers_) {
        m.albedo = l.texture;
        // Vertices sit at (dome - lift): translate(camera + lift) * spin about +Y puts them back on the
        // dome (the lift is vertical, so the spin leaves it alone) with the sort key at the lift.
        draws.push_back({l.mesh, m, glm::rotate(glm::translate(glm::mat4(1.0f), camera + kCloudLift), time_s * l.drift.x, glm::vec3(0, 1, 0))});
    }
}

}  // namespace joa::engine
