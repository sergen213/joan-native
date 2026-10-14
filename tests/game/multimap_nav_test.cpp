// Multi-node missions 2-8: NavWorld loads every level .pfd + the world mesh; cross-node paths are
// found and walkable; an AI MOVE_TO_TARGET order walks a unit over a node border in a running
// Mission (with level streaming keeping it active); a closed gate in a non-first node blocks its
// polygon; the crossing run is deterministic.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/campaign_runner.hpp"
#include "game/multimap_nav.hpp"
#include "game/orders_queue.hpp"

using namespace joa;
using namespace joa::game;

static int fails = 0;
#define CHECK(c, ...) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c), std::printf(__VA_ARGS__), std::printf("\n"), ++fails; \
    } while (0)

namespace {

NavPt centroid(const NavWorld& nav, int lv, int p) {
    const NavLevel& l = nav.levels()[std::size_t(lv)];
    const NavPoly& q = l.mesh.polys()[std::size_t(p)];
    const float x = float(q.cx) + l.ox, y = float(q.cy) + l.oy;
    return {x, y, nav.poly_height(l.base + p, x, y)};
}

// Fraction of 16-unit samples along from -> points that lie on an open poly.
float walkable_fraction(const NavWorld& nav, NavPt from, const std::vector<NavPt>& pts) {
    int on = 0, all = 0;
    NavPt a = from;
    for (const NavPt& b : pts) {
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        for (float s = 0; s < len; s += 16) {
            const float t = s / len, x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t;
            ++all;
            on += nav.find_poly_xy(x, y, nav.grid_height(x, y)) >= 0;
        }
        a = b;
    }
    return all ? float(on) / float(all) : 1.0f;
}

struct Cross {
    int a = -1, b = -1;
    NavPt pa, pb;
    PathResult path;
};

// Ground poly pairs across a shared border of two levels, nearest first, until the planner finds a
// route into level b: a full one ending at the target, or (world meshes made of border doorway
// polys only, S4L1w) a partial one whose end already lies in level b; the unit replans from there.
bool find_cross(const NavWorld& nav, Cross& out, int only_a = -1) {
    const auto& L = nav.levels();
    for (int a = 0; a < int(L.size()); ++a) {
        if (only_a >= 0 && a != only_a) continue;
        for (int b = 0; b < int(L.size()); ++b) {
            if (a == b) continue;
            const NavLevel &A = L[std::size_t(a)], &B = L[std::size_t(b)];
            const bool xb = A.x1 == B.ox && std::min(A.y1, B.y1) > std::max(A.oy, B.oy);
            const bool yb = A.y1 == B.oy && std::min(A.x1, B.x1) > std::max(A.ox, B.ox);
            if (!xb && !yb) continue;
            std::vector<std::pair<float, int>> cand;  // (distance to the border, poly)
            for (int p = 0; p < int(A.mesh.polys().size()); ++p) {
                const NavPoly& q = A.mesh.polys()[std::size_t(p)];
                if (q.type != 0 || q.state != 0) continue;
                const NavPt c = centroid(nav, a, p);
                const float d = xb ? A.x1 - c.x : A.y1 - c.y;
                if (d > 300 && d < 2500) cand.push_back({d, p});
            }
            std::sort(cand.begin(), cand.end());
            for (std::size_t i = 0; i < cand.size() && i < 40; ++i) {
                const NavPt pa = centroid(nav, a, cand[i].second);
                const NavPt guess{xb ? 2 * A.x1 - pa.x : pa.x, yb ? 2 * A.y1 - pa.y : pa.y, 0};
                const int pb = nav.find_poly_xy(guess.x, guess.y, nav.grid_height(guess.x, guess.y));
                if (pb < 0 || nav.level_of_poly(pb) != b || nav.poly(pb).type != 0) continue;
                PathQuery q;
                q.from = pa;
                q.to = centroid(nav, b, pb - B.base);
                q.max_expand = 2000;
                PathResult r = nav.find_path(q);
                if (r.points.empty() || nav.level_at(r.points.back().x, r.points.back().y) != b) continue;
                const NavPt& e = r.points.back();
                if (r.status == PathResult::Full && std::hypot(e.x - q.to.x, e.y - q.to.y) > 32) continue;
                out = {a, b, pa, q.to, std::move(r)};
                return true;
            }
        }
    }
    return false;
}

struct RunResult {
    bool crossed = false;
    int ticks = 0;
    int unloaded = 0;          // levels streamed out at the end (player kept at the border)
    bool streaming_ok = true;  // units in unloaded levels inactive, in loaded levels active
    std::uint64_t hash = 0;
    Vec3 pos;
    bool roundtrip_ok = false;  // save/load into a fresh mission keeps the hash (incl. NAVL)
};

// Mission `no`: player (if any; S9L1 has none for its first minutes) teleported next to the border,
// a fresh unit of the player's nation gets MOVE_TO_TARGET into the neighbour level.
RunResult run_crossing(GameData& gd, const formats::Vfs& vfs, int no, bool verbose, bool roundtrip = false) {
    RunResult res;
    CampaignRunner run(gd, vfs);
    std::string err;
    if (!run.init(&err) || !run.start(no, &err)) return std::printf("start %d: %s\n", no, err.c_str()), res;
    Mission& m = *run.mission();
    m.autosave = false;
    for (int t = 0; t < 5; ++t) m.tick();
    NavWorld& nav = *m.nav();
    World& w = m.world();
    Unit* pl = w.unit(w.player_id());
    Cross c;
    if (!pl || !find_cross(nav, c, nav.level_at(pl->pos.x, pl->pos.y))) find_cross(nav, c);
    if (c.a < 0) return std::printf("mission %d: no crossing pair\n", no), res;
    if (pl) pl->pos = {c.pa.x - 150, c.pa.y - 150, c.pa.z};
    const int id = w.spawn_unit("FRENCH_LIGHT_INFANTRY", pl ? pl->nation : 0, {c.pa.x, c.pa.y, c.pa.z}, 0);
    Unit* u = w.unit(id);
    if (!u) return std::printf("mission %d: spawn failed\n", no), res;
    u->ai_controlled = true;
    m.ai().give_order(*u, order::MOVE_TO_TARGET, 99, false, int(c.pb.x), int(c.pb.y), int(c.pb.z), 0, 120000);
    for (int t = 0; t < 2400; ++t) {
        m.tick();
        u = w.unit(id);
        if (!u || u->deleted) break;
        if (Unit* p = w.unit(w.player_id())) p->pos = {c.pa.x - 150, c.pa.y - 150, p->pos.z};  // keep both levels streamed in
        if (!res.crossed && nav.level_at(u->pos.x, u->pos.y) == c.b && u->m.active) res.crossed = true, res.ticks = t + 1;
        if (res.crossed && t + 1 >= res.ticks + 30) break;
    }
    nav.stream(w);  // settle units that changed level during the last tick
    for (const NavLevel& l : nav.levels()) res.unloaded += !l.loaded;
    for (const Unit& x : w.units()) {
        if (x.deleted) continue;
        const int lv = nav.level_at(x.pos.x, x.pos.y);
        if (lv >= 0 && bool(x.m.active) != nav.levels()[std::size_t(lv)].loaded) res.streaming_ok = false;
    }
    if (verbose)
        std::printf("  mission %d run: level %d -> %d, unit at (%.0f,%.0f) target (%.0f,%.0f), crossed=%d after %d ticks, "
                    "%d/%zu levels streamed out\n", no, c.a, c.b, u ? u->pos.x : 0.f, u ? u->pos.y : 0.f, c.pb.x, c.pb.y,
                    res.crossed, res.ticks, res.unloaded, nav.levels().size());
    res.hash = m.hash();
    if (u) res.pos = u->pos;
    if (roundtrip) {  // save -> load into a fresh start of the same mission -> 30 more ticks on both
        CampaignRunner run2(gd, vfs);
        if (!run2.init(&err) || !run2.start(no, &err)) return res;
        Mission& m2 = *run2.mission();
        m2.autosave = false;
        const bool loaded = m2.load(m.save(), &err);
        auto diff = [&](const char* when) {
            const SaveFile fa = m.save(), fb = m2.save();
            for (std::size_t i = 0; i < fa.sections.size() && i < fb.sections.size(); ++i)
                if (fa.sections[i].data != fb.sections[i].data)
                    std::printf("roundtrip %s: section %.4s differs\n", when, fa.sections[i].tag.data());
        };
        res.roundtrip_ok = loaded && m2.hash() == m.hash();
        diff("after load");
        for (int t = 0; t < 30; ++t) m.tick(), m2.tick();
        res.roundtrip_ok = res.roundtrip_ok && m2.hash() == m.hash();
        diff("after 30 ticks");
        if (!loaded) std::printf("roundtrip: %s\n", err.c_str());
    }
    return res;
}

}  // namespace

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);

    bool gate_tested = false;
    for (int no = 2; no <= 8; ++no) {
        CampaignRunner run(*gd, vfs);
        if (!run.init(&err) || !run.start(no, &err)) {
            CHECK(false, "start %d: %s", no, err.c_str());
            continue;
        }
        Mission& m = *run.mission();
        m.autosave = false;
        NavWorld* nav = m.nav();
        const auto nodes = world_nodes(vfs, m.map_dir(), m.info().step);
        CHECK(nav && nav->levels().size() == nodes.size() && nodes.size() > 1, "mission %d: %zu nodes", no, nodes.size());
        CHECK(nav && nav->world_mesh(), "mission %d: world mesh", no);
        if (!nav) continue;

        // Cross-node path between two levels, walkable along the way.
        Cross c;
        CHECK(find_cross(*nav, c), "mission %d: no cross-node path", no);
        if (c.a >= 0) {
            const float wf = walkable_fraction(*nav, c.pa, c.path.points);
            std::printf("%s (%zu levels): path level %d (%.0f,%.0f) -> level %d (%.0f,%.0f): %zu waypoints, walkable %.3f\n",
                        m.info().stem().c_str(), nav->levels().size(), c.a, c.pa.x, c.pa.y, c.b, c.pb.x, c.pb.y,
                        c.path.points.size(), wf);
            CHECK(wf >= 0.9f, "mission %d: path walkable %.3f", no, wf);
            PathQuery q;
            q.from = c.pa, q.to = c.pb, q.max_expand = 2000;
            const PathResult again = nav->find_path(q);
            CHECK(again.points.size() == c.path.points.size() && again.points.back().x == c.path.points.back().x, "deterministic path");
        }

        // A closed gate in a non-first node: its poly is blocked and paths around it avoid it.
        for (int i = 1; i <= m.objects().size() && !gate_tested; ++i) {
            const MapItem* g = m.objects().item(i);
            if (!g || g->cls != 1 || !g->blocking) continue;
            const int lv = nav->level_at(g->pos.x, g->pos.y);
            if (lv <= 0) continue;
            const NavLevel& L = nav->levels()[std::size_t(lv)];
            int gp = -1;
            for (int p = 0; p < int(L.mesh.polys().size()) && gp < 0; ++p)
                if (L.mesh.polys()[std::size_t(p)].state == 1 && L.mesh.contains(p, g->pos.x - L.ox, g->pos.y - L.oy, 0, true)) gp = p;
            if (gp < 0) continue;
            std::vector<int> nb;
            for (int n : L.mesh.polys()[std::size_t(gp)].nb)
                if (n >= 0 && L.mesh.polys()[std::size_t(n)].state != 1) nb.push_back(n);
            if (nb.size() < 2) continue;
            gate_tested = true;
            PathQuery q;
            q.from = centroid(*nav, lv, nb.front()), q.to = centroid(*nav, lv, nb.back()), q.max_expand = 2000;
            auto crosses = [&](const PathResult& r) {
                NavPt a = q.from;
                for (const NavPt& b : r.points) {
                    const float len = std::hypot(b.x - a.x, b.y - a.y);
                    for (float s = 0; s <= len; s += 4) {
                        const float t = len > 0 ? s / len : 0;
                        if (L.mesh.contains(gp, a.x + (b.x - a.x) * t - L.ox, a.y + (b.y - a.y) * t - L.oy, 0, true)) return true;
                    }
                    a = b;
                }
                return false;
            };
            const PathResult closed = nav->find_path(q);
            CHECK(nav->find_poly_xy(g->pos.x, g->pos.y, g->pos.z) != L.base + gp, "closed gate poly is not walkable");
            CHECK(!crosses(closed), "mission %d: path crosses the closed gate (level %d poly %d)", no, lv, gp);
            nav->set_blocked(L.base + gp, false);
            const PathResult open = nav->find_path(q);
            CHECK(open.status == PathResult::Full && crosses(open), "mission %d: open gate path should pass the gate", no);
            nav->set_blocked(L.base + gp, true);
            std::printf("  gate item %d in %s level %d poly %d: closed path %zu wp (crosses %d), open %zu wp (crosses %d)\n", i,
                        m.info().stem().c_str(), lv, gp, closed.points.size(), crosses(closed), open.points.size(), crosses(open));
        }
    }
    CHECK(gate_tested, "no closed gate in a non-first node was found");

    // AI MOVE order crosses a node border in a running mission, for every multi-node mission.
    for (int no = 2; no <= 8; ++no) {
        const RunResult r = run_crossing(*gd, vfs, no, true);
        CHECK(r.crossed, "mission %d: unit did not cross into the neighbour level", no);
        CHECK(r.streaming_ok, "mission %d: unit active flag disagrees with its level's load state", no);
        if (no == 5) CHECK(r.unloaded > 0, "S5L1: far levels should be streamed out");
    }
    // Determinism of the crossing run (S5L1: 10 levels, streaming) and the save/load round trip.
    const RunResult a = run_crossing(*gd, vfs, 5, false, true), b = run_crossing(*gd, vfs, 5, false);
    CHECK(a.hash == b.hash && a.pos.x == b.pos.x && a.pos.y == b.pos.y, "crossing run not deterministic");
    CHECK(a.roundtrip_ok, "save/load round trip of a multi-node mission changed the hash");
    std::printf("determinism: %016llx %s %016llx, round trip %s\n", (unsigned long long)a.hash, a.hash == b.hash ? "==" : "!=",
                (unsigned long long)b.hash, a.roundtrip_ok ? "ok" : "FAILED");

    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
