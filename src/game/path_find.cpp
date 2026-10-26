#include "game/path_find.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <tuple>

namespace joa::game {

namespace {

int idist(long long dx, long long dy) {  // 00590410 integer sqrt
    const long long s = dx * dx + dy * dy;
    long long r = (long long)std::sqrt(double(s));
    while (r * r > s) --r;
    while ((r + 1) * (r + 1) <= s) ++r;
    return int(r);
}

float cross2(NavPt u, NavPt v) { return u.x * v.y - u.y * v.x; }
NavPt sub(NavPt a, NavPt b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
NavPt lerp(NavPt a, NavPt b, float t) { return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)}; }
NavPt P(const formats::PfdPoint& p) { return {float(p.x), float(p.y), float(p.z)}; }

struct Portal { NavPt a, b; };

// Path_smooth 0066ece0: radius-shrunk portals, funnel over <= 8 portals, waypoint = crossing of
// (prev -> funnel point) with the portal. lateral/width (formation offset) are not modelled.
std::vector<NavPt> smooth(NavPt start, const std::vector<Portal>& portals, NavPt dest, float radius) {
    const std::size_t n = portals.size();
    std::vector<Portal> sh(n);
    for (std::size_t i = 0; i < n; ++i) {  // 0066f820
        const NavPt a = portals[i].a, b = portals[i].b;
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        if (len > 2 * radius) {
            sh[i] = {lerp(a, b, radius / len), lerp(a, b, 1 - radius / len)};
        } else {
            const NavPt m = lerp(a, b, 0.5f);
            sh[i] = {m, m};
        }
    }
    std::vector<NavPt> out;
    NavPt prev = start;
    auto orient = [&](const Portal& p, NavPt& L, NavPt& R) {
        if (cross2(sub(p.b, prev), sub(p.a, prev)) >= 0) { L = p.a; R = p.b; } else { L = p.b; R = p.a; }
    };
    for (std::size_t i = 0; i < n; ++i) {
        NavPt Li, Ri;
        orient(sh[i], Li, Ri);
        NavPt aL = Li, aR = Ri, aim{};
        bool have = false;
        for (std::size_t j = i + 1; j <= std::min(n, i + 8) && !have; ++j) {
            NavPt Lj, Rj;
            if (j == n) { Lj = Rj = dest; } else { orient(sh[j], Lj, Rj); }
            if (cross2(sub(aR, prev), sub(Rj, prev)) >= 0) {  // right side tightens
                if (cross2(sub(aL, prev), sub(Rj, prev)) > 0) { aim = aL; have = true; break; }
                aR = Rj;
            }
            if (cross2(sub(aL, prev), sub(Lj, prev)) <= 0) {  // left side tightens
                if (cross2(sub(aR, prev), sub(Lj, prev)) < 0) { aim = aR; have = true; break; }
                aL = Lj;
            }
            if (j == n) { aim = dest; have = true; }
        }
        if (!have) aim = lerp(aL, aR, 0.5f);
        const NavPt d = sub(aim, prev), e = sub(Ri, Li);
        const float den = cross2(d, e);
        float t = std::fabs(den) > 1e-6f ? cross2(d, sub(prev, Li)) / den : 0.5f;
        t = std::clamp(t, 0.0f, 1.0f);
        const NavPt w = lerp(Li, Ri, t);
        out.push_back(w);
        prev = w;
    }
    out.push_back(dest);
    std::vector<NavPt> clean;  // drop waypoints closer than 0.5 in x and y to the previous one
    NavPt last = start;
    for (const auto& w : out) {
        if (std::fabs(w.x - last.x) < 0.5f && std::fabs(w.y - last.y) < 0.5f && &w != &out.back()) continue;
        clean.push_back(w);
        last = w;
    }
    return clean;
}

}  // namespace

PathResult find_path(const NavMesh& mesh, const PathQuery& q) {
    PathResult res;
    const auto& polys = mesh.polys();
    int sp = q.start_poly;
    if (sp < 0) sp = mesh.find_poly(q.from.x, q.from.y, q.from.z, q.radius);
    if (sp < 0) sp = mesh.nearest_poly(float(int(q.from.x)), float(int(q.from.y)), float(int(q.from.z)));
    if (sp < 0) return res;
    const float reff = q.radius < NavMesh::kNarrowClearance ? 0.0f : q.radius;
    const int tx = int(q.to.x), ty = int(q.to.y);  // ftol
    const int sx = int(q.from.x), sy = int(q.from.y);

    if (mesh.contains(sp, q.to.x, q.to.y)) {
        res.status = PathResult::Full;
        res.points = {q.to};
        return res;
    }

    // Search state reused across calls (100+ searches per tick in battles), one per thread: a node's slot is live
    // only while its stamp equals this search's epoch, so nothing is cleared or allocated per call.
    struct Node { int g, h, f, parent, seq; std::uint32_t stamp; char closed; };
    using E = std::tuple<int, int, int>;
    struct Scratch {
        std::vector<Node> node;
        std::vector<int> order;
        std::vector<E> heap;
        std::uint32_t epoch = 0;
    };
    thread_local Scratch S;
    const std::size_t N = polys.size();
    if (S.node.size() < N) S.node.resize(N, Node{0, 0, 0, -1, 0, 0, 0});
    if (++S.epoch == 0) {  // wrapped: forget every stamp
        for (auto& x : S.node) x.stamp = 0;
        S.epoch = 1;
    }
    const std::uint32_t ep = S.epoch;
    Node* nd = S.node.data();
    std::vector<int>& order = S.order;
    order.clear();
    // (f, -seq, poly): smallest f first; among equal f the newest insertion first (0066fca0 inserts before equals).
    // Min-heap on a reused vector: exactly std::priority_queue<E, vector<E>, greater<E>>'s push_heap / pop_heap.
    std::vector<E>& open = S.heap;
    open.clear();
    const std::greater<E> cmp;
    int counter = 0;
    auto push = [&](int p) {
        nd[p].seq = ++counter;
        open.emplace_back(nd[p].f, -counter, p);
        std::push_heap(open.begin(), open.end(), cmp);
    };

    nd[sp] = Node{0, 0, 0, -1, 0, ep, 0};
    nd[sp].h = idist(tx - polys[std::size_t(sp)].cx, ty - polys[std::size_t(sp)].cy);
    nd[sp].f = nd[sp].h;
    order.push_back(sp);
    push(sp);

    int budget = q.max_expand, goal = -1;
    while (!open.empty() && budget > 0) {
        std::pop_heap(open.begin(), open.end(), cmp);
        const auto [ff, ns, p] = open.back();
        open.pop_back();
        Node& cur = nd[p];
        if (cur.closed || -ns != cur.seq) continue;  // stale entry
        cur.closed = 1;
        --budget;
        ++res.expanded;
        if (mesh.contains(p, q.to.x, q.to.y)) { goal = p; break; }
        const auto& pp = polys[std::size_t(p)];
        for (std::size_t e = 0; e < pp.nb.size(); ++e) {
            const int n = pp.nb[e];
            if (n < 0 || n == cur.parent) continue;
            const auto& np = polys[std::size_t(n)];
            if (np.state == 1 || (np.state == -1 && reff > 0)) continue;
            const int fx = p == sp ? sx : pp.cx, fy = p == sp ? sy : pp.cy;
            const int ng = cur.g + idist(np.cx - fx, np.cy - fy);
            const int nh = idist(tx - np.cx, ty - np.cy);
            Node& nn = nd[n];
            if (nn.stamp != ep) {  // first visit
                nn.stamp = ep;
                order.push_back(n);
            } else if (ng + nh >= nn.f) {
                continue;
            }
            // ponytail: improved closed nodes are re-opened instead of 0066ddb0's in-place subtree
            // propagation; identical routes with a consistent heuristic, may cost an extra expansion.
            nn.parent = p;
            nn.g = ng; nn.h = nh; nn.f = ng + nh;
            nn.closed = 0;
            push(n);
        }
    }

    NavPt dest = q.to;
    int end = goal;
    if (goal < 0) {
        int best = sp, bv = nd[sp].h + nd[sp].g / 16;
        for (int p : order) {
            const int v = nd[p].h + nd[p].g / 16;
            if (v < bv) { bv = v; best = p; }
        }
        end = best;
        const auto& bp = polys[std::size_t(best)];
        dest = {float(bp.cx), float(bp.cy), float(bp.cz)};
        // 0066df30: crossing of centroid->target with the best poly's edges, largest fraction wins.
        double bestf = -1;
        for (std::size_t e = 0; e < bp.v.size(); ++e) {
            const auto& a = mesh.pt(best, int(e));
            const auto& b = mesh.pt(best, int(e) + 1);
            const double dx = double(tx) - bp.cx, dy = double(ty) - bp.cy;
            const double ex = double(b.x) - a.x, ey = double(b.y) - a.y;
            const double den = dx * ey - dy * ex;
            if (den == 0) continue;
            const double wx = double(a.x) - bp.cx, wy = double(a.y) - bp.cy;
            const double s = (wx * ey - wy * ex) / den;   // along centroid->target
            const double u = (wx * dy - wy * dx) / den;   // along edge
            if (s < 0 || s > 1 || u < 0 || u > 1) continue;
            if (s > bestf) {
                bestf = s;
                dest = {float(bp.cx + s * dx), float(bp.cy + s * dy), float(a.z + u * (b.z - a.z))};
            }
        }
        res.status = PathResult::Partial;
    } else {
        res.status = PathResult::Full;
    }

    std::vector<Portal> portals;
    for (int p = end; nd[p].parent >= 0; p = nd[p].parent) {
        const auto& pp = polys[std::size_t(p)];
        for (std::size_t e = 0; e < pp.nb.size(); ++e)
            if (pp.nb[e] == nd[p].parent) {
                portals.push_back({P(mesh.pt(p, int(e))), P(mesh.pt(p, int(e) + 1))});
                break;
            }
    }
    std::reverse(portals.begin(), portals.end());
    res.points = smooth(q.from, portals, dest, q.radius);
    return res;
}

}  // namespace joa::game
