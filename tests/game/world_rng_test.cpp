// Gameplay RNG (Misc 0xa45fa8, GAMEPLAY_CHARACTER_COMBAT.md §2): Borland LCG seed*0x15A4E35+1.
// No game data needed.
#include "sim_fixture.hpp"

#include "game/rng_lcg.hpp"

using namespace joa::game;

int main() {
    Rng r;
    r.seed = 0;
    const int want[] = {0, 1, 0, 33, 3, 35};
    for (int i = 0; i < 6; ++i) {
        const int v = r.next(100);
        CHECK(v == want[i], "draw %d = %d, want %d", i, v, want[i]);
    }
    // rand15 / random_range on the same stream: seed 0 -> seed 1 -> bits 16..30 = 0; second = 0x15a.
    Rng q;
    CHECK(rand15(q) == 0 && rand15(q) == 0x15a, "rand15");
    Rng s;
    for (int i = 0; i < 1000; ++i) {
        const int v = random_range(s, 0, 3);
        CHECK(v >= 0 && v <= 3, "random_range out of range %d", v);
    }
    // World draws from the shared stream when pointed at another Rng (Vm::rng()).
    return finish();
}
