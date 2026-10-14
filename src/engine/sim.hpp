#pragma once
// Fixed-timestep simulation. No rendering, windowing or input-device dependencies: the app layer
// converts actions into PlayerCommand and renders an interpolated copy of World.
#include <glm/glm.hpp>

#include <cstdint>

namespace joa::engine {

// HIGH CONFIDENCE (EngineRE): joan.exe runs logic/draw on a 30 Hz timer (0x58a830(30,30)) and scales
// movement by speed = elapsed_ms/1000*30 clamped to 5, i.e. variable steps in 30 Hz units. The port
// uses exact fixed 30 Hz ticks instead (deterministic); behaviour matches whenever frames keep up.
inline constexpr int kTickHz = 30;
inline constexpr double kTickSeconds = 1.0 / kTickHz;

// Accumulator: advance(real_seconds) returns how many ticks to run; alpha() blends prev->current.
struct FixedStep {
    double tick = kTickSeconds;
    double accumulator = 0.0;
    int max_ticks_per_frame = 8;  // avoids the spiral of death after stalls (excess time is dropped)

    int advance(double frame_seconds);
    float alpha() const { return float(accumulator / tick); }
};

struct PlayerCommand {
    float move_x = 0.0f;   // strafe, [-1,1]
    float move_y = 0.0f;   // forward, [-1,1]
    float heading = 0.0f;  // camera yaw the move is relative to, radians
    bool run = false;
};

struct World {
    std::uint64_t tick = 0;
    glm::vec3 player_position{0.0f};
    float player_yaw = 0.0f;  // facing, radians; 0 faces -Z
    glm::vec3 player_velocity{0.0f};
};

void step(World& world, const PlayerCommand& cmd, float dt);
std::uint64_t state_hash(const World& world);  // FNV-1a over the state bytes
World interpolate(const World& previous, const World& current, float alpha);

}  // namespace joa::engine
