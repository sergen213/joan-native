#include "game/nav_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "formats/world_text.hpp"

namespace joa::game {

namespace {
float seg_dist2(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = ax + t * dx - px, ey = ay + t * dy - py;
    return ex * ex + ey * ey;
}
NavPt sub(NavPt a, NavPt b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
NavPt cross(NavPt a, NavPt b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(NavPt a, NavPt b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
}  // namespace

NavMesh::NavMesh(const formats::PfdMesh& m) : pts_(m.points), grid_(m.grid) {
    // Ground-poly vertices are stored with z = 0; give them the grid height so bboxes, centroids and
    // portal z are real (HYPOTHESIS: the engine's z-tolerant lookups need it too). Walkway verts keep z.
    if (grid_) {
        std::vector<char> walkway(pts_.size(), 0);
        for (const auto& s : m.polys)
            if (s.type != 0) for (int vi : s.verts) walkway[std::size_t(vi)] = 1;
        for (std::size_t i = 0; i < pts_.size(); ++i)
            if (!walkway[i]) pts_[i].z = int(grid_height(float(pts_[i].x), float(pts_[i].y)));
    }
    polys_.resize(m.polys.size());
    for (std::size_t i = 0; i < m.polys.size(); ++i) {
        const auto& s = m.polys[i];
        auto& p = polys_[i];
        p.type = s.type;
        p.model = s.model;
        p.v = s.verts;
        p.nb.assign(p.v.size(), -1);
        long long sx = 0, sy = 0, sz = 0;
        p.bbox[0] = p.bbox[1] = p.bbox[2] = std::numeric_limits<int>::max();
        p.bbox[3] = p.bbox[4] = p.bbox[5] = std::numeric_limits<int>::min();
        for (int vi : p.v) {
            const auto& q = pts_[std::size_t(vi)];
            const int c[3] = {q.x, q.y, q.z};
            for (int k = 0; k < 3; ++k) {
                p.bbox[k] = std::min(p.bbox[k], c[k]);
                p.bbox[k + 3] = std::max(p.bbox[k + 3], c[k]);
            }
            sx += q.x; sy += q.y; sz += q.z;
        }
        const long long n = (long long)p.v.size();
        p.cx = int(sx / n); p.cy = int(sy / n); p.cz = int(sz / n);
    }
    for (const auto& l : m.links) {  // FUN_00664130
        polys_[std::size_t(l.poly_a - 1)].nb[std::size_t(l.edge_a - 1)] = l.poly_b - 1;
        if (l.edge_b != 0) polys_[std::size_t(l.poly_b - 1)].nb[std::size_t(l.edge_b - 1)] = l.poly_a - 1;
    }
    for (std::size_t i = 0; i < polys_.size(); ++i) {  // narrow flag (00671364..00671545)
        auto& p = polys_[i];
        if (p.state == 1) continue;
        double sum = 0;
        int linked = 0;
        for (std::size_t e = 0; e < p.v.size(); ++e) {
            if (p.nb[e] < 0) continue;
            const auto& a = pt(int(i), int(e));
            const auto& b = pt(int(i), int(e) + 1);
            sum += std::sqrt(double(b.x - a.x) * (b.x - a.x) + double(b.y - a.y) * (b.y - a.y));
            ++linked;
        }
        if (sum < double(linked) * kNarrowEdgeLen) p.state = -1;
    }
    if (grid_) {
        border_[0] = float(grid_->x0);
        border_[1] = float(grid_->y0);
        border_[2] = float(grid_->x0 + (grid_->cols - 1) * grid_->cell_w);
        border_[3] = float(grid_->y0 + (grid_->rows - 1) * grid_->cell_h);
    } else if (!pts_.empty()) {
        border_[0] = border_[1] = 1e30f;
        border_[2] = border_[3] = -1e30f;
        for (auto& q : pts_) {
            border_[0] = std::min(border_[0], float(q.x)); border_[1] = std::min(border_[1], float(q.y));
            border_[2] = std::max(border_[2], float(q.x)); border_[3] = std::max(border_[3], float(q.y));
        }
    }
    // Buckets cover the poly bboxes (which can extend past the grid border on world meshes).
    int maxx = 0, maxy = 0;
    for (auto& p : polys_) { maxx = std::max(maxx, p.bbox[3]); maxy = std::max(maxy, p.bbox[4]); }
    bcols_ = maxx / kBucket + 1;
    brows_ = maxy / kBucket + 1;
    buckets_.resize(std::size_t(bcols_) * brows_);
    for (std::size_t i = 0; i < polys_.size(); ++i) {
        const auto& b = polys_[i].bbox;
        bool listed = false;
        for (int by = std::max(0, b[1] / kBucket); by <= std::min(brows_ - 1, b[4] / kBucket); ++by)
            for (int bx = std::max(0, b[0] / kBucket); bx <= std::min(bcols_ - 1, b[3] / kBucket); ++bx)
                buckets_[std::size_t(by) * bcols_ + bx].push_back(int(i)), listed = true;
        if (!listed) unbucketed_.push_back(int(i));  // bbox wholly at x or y <= -512
    }
    compute_components();
}

template <class F>
void NavMesh::for_candidates(float x, float y, F&& f) const {
    const int bx = int(std::floor(x)) / kBucket, by = int(std::floor(y)) / kBucket;
    if (x < 0 || y < 0 || bx >= bcols_ || by >= brows_) return;
    for (int i : buckets_[std::size_t(by) * bcols_ + bx]) f(i);
}

bool NavMesh::is_border_edge(const formats::PfdPoint& a, const formats::PfdPoint& b) const {
    if (a.x == b.x) return float(a.x) == border_[0] || float(a.x) == border_[2];
    if (a.y == b.y) return float(a.y) == border_[1] || float(a.y) == border_[3];
    return false;
}

bool NavMesh::clearance_ok(int poly, float x, float y, float r, std::vector<int>& seen) const {
    seen.push_back(poly);
    const auto& p = polys_[std::size_t(poly)];
    for (std::size_t e = 0; e < p.v.size(); ++e) {
        const auto& a = pt(poly, int(e));
        const auto& b = pt(poly, int(e) + 1);
        if (seg_dist2(x, y, float(a.x), float(a.y), float(b.x), float(b.y)) >= r * r) continue;
        const int n = p.nb[e];
        if (n < 0 || polys_[std::size_t(n)].state == 1) {
            if (!is_border_edge(a, b)) return false;  // wall within radius
        } else if (std::find(seen.begin(), seen.end(), n) == seen.end() && !clearance_ok(n, x, y, r, seen)) {
            return false;
        }
    }
    return true;
}

bool NavMesh::contains(int poly, float x, float y, float radius, bool ignore_blocked) const {
    const auto& p = polys_[std::size_t(poly)];
    if (p.state == 1 && !ignore_blocked) return false;
    if (x < p.bbox[0] || x > p.bbox[3] || y < p.bbox[1] || y > p.bbox[4]) return false;
    for (std::size_t e = 0; e < p.v.size(); ++e) {
        const auto& a = pt(poly, int(e));
        const auto& b = pt(poly, int(e) + 1);
        const double c = (double(x) - a.x) * (b.y - a.y) - (double(y) - a.y) * (b.x - a.x);
        if (c > 0) return false;
    }
    if (radius > 0) {
        thread_local std::vector<int> seen;  // reused: find_poly(radius) runs this per candidate, per path search
        seen.clear();
        return clearance_ok(poly, x, y, radius, seen);
    }
    return true;
}

int NavMesh::find_poly(float x, float y, float z, float radius) const {
    int best = -1;
    float bestd = 0;
    for_candidates(x, y, [&](int i) {
        const auto& p = polys_[std::size_t(i)];
        const auto& b = p.bbox;
        if (x + 1 < b[0] || x - 1 > b[3] || y + 1 < b[1] || y - 1 > b[4]) return;
        if (p.type != 0 && (z + 17 < b[2] || z - 17 > b[5])) return;  // ground: terrain may bulge past vertex z
        if (!contains(i, x, y, radius)) return;
        const float d = std::fabs(float(int(poly_height(i, x, y))) - z);
        if (d <= 16 && (best < 0 || d < bestd)) { best = i; bestd = d; }
    });
    return best;
}

int NavMesh::find_poly_xy(float x, float y, float z, bool ignore_blocked) const {
    int best = -1;
    float bestd = 0;
    for_candidates(x, y, [&](int i) {
        if (!contains(i, x, y, 0, ignore_blocked)) return;
        const float d = std::fabs(poly_height(i, x, y) - z);
        if (best < 0 || d < bestd) { best = i; bestd = d; }
    });
    return best;
}

int NavMesh::nearest_poly(float x, float y, float z) const {
    // The ascending scan keeping the first of equal (d, c) picks the lexicographic min of (d, c, index), so any visit
    // order gives the same poly. ponytail: visit bucket rings around (x,y) and stop once the next ring lies farther
    // than the best d (a poly's nearest bbox point lies in a bucket that lists it), not the whole-mesh scan.
    int best = -1;
    double bd = 0, bc = 0;
    auto consider = [&](int i) {
        const auto& p = polys_[std::size_t(i)];
        if (p.state == 1) return;
        const float q[3] = {x, y, z};
        double d = 0;
        for (int k = 0; k < 3; ++k) {
            const double e = q[k] < p.bbox[k] ? p.bbox[k] - q[k] : q[k] > p.bbox[k + 3] ? q[k] - p.bbox[k + 3] : 0;
            d += e * e;
        }
        const double c = (double(x) - p.cx) * (x - p.cx) + (double(y) - p.cy) * (y - p.cy);
        if (best < 0 || d < bd || (d == bd && (c < bc || (c == bc && i < best)))) { best = i; bd = d; bc = c; }
    };
    if (std::isnan(x) || std::isnan(y)) {  // NaN centroid distances never tie-break: ascending scan as before
        for (int i = 0; i < int(polys_.size()); ++i) consider(i);
        return best;
    }
    for (int i : unbucketed_) consider(i);
    auto cell = [](float v, int n) { return v < 0 ? 0 : int(std::min<double>(std::floor(v) / kBucket, n - 1)); };
    const int cx = cell(x, bcols_), cy = cell(y, brows_);
    const int maxk = std::max(std::max(cx, bcols_ - 1 - cx), std::max(cy, brows_ - 1 - cy));
    for (int k = 0; k <= maxk; ++k) {
        // Every bucket of ring k is >= (k - 1) * kBucket from (x,y) in x or y (q lies in, or clamped outward of, (cx,cy)).
        const double lb = double(k - 1) * kBucket - 1;
        if (best >= 0 && lb > 0 && lb * lb > bd) break;
        auto visit = [&](int bx, int by) { for (int i : buckets_[std::size_t(by) * bcols_ + bx]) consider(i); };
        for (int by = std::max(0, cy - k); by <= std::min(brows_ - 1, cy + k); ++by) {
            if (by == cy - k || by == cy + k) {
                for (int bx = std::max(0, cx - k); bx <= std::min(bcols_ - 1, cx + k); ++bx) visit(bx, by);
            } else {
                if (cx - k >= 0) visit(cx - k, by);
                if (cx + k < bcols_) visit(cx + k, by);
            }
        }
    }
    return best;
}

float NavMesh::grid_height(float x, float y) const {
    if (!grid_) return 0.0f;
    const auto& g = *grid_;
    const float gx = (x - float(g.x0)) / float(g.cell_w), gy = (y - float(g.y0)) / float(g.cell_h);
    int ix = int(gx), iy = int(gy);  // ftol truncation
    const bool diag = ((ix + iy) & 1) == 0;
    ix = std::clamp(ix, 0, g.cols - 2);
    iy = std::clamp(iy, 0, g.rows - 2);
    const float fx = gx - float(ix), fy = gy - float(iy);
    auto H = [&](int cx, int cy) { return float(g.h[std::size_t(cy) * g.cols + cx]); };
    const float h00 = H(ix, iy), h10 = H(ix + 1, iy), h01 = H(ix, iy + 1), h11 = H(ix + 1, iy + 1);
    if (!diag) {
        return fx + fy > 1 ? h11 + (1 - fx) * (h01 - h11) + (1 - fy) * (h10 - h11)
                           : h00 + fx * (h10 - h00) + fy * (h01 - h00);
    }
    return fy - fx > 0 ? h01 + fx * (h11 - h01) + (1 - fy) * (h00 - h01)
                       : h10 + fy * (h11 - h10) + (1 - fx) * (h00 - h10);
}

float NavMesh::poly_height(int poly, float x, float y) const {
    const auto& p = polys_[std::size_t(poly)];
    if (p.type == 0 && grid_) return grid_height(x, y);
    const auto& a = pt(poly, 0);
    const auto& b = pt(poly, 1);
    const auto& c = pt(poly, 2);
    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    if (nz == 0) return float(a.z);
    return float(a.z - (nx * (x - a.x) + ny * (y - a.y)) / nz);
}

float NavMesh::ground_height(float x, float y, float z) const {
    const int p = find_poly_xy(x, y, z);
    return p >= 0 ? poly_height(p, x, y) : 0.0f;
}

void NavMesh::set_blocked(int poly, bool blocked) {
    polys_[std::size_t(poly)].state = blocked ? 1 : 0;
    compute_components();
}

void NavMesh::compute_components() {
    for (auto& p : polys_) p.component = -1;
    int next = 0;
    std::vector<int> stack;
    for (std::size_t i = 0; i < polys_.size(); ++i) {
        if (polys_[i].component >= 0 || polys_[i].state == 1) continue;
        polys_[i].component = next;
        stack.push_back(int(i));
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            for (int n : polys_[std::size_t(c)].nb) {
                if (n < 0 || polys_[std::size_t(n)].state == 1 || polys_[std::size_t(n)].component >= 0) continue;
                polys_[std::size_t(n)].component = next;
                stack.push_back(n);
            }
        }
        ++next;
    }
}

bool NavMesh::same_component(int a, int b) const {
    return a >= 0 && b >= 0 && polys_[std::size_t(a)].component >= 0 &&
           polys_[std::size_t(a)].component == polys_[std::size_t(b)].component;
}

void NavMesh::add_los_quads_from_por(const std::string& por_text) {
    auto bt = formats::parse_block_text(por_text);
    if (!bt) return;
    for (const auto& blk : bt->blocks) {
        std::vector<float> nums;
        for (const auto& f : blk.fields) {
            if (!f.key.empty()) continue;
            const std::string s = f.value;
            const char* c = s.c_str();
            while (*c) {
                if ((*c >= '0' && *c <= '9') || *c == '-' || *c == '.') {
                    char* end = nullptr;
                    nums.push_back(std::strtof(c, &end));
                    c = end > c ? end : c + 1;
                } else {
                    ++c;
                }
            }
        }
        if (nums.size() < 12) continue;
        auto P = [&](int i) { return NavPt{nums[std::size_t(i) * 3], nums[std::size_t(i) * 3 + 1], nums[std::size_t(i) * 3 + 2]}; };
        LosQuad q;
        q.v[0] = P(0); q.v[1] = P(1); q.v[2] = P(3); q.v[3] = P(2);  // 005593e0: 2nd pair swapped
        q.n = cross(sub(q.v[1], q.v[0]), sub(q.v[2], q.v[1]));
        q.d = -dot(q.n, q.v[0]);
        quads_.push_back(q);
    }
}

bool NavMesh::segment_blocked(NavPt a, NavPt b, float eye_z) const {
    a.z += eye_z;
    const NavPt d = sub(b, a);
    for (const auto& q : quads_) {
        if (!q.enabled) continue;
        const float den = dot(q.n, d);
        if (den == 0) continue;
        const float t = -(dot(q.n, a) + q.d) / den;
        if (!(t > 0 && t < 1)) continue;
        const NavPt p{a.x + t * d.x, a.y + t * d.y, a.z + t * d.z};
        bool inside = true;
        for (int k = 0; k < 4 && inside; ++k)
            inside = dot(cross(sub(q.v[(k + 1) & 3], q.v[k]), sub(p, q.v[k])), q.n) >= 0;
        if (inside) return true;
    }
    return false;
}

}  // namespace joa::game
