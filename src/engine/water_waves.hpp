#pragma once
// Interactive water (port extension, no original counterpart): a damped 2D wave equation on a kWaterWaveGrid^2 height
// field that follows the camera (FrameData::waves, shade_water in shaders/common/pbr_shade.glsl). Waves from every kick
// add up (interfere), reflect off walls - cells the `wet` predicate rejects (shores) and per-frame body obstacles
// (wading legs) - and damp out. Heights are displacements relative to whichever surface a cell lies on, so ponds at
// different heights share the grid. Allocation-free after construction.
#include "engine/renderer.hpp"

#include <functional>
#include <vector>

namespace joa::engine {

class WaveField {
public:
    static constexpr int kN = kWaterWaveGrid;
    static constexpr float kCell = 4.0f;          // world units: a 1024-unit window (~30-unit ripples need 4-unit cells)
    static constexpr float kSpeed = 80.0f;        // units/s (a unit is ~1.8 m tall at 80 units)
    static constexpr float kStep = 1.0f / 60.0f;  // fixed substep: c * dt / cell = 0.33, inside CFL (1/sqrt 2)

    // wet(engine x, z): open water there; called once per cell as it scrolls into the window.
    explicit WaveField(std::function<bool(glm::vec2)> wet);

    // Scrolls the window by whole cells so `centre` (engine x, z) is in its middle; shifted-in cells start at rest.
    void follow(glm::vec2 centre);
    // Runs the fixed substeps owed after dt seconds; bodies added since the last call are walls for them.
    void step(float dt);
    // Displacement kick at engine x, z: `amplitude` down at the centre, a rim up around it (Mexican hat of width
    // sigma: zero volume, so kicks never raise or lower the mean level). Walls and the bodies added since the last
    // step() are not touched; the volume they clip off is taken evenly off the other cells.
    void kick(glm::vec2 p, float amplitude, float sigma);
    // A wading unit at engine x, z moving at `velocity` (engine xz units/s) over dt: its legs are walls for the next
    // step() (waves scatter around them) and, moving, it heaps water up ahead and leaves a trough behind (the wake).
    void wade(glm::vec2 p, glm::vec2 velocity, float dt);

    float height(glm::vec2 p) const;  // the cell holding p, 0 outside the window
    bool wall(glm::vec2 p) const;     // the cell holding p is a static wall (or outside the window)
    bool covers(glm::vec2 p) const;
    // Empty once nothing kicked for kQuiet seconds (the field has damped to rest).
    WaterWaves data() const;

private:
    static constexpr float kQuiet = 8.0f;
    int cell_index(glm::vec2 p) const;  // -1 outside the window
    void shift(std::vector<float>& a, int dx, int dz, float fill);

    std::function<bool(glm::vec2)> wet_;
    std::vector<float> h_, prev_;  // current and previous substep heights
    std::vector<float> open_;      // 1 open water, 0 static wall (the window border is always a wall)
    std::vector<float> mask_;      // open_ with the bodies of the current step cut out
    std::vector<float> scratch_;
    std::vector<glm::vec3> bodies_;  // x, z, radius; reserved
    glm::ivec2 corner_{0};           // window origin in cells
    bool placed_ = false;
    float owed_ = 0.0f, idle_ = kQuiet;
};

}  // namespace joa::engine
