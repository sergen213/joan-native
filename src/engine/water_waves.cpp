#include "engine/water_waves.hpp"

#include <algorithm>
#include <cmath>

namespace joa::engine {
namespace {

// Velocity damping (1/s): amplitude falls as exp(-1.25 t), so with the 2D spreading a splash is gone in ~2-3 s and a
// small pond does not fill up with reverberating reflections (a busy, blotchy surface).
constexpr float kDamping = 2.5f;
// Viscous damping of the velocity Laplacian: a checkerboard loses 4% per substep (gone in ~0.4 s), an 8-cell (32-unit) wave 0.3%.
constexpr float kViscosity = 0.005f;
// Weak spring back to the rest level (omega ~0.6 rad/s, far below the ~8 rad/s of the waves): legs wading on zero the
// cells they step into, and in a closed pond nothing else would ever drain the offset that leaves.
constexpr float kRelax = 1e-4f;
// Wading: legs radius (units) and the wake, a bow mound / stern trough pair (two kicks of width kWakeSigma just outside
// the legs) deepened by kWake per unit walked each frame, the speed capped at 500 (a teleport is no sprint).
constexpr float kLegs = 10.0f, kWakeSigma = 6.0f, kWake = 0.3f;

}  // namespace

WaveField::WaveField(std::function<bool(glm::vec2)> wet)
    : wet_(std::move(wet)), h_(kN * kN), prev_(kN * kN), open_(kN * kN), mask_(kN * kN), scratch_(kN * kN) {}

int WaveField::cell_index(glm::vec2 p) const {
    if (!placed_) return -1;
    const glm::ivec2 c = glm::ivec2(glm::floor(p / kCell)) - corner_;
    return c.x < 0 || c.y < 0 || c.x >= kN || c.y >= kN ? -1 : c.y * kN + c.x;
}

void WaveField::shift(std::vector<float>& a, int dx, int dz, float fill) {
    for (int z = 0; z < kN; ++z) {
        float* dst = scratch_.data() + z * kN;
        const int oz = z + dz;
        if (oz < 0 || oz >= kN) {
            std::fill(dst, dst + kN, fill);
            continue;
        }
        for (int x = 0; x < kN; ++x) dst[x] = x + dx >= 0 && x + dx < kN ? a[std::size_t(oz * kN + x + dx)] : fill;
    }
    a.swap(scratch_);
}

void WaveField::follow(glm::vec2 centre) {
    const glm::ivec2 corner = glm::ivec2(glm::floor(centre / kCell)) - kN / 2;
    if (placed_ && corner == corner_) return;
    const glm::ivec2 d = corner - corner_;
    if (!placed_ || std::abs(d.x) >= kN || std::abs(d.y) >= kN) {
        std::fill(h_.begin(), h_.end(), 0.0f);
        std::fill(prev_.begin(), prev_.end(), 0.0f);
        std::fill(open_.begin(), open_.end(), -1.0f);
    } else {
        shift(h_, d.x, d.y, 0.0f);
        shift(prev_, d.x, d.y, 0.0f);
        shift(open_, d.x, d.y, -1.0f);
        // Rows 0 / kN-1 are never stepped (walls): clear what scrolled onto them.
        for (std::vector<float>* a : {&h_, &prev_}) {
            std::fill_n(a->begin(), kN, 0.0f);
            std::fill_n(a->end() - kN, kN, 0.0f);
        }
    }
    corner_ = corner, placed_ = true;
    for (int i = 0; i < kN * kN; ++i)
        if (open_[std::size_t(i)] < 0.0f)
            open_[std::size_t(i)] = wet_((glm::vec2(corner_ + glm::ivec2(i % kN, i / kN)) + 0.5f) * kCell) ? 1.0f : 0.0f;
}

void WaveField::kick(glm::vec2 p, float amplitude, float sigma) {
    if (!placed_) return;
    if (idle_ > kQuiet) {  // waking: the damped remainder is cleared
        std::fill(h_.begin(), h_.end(), 0.0f);
        std::fill(prev_.begin(), prev_.end(), 0.0f);
    }
    idle_ = 0.0f;
    const glm::vec2 local = p / kCell - glm::vec2(corner_);  // in cells
    const float reach = 4.0f * sigma / kCell, k = kCell * kCell / (2.0f * sigma * sigma);
    const int x0 = std::max(1, int(std::floor(local.x - reach))), x1 = std::min(kN - 2, int(std::ceil(local.x + reach)));
    const int z0 = std::max(1, int(std::floor(local.y - reach))), z1 = std::min(kN - 2, int(std::ceil(local.y + reach)));
    // Free: open water, not under a body of this frame (a kick at a wader's feet would otherwise lose its crater to the
    // leg and leave the rim: the pond level creeps up).
    const auto free = [&](int x, int z) {
        if (open_[std::size_t(z * kN + x)] == 0.0f) return false;
        for (const glm::vec3& b : bodies_)
            if (glm::distance(glm::vec2(x, z) + 0.5f, glm::vec2(b) / kCell - glm::vec2(corner_)) <= b.z / kCell) return false;
        return true;
    };
    const auto shape = [&](int x, int z) {
        const glm::vec2 o = glm::vec2(x, z) + 0.5f - local;
        const float q = glm::dot(o, o) * k;
        return q > 8.0f ? 0.0f : -amplitude * (1.0f - q) * std::exp(-q);
    };
    // The Mexican hat clipped by walls and bodies keeps zero volume: what it lost comes off its free cells evenly.
    float volume = 0.0f;
    int cells = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            if (const float v = shape(x, z); v != 0.0f && free(x, z)) volume += v, ++cells;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            if (const float v = shape(x, z); v != 0.0f && free(x, z)) {
                // Pure displacement (h and prev alike): the crater and its rim fall back and run out as rings.
                const std::size_t i = std::size_t(z * kN + x);
                h_[i] += v - volume / float(cells), prev_[i] += v - volume / float(cells);
            }
}

void WaveField::wade(glm::vec2 p, glm::vec2 velocity, float dt) {
    bodies_.push_back({p, kLegs});
    const float speed = glm::length(velocity);
    if (speed < 30.0f) return;
    const glm::vec2 ahead = velocity / speed * (kLegs + kWakeSigma);
    const float push = kWake * std::min(speed, 500.0f) * dt;
    kick(p + ahead, -push, kWakeSigma);
    kick(p - ahead, push, kWakeSigma);
}

void WaveField::step(float dt) {
    idle_ += dt;
    if (idle_ > kQuiet || !placed_) {
        bodies_.clear();
        return;
    }
    owed_ = std::min(owed_ + dt, 0.25f);  // a hitch does not run seconds of substeps
    if (owed_ >= kStep) {
        mask_ = open_;
        for (float* row : {mask_.data(), mask_.data() + (kN - 1) * kN}) std::fill_n(row, kN, 0.0f);
        for (int z = 0; z < kN; ++z) mask_[std::size_t(z * kN)] = mask_[std::size_t(z * kN + kN - 1)] = 0.0f;
        for (const glm::vec3& b : bodies_) {
            const glm::vec2 local = glm::vec2(b) / kCell - glm::vec2(corner_);
            const float r = b.z / kCell;
            for (int z = std::max(0, int(local.y - r)); z <= std::min(kN - 1, int(local.y + r)); ++z)
                for (int x = std::max(0, int(local.x - r)); x <= std::min(kN - 1, int(local.x + r)); ++x)
                    if (glm::distance(glm::vec2(x, z) + 0.5f, local) <= r) mask_[std::size_t(z * kN + x)] = 0.0f;
        }
    }
    bodies_.clear();
    // Verlet: h' = h + v * damping + (c dt / cell)^2 * lap(h) + viscosity * lap(v) - relax * h, v = h - h_prev.
    // Neighbours behind a wall count as the cell itself (zero gradient into the wall), so waves reflect there without
    // a phase flip. The viscous term eats the cell-sized checkerboard that moving legs stamp into the field (~7x
    // faster than on the 4-8 cell waves).
    constexpr float a = (kSpeed * kStep / kCell) * (kSpeed * kStep / kCell), damp = 1.0f - kDamping * kStep;
    for (; owed_ >= kStep; owed_ -= kStep) {
        const float* h = h_.data();
        const float* p = prev_.data();
        const float* m = mask_.data();
        float* out = scratch_.data();
        std::fill_n(out, kN, 0.0f);
        std::fill_n(out + kN * (kN - 1), kN, 0.0f);
        for (int i = kN; i < kN * (kN - 1); ++i) {
            const float c = h[i], v = c - p[i];
            const float lap = m[i - 1] * (h[i - 1] - c) + m[i + 1] * (h[i + 1] - c) + m[i - kN] * (h[i - kN] - c) +
                              m[i + kN] * (h[i + kN] - c);
            const float lapv = m[i - 1] * (h[i - 1] - p[i - 1] - v) + m[i + 1] * (h[i + 1] - p[i + 1] - v) +
                               m[i - kN] * (h[i - kN] - p[i - kN] - v) + m[i + kN] * (h[i + kN] - p[i + kN] - v);
            out[i] = m[i] * (c + v * damp + a * lap + kViscosity * lapv - kRelax * c);
        }
        prev_.swap(h_);    // prev <- h
        h_.swap(scratch_);  // h <- out
    }
}

float WaveField::height(glm::vec2 p) const {
    const int i = cell_index(p);
    return i < 0 ? 0.0f : h_[std::size_t(i)];
}

bool WaveField::wall(glm::vec2 p) const {
    const int i = cell_index(p);
    return i < 0 || i < kN || i >= kN * (kN - 1) || i % kN == 0 || i % kN == kN - 1 || open_[std::size_t(i)] == 0.0f;
}

bool WaveField::covers(glm::vec2 p) const { return cell_index(p) >= 0; }

WaterWaves WaveField::data() const {
    if (idle_ > kQuiet || !placed_) return {};
    return {h_, glm::vec2(corner_) * kCell, kCell};
}

}  // namespace joa::engine
