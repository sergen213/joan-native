// Interactive water wave field (water_waves.hpp): superposition / interference, shore reflection, legs as obstacles,
// damping, window scrolling, a walking wake's height and the cost of a substep.
#include "check.hpp"
#include "engine/water_waves.hpp"

#include <algorithm>
#include <chrono>
#include <functional>

using namespace joa::engine;

namespace {

constexpr float kDt = WaveField::kStep;

WaveField field(std::function<bool(glm::vec2)> wet = [](glm::vec2) { return true; }) {
    WaveField w(std::move(wet));
    w.follow({0.0f, 0.0f});
    return w;
}

void run(WaveField& w, float seconds) {
    for (int i = 0; i < int(seconds / kDt + 0.5f); ++i) w.step(kDt);
}

float energy(const WaveField& w, const std::function<bool(glm::vec2)>& in) {
    const WaterWaves d = w.data();
    float e = 0.0f;
    for (int i = 0; i < kWaterWaveGrid * kWaterWaveGrid; ++i)
        if (in(d.origin + (glm::vec2(i % kWaterWaveGrid, i / kWaterWaveGrid) + 0.5f) * d.cell)) e += d.height[std::size_t(i)] * d.height[std::size_t(i)];
    return e;
}

float peak(const WaveField& w) {
    float m = 0.0f;
    for (float h : w.data().height) m = std::max(m, std::abs(h));
    return m;
}

}  // namespace

int main() {
    // Two splashes 400 units apart: the field is linear, so at the midpoint both runs together are exactly the sum
    // of each alone; equal kicks double there (constructive), opposite ones cancel (destructive).
    {
        const glm::vec2 p1(-198.0f, 2.0f), p2(202.0f, 2.0f), mid(2.0f, 2.0f);  // mirror images about a cell centre
        WaveField a = field(), b = field(), ab = field(), anti = field();
        a.kick(p1, 6.0f, 24.0f), b.kick(p2, 6.0f, 24.0f);
        ab.kick(p1, 6.0f, 24.0f), ab.kick(p2, 6.0f, 24.0f);
        anti.kick(p1, 6.0f, 24.0f), anti.kick(p2, -6.0f, 24.0f);
        float worst = 0.0f, single = 0.0f, both = 0.0f, cancel = 0.0f;
        for (int i = 0; i < 240; ++i) {
            for (WaveField* w : {&a, &b, &ab, &anti}) w->step(kDt);
            worst = std::max(worst, std::abs(ab.height(mid) - a.height(mid) - b.height(mid)));
            single = std::max(single, std::abs(a.height(mid)));
            both = std::max(both, std::abs(ab.height(mid)));
            cancel = std::max(cancel, std::abs(anti.height(mid)));
        }
        std::fprintf(stderr, "superposition: single peak %.3f both %.3f (sum error %.2e) opposite %.2e\n", single, both, worst, cancel);
        CHECK(single > 0.02f && worst < 1e-4f && std::abs(both - 2.0f * single) < 1e-3f && cancel < 1e-4f);
    }
    // A shore at x = 160: a splash 64 units from it keeps its energy on the water side (it reflects) where open water
    // lets it run on; wall cells never move.
    {
        const auto water = [](glm::vec2 p) { return p.x < 160.0f; };
        WaveField shore = field(water), open = field();
        for (WaveField* w : {&shore, &open}) w->kick({96.0f, 4.0f}, 6.0f, 24.0f);
        run(shore, 2.0f), run(open, 2.0f);
        const float kept = energy(shore, water), lost = energy(open, water);
        const float wall = energy(shore, [&](glm::vec2 p) { return !water(p); });
        std::fprintf(stderr, "reflection: water-side energy %.2f with shore, %.2f open; in the wall %.2f\n", kept, lost, wall);
        CHECK(kept > 1.5f * lost && wall == 0.0f && shore.wall({200.0f, 0.0f}) && !shore.wall({100.0f, 0.0f}));
    }
    // A leg (body) is a wall for the step after it is placed: no water moves inside it. A kick at the wader's feet,
    // clipped by the leg and a shore, still moves no water in or out (the pond level stays put).
    {
        WaveField w = field([](glm::vec2 p) { return p.x < 40.0f; });
        w.wade({0.0f, 0.0f}, {0.0f, 0.0f}, kDt);
        w.kick({0.0f, 0.0f}, 6.0f, 22.0f);
        float volume = 0.0f;
        for (float h : w.data().height) volume += h;
        w.step(kDt);
        std::fprintf(stderr, "legs: clipped kick volume %.2e, in the leg %.3f\n", volume, w.height({0.0f, 0.0f}));
        CHECK(std::abs(volume) < 1e-3f && w.height({0.0f, 0.0f}) == 0.0f && w.height({-40.0f, 0.0f}) != 0.0f);
    }
    // Damping: a full splash is gone after a few seconds, and the empty span tells the renderer the water is at rest.
    {
        WaveField w = field();
        w.kick({0.0f, 0.0f}, 6.0f, 24.0f);
        const float start = peak(w);
        run(w, 1.0f);
        const float one = peak(w);
        run(w, 3.0f);
        const float four = peak(w);
        run(w, 6.0f);
        std::fprintf(stderr, "damping: peak %.3f, after 1 s %.3f, 4 s %.4f; quiet span %zu\n", start, one, four, w.data().height.size());
        CHECK(start > 5.0f && one < 1.5f && four < 0.05f && w.data().height.empty());
    }
    // Scrolling the window by whole cells keeps every wave at its world position (the window edges are far away).
    {
        WaveField still = field(), moved = field();
        for (WaveField* w : {&still, &moved}) w->kick({40.0f, 40.0f}, 6.0f, 24.0f), run(*w, 1.0f);
        moved.follow({37.0f * 8.0f + 3.0f, -21.0f * 8.0f});
        CHECK(moved.data().origin != still.data().origin);
        run(still, 0.5f), run(moved, 0.5f);
        float worst = 0.0f, seen = 0.0f;
        for (float x = -160.0f; x <= 240.0f; x += 8.0f)
            for (float z = -160.0f; z <= 240.0f; z += 8.0f)
                worst = std::max(worst, std::abs(still.height({x, z}) - moved.height({x, z}))), seen = std::max(seen, std::abs(still.height({x, z})));
        std::fprintf(stderr, "scroll: peak %.3f, difference %.2e\n", seen, worst);
        CHECK(seen > 0.1f && worst == 0.0f);
    }
    // A unit walking (180 units/s) / running (400) through open water at 240 fps (WorldView::wade): its bow mound and
    // stern trough leave ~2-unit wake waves, walking or running.
    for (const float speed : {180.0f, 400.0f}) {
        WaveField w = field();
        float top = 0.0f;
        glm::vec2 p(-250.0f, 0.0f);
        for (int i = 0; i < 360; ++i) {
            const float dt = 1.0f / 240.0f;
            w.step(dt);
            p.x += speed * dt;
            w.wade(p, {speed, 0.0f}, dt);
            top = std::max(top, peak(w));
        }
        std::fprintf(stderr, "wake at %.0f units/s: peak %.3f\n", speed, top);
        CHECK(top > 0.5f && top < 2.5f);
    }
    // Cost: one 256^2 substep (a frame at 60 fps).
    {
        WaveField w = field();
        w.kick({0.0f, 0.0f}, 6.0f, 24.0f);
        const auto t0 = std::chrono::steady_clock::now();
        run(w, 5.0f);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 300.0;
        std::fprintf(stderr, "substep %.3f ms\n", ms);
        CHECK(ms < 2.0);
    }
    return test_result();
}
