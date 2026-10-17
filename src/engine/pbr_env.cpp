#include "engine/pbr_env.hpp"

#include <glm/gtc/color_space.hpp>

#include <algorithm>
#include <array>
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
// brown under the frame's ambient + sun, fading into the fog colour at the horizon. The radiance is linear in
// the two per-frame colours: fog * F + ground * G + rest, F = the linear fog colour, G = the lit ground.
struct SkyTerms {
    float fog, ground;
    glm::vec3 rest;
};
SkyTerms sky_terms(glm::vec3 d) {
    if (d.y >= 0.0f) {
        const auto ss = [](float a, float b, float x) {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        const float a = ss(0.0f, 0.12f, d.y), b = ss(0.15f, 0.8f, d.y);
        return {(1.0f - b) * (1.0f - a), 0.0f, (1.0f - b) * a * lin8(150, 186, 220) + b * lin8(118, 160, 206)};
    }
    const float t = std::clamp(-d.y / 0.1f, 0.0f, 1.0f);
    return {1.0f - t, t, glm::vec3(0.0f)};
}
glm::vec3 ground_light(const Lighting& l) {
    const glm::vec3 sun = glm::normalize(l.sun_direction);
    const glm::vec3 light = lin(glm::min(l.ambient + l.sun_color * std::max(-sun.y, 0.0f) * 0.6f, glm::vec3(1.0f)));
    return lin8(110, 98, 70) * light;
}

struct Sample {
    glm::vec3 dir;
    glm::vec3 weights;  // SkyTerms fog, ground, 0
    glm::vec3 rest;
    float solid_angle;
};
struct Texel {
    glm::vec3 weights, rest;
};

// Cosine-power convolution of the source samples into a w x h equirect.
std::vector<Texel> convolve(const std::vector<Sample>& src, int w, int h, float power) {
    std::vector<Texel> out(std::size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const glm::vec3 n = direction((float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h));
            glm::vec3 sum(0.0f), rest(0.0f);
            float wsum = 0.0f;
            for (const Sample& s : src) {
                const float c = glm::dot(n, s.dir);
                if (c <= 0.0f) continue;
                const float wt = std::pow(c, power) * s.solid_angle;
                sum += s.weights * wt;
                rest += s.rest * wt;
                wsum += wt;
            }
            if (wsum > 0.0f) out[std::size_t(y) * w + x] = {sum / wsum, rest / wsum};
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

// The source samples: texel centres of the base level.
std::vector<Sample> samples() {
    std::vector<Sample> src;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const float v = (float(y) + 0.5f) / kH;
            const glm::vec3 d = direction((float(x) + 0.5f) / kW, v);
            const SkyTerms t = sky_terms(d);
            src.push_back({d, {t.fog, t.ground, 0.0f}, t.rest, std::sin(v * kPi) * (2.0f * kPi / kW) * (kPi / kH)});
        }
    return src;
}

constexpr int kLevels = 7;  // radiance: 64x32 .. 1x1

float level_power(int level) {  // prefilter cosine power of radiance mip `level` (>= 1)
    const float roughness = float(level) / 6.0f;
    const float a = roughness * roughness;
    return std::max(2.0f / (a * a) - 2.0f, 1.0f);
}

// Convolutions are linear in the radiance: each prefiltered texel = fog * F + ground * G + rest, the terms
// convolved once (a rebuild cost 14 ms of std::pow before; the fog colour follows the view direction, so turning
// the camera rebuilt the maps nearly every frame). [kLevels] = irradiance.
const std::vector<Texel>* basis() {
    static const auto b = [] {
        std::array<std::vector<Texel>, kLevels + 1> r;
        const std::vector<Sample> src = samples();
        for (int level = 1; level <= kLevels; ++level) {
            const bool irr = level == kLevels;
            r[std::size_t(level)] = convolve(src, irr ? 32 : std::max(kW >> level, 1), irr ? 16 : std::max(kH >> level, 1),
                                             irr ? 1.0f : level_power(level));
        }
        return r;
    }();
    return b.data();
}

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
    const glm::vec3 fog = lin(l.fog_color), ground = ground_light(l);
    const auto combine = [&](int level) {
        const std::vector<Texel>& t = basis()[level];
        std::vector<glm::vec3> out(t.size());
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = t[i].weights.x * fog + t[i].weights.y * ground + t[i].rest;
        return out;
    };
    std::vector<glm::vec3> base(std::size_t(kW) * kH);
    glm::vec3 sum(0.0f);
    float wsum = 0.0f;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const float v = (float(y) + 0.5f) / kH;
            const glm::vec3 d = direction((float(x) + 0.5f) / kW, v);
            const SkyTerms t = sky_terms(d);
            const glm::vec3 c = base[std::size_t(y) * kW + x] = t.fog * fog + t.ground * ground + t.rest;
            const float solid_angle = std::sin(v * kPi) * (2.0f * kPi / kW) * (kPi / kH);
            if (d.y > 0.0f) sum += c * d.y * solid_angle, wsum += d.y * solid_angle;
        }
    EnvMaps m;
    m.radiance.width = kW;
    m.radiance.height = kH;
    m.radiance.format = PixelFormat::Rgba8Srgb;
    m.radiance.repeat = false;
    m.radiance.pixels = encode(base, true, 1.0f);
    for (int level = 1; level < kLevels; ++level) m.radiance.mip_pixels.push_back(encode(combine(level), true, 1.0f));
    m.radiance_levels = kLevels;

    // Irradiance relative to an upward normal (the terrain keeps the Original ambient level), x0.5.
    const auto irr = combine(kLevels);
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
