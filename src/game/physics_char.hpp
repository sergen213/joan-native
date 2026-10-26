// Character physics (charmove 0x6ad660 -> 0x6a45a0) and the navmesh-backed terrain query.
// Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md Part C §4-§5.
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "game/multimap_nav.hpp"
#include "game/world_sim.hpp"

namespace joa::game {

// ITerrainQuery over the path meshes: height = surface of the poly under (x,y) (ground grid or walkway
// plane), walkable = some open poly contains (x,y). normal() is always up: Phys_ground flattens every
// upward normal (0x6ae0db), so slopes never feed contact forces.
class NavTerrain final : public ITerrainQuery {
public:
    explicit NavTerrain(const NavWorld& m) : m_(m) {}
    float height(float x, float y) const override {
        const float g = m_.grid_height(x, y);
        const int p = m_.find_poly_xy(x, y, g);
        return p >= 0 ? m_.poly_height(p, x, y) : g;
    }
    Vec3 normal(float, float) const override { return {0, 0, 1}; }
    bool walkable(float x, float y) const override { return m_.find_poly_xy(x, y, m_.grid_height(x, y)) >= 0; }

private:
    const NavWorld& m_;
};

class CharPhysics final : public IUnitPhysics {
public:
    static constexpr float kStepUp = 80.0f;            // 0x9dd750: 1.7353579750096775 m
    static constexpr float kAirborne = 0.5f * World::kUnitsPerMetre;  // AI airborne test (0x52daa0)
    static constexpr int kSubstepMs = 20;              // dt cap per integrator call
    static constexpr float kPushSpeed = 100.0f;        // push-aside speed (units/s), HYPOTHESIS

    // nav may be null: then World::terrain() (height / walkable) is used.
    explicit CharPhysics(const NavWorld* nav) : nav_(nav) {}
    void integrate(World& w, float dt_s) override;
    bool floor(const World& w, float x, float y, float z, float& out) const override;
    // Pending push-asides and wall-slide contacts (Mission save section "PHYS", written only while one is active).
    bool pushing() const { return !push_.empty() || !slide_.empty(); }
    std::vector<std::uint8_t> save_state() const;
    bool load_state(std::span<const std::uint8_t> b);

private:
    struct Push { float x = 0, y = 0; int until = 0; };
    float ground(const World& w, float x, float y, float z) const;
    bool walkable(const World& w, float x, float y, float z) const;
    bool wall_edge(int p, float x0, float y0, float x1, float y1, float& ex, float& ey) const;  // blocking edge dir

    const NavWorld* nav_;
    std::vector<std::pair<std::int64_t, int>> cells_;  // integrate's blocking-body hash (cell key, unit id), reused
    std::map<int, Push> push_;   // idle blockers being pushed aside (<= 4 frames)
    std::map<int, Vec3> slide_;  // last accepted wall-slide direction x,y and its game ms (Phys_walk_step +0x10e)
};

}  // namespace joa::game
