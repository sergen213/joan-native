// find_path's reused search state (epoch stamps, heap storage) and nearest_poly's bucket-ring scan on synthetic
// meshes (no game data): a query gives the same path whatever ran before it (other queries, a smaller / larger
// mesh, a gate toggle in between), and nearest_poly equals the whole-mesh scan of 0066c710.
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "formats/pfd_mesh.hpp"
#include "game/nav_mesh.hpp"
#include "game/path_find.hpp"

using namespace joa;
using namespace joa::game;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

namespace {

std::uint32_t rng = 12345;
int rnd(int n) { rng = rng * 1103515245u + 12345u; return int((rng >> 8) % std::uint32_t(n)); }

// cols x rows rectangles of varying width / height (some < 170: narrow polys), ~1 in 8 cells left out (holes),
// starting at (ox, oy) (negative: polys outside every bucket).
formats::PfdMesh grid_mesh(int cols, int rows, int ox, int oy) {
    formats::PfdMesh m;
    std::vector<int> xs{ox}, ys{oy};
    for (int i = 0; i < cols; ++i) xs.push_back(xs.back() + 120 + rnd(300));
    for (int i = 0; i < rows; ++i) ys.push_back(ys.back() + 120 + rnd(300));
    for (int y = 0; y <= rows; ++y)
        for (int x = 0; x <= cols; ++x) m.points.push_back({xs[std::size_t(x)], ys[std::size_t(y)], (x * 7 + y * 3) % 40});
    std::vector<int> id(std::size_t(cols * rows), -1);
    auto P = [&](int x, int y) { return y * (cols + 1) + x; };
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            if (rnd(8) == 0 && x + y > 1) continue;
            id[std::size_t(y * cols + x)] = int(m.polys.size());
            formats::PfdPoly p;
            p.verts = {P(x, y), P(x + 1, y), P(x + 1, y + 1), P(x, y + 1)};  // edges: -y, +x, +y, -x
            m.polys.push_back(p);
        }
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            const int a = id[std::size_t(y * cols + x)];
            if (a < 0) continue;
            if (x + 1 < cols && id[std::size_t(y * cols + x + 1)] >= 0) m.links.push_back({a + 1, 2, id[std::size_t(y * cols + x + 1)] + 1, 4});
            if (y + 1 < rows && id[std::size_t((y + 1) * cols + x)] >= 0) m.links.push_back({a + 1, 3, id[std::size_t((y + 1) * cols + x)] + 1, 1});
        }
    return m;
}

std::string key(const PathResult& r) {
    std::string s = std::to_string(int(r.status)) + "/" + std::to_string(r.expanded);
    for (const NavPt& p : r.points) {
        char b[96];
        std::snprintf(b, sizeof b, " %a,%a,%a", double(p.x), double(p.y), double(p.z));
        s += b;
    }
    return s;
}

int nearest_ref(const NavMesh& m, float x, float y, float z) {  // 0066c710 as the whole-mesh scan
    int best = -1;
    double bd = 0, bc = 0;
    for (std::size_t i = 0; i < m.polys().size(); ++i) {
        const auto& p = m.polys()[i];
        if (p.state == 1) continue;
        const float q[3] = {x, y, z};
        double d = 0;
        for (int k = 0; k < 3; ++k) {
            const double e = q[k] < p.bbox[k] ? p.bbox[k] - q[k] : q[k] > p.bbox[k + 3] ? q[k] - p.bbox[k + 3] : 0;
            d += e * e;
        }
        const double c = (double(x) - p.cx) * (x - p.cx) + (double(y) - p.cy) * (y - p.cy);
        if (best < 0 || d < bd || (d == bd && c < bc)) { best = int(i); bd = d; bc = c; }
    }
    return best;
}

std::vector<PathQuery> queries(const NavMesh& m, int n) {
    std::vector<PathQuery> v;
    const int x0 = int(m.min_x()) - 600, y0 = int(m.min_y()) - 600;
    const int w = int(m.max_x() - m.min_x()) + 1200, h = int(m.max_y() - m.min_y()) + 1200;
    for (int i = 0; i < n; ++i) {
        PathQuery q;
        q.from = {float(x0 + rnd(w)), float(y0 + rnd(h)), float(rnd(60))};
        q.to = {float(x0 + rnd(w)), float(y0 + rnd(h)), float(rnd(60))};
        q.radius = rnd(3) == 0 ? 250.0f : 64.0f;  // >= 200 avoids narrow polys
        q.max_expand = rnd(4) == 0 ? 15 : 500;    // small budgets: partial paths
        v.push_back(q);
    }
    return v;
}

}  // namespace

int main() {
    const NavMesh big(grid_mesh(40, 30, -1500, -900));
    const NavMesh small(grid_mesh(6, 5, 100, 100));
    const auto qb = queries(big, 300), qs = queries(small, 100);

    std::vector<std::string> first;
    int full = 0, partial = 0;
    for (const auto& q : qb) {
        const PathResult r = find_path(big, q);
        full += r.status == PathResult::Full, partial += r.status == PathResult::Partial;
        first.push_back(key(r));
    }
    std::printf("big: %zu polys, %d full, %d partial of %zu\n", big.polys().size(), full, partial, qb.size());
    CHECK(full > 20 && partial > 20, "queries cover full and partial paths");
    std::vector<std::string> sfirst;
    for (const auto& q : qs) sfirst.push_back(key(find_path(small, q)));

    // Same queries backwards, each after a small-mesh query and a repeat of its predecessor.
    for (std::size_t k = qb.size(); k-- > 0;) {
        CHECK(key(find_path(small, qs[k % qs.size()])) == sfirst[k % qs.size()], "small mesh query " + std::to_string(k));
        CHECK(key(find_path(big, qb[k])) == first[k], "big mesh query " + std::to_string(k));
        CHECK(key(find_path(big, qb[k])) == first[k], "big mesh query repeated " + std::to_string(k));
    }

    // A gate toggle changes the mesh between searches: the closed-then-reopened mesh gives the first answers again.
    NavMesh gated(grid_mesh(40, 30, -1500, -900));
    std::vector<std::string> g0;
    for (const auto& q : qb) g0.push_back(key(find_path(gated, q)));
    std::vector<int> gates;  // open polys only: reopening sets state 0, which would drop a narrow flag
    for (int p = 0; p < int(gated.polys().size()); p += 37)
        if (gated.polys()[std::size_t(p)].state == 0) gates.push_back(p);
    for (int p : gates) gated.set_blocked(p, true);
    int changed = 0;
    for (std::size_t k = 0; k < qb.size(); ++k) changed += key(find_path(gated, qb[k])) != g0[k];
    CHECK(changed > 0, "closed gates change some paths");
    for (int p : gates) gated.set_blocked(p, false);
    for (std::size_t k = 0; k < qb.size(); ++k) CHECK(key(find_path(gated, qb[k])) == g0[k], "after gate toggle " + std::to_string(k));

    // nearest_poly == the whole-mesh scan, inside, around and far outside the mesh, also with blocked polys.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < 4000; ++i) {
            const float x = float(rnd(30000) - 9000), y = float(rnd(24000) - 7000), z = float(rnd(400) - 200);
            CHECK(gated.nearest_poly(x, y, z) == nearest_ref(gated, x, y, z), "nearest_poly " + std::to_string(i));
        }
        for (int p = 0; p < int(gated.polys().size()); p += 5) gated.set_blocked(p, true);
    }
    // Clearance contains (reused `seen`): the same answers in reverse order.
    struct C { int p; float x, y, r; bool in; };
    std::vector<C> cs;
    int inside = 0;
    for (int i = 0; i < 3000; ++i) {
        C c{rnd(int(small.polys().size())), float(100 + rnd(2000)), float(100 + rnd(2000)), float(rnd(300)), false};
        c.in = small.contains(c.p, c.x, c.y, c.r);
        inside += c.in;
        cs.push_back(c);
    }
    CHECK(inside > 10, "some clearance hits");
    for (std::size_t i = cs.size(); i-- > 0;) CHECK(small.contains(cs[i].p, cs[i].x, cs[i].y, cs[i].r) == cs[i].in, "contains " + std::to_string(i));
    std::printf("ok\n");
    return 0;
}
