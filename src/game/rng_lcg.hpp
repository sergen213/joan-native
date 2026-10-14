// Global gameplay RNG helpers (Misc object 0xa45fa8; GAMEPLAY_CHARACTER_COMBAT.md §2, CONFIRMED).
// The generator state is the `Rng` already used by the script VM (script_vm.hpp: seed*0x15A4E35+1,
// Rng::next(n) = random 0x590540). These are the sibling draws that share the same stream.
#pragma once

#include <cstdint>

#include "game/script_vm.hpp"

namespace joa::game {

// 0x5905a0: advance, return bits 16..30.
inline int rand15(Rng& r) {
    r.seed = r.seed * 0x015A4E35u + 1u;
    return int((r.seed >> 16) & 0x7fff);
}
// 0x590690: [lo, hi]; lo + ftol(rand15/32767.0f * (hi-lo) + 0.5).
inline int random_range(Rng& r, int lo, int hi) {
    const float f = float(rand15(r)) / 32767.0f * float(hi - lo);
    return lo + int(double(f) + 0.5);
}
// 0x590660: [0, 1] (/32767).
inline double rand_unit_incl(Rng& r) { return rand15(r) * 3.051851e-05; }
// 0x5906f0
inline float random_float(Rng& r, float a, float b) { return a + (b - a) * float(rand15(r) / 32767.0); }

}  // namespace joa::game
