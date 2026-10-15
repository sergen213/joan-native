// Campaign-bot plan for mission 8, S6L1 "The Road to Coronation" (script/scenario/S6L1*.sce). Test / diagnostic only.
//
// The heroes and Charles VII never move on their own in this mission (the script's FOLLOW_UNIT orders are commented out):
// the player moves them in RTS mode (scenario 5+), as the Steam walkthrough does ("Assign Jean, La Hire, and Orleans to
// follow Joan; have Charles follow ..."). The plan gives those orders the way the RTS view does: enter RTS (Session::
// toggle_rts: Rts_init hands Joan to the AI), select the hero with his team (click), right-click Joan (0x44f750 branch B:
// FOLLOW_UNIT) or the ground (0x44cfb0 RTS_MOVE_TO), leave RTS (Rts_deinit, Joan's task parked).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include "game/autopilot_player.hpp"
#include "game/interact_player.hpp"

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
        ap.shop_plain_max = 100;   // the generic buy spends every coin on arrows (100 each); later shops refill
        if (m.ui().game_cut_scene || w.cut_scene) return false;
        if (is(m, p, "ALENCON")) return duke_errand(ap, f, p);
        const Places& pl = m.places();
        const Territory* aux = pl.territory(pl.territory_by_name("Auxerres"));
        const bool auxerre_open = aux && aux->owner == p.nation;
        if (arm(ap, p, auxerre_open)) return true;
        const Place* gate = pl.place(pl.place_by_name("S6L1_2_Open_Gate_Place"));
        // Auxerre is a trap (the gate shuts behind the first French, 40+ English and Burgundians spawn inside): the king
        // and the duke wait outside the gate by the hut until it is taken: beyond the 2000 reach of the gate-tower archers
        // (teams 224 / 225, TOWER_ATTACK range 2000, at x 21060..21290) and near enough that Joan never gets 11585 from the
        // king anywhere inside (after 15 min that sends four assassin squads at him, tactic l.1893).
        bool trap = !auxerre_open && gate &&
                    (d2(p.pos, gate->pos) < 3000 || pl.inside(pl.place_by_name("S6L1_2_Check_Enemy_Place"), p.pos));
        Vec3 park{18700, 25300, 0};
        // Troyes' wall archers, crossbowmen and arbalests shoot far beyond its walls (the king fell at (36517,24418)
        // following Joan past Croncels), and the field between Auxerre and Troyes is full of roaming squads (an archer
        // shot him at (32714,24826)): while Troyes is English they wait in Auxerre's yard while Joan fights north of the
        // gate road; once she is past it towards the six light infantry and the montagne du lion (y < 19000) they move to
        // kSouthPark, 2800 off the walls, from where the hill camp (9300) and all of Troyes (< 9900) stay within the 11585
        // that sends the 15-min assassin squads at the king (tactic l.1893), until Troyes falls.
        if (auxerre_open && !owned(m, p, "Troyes")) {
            south_ |= p.pos.y < 19000;
            trap = true, park = south_ ? kSouthPark : kAuxerrePark;
        }
        feed_wards(ap, p);
        Unit* charles = hero(w, "CHARLIE7");
        if (charles && (charles->pos.y < 19000 || south_)) wards_south_ |= d2(charles->pos, kSouthPark) < 1500;
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
                    if (d2(h->pos, park) > 400 &&
                        (m.ai().curr_task(*h) != task::RTS_MOVE_TO || std::abs(m.ai().curr_task_para(*h, 1) - int(park.x)) > 50))
                        rts(m, p, [&](AiSystem& ai) { ai.rts_move(squad(m, ai, h->id), park, false); });
                    continue;
                }
                if (ward && foe_near(w, h->pos, 1800)) {
                    if (m.ai().curr_task(*h) == task::FOLLOW_UNIT) {
                        rts(m, p, [&](AiSystem& ai) { ai.rts_move(squad(m, ai, h->id), h->pos, false); });
                        if (ap.trace) std::printf("[plan %d] %s waits\n", ap.tick(), h->name.c_str());
                    }
                    continue;
                }
                if (follows(m, *h, *lead) || foe_near(w, p.pos, 600)) continue;  // orders between fights (Rts_init stops Joan)
                rts(m, p, [&](AiSystem& ai) { ai.rts_assign(squad(m, ai, h->id), lead->id, false); });
                if (ap.trace) std::printf("[plan %d] %s follows %s\n", ap.tick(), h->name.c_str(), lead->name.c_str());
            }
        }
        if (provisions(ap, f, p)) return true;
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
    // Troyes' ~250 level-29 men (Fastolf among them) are left alone until the trebuchets break its wall (step 3 of
    // troyes_stage deletes eight of its teams first): she fought her way in through the gate unprovoked, ate all her food
    // and fled at 4 % HP. Only those closing in on her (within 400) are fought.
    float foe_weight(Autopilot& ap, const Unit& p, const Unit& u) override {
        Mission& m = ap.mission();
        if (owned(m, p, "Auxerres") && !owned(m, p, "Troyes") && d2(u.pos, p.pos) > 400 &&
            m.places().inside(m.places().place_by_name("S6L1_3_Capture_Troyes_Place"), u.pos))
            if (const MapItem* wall = m.objects().item(m.objects().find_by_name("S6L1_3_Wall3")); wall && wall->hp > 0) return 0.0f;
        return stranded(ap, p, u) ? 0.0f : MissionPlan::foe_weight(ap, p, u);
    }
    // The talk errand to Troyes' potion seller led her in through the gate while it was English.
    bool use_talker(Autopilot& ap, const Unit& u) override {
        Mission& m = ap.mission();
        const Unit* p = m.world().unit(m.world().player_id());
        return !p || owned(m, *p, "Troyes") || !m.places().inside(m.places().place_by_name("S6L1_3_Capture_Troyes_Place"), u.pos);
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

    // The gate-tower archers (teams 224 / 225, 14 on the two towers by the gate, z 544) stand on the wall walk: the
    // generic fight gives up on them from the yard (no headway in 30 s), but up the walkway stairs the walk reaches
    // them (the heroes following Joan up there killed them when she happened to pass). She walks up to the nearest
    // and fights him there. The cannons wait for them (siege).
    bool gate_towers(Autopilot& ap, InputFrame& f, Unit& p) {
        World& w = ap.mission().world();
        const Unit* t = nullptr;
        for (const Unit& u : w.units())
            if (alive(w, u) && w.hostile_to_player(u) && (u.team == 224 || u.team == 225) && (!t || d2(u.pos, p.pos) < d2(t->pos, p.pos))) t = &u;
        if (!t) return false;
        ap.goal = "gate tower #" + std::to_string(t->id);
        if (d2(p.pos, t->pos) > 150 || std::fabs(p.pos.z - t->pos.z) > 100) ap.travel(f, p, t->pos);
        else ap.fight(f, p, *t);
        return true;
    }

    // Nothing else to fight: the stranded enemies. Joan walks to a firing spot of the nearest one (kept while he lives)
    // and there shoots whichever stranded enemy within 1500 has a clear line now (they walk about their chamber),
    // holding the drawn bow while none has. None in 10 s there, or the spot not reached within 60 s: the next spot.
    bool before_hunt(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        if (gate_towers(ap, f, p) || siege(ap, f, p) || troyes_stage(ap, f, p)) return true;
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

    static bool is(Mission& m, const Unit& u, std::string_view code) { return u.cls >= 0 && m.data().units[std::size_t(u.cls)].code == code; }

    // Auxerre's corner-tower archers stand in roofed chambers few arrows enter (the firing spots above hit about one shot
    // in six); tactic l.3421 deletes a tower's team once that tower, a 200-HP wall that only stones and kegs damage
    // (Wall_take_ranged), is down. The walkthrough's way: the duke builds a cannon at the iron workshop by the hut (an
    // IRON workshop opens only to Alencon as the player, within 1024: 0x43c080) and it breaks the towers. The standing
    // tower with archers left nearest `from` (`keep` while it stands), 0 none.
    static int live_tower(Mission& m, Vec3 from, int keep = 0) {
        static constexpr std::pair<const char*, int> kTowers[] = {
            {"S6L1_2_Tower1", 216}, {"S6L1_2_Tower2", 223}, {"S6L1_2_Tower3", 203}, {"S6L1_2_Tower4", 205}};
        World& w = m.world();
        int best = 0;
        for (const auto& [name, team] : kTowers) {
            const int r = m.objects().find_by_name(name);
            const MapItem* it = m.objects().item(r);
            if (!it || it->hp <= 0) continue;
            bool manned = false;
            for (const Unit& u : w.units()) manned |= alive(w, u) && u.team == team && w.hostile_to_player(u);   // after the trap
            if (manned && r == keep) return r;
            if (manned && (!best || d2(it->pos, from) < d2(m.objects().item(best)->pos, from))) best = r;
        }
        return best;
    }

    // Ground spots 700..1400 from the tower, linked to cannon c, whose straight line from the muzzle (3/4 of the cylinder
    // up) to the stone's aim point (ThrowItem_launch 0x5133e0: the item's pos + its siege aim height) meets the tower
    // first: from most of the yard a building or the wall walk takes the stone. Nearest c first.
    static std::vector<Vec3> gun_spots(Autopilot& ap, const Unit& c, int tower) {
        Mission& m = ap.mission();
        const NavWorld& nav = *m.ai().nav();
        const StructureHit t = m.objects().structure_ray(tower, {}, {});
        const Vec3 aim{t.pos.x, t.pos.y, t.pos.z + t.aim_h_siege};
        const float up = 0.75f * m.data().units[std::size_t(c.cls)].cyl_height;
        const int here = ap.unit_region(c);
        std::vector<Vec3> v;
        for (float r = 700; r <= 1400; r += 50)
            for (int a = 0; a < 90; ++a) {
                const float x = aim.x + r * std::cos(float(a) * 0.0698132f), y = aim.y + r * std::sin(float(a) * 0.0698132f);
                const int poly = nav.find_poly_xy(x, y, 0);
                if (poly < 0) continue;
                const Vec3 s{x, y, nav.poly_height(poly, x, y)};
                if (s.z < 100 && ap.linked(here, ap.region(s)) && m.objects().structure_ray(0, {s.x, s.y, s.z + up}, aim).item == tower)
                    v.push_back(s);
            }
        std::sort(v.begin(), v.end(), [&](Vec3 a, Vec3 b) { return d2(a, c.pos) < d2(b, c.pos); });
        return v;
    }

    std::vector<Unit*> cannons(Mission& m, const Unit& p) {
        std::vector<Unit*> v;
        for (Unit& u : m.world().units())
            if (alive(m.world(), u) && u.nation == p.nation && is(m, u, "CANNON")) v.push_back(&u);
        return v;
    }

    // Nothing else left to fight while a tower stands: every 10 s (as the S7L1 trebuchet) each cannon is RTS-moved to its
    // gun spot and there sent at the tower (RTS_ATTACK_ITEM: in range, turn, shoot); Joan walks by the first cannon
    // (through the gate she opens for it).
    bool siege(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        const std::vector<Unit*> cs = cannons(m, p);
        if (cs.empty()) return false;
        const int tower = live_tower(m, cs[0]->pos, spots_tower_);
        if (!tower) return false;
        // The crew walk with the cannon through the gate: while anyone hostile is within 2200 of it (the gate-tower archers,
        // TOWER_ATTACK 2000; the trap's spawns) it stays parked (arm), or the crew die there and nothing moves the gun again
        // (light infantry are the only crew and none are recruited before Auxerre falls).
        if (const MapItem* g = m.objects().item(m.objects().find_by_name("S6L1_2_Entrance_Gate")))
            for (const Unit& u : m.world().units())
                if (alive(m.world(), u) && m.world().hostile_to_player(u) && d2(u.pos, g->pos) < 2200) return false;
        siege_seen_ = ap.tick();
        if (ap.tick() >= siege_tick_) {
            siege_tick_ = ap.tick() + 300;
            if (tower != spots_tower_) {
                spots_tower_ = tower, spots_ = gun_spots(ap, *cs[0], tower);
                if (ap.trace) std::printf("[plan %d] tower %d: %zu gun spots, first (%.0f,%.0f)\n", ap.tick(), tower, spots_.size(),
                                          spots_.empty() ? 0.f : spots_[0].x, spots_.empty() ? 0.f : spots_[0].y);
            }
            std::vector<Vec3> taken;
            rts(m, p, [&](AiSystem& ai) {
                for (Unit* c : cs) {
                    const int sel[] = {c->id};
                    const Vec3* s = nullptr;   // the nearest spot no other cannon of this round took
                    for (const Vec3& q : spots_) {
                        bool apart = true;
                        for (const Vec3& o : taken) apart &= d2(o, q) > 250;
                        if (apart && (!s || d2(q, c->pos) < d2(*s, c->pos))) s = &q;
                    }
                    if (!s || d2(c->pos, *s) <= 150) {
                        if (ai.curr_task(*c) != task::RTS_ATTACK_ITEM || ai.curr_task_para(*c, 1) != tower) ai.rts_attack_item(sel, tower);
                    } else {
                        ai.rts_move(sel, *s, false);
                    }
                    if (s) taken.push_back(*s);
                }
            });
        }
        ap.goal = "cannons at tower " + std::to_string(tower);
        // The trap shut the gate (now English) on the cannon outside: no path takes it in (path 0, RTS_MOVE_TO for good),
        // while Joan and the heroes still get in and out on foot. She opens the gate from its back side (the interact
        // opens a foreign gate from there only: FUN_0041af80), at a point 150 inside it.
        const MapItem* g = m.objects().item(m.objects().find_by_name("S6L1_2_Entrance_Gate"));
        if (g && g->state == 0 && !m.places().inside(m.places().place_by_name("S6L1_2_Check_Enemy_Place"), cs[0]->pos)) {
            const Vec3 in{g->pos.x + 150, g->pos.y, g->pos.z};   // the yard lies east of the west gate
            ap.goal += " (open the gate)";
            if (d2(p.pos, in) > 60 || std::fabs(p.pos.z - in.z) > 60) {
                ap.travel(f, p, in);
            } else {
                f.mouse_dx = m.world().rel_angle(p, g->pos) / 0.18f;
                if (ap.tick() % 30 == 0) f.press(IN_INTERACT);
            }
        } else if (d2(p.pos, cs[0]->pos) > 500) {
            ap.travel(f, p, cs[0]->pos);
        }
        return true;
    }

    // Before Auxerre falls: once Joan passes the hut, the player switches to the duke, who builds the cannons there
    // (duke_errand); two of Joan's light infantry (the only crew class, Rts_cmd_assign 0x44f750) crew the first at once,
    // while they live (the castle fights kill them otherwise); one cannon breaks a tower in five stones, and every man
    // more on a gun is one fewer beside Joan against Glasdale. True while handing over.
    bool arm(Autopilot& ap, Unit& p, bool auxerre_open) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (auxerre_open) return false;
        const std::vector<Unit*> cs = cannons(m, p);
        if (cs.empty()) {
            const Facility* fc = m.places().facility(m.places().facility_by_name("S6L1_2_Iron_Workshop"));
            const Unit* duke = hero(w, "ALENCON");
            if (!fc || fc->resource < 1500 || !fc->builds.empty() || !duke || d2(p.pos, fc->pos) > 3000 || foe_near(w, duke->pos, 1800) ||
                foe_near(w, p.pos, 1500) || !m.switch_hero(4))
                return false;
            if (ap.trace) std::printf("[plan %d] to the duke: cannons\n", ap.tick());
            return true;
        }
        if (ap.tick() < crew_tick_) return false;
        crew_tick_ = ap.tick() + 300;
        std::vector<int> free;
        for (const Unit& u : w.units())
            if (alive(w, u) && u.nation == p.nation && is(m, u, "FRENCH_LIGHT_INFANTRY") && !on_weapon(m, u)) free.push_back(u.id);
        for (Unit* c : cs) {
            int need = c == cs[0] ? int(m.data().units[std::size_t(c->cls)].attach_points.size()) : 0;
            for (const Unit& u : w.units())
                need -= alive(w, u) && (u.m.vehicle == c->id || (m.ai().curr_task(u) == task::LARGE_WEAPON_PUSH && m.ai().curr_task_para(u, 1) == c->id));
            std::vector<int> crew;
            while (need-- > 0 && !free.empty()) crew.push_back(free.back()), free.pop_back();
            if (!crew.empty()) rts(m, p, [&](AiSystem& ai) { ai.rts_assign(crew, c->id, true); });
            // An idle crewed cannon goes for any enemy item within 4000 (AI_cannon_default 0x543800: APPROACH_ITEM): after the
            // trap the gate, under the gate-tower archers who kill the crew. It waits more than 4000 from the walls instead.
            constexpr Vec3 kGunPark{16300, 26300, 0};
            if (ap.tick() - siege_seen_ > 600 && m.ai().curr_task(*c) == task::NONE && d2(c->pos, kGunPark) > 300) {
                const int sel[] = {c->id};
                rts(m, p, [&](AiSystem& ai) { ai.rts_move(sel, kGunPark, false); });
            }
        }
        return false;
    }
    int crew_tick_ = 0, siege_seen_ = -1000;
    int spots_tower_ = 0;
    std::vector<Vec3> spots_;
    static bool on_weapon(Mission& m, const Unit& u) { return u.m.vehicle != 0 || m.ai().curr_task(u) == task::LARGE_WEAPON_PUSH; }

    // The duke as the player: to the iron workshop by the hut, cannons as many as its iron pays for (the workshop
    // screen: CANNON up, BUILD), then back to Joan.
    bool duke_errand(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        const int fac = m.places().facility_by_name("S6L1_2_Iron_Workshop");
        const Facility* fc = m.places().facility(fac);
        if (!fc || !fc->builds.empty() || fc->resource < 1500 || !cannons(m, p).empty()) {
            m.switch_hero(1);
            return true;
        }
        ap.goal = "duke: cannons at the workshop";
        if (find_workshop(m, p) != fac || d2(p.pos, fc->pos) >= 1000) {
            ap.travel(f, p, fc->pos);
            return true;
        }
        FacilityOrder order = FacilityOrder::siege_workshop(m, fac);
        for (int row = 0; row < 4; ++row)
            if (const int t = order.row_type(m, row); t && m.data().units[std::size_t(t - 1)].code == "CANNON")
                while (order.more(m, row)) {}
        order.commit(m);
        if (ap.trace) std::printf("[plan %d] duke orders cannons\n", ap.tick());
        return true;
    }

    static const Unit* named(World& w, std::string_view name) {
        for (const Unit& u : w.units())
            if (alive(w, u) && u.name == name) return &u;
        return nullptr;
    }
    static bool done(Mission& m, int objective) {
        const auto& e = m.state().end;
        return std::size_t(objective) <= e.size() && e[std::size_t(objective - 1)].finish == 1;
    }
    static bool owned(Mission& m, const Unit& p, const char* territory) {
        const Territory* t = m.places().territory(m.places().territory_by_name(territory));
        return t && t->owner == p.nation;
    }

    // Shops and barracks along the road, each once while its stage lasts (0 before Auxerre falls, 1 until Troyes falls,
    // 2 after), between fights. The forced-win chain brings Joan here with JOAN_SWORD_LV1 (10 damage) and ~45000 gold:
    // the start camp sells JOAN_SWORD_LV6 (60) and food, Auxerre's blacksmith the RECURVEBOW (38 against the RAPIDFBOW's
    // 16) and JOAN_ARMOR_LV3, Troyes' potion shop food, Bussy's blacksmith JOAN_SWORD_LV9 (90). shop() below buys the
    // weapons and the food, the generic buy armour and arrows down to kShopKeep. Auxerre's barracks (S7L1's recruit_job):
    // light infantry up to three in the troop (the trebuchet's crew class), then crossbowmen and heavy infantry
    // alternately down to kRecruitKeep. A trip without 200 of headway in 100 s is dropped.
    static constexpr int kShopKeep = 13000, kRecruitKeep = 13000;
    struct Errand { const char* who; int stage; };
    static constexpr Errand kErrands[] = {{"Boniface Natier", 0}, {"Gerard Vintre", 0}, {"Thibaut Fanier", 1}, {"S6L1_2_Recruit", 1},
                                          {"Foucaud Telier", 2},   {"Aubert Lanier", 2}};
    static int stage(Mission& m, const Unit& p) { return !owned(m, p, "Auxerres") ? 0 : !owned(m, p, "Troyes") ? 1 : 2; }
    bool provisions(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        if (!is(m, p, "JOAN_OF_ARC") || foe_near(m.world(), p.pos, 1500)) return false;
        ap.shop_gold_keep = kShopKeep;
        Places& pl = m.places();
        const int st = stage(m, p);
        while (errand_ < int(std::size(kErrands)) && kErrands[errand_].stage < st) ++errand_, trip_tick_ = -1;
        if (errand_ >= int(std::size(kErrands)) || kErrands[errand_].stage > st) return false;
        const char* who = kErrands[errand_].who;
        Vec3 to{};
        if (const Unit* s = named(m.world(), who)) {
            ap.plan_talk = s->id, to = s->pos, ap.goal = std::string("to the shop of ") + who;
        } else if (const int fac = pl.facility_by_name(who); const Facility* fc = pl.facility(fac)) {
            if (find_recruit_facility(m, p) == fac) {
                FacilityOrder order = FacilityOrder::barracks(m, fac, p.id);
                int light = 0;
                for (int id : p.members)
                    if (const Unit* u = m.world().unit(id); u && is(m, *u, "FRENCH_LIGHT_INFANTRY")) ++light;
                int rows[3] = {-1, -1, -1};   // light infantry (slot 0), crossbowman (slot 5), heavy infantry (slot 1)
                for (int r = 0; r < 6; ++r) {
                    if (order.slot(m, r) == 0) rows[0] = r;
                    if (order.slot(m, r) == 5) rows[1] = r;
                    if (order.slot(m, r) == 1) rows[2] = r;
                }
                const auto afford = [&] { return m.state().gold - order.cost - 500 >= kRecruitKeep; };
                for (; light < 3 && rows[0] >= 0 && afford() && order.more(m, rows[0]); ++light) {}
                for (bool more = true; more;) {
                    more = false;
                    for (int r : {rows[1], rows[2]}) more |= r >= 0 && afford() && order.more(m, r);
                }
                ap.log.push_back(std::to_string(m.ticks()) + " recruit " + std::to_string(order.recruited) + " at " + who);
                order.commit(m);
                ++errand_, trip_tick_ = -1;
                return true;
            }
            to = fc->pos, ap.goal = std::string("recruit at ") + who;
        } else {
            ++errand_, trip_tick_ = -1;
            return false;
        }
        const float d = d2(p.pos, to);
        if (trip_tick_ < 0 || d < trip_best_ - 200) trip_tick_ = ap.tick(), trip_best_ = d;
        if (ap.tick() - trip_tick_ > 3000) {
            ++errand_, trip_tick_ = -1, ap.plan_talk = 0;
            return false;
        }
        if (ap.plan_talk && d < 250) return false;   // at the seller the generic interact presses R at his prompt (plan_talk)
        ap.travel(f, p, to);
        return true;
    }
    // The buy screen of the errand's shop: a sharper sword and a stronger bow than any carried (character_screen() puts
    // the sword in hand; the bow is equipped here, both shoot arrows), then food, the biggest first, while the bag has room.
    void shop(Autopilot& ap, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        const GameData& gd = m.data();
        int& gold = m.state().gold;
        const auto power = [&](const ItemInstance& i) { return i.item > 0 ? gd.items[std::size_t(i.item)].base_damage(i.level) : 0; };
        int sword = power(p.inv.melee[0]), bow = power(p.inv.ranged);
        for (const ItemInstance& it : p.inv.bag)
            if (gd.items[std::size_t(it.item)].type == 9) sword = std::max(sword, power(it));
        std::vector<ShopStock> stock = shop_stock(m, m.ui().buy_shop);
        std::sort(stock.begin(), stock.end(), [&](const ShopStock& a, const ShopStock& b) { return gd.items[std::size_t(a.item)].price > gd.items[std::size_t(b.item)].price; });
        for (const ShopStock& s : stock) {
            const ItemDef& d = gd.items[std::size_t(s.item)];
            const int dmg = d.base_damage(s.level);
            const bool want = (d.usable_unit < 0 || d.usable_unit == p.cls) &&
                              ((d.type == 9 && dmg > sword) || (d.type == 0xb && p.inv.ranged.item && gd.items[std::size_t(p.inv.ranged.item)].type == 0xb && dmg > bow));
            for (int n = 0; s.quantity != 0 && (want ? n < 1 : d.item_class == "POTION" && n < (s.quantity < 0 ? 5 : s.quantity)) &&
                            gold - d.price >= kRecruitKeep && w.buy_item(p, s.item, s.level, 1);
                 ++n) {
                gold -= d.price;
                ap.log.push_back(std::to_string(m.ticks()) + " buy " + d.code + " gold " + std::to_string(gold));
                if (d.type == 9) sword = dmg;
                if (d.type == 0xb) {
                    bow = dmg;
                    for (std::size_t i = p.inv.bag.size(); i-- > 0;)
                        if (p.inv.bag[i].item == s.item) {
                            w.equip(p, int(i));
                            break;
                        }
                }
            }
        }
        if (ap.plan_talk) ++errand_, trip_tick_ = -1, ap.plan_talk = 0;
    }
    int errand_ = 0, trip_tick_ = -1;
    float trip_best_ = 0;

    // Troyes (walkthrough parts 4-6, tactic l.3783-4195): its walls fall only to the montagne du lion trebuchets.
    // 1. A Frenchman in S6L1_3_Join_Troop_Place west of Troyes adds six level-17 light infantry (team 7).
    // 2. The hill camp: no ENGLISH left in S6L1_4_Camp_Place makes both trebuchets French (+2).
    // 3. Three light infantry on each trebuchet's F points: the script deletes eight of Troyes' teams and orders the
    //    trebuchets at S6L1_3_Wall3, its eastern wall; at wall HP 0 they bombard the town (ATTACK_PLACE).
    // 4. Joan clears S6L1_3_Capture_Troyes_Place (+5, the territory French), 5. the king walks in after her (objective 2).
    bool troyes_stage(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (!is(m, p, "JOAN_OF_ARC") || !owned(m, p, "Auxerres") || done(m, 2)) return false;
        if (owned(m, p, "Troyes")) {
            ap.goal = "into Troyes with the king";
            ap.travel(f, p, kTroyesIn);
            return true;
        }
        if (!joined_) {
            for (const Unit& u : w.units()) joined_ |= alive(w, u) && u.team == 7 && is(m, u, "FRENCH_LIGHT_INFANTRY");
            if (!joined_) {
                ap.goal = "to the six light infantry";
                ap.travel(f, p, kJoinTroop);
                return true;
            }
        }
        // The king and the duke come to kSouthPark past the gate road before she leaves them for the hill (3 min at most).
        if (!wards_south_ && wards_wait_ < 0) wards_wait_ = ap.tick();
        if (!wards_south_ && ap.tick() - wards_wait_ < 5400) {
            ap.goal = "wait for the king at the south park";
            if (d2(p.pos, kSouthPark) > 600) ap.travel(f, p, kSouthPark);
            return true;
        }
        const Unit* treb[2] = {named(w, "Trebuchet"), named(w, "Trebuchet 2")};
        if (treb[0] && treb[0]->nation != p.nation) {
            if (hunt_in(ap, f, p, "S6L1_4_Camp_Place")) return true;
            ap.goal = "to the montagne du lion camp";
            ap.travel(f, p, kHillCamp);
            return true;
        }
        const MapItem* wall = m.objects().item(m.objects().find_by_name("S6L1_3_Wall3"));
        if (wall && wall->hp > 0) {
            crew_trebuchets(ap, p, treb);
            ap.goal = "by the trebuchets";
            if (treb[0] && d2(p.pos, treb[0]->pos) > 800) ap.travel(f, p, treb[0]->pos);
            return true;
        }
        return hunt_in(ap, f, p, "S6L1_3_Capture_Troyes_Place");
    }
    static constexpr Vec3 kJoinTroop{36650, 17430, 0}, kHillCamp{41092, 7504, 1000}, kTroyesIn{41427, 20831, 0};
    static constexpr Vec3 kSouthPark{36000, 16000, 0}, kAuxerrePark{28300, 24000, 0};
    bool joined_ = false, south_ = false, wards_south_ = false;
    int treb_tick_ = 0, wards_wait_ = -1, feed_tick_ = 0;

    // The wards eat in danger (AI heroes' auto-eat) only what they carry, and Charles starts with an empty bag: every
    // 10 s each with fewer than 3 food items gets Joan's (the character window's hand-over, World::transfer_item).
    void feed_wards(Autopilot& ap, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (ap.tick() < feed_tick_ || !is(m, p, "JOAN_OF_ARC")) return;
        feed_tick_ = ap.tick() + 300;
        const auto food = [&](const ItemInstance& it) { return it.item > 0 && m.data().items[std::size_t(it.item)].item_class == "POTION"; };
        for (const char* code : {"CHARLIE7", "ALENCON"}) {
            Unit* h = hero(w, code);
            if (!h || !w.inventory_reachable(*h)) continue;
            int have = 0;
            for (const ItemInstance& it : h->inv.bag) have += food(it);
            for (std::size_t i = p.inv.bag.size(); have < 3 && i-- > 0;)
                if (food(p.inv.bag[i]) && w.transfer_item(p, 10, int(i), *h)) ++have;
        }
    }

    // Light infantry not on a weapon (the six, Joan's) RTS-assigned to each French trebuchet's free F points, every 10 s
    // (Rts_cmd_assign 0x44f750 branch A; S7L1's trebuchet_crew).
    void crew_trebuchets(Autopilot& ap, Unit& p, const Unit* const (&treb)[2]) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (ap.tick() < treb_tick_) return;
        treb_tick_ = ap.tick() + 300;
        std::vector<const Unit*> free;
        for (const Unit& u : w.units())
            if (alive(w, u) && u.nation == p.nation && is(m, u, "FRENCH_LIGHT_INFANTRY") && !on_weapon(m, u)) free.push_back(&u);
        for (const Unit* t : treb) {
            if (!t || t->nation != p.nation || m.ai().all_attached(*t, 'F') != 0) continue;
            int need = 0;
            for (const auto& a : m.data().units[std::size_t(t->cls)].attach_points) need += (a.usage & 2) != 0;
            for (const Unit& u : w.units())
                need -= alive(w, u) && (u.m.vehicle == t->id || (m.ai().curr_task(u) == task::LARGE_WEAPON_PUSH && m.ai().curr_task_para(u, 1) == t->id));
            std::sort(free.begin(), free.end(), [&](const Unit* a, const Unit* b) { return d2(a->pos, t->pos) > d2(b->pos, t->pos); });
            std::vector<int> crew;
            while (need-- > 0 && !free.empty()) crew.push_back(free.back()->id), free.pop_back();
            if (crew.empty()) continue;
            rts(m, p, [&](AiSystem& ai) { ai.rts_assign(crew, t->id, true); });
            if (ap.trace) std::printf("[plan %d] %zu crew to %s\n", ap.tick(), crew.size(), t->name.c_str());
        }
    }

    // The nearest hostile inside the place Joan's walk reaches (or with a clear shot within 1500): shot when in reach,
    // walked to otherwise (S7L1's hunt_in); after_tick gives up on one that takes no damage for 30 s.
    bool hunt_in(Autopilot& ap, InputFrame& f, Unit& p, const char* place) {
        Mission& m = ap.mission();
        World& w = m.world();
        const int pr = m.places().place_by_name(place);
        const int here = ap.unit_region(p);
        const Unit* best = nullptr;
        float best_d = 1e30f;
        for (const Unit& u : w.units()) {
            if (!alive(w, u) || !w.hostile_to_player(u) || ap.unreached(u) || !m.places().inside(pr, u.pos)) continue;
            const UnitClass& c = m.data().units[std::size_t(u.cls)];
            if (c.unit_class == 0 || c.unit_class == 8 || c.unit_class == 9 || c.special == 2 || c.special == 3 || c.special == 7) continue;
            const float d = d2(p.pos, u.pos) + std::fabs(u.pos.z - p.pos.z) * 2;
            if (d < best_d && (ap.linked(here, ap.unit_region(u)) || (d2(p.pos, u.pos) < 1500 && ap.clear_shot(p, u)))) best_d = d, best = &u;
        }
        if (!best) return false;
        ap.goal = "hunt #" + std::to_string(best->id) + " " + m.data().units[std::size_t(best->cls)].code;
        if (d2(p.pos, best->pos) < 1200 && ap.arrows(p, false) > 0 && ap.clear_shot(p, *best)) ap.shoot(f, p, *best);
        else if (d2(p.pos, best->pos) < 200 && std::fabs(p.pos.z - best->pos.z) < 100) ap.fight(f, p, *best);
        else ap.travel(f, p, best->pos);
        return true;
    }
    int siege_tick_ = 0;
    // A hero with his team as the RTS click selects them, less his men on a siege weapon.
    static std::vector<int> squad(Mission& m, AiSystem& ai, int id) {
        std::vector<int> v = ai.rts_select_with_team(id);
        std::erase_if(v, [&](int u) { const Unit* x = m.world().unit(u); return x && on_weapon(m, *x); });
        return v;
    }

    // A fight / hunt target the player's blows and arrows never reach (a wall archer behind a merlon): 30 s on it without
    // its HP going down, it is given up on (Autopilot::give_up), as in the S7L1 plan.
    void after_tick(Autopilot& ap) override {
        int id = 0;
        for (const char* k : {"fight #", "hunt #", "boss #", "volley at #"})
            if (ap.goal.rfind(k, 0) == 0) id = std::atoi(ap.goal.c_str() + std::strlen(k));
        const Unit* u = ap.mission().world().unit(id);
        if (!u || id != stall_id_ || u->hp < stall_hp_) {
            stall_id_ = id, stall_hp_ = u ? u->hp : 0, stall_tick_ = ap.tick();
            return;
        }
        if (ap.tick() - stall_tick_ > 900) ap.give_up(id), stall_id_ = 0;
    }
    int stall_id_ = 0, stall_hp_ = 0, stall_tick_ = 0;
    int tower_foe_ = 0, spot_tick_ = -1, seen_tick_ = -1;
    Vec3 spot_{};
    std::vector<Vec3> tried_;
    std::map<int, int> no_spot_;   // stranded enemy -> tick before which he is not tried again
};

}  // namespace

std::unique_ptr<MissionPlan> make_plan_S6L1() { return std::make_unique<PlanS6L1>(); }

}  // namespace joa::game
