// Navmesh runtime on every .pfd + paths on S1L1 (A* 100-node cap, partial paths, smoothing).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "formats/map_grid.hpp"
#include "formats/pfd_mesh.hpp"
#include "formats/vfs_fs.hpp"
#include "game/nav_mesh.hpp"
#include "game/path_find.hpp"

using namespace joa;
using namespace joa::game;
namespace fs = std::filesystem;

#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed: %s\n", __FILE__, __LINE__, #c, std::string(what).c_str()); return 1; } } while (0)

static bool walkable_segment(const NavMesh& m, NavPt a, NavPt b) {
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    for (float s = 0; s <= len; s += 8) {
        const float t = len > 0 ? s / len : 0;
        if (m.find_poly_xy(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)) < 0) return false;
    }
    return true;
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "joan.exe")) return 77;
    formats::Vfs vfs(dir);
    for (const auto& path : vfs.find_by_extension(".pfd")) {
        auto pfd = formats::parse_pfd(*formats::read_file(path));
        CHECK(pfd, path.string());
        NavMesh m(*pfd);
        // Every centroid of a convex poly lies inside it (also proves the cross<=0 winding rule).
        int inside = 0;
        for (std::size_t i = 0; i < m.polys().size(); ++i)
            inside += m.contains(int(i), float(m.polys()[i].cx), float(m.polys()[i].cy));
        CHECK(inside >= int(m.polys().size()) * 95 / 100, path.string() + ": centroids inside");
    }

    auto bytes = formats::read_file(fs::path(dir) / "map/scenario1/S1L1.pfd");
    CHECK(bytes, "S1L1.pfd");
    NavMesh m(*formats::parse_pfd(*bytes));
    {
        auto por = formats::read_file(fs::path(dir) / "map/scenario1/S1L1.por");
        CHECK(por, "S1L1.por");
        m.add_los_quads_from_por(std::string(por->begin(), por->end()));
        CHECK(m.los_quads().size() == 9, "9 anti-portals");
        // Quad 0 spans x 4244..6609 at y~8925, z -508..288: a horizontal sight line through it is blocked.
        CHECK(m.segment_blocked({5400, 8800, 0}, {5400, 9050, 0}, 0), "LOS through anti-portal");
        CHECK(!m.segment_blocked({5400, 8800, 0}, {5400, 8900, 0}, 0), "LOS short of anti-portal");
    }
    // Ground polys: navmesh height grid == .GRI heights (same data, both triangulated/bilinear in a cell).
    auto gri = formats::parse_gri(*formats::read_file(fs::path(dir) / "map/scenario1/S1L1.gri"));
    CHECK(gri, "S1L1.gri");
    int narrow = 0;
    for (std::size_t i = 0; i < m.polys().size(); ++i) {
        const auto& p = m.polys()[i];
        narrow += p.state == -1;
        if (p.type != 0) continue;
        CHECK(std::fabs(m.poly_height(int(i), float(p.cx), float(p.cy)) - gri->height_at(float(p.cx), float(p.cy))) <= 8.0f,
              "ground height == GRI height, poly " + std::to_string(i));
        CHECK(m.find_poly(float(p.cx), float(p.cy), m.poly_height(int(i), float(p.cx), float(p.cy))) >= 0, "find_poly");
    }
    std::printf("S1L1: %zu polys, %d narrow\n", m.polys().size(), narrow);

    // Two far-apart ground polys in the largest component.
    std::vector<int> csize(m.polys().size(), 0);
    for (auto& p : m.polys()) if (p.component >= 0) ++csize[std::size_t(p.component)];
    const int big = int(std::max_element(csize.begin(), csize.end()) - csize.begin());
    int a = -1, b = -1;
    for (std::size_t i = 0; i < m.polys().size(); ++i) {
        const auto& p = m.polys()[i];
        if (p.component != big || p.type != 0 || p.state != 0) continue;
        if (a < 0 || p.cx + p.cy < m.polys()[std::size_t(a)].cx + m.polys()[std::size_t(a)].cy) a = int(i);
        if (b < 0 || p.cx + p.cy > m.polys()[std::size_t(b)].cx + m.polys()[std::size_t(b)].cy) b = int(i);
    }
    CHECK(a >= 0 && b >= 0 && a != b, "endpoints");
    const auto& pa = m.polys()[std::size_t(a)];
    const auto& pb = m.polys()[std::size_t(b)];
    PathQuery q;
    q.from = {float(pa.cx), float(pa.cy), m.poly_height(a, float(pa.cx), float(pa.cy))};
    q.to = {float(pb.cx), float(pb.cy), m.poly_height(b, float(pb.cx), float(pb.cy))};
    q.radius = 32;  // units pass BODY_R*2=64; 0 lets exact-corner samples flicker on hole edges
    q.start_poly = a;
    q.max_expand = 100000;
    auto full = find_path(m, q);
    CHECK(full.status == PathResult::Full, "unbounded search reaches target");
    CHECK(std::fabs(full.points.back().x - q.to.x) < 1 && std::fabs(full.points.back().y - q.to.y) < 1, "ends at target");
    NavPt prev = q.from;
    double len = 0;
    for (auto& w : full.points) {
        CHECK(walkable_segment(m, prev, w), "smoothed segment stays on the mesh");
        len += std::hypot(w.x - prev.x, w.y - prev.y);
        prev = w;
    }
    std::printf("full path: %zu waypoints, %d expanded, length %.0f (straight %.0f)\n", full.points.size(),
                full.expanded, len, std::hypot(q.to.x - q.from.x, q.to.y - q.from.y));

    q.max_expand = 100;  // the original budget
    auto part = find_path(m, q);
    CHECK(part.expanded <= 100, "100-node cap");
    if (full.expanded > 100) {
        CHECK(part.status == PathResult::Partial, "capped search is partial");
        const auto& e = part.points.back();
        CHECK(std::hypot(e.x - q.to.x, e.y - q.to.y) < std::hypot(q.from.x - q.to.x, q.from.y - q.to.y),
              "partial dest is closer to target");
    }
    // Determinism.
    auto again = find_path(m, q);
    CHECK(again.points.size() == part.points.size() && again.points.back().x == part.points.back().x, "deterministic");
    return 0;
}
