#include "game/physics_char.hpp"

#include <cmath>
#include <unordered_map>
#include <vector>

#include "formats/vfs_bytes.hpp"
#include "game/savegame_container.hpp"

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

// The edge of the poly under (x0,y0) that the step to (x1,y1) crosses, as a unit direction.
bool CharPhysics::wall_edge(float x0, float y0, float z, float x1, float y1, float& ex, float& ey) const {
    if (!nav_) return false;
    const int p = nav_->find_poly_xy(x0, y0, z);
    if (p < 0) return false;
    const NavLevel& l = nav_->levels()[std::size_t(nav_->level_of_poly(p))];
    const int lp = p - l.base;
    const float ax = x0 - l.ox, ay = y0 - l.oy, bx = x1 - l.ox, by = y1 - l.oy;
    const auto& v = l.mesh.polys()[std::size_t(lp)].v;
    for (int i = 0; i < int(v.size()); ++i) {
        const auto& c = l.mesh.pt(lp, i);
        const auto& d = l.mesh.pt(lp, i + 1);
        const float cx = float(c.x), cy = float(c.y), dx = float(d.x) - cx, dy = float(d.y) - cy;
        const float mx = bx - ax, my = by - ay, den = mx * dy - my * dx;
        if (den == 0) continue;
        const float t = ((cx - ax) * dy - (cy - ay) * dx) / den;  // along the step
        const float s = ((cx - ax) * my - (cy - ay) * mx) / den;  // along the edge
        if (t < 0 || t > 1 || s < 0 || s > 1) continue;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0) continue;
        ex = dx / len, ey = dy / len;
        return true;
    }
    return false;
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
    // Inactive units (+0x2ed == 0) have no body actor; climbers (0x4ac910), cart drivers (0x454b20)
    // and riders (0x5174e0) have theirs disabled: neither move nor block (ladder / cart / mount sync
    // places them).
    auto bodiless = [](const Unit& u) { return u.deleted || !u.m.active || u.m.climb || u.m.cart || u.mount; };
    for (const Unit& u : units)
        if (!bodiless(u) && !w.dead(u)) grid[key(u.pos.x, u.pos.y)].push_back(u.id);

    const int steps = (frame_ms + kSubstepMs - 1) / kSubstepMs;
    for (Unit& u : units) {
        if (bodiless(u)) continue;
        if (!u.airborne && !is_locomotion(u.action)) {  // ground friction (same factor as World::integrate_units)
            u.vel.x *= 0.5f, u.vel.y *= 0.5f;
            if (std::fabs(u.vel.x) < 1 && std::fabs(u.vel.y) < 1) u.vel.x = u.vel.y = 0;
        }
        float px = u.root_vel.x, py = u.root_vel.y;   // root-motion move request (0x52daa0)
        if (auto it = push_.find(u.id); it != push_.end()) {
            if (now < it->second.until) px += it->second.x, py += it->second.y;
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
            // Navmesh walls and the 80-unit step-up limit.
            const float g0 = ground(w, u.pos.x, u.pos.y, u.pos.z);
            auto ok = [&](float x, float y) {
                if (!walkable(w, x, y, u.pos.z)) return false;
                return u.airborne || ground(w, x, y, u.pos.z) - g0 <= kStepUp;
            };
            float tx = u.pos.x + vx * h, ty = u.pos.y + vy * h;
            if (!ok(tx, ty)) {
                // Phys_walk_step 0x6a9570 (asm 0x6a9fc5..0x6aa1b5): slide along the blocking edge, its
                // direction flipped toward the motion, at |v|.|dot|; when that step is accepted
                // (0x6ae610) and |dot| > 0.707 (0x9dd770) the unit turns toward the slide: +2.0 deg when
                // the wrapped (slide - yaw) > 0, else -1.0 deg (Unit_set_yaw_deg); the player's camera 0
                // target yaw follows (CamMan_set_target). No edge found (no poly under the unit):
                // ponytail: axis slide, as before.
                // The dot uses the attempted step, which after an accepted slide is the slide itself: the
                // original stores the slide as the body velocity (+0x10e/+0x112/+0x116) and the next frame
                // steps along it, so a hero running at 65 deg into the mission-1 edge keeps (nearly) full
                // speed along it and turns 115 -> 182 deg in ~0.7 s (DIFFERENTIAL_VERIFICATION.md).
                // The slide direction stays the step direction while the hero is in contact (set within the
                // last 100 ms: unblocked substeps between blocked frames do not reset it).
                // ponytail: the last slide direction is not saved (a save taken mid-slide resumes one
                // step later with the request direction).
                float ex = 0, ey = 0;
                if (wall_edge(u.pos.x, u.pos.y, u.pos.z, tx, ty, ex, ey)) {
                    const float sp = std::sqrt(vx * vx + vy * vy);
                    auto prev = slide_.find(u.id);
                    if (prev != slide_.end() && float(now) - prev->second.z > 100.0f) slide_.erase(prev), prev = slide_.end();
                    const float mx = prev != slide_.end() ? prev->second.x : vx / sp;
                    const float my = prev != slide_.end() ? prev->second.y : vy / sp;
                    float dot = mx * ex + my * ey;
                    if (dot < 0) ex = -ex, ey = -ey, dot = -dot;
                    const float sx = ex * sp * dot, sy = ey * sp * dot;
                    if (ok(u.pos.x + sx * h, u.pos.y + sy * h)) {
                        tx = u.pos.x + sx * h, ty = u.pos.y + sy * h, vx = sx, vy = sy;
                        slide_[u.id] = {ex, ey, float(now)};
                        if (dot > 0.707f) {
                            float diff = std::fmod(std::atan2(sy, sx) * 57.29578f - u.yaw_deg, 360.0f);
                            if (diff > 180) diff -= 360;
                            if (diff < -180) diff += 360;
                            w.set_yaw(u, u.yaw_deg + (diff > 0 ? 2.0f : -1.0f));
                            if (u.id == w.player_id()) w.controller().camera_yaw_out = u.yaw_goal;
                        }
                    } else {
                        tx = u.pos.x, ty = u.pos.y, vx = vy = 0;
                        slide_.erase(u.id);
                    }
                } else if (ok(tx, u.pos.y)) ty = u.pos.y, vy = 0;
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

std::vector<std::uint8_t> CharPhysics::save_state() const {
    std::vector<std::uint8_t> b;
    ByteWriter w(b);
    w.i32(std::int32_t(push_.size()));
    for (const auto& [id, p] : push_) w.i32(id), w.f32(p.x), w.f32(p.y), w.i32(p.until);
    return b;
}

bool CharPhysics::load_state(std::span<const std::uint8_t> b) {
    formats::ByteReader r(b);
    push_.clear();
    for (int n = r.i32(); n > 0 && !r.at_end(); --n) {
        const int id = r.i32();
        Push p;
        p.x = r.f32(), p.y = r.f32(), p.until = r.i32();
        push_[id] = p;
    }
    return r.at_end();
}

}  // namespace joa::game
