#include "game/multimap_nav.hpp"

#include <cmath>

#include "game/world_sim.hpp"

namespace joa::game {

namespace {
constexpr float kWorldScale = 32.0f;  // world mesh units (Path_scale 32, 0x42000000)
constexpr float kWorldCell = 16.0f;   // Path_translate(16, 16, 0) after the scale: cell centre
constexpr float kLoadBox = 8000.0f;   // World_draw inner box half size (0x9cf7a4)
constexpr float kKeepBox = 16000.0f;  // outer box half size (0x9cf7a0)
}  // namespace

NavWorld::NavWorld(NavMesh single) { levels_.push_back({std::move(single)}); }

NavWorld::NavWorld(std::vector<NavLevel> levels, std::optional<NavMesh> world) : levels_(std::move(levels)), world_(std::move(world)) {
    int base = 0;
    for (auto& l : levels_) {
        l.base = base;
        base += int(l.mesh.polys().size());
    }
}

bool NavWorld::in_level(int lv, float x, float y) const {  // AABB_contains 0x60d5f0 (inclusive)
    const NavLevel& l = levels_[std::size_t(lv)];
    return levels_.size() == 1 || (x >= l.ox && x <= l.x1 && y >= l.oy && y <= l.y1);
}

int NavWorld::level_at(float x, float y, int prefer) const {
    if (prefer >= 0 && prefer < int(levels_.size()) && in_level(prefer, x, y)) return prefer;
    for (int i = 0; i < int(levels_.size()); ++i)
        if (in_level(i, x, y)) return i;
    return -1;
}

int NavWorld::level_of_poly(int poly) const {
    for (int i = int(levels_.size()) - 1; i >= 0; --i)
        if (poly >= levels_[std::size_t(i)].base) return i;
    return -1;
}

const NavPoly& NavWorld::poly(int poly) const {
    const NavLevel& l = levels_[std::size_t(level_of_poly(poly))];
    return l.mesh.polys()[std::size_t(poly - l.base)];
}

int NavWorld::find_poly_xy(float x, float y, float z, bool ignore_blocked) const {
    int best = -1;
    float bd = 0;
    for (int i = 0; i < int(levels_.size()); ++i) {
        if (!in_level(i, x, y)) continue;
        const NavLevel& l = levels_[std::size_t(i)];
        const float lx = x - l.ox, ly = y - l.oy;
        const int p = l.mesh.find_poly_xy(lx, ly, z, ignore_blocked);
        if (p < 0) continue;
        const float d = std::fabs(l.mesh.poly_height(p, lx, ly) - z);
        if (best < 0 || d < bd) best = l.base + p, bd = d;
    }
    return best;
}

float NavWorld::poly_height(int poly, float x, float y) const {
    const NavLevel& l = levels_[std::size_t(level_of_poly(poly))];
    return l.mesh.poly_height(poly - l.base, x - l.ox, y - l.oy);
}

float NavWorld::grid_height(float x, float y) const {
    const int lv = level_at(x, y);
    if (lv < 0) return 0.0f;
    const NavLevel& l = levels_[std::size_t(lv)];
    return l.mesh.grid_height(x - l.ox, y - l.oy);
}

void NavWorld::set_blocked(int poly, bool blocked) {
    NavLevel& l = levels_[std::size_t(level_of_poly(poly))];
    l.mesh.set_blocked(poly - l.base, blocked);
}

bool NavWorld::segment_blocked(NavPt a, NavPt b, float eye_z) const {
    const int lv = level_at(a.x, a.y);
    if (lv < 0) return false;
    const NavLevel& l = levels_[std::size_t(lv)];
    return l.mesh.segment_blocked({a.x - l.ox, a.y - l.oy, a.z}, {b.x - l.ox, b.y - l.oy, b.z}, eye_z);
}

PathResult NavWorld::local_path(int lv, PathQuery q) const {
    const NavLevel& l = levels_[std::size_t(lv)];
    q.from.x -= l.ox, q.from.y -= l.oy, q.to.x -= l.ox, q.to.y -= l.oy;
    if (q.start_poly >= 0) q.start_poly = level_of_poly(q.start_poly) == lv ? q.start_poly - l.base : -1;
    PathResult r = game::find_path(l.mesh, q);
    for (auto& p : r.points) p.x += l.ox, p.y += l.oy;
    return r;
}

PathResult NavWorld::find_path(const PathQuery& q) const {
    const int lv = level_at(q.from.x, q.from.y);
    if (!world_) return local_path(lv < 0 ? 0 : lv, q);
    if (lv < 0) return {};  // unit+0x339 == 0: empty path
    const NavLevel& l = levels_[std::size_t(lv)];
    if (l.loaded && in_level(lv, q.to.x, q.to.y)) return local_path(lv, q);

    // World mesh: coordinates / 32, z 0, radius BODY_R / 32; scaled back and moved to cell centres.
    PathQuery wq;
    wq.from = {q.from.x / kWorldScale, q.from.y / kWorldScale, 0};
    wq.to = {q.to.x / kWorldScale, q.to.y / kWorldScale, 0};
    wq.radius = q.body_r / kWorldScale;
    wq.max_expand = q.max_expand;
    PathResult w = game::find_path(*world_, wq);
    for (auto& p : w.points) p = {p.x * kWorldScale + kWorldCell, p.y * kWorldScale + kWorldCell, p.z};
    if (w.points.empty()) return w;  // Path_copy of the empty world path

    // seq[0] = start waypoint (Path_reset ftol), seq[1..n] = world waypoints, seq[n] = destination.
    const int n = int(w.points.size());
    std::vector<NavPt> seq{{float(int(wq.from.x)) * kWorldScale + kWorldCell, float(int(wq.from.y)) * kWorldScale + kWorldCell, 0}};
    seq.insert(seq.end(), w.points.begin(), w.points.end());
    int c0 = -1, c4 = 0;  // first..last waypoint of the leading run inside the (loaded) start level
    for (int i = 0; i <= n; ++i) {
        if (!l.loaded || !in_level(lv, seq[std::size_t(i)].x, seq[std::size_t(i)].y)) break;
        if (c0 < 0) c0 = i;
        c4 = i;
    }
    if (c4 <= c0 && c0 != -1 && c0 + 1 <= n) c4 = c0 + 1;
    if (c0 == -1 || c4 <= c0) return w;

    PathQuery lq = q;
    lq.from = seq[std::size_t(c0)];
    lq.to = seq[std::size_t(c4)];
    lq.start_poly = -1;
    const PathResult lp = local_path(lv, lq);
    // Unit path = world[1..c0] + local + world[c4+1..n-1] + destination (the local one when c4 == n).
    PathResult r;
    r.status = w.status;
    r.expanded = w.expanded + lp.expanded;
    r.points.assign(seq.begin() + 1, seq.begin() + c0 + 1);
    r.points.insert(r.points.end(), lp.points.begin(), lp.points.end());
    for (int i = c4 + 1; i < n; ++i) r.points.push_back(seq[std::size_t(i)]);
    r.points.push_back(c4 == n && !lp.points.empty() ? lp.points.back() : seq[std::size_t(n)]);
    return r;
}

void NavWorld::stream(World& w) {
    if (levels_.size() < 2) return;
    if (const Unit* p = w.unit(w.player_id()); p && !p->deleted) {  // ponytail: camera = player position
        auto overlaps = [&](const NavLevel& l, float h) {
            return l.ox <= p->pos.x + h && l.x1 >= p->pos.x - h && l.oy <= p->pos.y + h && l.y1 >= p->pos.y - h;
        };
        for (NavLevel& l : levels_) l.loaded = overlaps(l, kLoadBox) || (l.loaded && overlaps(l, kKeepBox));
    }
    auto& us = w.units();
    unit_key_.resize(us.size(), -1);
    for (std::size_t i = 0; i < us.size(); ++i) {
        Unit& u = us[i];
        int& key = unit_key_[i];
        if (u.deleted) { key = -1; continue; }
        const int lv = level_at(u.pos.x, u.pos.y, key >= 0 ? key / 2 : -1);
        if (lv < 0) continue;
        const bool loaded = levels_[std::size_t(lv)].loaded;
        if (lv * 2 + loaded == key) continue;
        key = lv * 2 + loaded;
        u.m.active = loaded;  // Unit_activate 0x442820 / Unit_deactivate 0x41fcc0
    }
}

void NavWorld::resync(const World& w) {
    const auto& us = w.units();
    unit_key_.assign(us.size(), -1);
    for (std::size_t i = 0; i < us.size(); ++i) {
        if (us[i].deleted) continue;
        if (const int lv = level_at(us[i].pos.x, us[i].pos.y); lv >= 0) unit_key_[i] = lv * 2 + levels_[std::size_t(lv)].loaded;
    }
}

std::vector<std::uint8_t> NavWorld::save_levels() const {
    std::vector<std::uint8_t> b;
    for (const NavLevel& l : levels_) b.push_back(l.loaded);
    return b;
}

bool NavWorld::load_levels(const std::vector<std::uint8_t>& b) {
    if (b.size() != levels_.size()) return false;
    for (std::size_t i = 0; i < b.size(); ++i) levels_[i].loaded = b[i] != 0;
    return true;
}

}  // namespace joa::game
