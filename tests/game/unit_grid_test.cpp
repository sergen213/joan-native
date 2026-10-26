// Unit grid (World::units_in) and the scans built on it: the candidates cover every unit a full scan accepts, in
// recno order; auto_join / search_for_unit with a grid window open match the full scan (results, RNG, hashes);
// find_move's index matches the reverse table scan. Synthetic game data (no JOA_GAME_DIR needed).
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string>

#include "game/ai_routines.hpp"
#include "game/ai_system.hpp"
#include "sim_fixture.hpp"

using namespace joa::game;

namespace {

GameData make_data() {
    GameData gd;
    gd.items.resize(1);
    auto cls = [&](const char* code, int unit_class, int ranking, unsigned mask, int special = 0) {
        UnitClass c;
        c.code = code;
        c.unit_class = unit_class, c.ranking = ranking, c.trigger_mask = mask, c.special = special;
        c.hp = 100, c.ep = 50, c.life = 0, c.level = 1, c.height = 80, c.lead = 30;
        gd.units.push_back(c);
    };
    cls("INF", 1, 1, 0xffff);
    cls("ARCHER", 5, 1, 0xffff);
    cls("CAPTAIN", 1, 3, 0xffff);
    cls("CITIZEN", 0, 0, 0xffff);
    cls("CANNON", 8, 0, 0x40);
    cls("HORSE", 1, 0, 0xffff, 2);
    cls("MOVER", 1, 1, 0xffff);   // move-table class (find_move test)
    return gd;
}

const char* kClasses[] = {"INF", "ARCHER", "CAPTAIN", "CITIZEN", "CANNON", "HORSE"};

// Same units in both worlds (one RNG stream each, same seed).
void populate(World& w, AiSystem& ai, unsigned seed, int n) {
    std::mt19937 g(seed);
    auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); };
    auto pick = [&](int k) { return int(g() % unsigned(k)); };
    ai.places[1] = AiPlace{{0, 0, 0}, {-1500, -1500, -1000}, {500, 500, 1000}, {}, {}, {}, 0};
    for (int i = 0; i < n; ++i) {
        Vec3 p;
        if (pick(3) == 0) p = {uni(-300, 300), uni(-300, 300), 0};   // dense cluster
        else p = {uni(-6000, 6000), uni(-6000, 6000), 0};
        const int id = w.spawn_unit(kClasses[pick(6)], pick(4), p, uni(0, 360));
        Unit& u = *w.unit(id);
        u.pos.z = uni(-50, 50);
        if (pick(50) == 0) u.pos = {std::numeric_limits<float>::quiet_NaN(), 0, 0};   // NaN passes every radius test
        if (pick(80) == 0) u.pos.x = std::numeric_limits<float>::infinity();
        if (pick(10) == 0) u.hp = 0;
        if (pick(20) == 0) u.retreat = -1;
        if (pick(5) == 0) u.team = 9999;
        if (pick(15) == 0) u.m.training = 1;
        u.attack_behaviour = 1 + pick(5);
    }
    for (auto& u : w.units()) ai.st(u);   // settles every unit's AI state (leaders create their team)
    for (auto& u : w.units())             // a few troops for centre_on_leader
        if (!u.deleted && ai.is_leader(u))
            for (int k = 0; k < 3; ++k) {
                Unit& m = w.units()[std::size_t(pick(int(w.units().size())))];
                if (!m.leader && !ai.is_leader(m) && m.nation == u.nation) ai.join_team(m, u.id);
            }
    for (int i = 0; i < n / 25; ++i) w.delete_unit(1 + pick(n));
}

void grid_vs_brute(World& w, std::mt19937& g) {
    std::vector<int> got;
    auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); };
    w.grid_open(true);
    for (int q = 0; q < 3000; ++q) {
        const float x = uni(-7000, 7000), y = uni(-7000, 7000), r = q % 7 == 0 ? uni(0, 20000) : uni(0, 1500);
        CHECK(w.units_in(x - r, y - r, x + r, y + r, got), "open window answers");
        CHECK(std::is_sorted(got.begin(), got.end()) && std::adjacent_find(got.begin(), got.end()) == got.end(), "ascending, unique");
        std::vector<int> want, kept;
        for (std::size_t i = 0; i < w.units().size(); ++i) {
            const Unit& u = w.units()[i];
            if (u.deleted) continue;
            const bool in = std::isnan(u.pos.x) || std::isnan(u.pos.y) ||
                            (u.pos.x >= x - r && u.pos.x <= x + r && u.pos.y >= y - r && u.pos.y <= y + r);
            if (in) want.push_back(int(i));
        }
        for (int i : got) {
            const Unit& u = w.units()[std::size_t(i)];
            if (!u.deleted && (std::isnan(u.pos.x) || std::isnan(u.pos.y) ||
                               (u.pos.x >= x - r && u.pos.x <= x + r && u.pos.y >= y - r && u.pos.y <= y + r)))
                kept.push_back(i);
        }
        CHECK(kept == want, "query %d: grid %zu vs brute %zu", q, kept.size(), want.size());
        if (q == 1000) {   // spawned in the window: always returned; delete_unit rebuilds
            w.spawn_unit("INF", 1, {uni(-6000, 6000), uni(-6000, 6000), 0}, 0);
            w.delete_unit(3);
        }
    }
    w.grid_open(false);
    CHECK(!w.units_in(0, 0, 1, 1, got), "closed window: full scan");
    float nan = std::numeric_limits<float>::quiet_NaN();
    w.grid_open(true);
    CHECK(!w.units_in(nan, 0, 1, 1, got), "NaN query: full scan");
    w.grid_open(false);
}

// The pre-index Unit_findCurrentAnimRec 0x504010 scan.
const MoveRecord* find_move_ref(const World& w, const Unit& u, int action, std::string_view combo, int posture) {
    const MoveTable* mt = w.data().moves(u.cls);
    if (!mt) return nullptr;
    if (posture < 0) posture = u.posture;
    const int sp = u.special == 4 && action != WALK && action != RUN ? 0 : u.special;
    const bool attack = action == ATTACK || action == H_ATTACK;
    const MoveRecord* best[4] = {};
    for (auto it = mt->rows.rbegin(); it != mt->rows.rend(); ++it) {
        const MoveRecord& r = *it;
        if (r.position != posture || r.movement != action || r.item_using != u.item_using) continue;
        if (r.special != sp && r.special != 0) continue;
        const bool cm = r.combo == combo;
        if (!cm && attack) continue;
        const int k = (r.special == sp ? 0 : 2) + (cm ? 0 : 1);
        if (k < 2 && best[k]) continue;
        best[k] = &r;
    }
    for (const MoveRecord* r : best)
        if (r) return r;
    return nullptr;
}

}  // namespace

int main() {
    // units_in vs brute force.
    {
        GameData gd = make_data();
        World w(gd, nullptr, 1);
        AiSystem ai(w, nullptr);
        populate(w, ai, 11, 600);
        std::mt19937 g(5);
        grid_vs_brute(w, g);
    }

    // search_for_unit / auto_join: grid window (A) vs full scan (B).
    for (unsigned seed : {1u, 2u, 3u}) {
        GameData gd = make_data();
        World a(gd, nullptr, seed), b(gd, nullptr, seed);
        AiSystem aa(a, nullptr), ab(b, nullptr);
        populate(a, aa, seed, 500);
        populate(b, ab, seed, 500);
        CHECK(a.hash() == b.hash() && aa.hash() == ab.hash(), "same start");
        std::mt19937 g(seed * 7);
        const char* types[] = {"E", "EP", "F", "P", "O", "S", "L"};
        const int radii[] = {0, 50, 150, 399, 400, 1000, 1600, 2500, 3200, -800};
        a.grid_open(true);
        int found = 0;
        for (int q = 0; q < 4000; ++q) {
            const int id = 1 + int(g() % unsigned(a.units().size()));
            const char* t = types[g() % 7];
            const int r = radii[g() % 10];
            const bool lead = g() % 3 == 0;
            const int place = std::string(t) == "EP" ? 1 : 0;
            const int yaw = g() % 4 == 0 ? 135 : 0, pitch = g() % 5 == 0 ? 30 : 0;
            Unit* ua = a.unit(id);
            Unit* ub = b.unit(id);
            if (!ua || !ub) continue;
            const int ra = aa.search_for_unit(*ua, t, r, lead, place, yaw, pitch);
            const int rb = ab.search_for_unit(*ub, t, r, lead, place, yaw, pitch);
            CHECK(ra == rb, "search %d (%s r %d): %d vs %d", q, t, r, ra, rb);
            found += ra > 0;
            if (q == 2000) {   // spawned inside the window
                const Vec3 p{float(int(g() % 600)) - 300, float(int(g() % 600)) - 300, 0};
                a.spawn_unit("INF", 2, p, 0), b.spawn_unit("INF", 2, p, 0);
            }
        }
        CHECK(found > 200, "searches find targets (%d)", found);
        CHECK(a.rng().seed == b.rng().seed, "same RNG draws");
        for (std::size_t i = 0; i < a.units().size(); ++i)   // everyone settled again for the joins
            if (!a.units()[i].deleted) aa.st(a.units()[i]), ab.st(b.units()[i]);
        int leaders = 0;
        for (std::size_t i = 0; i < a.units().size(); ++i) {
            Unit& la = a.units()[i];
            if (la.deleted || !aa.is_leader(la)) continue;
            ++leaders;
            aa.auto_join(la);
            ab.auto_join(b.units()[i]);
        }
        a.grid_open(false);
        CHECK(leaders > 5, "leaders %d", leaders);
        CHECK(a.hash() == b.hash(), "world hash after searches + joins");
        CHECK(aa.hash() == ab.hash(), "AI hash after searches + joins");
    }

    // find_move index vs the reverse scan.
    {
        GameData gd = make_data();
        std::mt19937 g(9);
        MoveTable mt;
        mt.code = "T";
        const int acts[] = {IDLE, WALK, RUN, ATTACK, H_ATTACK, GUARD1, DOWN};
        const char* combos[] = {"", "A", "AB", "B"};
        for (int i = 0; i < 400; ++i) {
            MoveRecord r;
            r.position = int(g() % 2), r.movement = acts[g() % 7], r.item_using = int(g() % 3), r.special = int(g() % 5);
            r.combo = combos[g() % 4];
            mt.rows.push_back(r);
        }
        gd.move_tables.push_back(mt);
        gd.units[std::size_t(gd.unit_index("MOVER"))].move_table = 0;
        World w(gd, nullptr, 1);
        const int id = w.spawn_unit("MOVER", 1, {0, 0, 0}, 0);
        Unit& u = *w.unit(id);
        for (int q = 0; q < 5000; ++q) {
            u.posture = int(g() % 2), u.item_using = int(g() % 3), u.special = int(g() % 5);
            const int act = acts[g() % 7], pos = int(g() % 3) - 1;
            const char* combo = combos[g() % 4];
            CHECK(w.find_move(u, act, combo, pos) == find_move_ref(w, u, act, combo, pos), "find_move query %d", q);
        }
    }
    return finish();
}
