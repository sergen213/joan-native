#include "game/physics_char.hpp"

#include <cmath>
#include <unordered_map>
#include <vector>

namespace joa::game {

namespace {
bool idle_able(int a) { return (a >= IDLE && a <= IDLEDN) || a == JATTBI || a == H_IDLE || a == STOP || a == STOP_B; }
}  // namespace

float CharPhysics::ground(const World& w, float x, float y, float z) const {  // Phys_ground 0x6adfd0
    if (nav_) {
        const int p = nav_->find_poly_xy(x, y, z);
        if (p >= 0) return nav_->poly_height(p, x, y);
        return nav_->grid_height(x, y);  // no poly: the original returns 0; units only get here when blocked
    }
    return w.terrain() ? w.terrain()->height(x, y) : 0.0f;
}

bool CharPhysics::walkable(const World& w, float x, float y, float z) const {
    if (nav_) return nav_->find_poly_xy(x, y, z) >= 0;
    return !w.terrain() || w.terrain()->walkable(x, y);
}

void CharPhysics::integrate(World& w, float dt_s) {
    const int now = w.now();
    const int frame_ms = int(dt_s * 1000.0f + 0.5f);
    auto& units = w.units();
    // Spatial hash of blocking bodies (alive units), cell 128 units.
    constexpr float kCell = 128.0f;
    auto key = [](float x, float y) {
        return (std::int64_t(std::floor(x / kCell)) << 32) ^ std::int64_t(std::uint32_t(std::int32_t(std::floor(y / kCell))));
    };
    std::unordered_map<std::int64_t, std::vector<int>> grid;
    auto radius_of = [&](const Unit& u) { return w.data().units[std::size_t(u.cls)].cyl_radius; };
    for (const Unit& u : units)
        if (!u.deleted && !w.dead(u)) grid[key(u.pos.x, u.pos.y)].push_back(u.id);

    const int steps = (frame_ms + kSubstepMs - 1) / kSubstepMs;
    for (Unit& u : units) {
        if (u.deleted) continue;
        if (!u.airborne && !is_locomotion(u.action)) {  // ground friction (same factor as World::integrate_units)
            u.vel.x *= 0.5f, u.vel.y *= 0.5f;
            if (std::fabs(u.vel.x) < 1 && std::fabs(u.vel.y) < 1) u.vel.x = u.vel.y = 0;
        }
        float px = 0, py = 0;
        if (auto it = push_.find(u.id); it != push_.end()) {
            if (now < it->second.until) px = it->second.x, py = it->second.y;
            else push_.erase(it);
        }
        const float r = radius_of(u);
        const bool alive = !w.dead(u);
        const float g = 9.81f * float(u.gravity_scale) * World::kUnitsPerMetre;
        float left = float(frame_ms) / 1000.0f;
        for (int s = 0; s < steps && left > 0; ++s) {
            const float h = std::min(left, kSubstepMs / 1000.0f);
            left -= h;
            float vx = u.vel.x + px, vy = u.vel.y + py;
            // Unit-unit block (swept shapes in the original): remove the velocity component toward
            // any body we would overlap, so the mover slides around it; idle blockers get pushed aside.
            if (alive && (vx != 0 || vy != 0)) {
                const float nx = u.pos.x + vx * h, ny = u.pos.y + vy * h;
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy) {
                        auto c = grid.find(key(nx + dx * kCell, ny + dy * kCell));
                        if (c == grid.end()) continue;
                        for (int oid : c->second) {
                            if (oid == u.id) continue;
                            Unit& o = units[std::size_t(oid - 1)];
                            if (std::fabs(o.pos.z - u.pos.z) > 150.0f) continue;  // different level (wall walk)
                            const float ex = o.pos.x - u.pos.x, ey = o.pos.y - u.pos.y;
                            const float rr = r + radius_of(o);
                            const float qx = o.pos.x - (u.pos.x + vx * h), qy = o.pos.y - (u.pos.y + vy * h);
                            if (qx * qx + qy * qy >= rr * rr) continue;
                            const float len = std::sqrt(ex * ex + ey * ey);
                            if (len < 0.00015f) continue;
                            const float nxn = ex / len, nyn = ey / len;
                            const float vn = vx * nxn + vy * nyn;
                            if (vn <= 0) continue;  // moving away: allowed (self-resolving overlap)
                            vx -= nxn * vn;
                            vy -= nyn * vn;
                            if (is_locomotion(u.action) && idle_able(o.action) && !push_.count(o.id)) {
                                const float side = nxn * u.vel.y - nyn * u.vel.x;  // mover heading left (>0) or right of n
                                Push p;
                                p.x = side > 0 ? nyn : -nyn;
                                p.y = side > 0 ? -nxn : nxn;
                                p.x *= kPushSpeed, p.y *= kPushSpeed;
                                p.until = now + frame_ms * 4;
                                push_[o.id] = p;
                            }
                        }
                    }
            }
            // Navmesh walls and the 80-unit step-up limit; blocked moves slide along x or y.
            const float g0 = ground(w, u.pos.x, u.pos.y, u.pos.z);
            auto ok = [&](float x, float y) {
                if (!walkable(w, x, y, u.pos.z)) return false;
                return u.airborne || ground(w, x, y, u.pos.z) - g0 <= kStepUp;
            };
            float tx = u.pos.x + vx * h, ty = u.pos.y + vy * h;
            if (!ok(tx, ty)) {
                if (ok(tx, u.pos.y)) ty = u.pos.y, vy = 0;
                else if (ok(u.pos.x, ty)) tx = u.pos.x, vx = 0;
                else tx = u.pos.x, ty = u.pos.y, vx = vy = 0;
            }
            if (px == 0 && py == 0) u.vel.x = vx, u.vel.y = vy;
            const float gnd = ground(w, tx, ty, u.pos.z);
            float tz = u.pos.z;
            if (u.airborne) {
                u.vel.z -= g * h;
                tz += u.vel.z * h;
                if (tz <= gnd && u.vel.z <= 0) { tz = gnd; u.vel.z = 0; u.airborne = false; }
            } else if (tz - gnd > kAirborne) {  // walked off a ledge: fall
                u.airborne = true;
                u.vel.z = 0;
            } else {
                tz = gnd;
                u.vel.z = 0;
            }
            u.pos = {tx, ty, tz};
        }
    }
}

}  // namespace joa::game
