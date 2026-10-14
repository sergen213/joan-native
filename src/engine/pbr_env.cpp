#include "engine/pbr_env.hpp"

#include <glm/gtc/color_space.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace joa::engine {
namespace {

constexpr int kW = 64, kH = 32;
constexpr float kPi = 3.14159265f;

glm::vec3 lin(glm::vec3 gamma) { return glm::convertSRGBToLinear(glm::clamp(gamma, 0.0f, 1.0f)); }
glm::vec3 lin8(int r, int g, int b) { return lin(glm::vec3(r, g, b) / 255.0f); }

glm::vec3 direction(float u, float v) {  // texel centre uv -> unit direction (row 0 = zenith)
    const float phi = (u - 0.5f) * 2.0f * kPi, theta = v * kPi;
    return {std::cos(phi) * std::sin(theta), std::cos(theta), std::sin(phi) * std::sin(theta)};
}

// Linear radiance seen along d. Above the horizon: sky_dome.cpp's gradient (horizon = fog colour, band
// 150,186,220, zenith 118,160,206 -- sampled from the reference captures); below: the terrain's typical
// brown under the frame's ambient + sun, fading into the fog colour at the horizon.
glm::vec3 sky(const Lighting& l, glm::vec3 d) {
    const glm::vec3 fog = lin(l.fog_color);
    if (d.y >= 0.0f) {
        const auto ss = [](float a, float b, float x) {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        return glm::mix(glm::mix(fog, lin8(150, 186, 220), ss(0.0f, 0.12f, d.y)), lin8(118, 160, 206), ss(0.15f, 0.8f, d.y));
    }
    const glm::vec3 sun = glm::normalize(l.sun_direction);
    const glm::vec3 light = lin(glm::min(l.ambient + l.sun_color * std::max(-sun.y, 0.0f) * 0.6f, glm::vec3(1.0f)));
    const glm::vec3 ground = lin8(110, 98, 70) * light;
    return glm::mix(fog, ground, std::clamp(-d.y / 0.1f, 0.0f, 1.0f));
}

struct Sample {
    glm::vec3 dir, radiance;
    float solid_angle;
};

// Cosine-power convolution of the source samples into a w x h equirect.
std::vector<glm::vec3> convolve(const std::vector<Sample>& src, int w, int h, float power) {
    std::vector<glm::vec3> out(std::size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const glm::vec3 n = direction((float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h));
            glm::vec3 sum(0.0f);
            float wsum = 0.0f;
            for (const Sample& s : src) {
                const float c = glm::dot(n, s.dir);
                if (c <= 0.0f) continue;
                const float wt = std::pow(c, power) * s.solid_angle;
                sum += s.radiance * wt;
                wsum += wt;
            }
            out[std::size_t(y) * w + x] = wsum > 0.0f ? sum / wsum : glm::vec3(0.0f);
        }
    return out;
}

std::vector<std::uint8_t> encode(const std::vector<glm::vec3>& c, bool srgb, float alpha) {
    std::vector<std::uint8_t> px(c.size() * 4);
    for (std::size_t i = 0; i < c.size(); ++i) {
        const glm::vec3 e = srgb ? glm::convertLinearToSRGB(glm::clamp(c[i], 0.0f, 1.0f)) : glm::clamp(c[i], 0.0f, 1.0f);
        for (int k = 0; k < 3; ++k) px[i * 4 + k] = std::uint8_t(std::lround(e[k] * 255.0f));
        px[i * 4 + 3] = std::uint8_t(std::lround(alpha * 255.0f));
    }
    return px;
}

std::uint64_t q(float v) { return std::uint64_t(std::clamp(int(std::lround(v * 255.0f)), -1024, 1024) + 1024); }

}  // namespace

std::uint64_t env_key(const Lighting& l) {
    const glm::vec3 s = glm::normalize(l.sun_direction);
    std::uint64_t k = 1469598103934665603ull;
    for (float v : {l.fog_color.r, l.fog_color.g, l.fog_color.b, l.ambient.r, l.ambient.g, l.ambient.b, l.sun_color.r, l.sun_color.g,
                    l.sun_color.b, s.x, s.y, s.z})
        k = (k ^ q(v)) * 1099511628211ull;
    return k;
}

EnvMaps build_env_maps(const Lighting& l) {
    std::vector<Sample> src;
    std::vector<glm::vec3> base(std::size_t(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const float v = (float(y) + 0.5f) / kH;
            const glm::vec3 d = direction((float(x) + 0.5f) / kW, v);
            base[std::size_t(y) * kW + x] = sky(l, d);
            src.push_back({d, base[std::size_t(y) * kW + x], std::sin(v * kPi) * (2.0f * kPi / kW) * (kPi / kH)});
        }
    EnvMaps m;
    m.radiance.width = kW;
    m.radiance.height = kH;
    m.radiance.format = PixelFormat::Rgba8Srgb;
    m.radiance.repeat = false;
    m.radiance.pixels = encode(base, true, 1.0f);
    int levels = 1;
    for (int w = kW >> 1, h = kH >> 1; w >= 1; w >>= 1, h = std::max(h >> 1, 1), ++levels) {
        const float roughness = float(levels) / 6.0f;  // 7 levels: 64x32 .. 1x1
        const float a = roughness * roughness;
        const float power = std::max(2.0f / (a * a) - 2.0f, 1.0f);
        m.radiance.mip_pixels.push_back(encode(convolve(src, w, h, power), true, 1.0f));
    }
    m.radiance_levels = levels;

    // Irradiance relative to an upward normal (the terrain keeps the Original ambient level), x0.5.
    const auto irr = convolve(src, 32, 16, 1.0f);
    glm::vec3 sum(0.0f);
    float wsum = 0.0f;
    for (const Sample& s : src)
        if (s.dir.y > 0.0f) sum += s.radiance * s.dir.y * s.solid_angle, wsum += s.dir.y * s.solid_angle;
    const glm::vec3 norm = sum / std::max(wsum, 1e-6f);
    m.sky_up = norm;
    std::vector<glm::vec3> rel(irr.size());
    for (std::size_t i = 0; i < irr.size(); ++i) rel[i] = 0.5f * irr[i] / glm::max(norm, glm::vec3(1e-4f));
    m.irradiance.width = 32;
    m.irradiance.height = 16;
    m.irradiance.format = PixelFormat::Rgba8Unorm;
    m.irradiance.repeat = false;
    m.irradiance.mipmaps = false;
    m.irradiance.pixels = encode(rel, false, 0.5f);
    return m;
}

}  // namespace joa::engine
