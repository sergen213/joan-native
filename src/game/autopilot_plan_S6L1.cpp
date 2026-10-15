// Campaign-bot plan for mission 8, S6L1 "The Road to Coronation" (script/scenario/S6L1*.sce). Test / diagnostic only.
//
// The heroes and Charles VII never move on their own in this mission (the script's FOLLOW_UNIT orders are commented out):
// the player moves them in RTS mode (scenario 5+), as the Steam walkthrough does ("Assign Jean, La Hire, and Orleans to
// follow Joan; have Charles follow ..."). The plan gives those orders the way the RTS view does: enter RTS (Session::
// toggle_rts: Rts_init hands Joan to the AI), select the hero with his team (click), right-click Joan (0x44f750 branch B:
// FOLLOW_UNIT) or the ground (0x44cfb0 RTS_MOVE_TO), leave RTS (Rts_deinit, Joan's task parked).
#include <cmath>
#include <map>
#include <vector>

#include "game/autopilot_player.hpp"

namespace joa::game {
namespace {

float d2(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); }
bool alive(const World& w, const Unit& u) { return !u.deleted && !w.dead(u); }

struct PlanS6L1 final : MissionPlan {
    int next_ = 0;             // next tick the RTS orders are checked
    int wait_ = -1;            // waiting for the king since (-1 not waiting)
    float best_ = 0;           // his nearest distance during that wait

    Unit* hero(World& w, std::string_view code) {
        for (Unit& u : w.units())
            if (alive(w, u) && u.nation == 2 && w.data().units[std::size_t(u.cls)].code == code) return &u;
        return nullptr;
    }

    // One RTS round trip (Session::toggle_rts in and out) around `orders`, as the player does it in the RTS view.
    template <class F> static void rts(Mission& m, Unit& p, F&& orders) {
        World& w = m.world();
        PlayerController& pc = w.controller();
        pc.prev_mode = w.game_mode;
        w.game_mode = 4;
        m.ai().rts_enter(p);
        orders(m.ai());
        m.ai().rts_exit(p);
        m.ai().clear_task(p);
        const int to = pc.prev_mode;
        pc.prev_mode = w.game_mode;
        w.game_mode = to;
    }

    bool follows(Mission& m, const Unit& h, const Unit& p) {
        return m.ai().curr_task(h) == task::FOLLOW_UNIT && m.ai().curr_task_para(h, 1) == p.id;
    }

    // An enemy that may come at q (awake: not scripted / caged) within r.
    static bool foe_near(World& w, Vec3 q, float r) {
        for (const Unit& u : w.units())
            if (alive(w, u) && u.m.active && !u.no_react && w.hostile_to_player(u) && d2(u.pos, q) < r) return true;
        return false;
    }

    bool first(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        if (m.ui().game_cut_scene || w.cut_scene) return false;
        const Places& pl = m.places();
        const Territory* aux = pl.territory(pl.territory_by_name("Auxerres"));
        const bool auxerre_open = aux && aux->owner == p.nation;
        const Place* gate = pl.place(pl.place_by_name("S6L1_2_Open_Gate_Place"));
        // Auxerre is a trap (the gate shuts behind the first French, 40+ English and Burgundians spawn inside): the king
        // and the duke wait outside the gate until it is taken, near enough that Joan never gets 11585 from the king anywhere
        // inside (after 15 min that sends four assassin squads at him, tactic l.1893).
        const bool trap = !auxerre_open && gate &&
                          (d2(p.pos, gate->pos) < 3000 || pl.inside(pl.place_by_name("S6L1_2_Check_Enemy_Place"), p.pos));
        constexpr Vec3 kTrapPark{19300, 24790, 0};
        Unit* charles = hero(w, "CHARLIE7");
        if (ap.tick() >= next_) {
            next_ = ap.tick() + 30;
            for (const char* code : {"JEAN_DE_METZ", "LAHIRE", "ORLEAN", "ALENCON", "CHARLIE7"}) {
                Unit* h = hero(w, code);
                if (!h || h->retreat == -1) continue;
                // Charles and Alencon die for good: they keep out of fights (an RTS move to where they stand) and come on
                // once no enemy is near them, the duke behind the king.
                const bool ward = h->retreat == 0;
                const Unit* lead = ward && charles && h != charles ? charles : &p;
                if (ward && trap) {
                    if (d2(h->pos, kTrapPark) > 400 && m.ai().curr_task(*h) != task::RTS_MOVE_TO)
                        rts(m, p, [&](AiSystem& ai) { ai.rts_move(ai.rts_select_with_team(h->id), kTrapPark, false); });
                    continue;
                }
                if (ward && foe_near(w, h->pos, 1800)) {
                    if (m.ai().curr_task(*h) == task::FOLLOW_UNIT) {
                        rts(m, p, [&](AiSystem& ai) { ai.rts_move(ai.rts_select_with_team(h->id), h->pos, false); });
                        if (ap.trace) std::printf("[plan %d] %s waits\n", ap.tick(), h->name.c_str());
                    }
                    continue;
                }
                if (follows(m, *h, *lead) || foe_near(w, p.pos, 600)) continue;  // orders between fights (Rts_init stops Joan)
                rts(m, p, [&](AiSystem& ai) { ai.rts_assign(ai.rts_select_with_team(h->id), lead->id, false); });
                if (ap.trace) std::printf("[plan %d] %s follows %s\n", ap.tick(), h->name.c_str(), lead->name.c_str());
            }
        }
        // Escort: the king, following, more than 3500 behind is waited for (and fetched when he does not close in within
        // 30 s); more than 8000 off Joan goes back to him whatever he does.
        if (!charles || trap || foe_near(w, p.pos, 1000)) return false;
        const float d = d2(p.pos, charles->pos);
        const bool coming = follows(m, *charles, p);
        if (d <= 3500 || (!coming && d <= 8000)) return wait_ = -1, false;
        if (wait_ < 0 || d < best_ - 300) wait_ = ap.tick(), best_ = d;
        ap.goal = d > 8000 || ap.tick() - wait_ > 900 ? "back to the king" : "wait for the king";
        if (ap.goal[0] == 'b') ap.travel(f, p, charles->pos);
        return true;
    }

    // An enemy no walk leads to (Auxerre's four corner-tower archers stand on tower tops linked to nothing; only siege
    // fire breaks a tower, tactic l.3421): shot from a spot the player walks to with a clear line to him, the walkways
    // beside the tower (the Steam walkthrough takes them from the balconies). From the yard the line runs into the tower.
    bool stranded(Autopilot& ap, const Unit& p, const Unit& u) {
        const int here = ap.unit_region(p), r = ap.unit_region(u);
        return here >= 0 && r >= 0 && r != here && !ap.linked(here, r);
    }
    float foe_weight(Autopilot& ap, const Unit& p, const Unit& u) override {
        return stranded(ap, p, u) ? 0.0f : MissionPlan::foe_weight(ap, p, u);
    }

    // Nearest walkable spot 200..700 from t, linked to the player, with a clear arrow line to him (clear_line). The
    // tower-top archers stand in a roofed chamber whose walls rise 140 above its floor; only a few windows let an arrow
    // in. Spots already tried are skipped; false if none.
    bool firing_spot(Autopilot& ap, const Unit& p, const Unit& t, Vec3& out) {
        Mission& m = ap.mission();
        const NavWorld& nav = *m.ai().nav();
        const int here = ap.unit_region(p);
        float best = 1e30f;
        for (float r = 200; r <= 700; r += 25)
            for (int a = 0; a < 180; ++a)
                for (const float zq : {t.pos.z, t.pos.z - 80, 0.0f}) {
                    const float x = t.pos.x + r * std::cos(float(a) * 0.0349066f), y = t.pos.y + r * std::sin(float(a) * 0.0349066f);
                    const int poly = nav.find_poly_xy(x, y, zq);
                    if (poly < 0) continue;
                    const Vec3 s{x, y, nav.poly_height(poly, x, y)};
                    const float d = d2(s, p.pos) + std::fabs(s.z - p.pos.z);
                    if (d >= best || std::fabs(s.z - zq) > 60 || !ap.linked(here, ap.region(s))) continue;
                    bool tried = false;
                    for (const Vec3& b : tried_) tried |= d2(b, s) < 100 && std::fabs(b.z - s.z) < 60;
                    if (tried || !clear_line(m, p, s, t)) continue;
                    best = d, out = s;
                }
        return best < 1e30f;
    }

    // The arrow from spot s (feet) reaches t before a structure stops it. A bow arrow (PROJ, throwi[1]) with a release-
    // lock target gets the ballistic solve of World::spawn_projectile / solve_aim (docs GAMEPLAY_CHARACTER_COMBAT
    // §3.2-3.4): launch point 3/4 of the class cylinder up, target 0.8 of his class height up, flight time d / AV. The
    // structure test is not along that arc but one look-ahead ray of FT ms cast at launch along (vx, vy, vz - V0)
    // (World::sweep_structures, Physics_sweep_structures 0x693750), below the arc early on: the arrow flies until the
    // ray's first hit, so that hit must lie beyond t. From 800 off the solver lobs the arrow (minSin 0.25) into the tower
    // chambers' ceiling: only targets within 700 count.
    static bool clear_line(Mission& m, const Unit& p, Vec3 s, const Unit& t) {
        const ThrowConsts& tc = m.data().throwi[1];
        const float k = World::kUnitsPerMetre;
        const Vec3 p0{s.x, s.y, s.z + 0.75f * m.data().units[std::size_t(p.cls)].cyl_height};
        const Vec3 d{t.pos.x - p0.x, t.pos.y - p0.y, t.pos.z + 0.8f * float(m.data().units[std::size_t(t.cls)].height) - p0.z};
        const float dh = std::hypot(d.x, d.y);
        if (dh > 700 || tc.av <= 0) return false;
        const float tf = std::sqrt(dh * dh + d.z * d.z) / tc.av, vz = tc.gl * 9.81f * tf / 2 + d.z / k / tf;   // m/s
        const float reach = tc.ft * 0.001f / tf;   // look-ahead ray length per unit of d (horizontal)
        const StructureHit h =
            m.objects().structure_ray(0, p0, {p0.x + d.x * reach, p0.y + d.y * reach, p0.z + (vz - tc.v0) * tc.ft * 0.001f * k});
        return !h.item || h.t * reach > 1;   // the ray's first hit lies beyond him
    }

    // Nothing else to fight: the stranded enemies. Joan walks to a firing spot of the nearest one (kept while he lives)
    // and there shoots whichever stranded enemy within 1500 has a clear line now (they walk about their chamber),
    // holding the drawn bow while none has. None in 10 s there, or the spot not reached within 60 s: the next spot.
    bool before_hunt(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        const Unit* t = w.unit(tower_foe_);
        if (!t || !alive(w, *t) || !w.hostile_to_player(*t) || !stranded(ap, p, *t)) {
            t = nullptr;
            for (const Unit& u : w.units())
                if (alive(w, u) && u.m.active && !u.no_react && w.hostile_to_player(u) && d2(u.pos, p.pos) < 8000 &&
                    ap.tick() >= no_spot_[u.id] && (!t || d2(u.pos, p.pos) < d2(t->pos, p.pos)) && stranded(ap, p, u))
                    t = &u;
            if (!t) return false;
            tower_foe_ = t->id;
            tried_.clear();
            spot_tick_ = -1;
        }
        const bool at_spot = spot_tick_ >= 0 && d2(p.pos, spot_) <= 60 && std::fabs(p.pos.z - spot_.z) <= 60;
        const Unit* shot = nullptr;
        if (at_spot) {
            for (const Unit& u : w.units())
                if (alive(w, u) && u.m.active && w.hostile_to_player(u) && d2(u.pos, p.pos) < 1500 && (!shot || d2(u.pos, p.pos) < d2(shot->pos, p.pos)) &&
                    stranded(ap, p, u) && clear_line(m, p, p.pos, u))
                    shot = &u;
            if (shot || seen_tick_ < 0) seen_tick_ = ap.tick();
        }
        if (spot_tick_ < 0 || (at_spot && ap.tick() - seen_tick_ > 300) || (!at_spot && ap.tick() - spot_tick_ > 1800)) {
            if (spot_tick_ >= 0) tried_.push_back(spot_);
            if (!firing_spot(ap, p, *t, spot_)) {
                no_spot_[t->id] = ap.tick() + 3000;   // none from here: again after 100 s (the player moves on)
                tower_foe_ = 0;
                return false;
            }
            spot_tick_ = ap.tick(), seen_tick_ = -1;
            if (ap.trace) std::printf("[plan %d] #%d firing spot (%.0f,%.0f,%.0f)\n", ap.tick(), t->id, spot_.x, spot_.y, spot_.z);
        }
        ap.goal = "shoot up at #" + std::to_string(shot ? shot->id : t->id);
        if (!at_spot) {
            ap.travel(f, p, spot_);
        } else if (ap.load_ammo(p, false)) {
            const Unit& a = shot ? *shot : *t;
            ap.shoot_at(f, p, {a.pos.x, a.pos.y, a.pos.z + 60}, shot && p.action == SHTIDLE);
        }
        return true;
    }
    int tower_foe_ = 0, spot_tick_ = -1, seen_tick_ = -1;
    Vec3 spot_{};
    std::vector<Vec3> tried_;
    std::map<int, int> no_spot_;   // stranded enemy -> tick before which he is not tried again
};

}  // namespace

std::unique_ptr<MissionPlan> make_plan_S6L1() { return std::make_unique<PlanS6L1>(); }

}  // namespace joa::game
