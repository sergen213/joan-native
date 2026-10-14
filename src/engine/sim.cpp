#include "engine/sim.hpp"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstring>

namespace joa::engine {

int FixedStep::advance(double frame_seconds) {
    accumulator += frame_seconds > 0.0 ? frame_seconds : 0.0;
    int n = 0;
    while (accumulator >= tick && n < max_ticks_per_frame) {
        accumulator -= tick;
        ++n;
    }
    if (n == max_ticks_per_frame && accumulator >= tick) accumulator = std::fmod(accumulator, tick);
    return n;
}

void step(World& w, const PlayerCommand& cmd, float dt) {
    // Demo locomotion only; real character movement comes from the recovered gameplay code.
    const float speed = cmd.run ? 7.0f : 3.5f;
    glm::vec2 m{cmd.move_x, cmd.move_y};
    if (const float len = glm::length(m); len > 1.0f) m /= len;
    const float s = std::sin(cmd.heading), c = std::cos(cmd.heading);
    // heading 0 => forward is -Z, right is +X
    const glm::vec3 forward{-s, 0.0f, -c}, right{c, 0.0f, -s};
    const glm::vec3 target_vel = (forward * m.y + right * m.x) * speed;
    const float k = 1.0f - std::exp(-12.0f * dt);
    w.player_velocity += (target_vel - w.player_velocity) * k;
    w.player_position += w.player_velocity * dt;
    if (glm::dot(m, m) > 1e-4f) w.player_yaw = std::atan2(-(forward.x * m.y + right.x * m.x), -(forward.z * m.y + right.z * m.x));
    ++w.tick;
}

std::uint64_t state_hash(const World& w) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(&w.tick, sizeof w.tick);
    mix(&w.player_position, sizeof w.player_position);
    mix(&w.player_yaw, sizeof w.player_yaw);
    mix(&w.player_velocity, sizeof w.player_velocity);
    return h;
}

World interpolate(const World& a, const World& b, float t) {
    World r = b;
    r.player_position = glm::mix(a.player_position, b.player_position, t);
    float d = b.player_yaw - a.player_yaw;
    d = std::remainder(d, glm::two_pi<float>());
    r.player_yaw = a.player_yaw + d * t;
    return r;
}

}  // namespace joa::engine
