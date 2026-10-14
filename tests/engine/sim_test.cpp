// Fixed-step accumulator and simulation determinism independent of render frame rate.
#include "check.hpp"
#include "engine/sim.hpp"

#include <vector>

using namespace joa::engine;

static PlayerCommand command_for(std::uint64_t tick) {
    PlayerCommand c;
    c.move_y = tick % 50 < 30 ? 1.0f : -0.3f;
    c.move_x = float(int(tick % 7) - 3) / 3.0f;
    c.heading = float(tick) * 0.013f;
    c.run = tick % 64 > 40;
    return c;
}

// Plays until `ticks` simulation ticks have run, feeding render frames of the given durations.
static World play(const std::vector<double>& frame_times, std::uint64_t ticks) {
    World w;
    FixedStep clock;
    for (std::size_t f = 0; w.tick < ticks; ++f) {
        for (int n = clock.advance(frame_times[f % frame_times.size()]); n > 0 && w.tick < ticks; --n)
            step(w, command_for(w.tick), float(clock.tick));
        const float a = clock.alpha();
        CHECK(a >= 0.0f && a <= 1.0f);  // 1.0 only through float rounding just below a full tick
    }
    return w;
}

int main() {
    {
        FixedStep c;  // 30 Hz
        CHECK(c.advance(1.0 / 60.0) == 0);
        CHECK_NEAR(c.alpha(), 0.5f, 1e-5);
        CHECK(c.advance(1.0 / 60.0) == 1);
        CHECK(c.advance(-1.0) == 0);  // clock going backwards is ignored
        CHECK(c.advance(10.0) == c.max_ticks_per_frame);  // stall: bounded catch-up
        CHECK(c.accumulator >= 0.0 && c.accumulator < c.tick);
    }

    const std::uint64_t kTicks = 600;  // 20 s at 30 Hz
    const World ref = play({1.0 / 144.0}, kTicks);
    const World w60 = play({1.0 / 60.0}, kTicks);
    const World w24 = play({1.0 / 24.0}, kTicks);
    const World jitter = play({0.004, 0.051, 0.017, 0.0333, 0.2, 0.0}, kTicks);
    const World again = play({1.0 / 144.0}, kTicks);
    std::fprintf(stderr, "hash %016llx pos %.3f %.3f\n", static_cast<unsigned long long>(state_hash(ref)), ref.player_position.x,
                 ref.player_position.z);
    CHECK(ref.tick == kTicks);
    CHECK(state_hash(ref) == state_hash(w60));
    CHECK(state_hash(ref) == state_hash(w24));
    CHECK(state_hash(ref) == state_hash(jitter));
    CHECK(state_hash(ref) == state_hash(again));
    CHECK(glm::length(ref.player_position) > 1.0f);  // the scripted input actually moved the player

    World other;
    PlayerCommand c = command_for(0);
    c.move_x += 0.001f;
    step(other, c, float(kTickSeconds));
    World base;
    step(base, command_for(0), float(kTickSeconds));
    CHECK(state_hash(other) != state_hash(base));

    {  // interpolation endpoints and yaw wrap-around
        World a, b;
        a.player_position = {0, 0, 0};
        b.player_position = {2, 0, -4};
        a.player_yaw = 3.1f;
        b.player_yaw = -3.1f;
        CHECK(interpolate(a, b, 0.0f).player_position == a.player_position);
        CHECK(interpolate(a, b, 1.0f).player_position == b.player_position);
        CHECK_NEAR(interpolate(a, b, 0.5f).player_position.z, -2.0f, 1e-6);
        CHECK(std::fabs(interpolate(a, b, 0.5f).player_yaw) > 3.1f);  // short way through +-pi, not 0
    }
    return test_result();
}
