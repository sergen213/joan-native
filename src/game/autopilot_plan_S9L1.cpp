// Campaign mission 6 (S9L1, "The Sieges of Meung and Beaugency"): S9L1 continues in S5L1's world (Jargeau taken).
// Win (S5L1_des records, enabled by S9L1_tactic): 2 Meung, 9 every north village + Meung + Beaugency, 27 every south
// village + Jargeau, then 3 Beaugency (after Meung), 32 Talbot, 16 / 24 / 25 Glasdale, Fastolf, Scales. A place is
// liberated once Joan stands in it with no enemy left there (number_of_enemy_in_place; Meung / Beaugency count fightable
// enemies only); Meung, Loches and Montargis only once Jargeau is held, Janville, Patay and Tours once Beaugency is.
// The plan walks the bot to the nearest open place, hunts what stands in it, then stands inside.
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

#include "game/autopilot_player.hpp"
#include "game/multimap_nav.hpp"

namespace joa::game {

namespace {

float d2(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// need: place that must be captured first ("" none). fightable: the script counts fightable enemies only.
struct Cap { const char* place; const char* also; const char* need; bool fightable; };
constexpr Cap kCaps[] = {
    {"Town_Loches", nullptr, "Castle_Jargeau", false},     {"Settle_Montargis", nullptr, "Castle_Jargeau", false},
    {"Castle_Meung", nullptr, "Castle_Jargeau", true},     {"Town_St_Aignar", nullptr, nullptr, false},
    {"Town_Chateaudun", nullptr, nullptr, false},          {"Town_Chateauneut9", "Town_Chateauneut8", nullptr, false},
    {"Town_Checy", nullptr, nullptr, false},               {"Town_Rouvray", nullptr, nullptr, false},
    {"Settle_Fountainbleu", nullptr, nullptr, false},      {"Town_Saint", nullptr, nullptr, false},
    {"Settlement_Bourges", nullptr, nullptr, false},       {"Town_Blois", nullptr, nullptr, false},
    {"Castle_Beaugency", nullptr, "Castle_Meung", true},   {"Town_Javille", nullptr, "Castle_Beaugency", false},
    {"Town_Patay", nullptr, "Castle_Beaugency", false},    {"Town_Tours", nullptr, "Castle_Beaugency", false},
};

class PlanS9L1 final : public MissionPlan {
public:
    bool first(Autopilot& ap, InputFrame& f, Unit& p) override {
        ap.plan_talk = 0;  // set again by supplies() each tick it leads to a seller
        // Buy screen: explosive arrows for the towns' bombards (siege weapons take only stone / explosive hits: unit.res
        // TriggerType 0x6040), plain ones for the archers on walls and towers no path reaches, 1500 kept for food (buy_food
        // spends it). The spy (Elya Tinchor's 3000, S9L1_tactic l.4409) is never paid: 3000 kept for him starved the legit
        // chain's Normal Joan (~4500 gold) at Loches.
        ap.shop_explosive_max = kExplosive, ap.shop_plain_max = kPlain, ap.shop_gold_keep = 1500;
        ap.explosive_reserve = kExplosiveKeep;
        ap.gate_unlinked = true;  // Autopilot::travel: Orleans' shut east gate
        const Unit* b = bastard(ap.mission());
        guard_ = b && ap.mission().ai().rts_selectable(*b) ? std::optional<Vec3>(b->pos) : std::nullopt;
        bastard_id_ = b ? b->id : 0;
        army(ap, p), pick(ap, p);
        // Out of food: to the seller even mid-fight (the hunt's supplies() runs only between fights; the Normal start's Joan
        // fought Chateaudun's garrison down from 23 food to none and was killed, LOSE 5).
        return food(ap.mission().world(), p) < kFoodLow / 2 && supplies(ap, f, p) && trip_d_ > 150;  // near: the generic talk
    }
    std::optional<Vec3> guard_;  // the Bastard once he is the player's (his attackers are fought first)
    int bastard_id_ = 0;

    void shop(Autopilot& ap, Unit& p) override { buy_food(ap, p), buy_sword(ap, p); }

    void after_tick(Autopilot& ap) override {
        if (!ap.trace || ap.tick() % ap.trace) return;
        Mission& m = ap.mission();
        const Place* t = m.places().place(target_);
        int caps = 0;
        for (const Cap& c : kCaps) caps += m.places().place(m.places().place_by_name(c.place))->captured;
        std::printf("[plan %d] target %s captured %d/%zu gold %d food %d\n", ap.tick(), t ? t->name.c_str() : "-", caps, std::size(kCaps),
                    m.state().gold, food(m.world(), *m.world().unit(m.world().player_id())));
    }

    // Only the final camp (9, Janville: the bosses beaten at Beaugency regroup there) leads; 4 / 5 mark Meung / Beaugency
    // behind shut English gates all mission (the places below lead there), 8 the spy's house at Selles-en-Berry past
    // Beaugency (Meung falls to the Bastard's cannons instead), the others are side errands (Dunois, d'Illiers, the child,
    // Chateauneut).
    // The plan walks there itself (final_camp_): the generic marker walk has no level-seam detour and stood 50000 ticks east of
    // Beaugency where no path on that level leads to the camp.
    bool use_waypoint(Autopilot& ap, int slot, Vec3 at) override {
        if (slot == 9) final_camp_ = at, final_tick_ = ap.tick();
        return false;
    }
    Vec3 final_camp_{};
    int final_tick_ = -1;  // tick marker 9 was last seen shown

    float foe_weight(Autopilot& ap, const Unit& p, const Unit& u) override {
        Mission& m = ap.mission();
        const auto& c = m.data().units[std::size_t(u.cls)];
        if (c.special == 2) return 0;  // horses
        if (spy_walking(m) && in_box(m, "attackMeung", u.pos)) return 0;  // the spy only gets in with no French there
        const bool in = target_ && in_place(m, target_, u);
        if (c.unit_class == 8) return 0;  // siege weapons: shot with explosive arrows (before_hunt), a sword does nothing
        if (!walkable(ap, p, u)) return 0;  // tower tops: shot from a perch (before_hunt), not from wherever the fight is
        if (guard_ && d2(u.pos, *guard_) < 1500) return 0.1f;  // the Bastard's attackers first
        // Also before he is met (imperative 36): Easy lost at t245554 to a group that walked into his camp while Joan was
        // 5000 off at Bourges.
        if (bastard_id_ && u.target == bastard_id_) return 0.1f;
        // Bosses (objectives 32 / 16 / 24 / 25) first when near: weighted from afar, Talbot 15000 off at Janville pulled
        // Joan out of Meung's fight (the generic pick scales the distance by the weight).
        for (const char* boss : {"LORD_TALBOT", "WILLIAM_GLASDALE", "JOHN_FASTOLF", "LORD_SCALES"})
            if (c.code == boss && u.retreat != -1 && d2(p.pos, u.pos) < 1500) return 0.05f;
        return in ? 0.3f : 1.0f;
    }

    bool before_hunt(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        if (supplies(ap, f, p)) return true;
        // Marker 9 shown with Beaugency taken: the bosses beaten once at Beaugency (bit 19, event_2 bits 30 / 31, which its
        // capture needs) regroup at the final camp and are beaten there again (objectives 16 / 24 / 25); the fights there
        // are the generic ones. The camp lies behind Janville's gates, which open only with Beaugency's capture
        // (S9L1_tactic l.2845): before that the walk stood 50000 ticks at the shut gate.
        if (final_tick_ == ap.tick() && d2(p.pos, final_camp_) > 300 && captured(m, "Castle_Beaugency")) {
            ap.goal = "to the final camp";
            walk(ap, f, p, final_camp_);
            return true;
        }
        pick(ap, p);
        const Place* pl = m.places().place(target_);
        if (!pl) return meung(ap, f, p) || reinforce(ap, f, p);
        // An enemy still holding the place: hunt the nearest the bot can get at; the generic fight takes over in reach.
        // Siege weapons and the foes no path reaches (archers on walls and towers) are shot (shoot_foe).
        const Unit* foe = nullptr;
        const Unit* mark = nullptr;
        int left = 0;
        for (const Unit& u : w.units()) {
            if (!counts(m, cap(target_), u)) continue;
            ++left;
            const bool siege = m.data().units[std::size_t(u.cls)].unit_class == 8;
            if (!siege && !ap.unreached(u) && walkable(ap, p, u)) {
                if (!foe || d2(p.pos, u.pos) < d2(p.pos, foe->pos)) foe = &u;
            } else if (ap.tick() >= shot_skip_[u.id] && (ap.arrows(p, siege) > 0 || (!siege && ap.arrows(p, true) > 0) || tower(m, u)) && (!mark || mark->id != shot_id_) &&
                       (!mark || u.id == shot_id_ || d2(p.pos, u.pos) < d2(p.pos, mark->pos))) {
                mark = &u;  // the one being shot at kept (walking to the nearest's perch made another the nearest)
            }
        }
        if (foe) {
            // Headway: one foe fewer, or 200 closer to the nearest (the clock runs only while the hunt leads: fights and
            // errands in between restart it). Without it for 60 s (behind a wall the path mesh crosses) that foe is left to
            // the bow (Autopilot::give_up); for 3 min, the place is left (foes in turn out of reach).
            const float d = d2(p.pos, foe->pos);
            if (target_ != hunt_place_ || ap.tick() - hunt_seen_ > 300 || left < hunt_left_ || d < hunt_best_ - 200)
                hunt_place_ = target_, hunt_left_ = left, hunt_best_ = d, hunt_tick_ = hunt_given_ = ap.tick();
            hunt_seen_ = ap.tick();
            if (ap.tick() - hunt_tick_ > 5400) return skip(ap, "no headway to its foes"), false;
            if (ap.tick() - hunt_given_ > 1800) ap.give_up(foe->id), hunt_given_ = ap.tick();
            ap.goal = "capture " + pl->name + ": #" + std::to_string(foe->id) + " (" + std::to_string(left) + " left)";
            walk(ap, f, p, foe->pos);
            return true;
        }
        if (mark) {
            // Meung's tower archers (WALL tops only stone breaks here; the script deletes a team when its tower falls,
            // S9L1_tactic l.6560): the Bastard's cannons at that tower, else the bow.
            if (const int t = tower(m, *mark); t && guns(ap, f, p, t, "the tower")) return true;
            return shoot_foe(ap, f, p, *mark, left), true;
        }
        if (left) return skip(ap, "unreached"), false;
        const Vec3 at = stand(ap, *pl);
        if (m.places().inside(target_, p.pos)) {
            if (inside_since_ < 0) inside_since_ = ap.tick();
            if (ap.tick() - inside_since_ > 600) return skip(ap, "no capture"), false;  // 20 s inside: something else holds it
            ap.goal = "capture " + pl->name + ": inside";
            return true;
        }
        inside_since_ = -1;
        if (const float d = d2(p.pos, at); d < best_ - 300) best_ = d, headway_ = ap.tick();
        if (ap.tick() - headway_ > 2700) return skip(ap, "no headway"), false;  // 90 s
        ap.goal = "capture " + pl->name + ": walk";
        walk(ap, f, p, at);
        return true;
    }

private:
    int target_ = 0, inside_since_ = -1, headway_ = 0, next_army_ = 0;
    int hunt_place_ = 0, hunt_left_ = 0, hunt_tick_ = 0, hunt_given_ = 0, hunt_seen_ = -1000;  // hunt headway (before_hunt)
    float hunt_best_ = 0;
    // A foe the player can walk to: not on a nav island of a few polys no seam or ladder links to her region (Meung's tower
    // tops: 2-poly islands at z 521, no ladder item or .pfd link, as Auxerre's), which only the bow reaches. The region
    // test alone misjudged Blois' hilltop (another component, yet find_path leads up).
    bool walkable(Autopilot& ap, const Unit& p, const Unit& u) {
        if (std::fabs(u.pos.z - p.pos.z) < 150) return true;  // the ground beyond a shut gate: travel() opens it
        const int b = ap.unit_region(u);
        auto [it, fresh] = island_.try_emplace(b, false);
        if (fresh)
            if (const NavWorld* nav = ap.mission().nav(); nav && b >= 0) {
                const NavLevel& l = nav->levels()[std::size_t(b / 100000)];
                int n = 0;
                for (const NavPoly& q : l.mesh.polys()) n += q.component == b % 100000;
                it->second = n <= 4;
            }
        if (!it->second) return true;
        const int a = ap.unit_region(p);
        Vec3 end;
        bool up;
        return ap.linked(a, b) || ap.ladder_to(a, b, end, up);
    }
    std::map<int, bool> island_;  // region -> a nav island of 4 polys or fewer

    // Walk to g. The engine plans a goal on another level over the coarse world mesh (from Orleans straight over the
    // Loire) and travel() only detours a path that ends short: without headway for 20 s, by the level-seam route
    // (Autopilot::detour) to its first crossing, then on.
    void walk(Autopilot& ap, InputFrame& f, const Unit& p, Vec3 g) {
        if (d2(g, walk_goal_) > 500) walk_goal_ = g, walk_best_ = 1e30f, walk_headway_ = ap.tick(), walk_via_ = false;
        if (const float d = d2(p.pos, g); d < walk_best_ - 300) walk_best_ = d, walk_headway_ = ap.tick();
        if (ap.tick() - walk_headway_ > 600) {
            walk_headway_ = ap.tick(), walk_best_ = 1e30f, walk_via_ = false;
            if (const Autopilot::Seam* s = ap.detour(p, g)) walk_via_ = true, via_ = s->at;
        }
        if (walk_via_ && d2(p.pos, via_) < 100) walk_via_ = false;
        if (walk_via_) ap.goal += " via " + std::to_string(int(via_.x)) + "," + std::to_string(int(via_.y));
        ap.travel(f, p, walk_via_ ? via_ : g);
    }
    Vec3 walk_goal_{}, via_{};
    float walk_best_ = 1e30f;
    int walk_headway_ = 0;
    bool walk_via_ = false;  // walking to the detour crossing via_

    // Meung's gates (English; north 1500, south 3000 HP) take only La Hire's melee, explosives (~5 per arrow for Joan) and
    // siege shots. The Bastard's four cannons are the player's to command via RTS once Joan meets him in meetOrlean (the
    // cut scene sets event bit_3 11 and clears their control flags): with no other place open, Joan walks there, then
    // sends every crewed cannon at the north gate (Rts_cmd_attack_item 0x44f1f0; re-given every 20 s, a weapon under fire
    // drops it) and stands by them. pick() opens Meung once the gate is down.
    bool meung(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        Places& ps = m.places();
        const Place* mp = ps.place(ps.place_by_name("Castle_Meung"));
        const Place* jp = ps.place(ps.place_by_name("Castle_Jargeau"));
        const int gate = m.objects().find_by_name("MeungNorthGate");
        if (!mp || mp->captured || !jp || !jp->captured || gate_down(m, "MeungNorthGate") || gate_down(m, "MeungSouthGate")) return false;
        if (!((m.vm().state().bits[3] >> 10) & 1u)) {  // event bit_3 11: the Bastard met
            if (supplies(ap, f, p, true)) return true;
            const Place* meet = ps.place(ps.place_by_name("meetOrlean"));
            if (!meet) return false;
            ap.goal = "Meung: meet the Bastard";
            walk(ap, f, p, {(meet->box_min.x + meet->box_max.x) / 2, (meet->box_min.y + meet->box_max.y) / 2, p.pos.z});
            return true;
        }
        return guns(ap, f, p, gate, "the north gate");
    }
    // Every crewed cannon of the Bastard's at map item `item` (Rts_cmd_attack_item 0x44f1f0; re-given every 20 s, a weapon
    // under fire drops it); Joan stays by the guns. A crew walks to its attach point only near the player: MOVE_TO_TARGET
    // ends within REACH (x2 at LOD > 1, beyond 2000 of the player) while attaching needs REACH_DT (0x535020), so a crew 23
    // off its point far from Joan never mans the cannon (rts_selectable needs every point manned). False: no cannon left.
    bool guns(Autopilot& ap, InputFrame& f, Unit& p, int item, const char* what) {
        Mission& m = ap.mission();
        World& w = m.world();
        std::vector<int> ids;
        Vec3 at{};  // the cannon nearest the player (a centroid of guns on two levels lies on neither)
        int all = 0;
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && u.nation == p.nation && m.data().units[std::size_t(u.cls)].unit_class == 8 && u.name.starts_with("fCannon")) {
                if (!all++ || d2(p.pos, u.pos) < d2(p.pos, at)) at = u.pos;
                if (m.ai().rts_selectable(u)) ids.push_back(u.id);
            }
        const MapItem* g = m.objects().item(item);
        ap.goal = "Meung: " + std::to_string(ids.size()) + "/" + std::to_string(all) + " cannons at " + what + " (hp " + std::to_string(g ? g->hp : 0) + ")";
        if (!all) return false;
        if (ap.tick() >= gun_tick_ && !ids.empty()) {
            gun_tick_ = ap.tick() + 600;
            std::erase_if(ids, [&](int id) { return m.ai().curr_task(*w.unit(id)) == task::RTS_ATTACK_ITEM && m.ai().curr_task_para(*w.unit(id), 1) == item; });
            if (!ids.empty()) rts(m, p, [&](AiSystem& a) { a.rts_attack_item(ids, item); });
        }
        if (d2(p.pos, at) > 400) walk(ap, f, p, at);
        return true;
    }
    int gun_tick_ = 0;
    // The standing Meung tower under u (MeungRightTower / MeungLeftTower, WALL 200 HP) while a crewed cannon is the
    // player's; 0 otherwise.
    static int tower(Mission& m, const Unit& u) {
        const World& w = m.world();
        if (std::none_of(w.units().begin(), w.units().end(), [&](const Unit& c) {
                return !c.deleted && !w.dead(c) && c.name.starts_with("fCannon") && m.data().units[std::size_t(c.cls)].unit_class == 8 && m.ai().rts_selectable(c);
            }))
            return 0;
        for (const char* name : {"MeungRightTower", "MeungLeftTower"})
            if (const int r = m.objects().find_by_name(name); r)
                if (const MapItem* it = m.objects().item(r); it && it->hp > 0 && d2(it->pos, u.pos) < 600) return r;
        return 0;
    }

    // Beaugency: once Meung is taken a Frenchman in getReinforcement (by its north gate) brings Glasdale, Fastolf and
    // Scales with their troops (scenario bit 1; they follow the player) and Richemont; Glasdale's cut scene raises the
    // north gate. Each boss beaten once retreats to Janville (bit 19, event_2 bits 30 / 31: Beaugency's capture needs all
    // three) and is beaten there again for objectives 16 / 24 / 25. With no place open, Joan walks into getReinforcement.
    bool reinforce(Autopilot& ap, InputFrame& f, const Unit& p) {
        Mission& m = ap.mission();
        Places& ps = m.places();
        const Place* mp = ps.place(ps.place_by_name("Castle_Meung"));
        const Place* r = ps.place(ps.place_by_name("getReinforcement"));
        if (!mp || !mp->captured || !r || (m.vm().state().bits[0] & 1u)) return false;
        ap.goal = "Beaugency: to getReinforcement";
        walk(ap, f, p, {(r->box_min.x + r->box_max.x) / 2, (r->box_min.y + r->box_max.y) / 2, p.pos.z});
        return true;
    }

    // A siege weapon (explosive arrows) or a foe no path reaches (plain arrows) holding the place: from a perch (perch()),
    // then the bow. A perch where 15 s of shots do no harm (the prediction is marginal: the shooter's pose moves the launch
    // point) is dropped for the next; one with no perch left is left for 5 min.
    void shoot_foe(Autopilot& ap, InputFrame& f, Unit& p, const Unit& u, int left) {
        World& w = ap.mission().world();
        const auto& c = w.data().units[std::size_t(u.cls)];
        const bool siege = c.unit_class == 8, expl = siege || ap.arrows(p, false) == 0;  // explosive at an archer with no plain left
        shot_id_ = u.id;
        const Vec3 aim{u.pos.x, u.pos.y, u.pos.z + float(c.height) * (siege ? 0.5f : 0.8f)};
        Perch& pc = perch(ap, p, u, aim, expl);
        if (pc.i >= pc.spots.size()) {
            shot_skip_[u.id] = ap.tick() + 9000, shot_id_ = 0, perch_.erase(u.id);
            if (ap.trace) std::printf("[plan %d] leave #%d %s (hp %d)\n", ap.tick(), u.id, c.code.c_str(), u.hp);
            return;
        }
        const Vec3 at = pc.spots[pc.i];
        ap.goal = "capture " + ap.mission().places().place(target_)->name + ": shoot #" + std::to_string(u.id) + " " + c.code + " (" +
                  std::to_string(left) + " left) from " + std::to_string(int(at.x)) + "," + std::to_string(int(at.y));
        if (d2(p.pos, at) > 60) {
            if (pc.since < 0) pc.since = ap.tick();  // walking there: 60 s at most
            if (ap.tick() - pc.since > 1800) ++pc.i, pc.since = -1;
            pc.at = -1;
            return walk(ap, f, p, at);
        }
        // Released only while the arrow's flight from here is clear: the archers pace their tower top.
        const float face = std::atan2(u.pos.y - p.pos.y, u.pos.x - p.pos.x);
        const Vec3 p0{p.pos.x + std::cos(face) * 48, p.pos.y + std::sin(face) * 48, p.pos.z + 72};  // as perch()
        const bool clear = lob_margin(ap.mission(), expl, p0, aim) >= 0.05f;
        if (pc.at < 0 || u.hp < pc.hp) pc.at = ap.tick(), pc.hp = u.hp, pc.since = -1;
        if (!clear) {  // waiting for an opening (the 15 s run only while it is clear), 30 s at most
            pc.at = std::min(ap.tick(), pc.at + 1);
            if (pc.since < 0) pc.since = ap.tick();
        } else {
            pc.since = -1;
        }
        if (ap.tick() - pc.at > 450 || (pc.since >= 0 && ap.tick() - pc.since > 900)) {
            if (ap.trace) std::printf("[plan %d] no harm to #%d from (%.0f,%.0f,%.0f)\n", ap.tick(), u.id, at.x, at.y, at.z);
            ++pc.i, pc.at = pc.since = -1;
            return;
        }
        ap.load_ammo(p, expl);
        ap.shoot_at(f, p, aim, p.action == SHTIDLE && clear);
    }
    int shot_id_ = 0;
    std::map<int, int> shot_skip_;  // foe -> tick it is shot at again

    // Where to shoot u from: walkable points within 1600 whose locked arrow (lob_margin) beats the structures by 0.1 s, the
    // nearest the player first. Meung's tower tops (2-poly nav islands at z 521, no ladder or .pfd link; the towers are
    // WALLs only stone breaks in this scenario, Wall_take_ranged 0x492b10) have a parapet up to 664 with 570..650
    // embrasures: the archers (80 tall) are hit through those or by lobs (800+ off) dropping in. Kept while u stays put.
    struct Perch {
        Vec3 from{};
        std::vector<Vec3> spots;
        std::size_t i = 0;
        int at = -1, hp = 0, since = -1, built = 0;  // tick reached / u's hp then; tick the walk there began; tick computed
    };
    Perch& perch(Autopilot& ap, const Unit& p, const Unit& u, Vec3 aim, bool explosive) {
        Perch& pc = perch_[u.id];
        // Kept while u stays put, and for 10 s while it walks (each search is ~4000 structure-ray lobs: a pacing archer
        // recomputed it every few ticks).
        if (!pc.spots.empty() && ((d2(pc.from, u.pos) < 50 && std::fabs(pc.from.z - u.pos.z) < 50) || ap.tick() - pc.built < 300)) return pc;
        pc = Perch{}, pc.from = u.pos, pc.built = ap.tick();
        Mission& m = ap.mission();
        const NavWorld* nav = m.nav();
        if (!nav) return pc;
        const int here = ap.unit_region(p);
        // The launch point (World::launch_point: the ammo's bone) in the draw pose, relative to the feet: 48 forward, 72 up
        // (Joan, measured at release). ponytail: Joan's bow pose only; read the SHTIDLE bone if another archer leads.
        const float fwd = 48, side = 0, up = 72;
        std::vector<std::pair<float, Vec3>> cand;
        for (float x = u.pos.x - 1600; x <= u.pos.x + 1600; x += 50)
            for (float y = u.pos.y - 1600; y <= u.pos.y + 1600; y += 50) {
                if (std::hypot(x - u.pos.x, y - u.pos.y) > 1600) continue;
                for (const float z0 : {0.0f, 250.0f, 500.0f, 750.0f, 1000.0f}) {
                    const int poly = nav->find_poly_xy(x, y, z0);
                    if (poly < 0) continue;
                    const Vec3 q{x, y, nav->poly_height(poly, x, y)};
                    if (std::fabs(q.z - z0) > 125) continue;
                    const float face = std::atan2(u.pos.y - q.y, u.pos.x - q.x), c = std::cos(face), s = std::sin(face);
                    const Vec3 p0{q.x + c * fwd - s * side, q.y + s * fwd + c * side, q.z + up};
                    if (lob_margin(m, explosive, p0, aim) >= 0.1f && ap.linked(here, ap.region(q))) cand.push_back({d2(q, p.pos), q});
                }
            }
        std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& c : cand) pc.spots.push_back(c.second);
        if (ap.trace) std::printf("[plan %d] %zu perches for #%d\n", ap.tick(), pc.spots.size(), u.id);
        return pc;
    }
    // How much later (s) than its arrival at `t` a locked arrow from p0 (World::spawn_projectile: solve_aim, lobbed at 800+)
    // would first be stopped by a structure: Physics_sweep_structures 0x693750 casts a FT-ms ray along (vx, vy, vz - V0)
    // from frame starts (again once FT has passed) and stops the arrow at its predicted hit. Negative: blocked.
    static float lob_margin(Mission& m, bool explosive, Vec3 p0, Vec3 t) {
        const ThrowConsts& tc = m.data().throwi[explosive ? 2 : 1];
        const float K = World::kUnitsPerMetre, g = tc.gl * 9.81f, T = tc.ft * 0.001f, dt = World::kTickMs * 0.001f;
        const float d = std::sqrt((t.x - p0.x) * (t.x - p0.x) + (t.y - p0.y) * (t.y - p0.y) + (t.z - p0.z) * (t.z - p0.z));
        if (d > tc.mxd) return -1;
        const Vec3 f = solve_aim(p0, t, tc.m, tc.mxd, tc.av, tc.gl, 0.25f);
        const Vec3 v{f.x / (tc.m * 1000), f.y / (tc.m * 1000), f.z / (tc.m * 1000)};
        const float vh = std::hypot(v.x, v.y) * K;
        if (vh <= 0) return -1;
        const float tt = std::hypot(t.x - p0.x, t.y - p0.y) / vh;
        float margin = 1;
        for (float s = 0; s < tt;) {
            const Vec3 a{p0.x + v.x * s * K, p0.y + v.y * s * K, p0.z + (v.z * s - g * s * s / 2) * K}, r{v.x, v.y, v.z - g * s - tc.v0};
            if (const StructureHit h = m.objects().structure_ray(0, a, {a.x + r.x * T * K, a.y + r.y * T * K, a.z + r.z * T * K}); h.item)
                margin = std::min(margin, s + T * h.t - tt);
            s = std::max(dt, dt * std::ceil((s + T - dt) / dt - 1e-4f));
        }
        return margin;
    }
    std::map<int, Perch> perch_;  // foe -> its perches

    // Supplies: below kFoodLow food or kArrowsLow arrows of either kind (with the gold for a loaf), the
    // nearest French seller within kTripMax that a walk reaches (linked regions: from inside Beaugency, before its capture
    // opens the gates, every trip to Meung's sellers stood at the shut south gate); talking opens the buy screen (Autopilot::shop
    // + shop()). `stock_up` (before Meung: its walls hold ~40 archers and crossbowmen, its shops open only once it is
    // taken) asks for half the stock. A seller visited or walked no closer to for 100 s is left for 5 min. A seller with 4+
    // English within 2000 is no seller (Normal: out of food Joan walked into Orleans' Luxembourg host and was killed); out of
    // food (< kFoodLow / 3) one twice as far is worth the walk.
    bool supplies(Autopilot& ap, InputFrame& f, const Unit& p, bool stock_up = false) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (!is_joan(m, p) || m.state().gold < 400) return false;
        const int arrows = stock_up ? kPlain / 2 : kArrowsLow, bombs = stock_up ? kExplosive / 2 : kArrowsLow;
        const bool ammo = p.inv.ranged.item && (ap.arrows(p, false) < arrows || ap.arrows(p, true) < bombs);
        const bool eat = food(w, p) < (stock_up ? kFood / 2 : kFoodLow);
        if (!ammo && !eat) return false;
        const float range = food(w, p) < kFoodLow / 3 ? 2 * kTripMax : kTripMax;
        const auto crowded = [&](const Unit& s) {
            int n = 0;
            for (const Unit& u : w.units()) n += !u.deleted && !w.dead(u) && w.hostile_to_player(u) && d2(u.pos, s.pos) < 2000;
            return n >= 4;
        };
        const Unit* s = nullptr;
        for (const Unit& u : w.units()) {
            const std::string& code = m.data().units[std::size_t(u.cls)].code;
            const bool weapons = code.starts_with("WEAPON_SHOPKEEPER"), potions = code.starts_with("POTION_SHOPKEEPER");
            if ((weapons && ammo) || (potions && eat))
                if (!u.deleted && u.m.can_interact && u.nation == p.nation && ap.tick() >= trip_skip_[u.id] && d2(p.pos, u.pos) < range &&
                    ap.linked(ap.unit_region(p), ap.unit_region(u)) && (!s || d2(p.pos, u.pos) < d2(p.pos, s->pos)) && !crowded(u))
                    s = &u;
        }
        if (!s) return false;
        const float d = trip_d_ = d2(p.pos, s->pos);
        if (s->id != trip_id_ || d < trip_best_ - 200) trip_id_ = s->id, trip_best_ = d, trip_tick_ = ap.tick();
        if (ap.tick() - trip_tick_ > 3000) {
            trip_skip_[s->id] = ap.tick() + 9000, trip_id_ = 0;
            return false;
        }
        ap.goal = "to the shop of " + s->name;
        ap.plan_talk = s->id;
        // Bag room before the buy screen opens (a purchase needs a free cell or stack room, FUN_006c5a00; one cell per loaf,
        // the loot fills the bag with apples): 30 apples and a crossbow, then 26 loaves scattered over the personal grid,
        // left no 1x4 column for plain arrows, and each failed visit was walked again 5 min later.
        // Arrows (a 1x4 column) also lost to the loot's food and gems: Easy stood at Janville for 100000+ ticks with no
        // arrow for its last two wall archers, the personal grid full of 41 food and 20 ENHANCER gems (unused by the bot).
        if (d < 400) {
            Unit& me = *w.unit(p.id);
            for (int i = int(me.inv.bag.size()), meals = 0; ammo && i-- > 0;)
                if (const ItemDef& it = m.data().items[std::size_t(me.inv.bag[std::size_t(i)].item)];
                    it.item_class == "ENHANCER" || ((it.type == 0x15 || it.type == 0x19) && ++meals > kFood))
                    ap.discard(me, i);
            ap.make_room(me);
        }
        walk(ap, f, p, s->pos);
        return true;
    }
    static constexpr int kFoodLow = 12, kFood = 30, kArrowsLow = 8, kExplosive = 40, kPlain = 300, kExplosiveKeep = 12;
    static constexpr float kTripMax = 20000;  // farther sellers (Jargeau's, Chateauneuf's from Beaugency: 55000) cost more than the stock
    int trip_id_ = 0, trip_tick_ = 0;
    float trip_best_ = 0, trip_d_ = 0;  // trip_d_: distance to the seller supplies() last walked to
    std::map<int, int> trip_skip_;  // seller -> tick it is walked to again

    // A sharper sword for Joan than the one in hand (Jargeau sells JOAN_SWORD_LV5); character_screen() puts it in hand.
    void buy_sword(Autopilot& ap, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        trip_skip_[trip_id_] = ap.tick() + 9000;
        const ItemInstance& hand = p.inv.melee[0];
        int best = hand.item ? w.data().items[std::size_t(hand.item)].base_damage(hand.level) : 0;
        for (const ItemInstance& it : p.inv.bag)
            if (const ItemDef& d = w.data().items[std::size_t(it.item)]; d.type == 9) best = std::max(best, d.base_damage(it.level));
        for (const ShopStock& s : shop_stock(m, m.ui().buy_shop)) {
            const ItemDef& d = w.data().items[std::size_t(s.item)];
            if (d.type == 9 && (d.usable_unit < 0 || d.usable_unit == p.cls) && d.base_damage(s.level) > best && s.quantity != 0 &&
                m.state().gold >= d.price && w.buy_item(p, s.item, s.level, 1)) {
                m.state().gold -= d.price, best = d.base_damage(s.level);
                ap.log.push_back(std::to_string(m.ticks()) + " buy " + d.code + " gold " + std::to_string(m.state().gold));
            }
        }
    }

    // The army. The test start leaves Joan alone at Jargeau and the heroes with their squads at Orleans; units in a map
    // level the player is not near are deactivated (NavWorld::stream, Unit_deactivate 0x41fcc0) and stand still whatever
    // their orders, and the march between the two (over the eastern bridge and through the English villages) killed the
    // hero leading it each time. A place is liberated by the player's hero standing in it (search_for_unit_in_place "P"),
    // so the player switches from Joan to the sturdiest hero (Mission::switch_hero, the hero keys) and plays the mission
    // from Orleans; Joan waits in Jargeau. Every other commandable hero (and the Bastard once selectable) within 4000 is
    // assigned with his squad to follow the player, as the walkthrough's "assign everyone to follow Joan" (RTS: select the
    // hero with his team, right-click the leader: Rts_cmd_assign 0x44f750 branch B, FOLLOW_UNIT). Every hero must live
    // (imperatives 14 / 17 / 30 / 36, S9L1_finish: game over): one below kSturdy max HP stays home (Alencon, 106 HP with
    // award experience only, died in the first fight east of Orleans), one below 35 % HP is RTS-moved with his squad back
    // to where the plan first saw him (the camp at Orleans; 1500 straight away from the nearest enemy when already
    // there) and left there until back above 80 % (stepping only 1500 away, the foes ran after him). Orders are given
    // between fights (Rts_init stops the player).
    void army(Autopilot& ap, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (ap.tick() < next_army_ || m.ui().game_cut_scene || w.cut_scene || w.dead(p) || ap.hero_key(p) == 0) return;
        next_army_ = ap.tick() + 30;
        AiSystem& ai = m.ai();
        // The Bastard (imperative 36; 147 HP) is the player's to command once met: walked into Meung's fights he was killed
        // by its wall archers. He and his team are kept at his camp (meetOrlean); Joan takes on the foes near him first
        // (foe_weight).
        if (Unit* b = bastard(m); b && ai.rts_selectable(*b) && d2(b->pos, bastard_home(m)) > 400 && ai.curr_task(*b) != task::RTS_MOVE_TO)
            rts(m, p, [&](AiSystem& a) { a.rts_move(a.rts_select_with_team(b->id), bastard_home(m), false); });
        for (Unit& h : w.units()) {  // the hurt fall back (also mid-fight)
            if (!commandable(m, p, h) || is_joan(m, h)) continue;
            const Vec3 home = camp_.try_emplace(h.id, h.pos).first->second;
            const int max = w.max_hp(h);
            if (h.hp * 100 >= max * 80) resting_.erase(h.id);
            if (h.hp * 100 >= max * 35) continue;
            const Unit* foe = nullptr;
            for (const Unit& u : w.units())
                if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && (!foe || d2(u.pos, h.pos) < d2(foe->pos, h.pos))) foe = &u;
            if (!foe || d2(foe->pos, h.pos) > 1500 || (resting_.count(h.id) && ai.curr_task(h) == task::RTS_MOVE_TO)) continue;
            const float d = std::max(1.0f, d2(foe->pos, h.pos));
            const Vec3 to = d2(home, h.pos) > 1500 ? home
                                                   : Vec3{h.pos.x + (h.pos.x - foe->pos.x) / d * 1500, h.pos.y + (h.pos.y - foe->pos.y) / d * 1500, h.pos.z};
            bool ok = false;
            rts(m, p, [&](AiSystem& a) { ok = a.rts_move(a.rts_select_with_team(h.id), to, false); });
            if (!ok) rts(m, p, [&](AiSystem& a) { ok = a.rts_move(a.rts_select_with_team(h.id), p.pos, false); });
            resting_.insert(h.id);
            if (ap.trace) std::printf("[plan %d] %s falls back (hp %d/%d)\n", ap.tick(), h.name.c_str(), h.hp, max);
        }
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && d2(u.pos, p.pos) < 600) return;
        // A Joan sturdier than every hero (a played campaign's: level 21, 640 HP against their 106-206) plays alone: Jean
        // following her into Saint-Aignan's fights was killed (imperative 17).
        const Unit* best = nullptr;
        for (Unit& h : w.units())
            if (commandable(m, p, h) && !is_joan(m, h) && w.max_hp(h) >= kSturdy && !resting_.count(h.id) && ap.hero_key(h) && (!best || h.hp > best->hp))
                best = &h;
        if (is_joan(m, p)) {
            if (best && w.max_hp(*best) > w.max_hp(p) && m.switch_hero(ap.hero_key(*best)) && ap.trace)
                std::printf("[plan %d] switch to %s to lead the army\n", ap.tick(), best->name.c_str());
            return;
        }
        for (Unit& h : w.units()) {
            if (!commandable(m, p, h) || is_joan(m, h) || w.max_hp(h) < kSturdy || resting_.count(h.id)) continue;
            if (d2(h.pos, p.pos) <= kRally && (ai.curr_task(h) != task::FOLLOW_UNIT || ai.curr_task_para(h, 1) != p.id)) {
                rts(m, p, [&](AiSystem& a) { a.rts_assign(a.rts_select_with_team(h.id), p.id, false); });
                if (ap.trace) std::printf("[plan %d] %s follows %s\n", ap.tick(), h.name.c_str(), p.name.c_str());
            }
        }
    }
    static constexpr float kRally = 4000;
    static constexpr int kSturdy = 150;
    std::set<int> resting_;  // heroes sent back hurt, until healed
    std::map<int, Vec3> camp_;  // hero -> where the plan first saw him
    static bool commandable(Mission& m, const Unit& p, const Unit& h) {
        const World& w = m.world();
        return !h.deleted && !w.dead(h) && h.id != p.id && h.nation == p.nation && h.rank >= 4 && m.data().units[std::size_t(h.cls)].hero &&
               m.ai().rts_selectable(h);
    }
    static bool is_joan(Mission& m, const Unit& u) { return m.data().units[std::size_t(u.cls)].code == "JOAN_OF_ARC"; }
    static Unit* bastard(Mission& m) {
        for (Unit& u : m.world().units())
            if (!u.deleted && !m.world().dead(u) && m.data().units[std::size_t(u.cls)].code == "ORLEAN") return &u;
        return nullptr;
    }
    static Vec3 bastard_home(Mission& m) {
        const Place* c = m.places().place(m.places().place_by_name("meetOrlean"));
        return c ? Vec3{(c->box_min.x + c->box_max.x) / 2, (c->box_min.y + c->box_max.y) / 2, 0} : Vec3{};
    }
    // One RTS round trip (Session::toggle_rts in and out) around `orders`, as the S6L1 plan.
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
    float best_ = 1e30f;
    std::map<int, int> skip_;    // place -> tick it is tried again
    int picker_ = 0;             // the player's hero target_ was picked for
    std::map<int, Vec3> stand_;  // place -> walkable point inside it

    static const Cap& cap(int place) { return kCaps[std::size_t(index_[place])]; }
    static inline std::map<int, int> index_;  // place recno -> kCaps index

    static bool in_place(Mission& m, int place, const Unit& u) {
        const Cap& c = cap(place);
        return m.places().inside(place, u.pos) || (c.also && m.places().inside(m.places().place_by_name(c.also), u.pos));
    }
    // An enemy the script counts in the place (number_of_(fightable_)enemy_in_place: alive in the box, hostile nation).
    bool counts(Mission& m, const Cap& c, const Unit& u) const {
        const World& w = m.world();
        if (u.deleted || (u.hp < 1 && u.lives == 0) || w.dead(u) || u.nation == 0 || !w.hostile_to_player(u) || u.retreat == -1) return false;
        if (!in_place(m, target_, u)) return false;
        const auto& uc = m.data().units[std::size_t(u.cls)];
        return !c.fightable || !(uc.unit_class == 0 || uc.unit_class == 8 || uc.unit_class == 9 || uc.special == 2 || uc.special == 3 || uc.special == 7);
    }
    void skip(Autopilot& ap, const char* why) {
        skip_[target_] = ap.tick() + 9000;  // 5 min
        if (ap.trace) std::printf("[plan %d] skip %s: %s\n", ap.tick(), ap.mission().places().place(target_)->name.c_str(), why);
        target_ = 0;
    }
    // Nearest open place to the player's hero (picked again when the player switches hero): not captured, its prerequisite
    // captured, not skipped.
    void pick(Autopilot& ap, const Unit& p) {
        Places& ps = ap.mission().places();
        if (const Place* t = ps.place(target_); t && !t->captured && picker_ == p.id) return;
        target_ = 0, picker_ = p.id;
        float best = 1e30f;
        for (std::size_t i = 0; i < std::size(kCaps); ++i) {
            const int r = ps.place_by_name(kCaps[i].place);
            const Place* pl = ps.place(r);
            if (!pl || pl->captured || ap.tick() < skip_[r]) continue;
            if (kCaps[i].need)
                if (const Place* n = ps.place(ps.place_by_name(kCaps[i].need)); !n || !n->captured) continue;
            // Meung: once one of its gates is down (the cannons, meung()); one the English open for a sortie shuts behind the
            // player, who was then caught inside or out (the rest of the plan walks about outside its walls).
            if (std::string_view(kCaps[i].place) == "Castle_Meung" && !gate_down(ap.mission(), "MeungSouthGate") &&
                !gate_down(ap.mission(), "MeungNorthGate"))
                continue;
            index_[r] = int(i);
            const Vec3 c{(pl->box_min.x + pl->box_max.x) / 2, (pl->box_min.y + pl->box_max.y) / 2, p.pos.z};
            if (const float d = d2(p.pos, c); d < best) best = d, target_ = r;
        }
        best_ = 1e30f, headway_ = ap.tick(), inside_since_ = -1;
        if (target_ && ap.trace) std::printf("[plan %d] target %s\n", ap.tick(), ps.place(target_)->name.c_str());
    }
    // A walkable point inside the place box (on the mesh, within its height range) linked to the player's region, nearest its
    // centre.
    Vec3 stand(Autopilot& ap, const Place& pl) {
        if (const auto s = stand_.find(pl.recno); s != stand_.end()) return s->second;
        const NavWorld* nav = ap.mission().nav();
        const Vec3 c{(pl.box_min.x + pl.box_max.x) / 2, (pl.box_min.y + pl.box_max.y) / 2, (pl.box_min.z + pl.box_max.z) / 2};
        const int here = ap.region(ap.mission().world().unit(ap.mission().world().player_id())->pos);
        Vec3 best = c;
        float bd = 1e30f;
        for (int i = 1; i < 12 && nav; ++i)
            for (int j = 1; j < 12; ++j) {
                const float x = pl.box_min.x + (pl.box_max.x - pl.box_min.x) * float(i) / 12, y = pl.box_min.y + (pl.box_max.y - pl.box_min.y) * float(j) / 12;
                const int poly = nav->find_poly_xy(x, y, c.z);
                if (poly < 0) continue;
                const Vec3 q{x, y, nav->poly_height(poly, x, y)};
                if (q.z < pl.box_min.z || q.z > pl.box_max.z || !ap.linked(here, ap.region(q))) continue;
                if (const float d = d2(q, c); d < bd) bd = d, best = q;
            }
        return stand_[pl.recno] = best;
    }
    static bool captured(Mission& m, std::string_view place) {
        const Place* pl = m.places().place(m.places().place_by_name(place));
        return pl && pl->captured;
    }
    static bool gate_down(Mission& m, std::string_view name) {
        for (int r = 1; r <= m.objects().size(); ++r)
            if (const MapItem* it = m.objects().item(r); it && it->cls == 1 && it->name == name) return it->hp <= 0;
        return true;  // deleted (destroyed)
    }
    static bool in_box(Mission& m, std::string_view place, Vec3 q) { return m.places().inside(m.places().place_by_name(place), q); }
    // The paid spy (Elya Tinchor, ENGLISH_SPY of the player's nation) on his way to Meung's south gate.
    static bool spy_walking(Mission& m) {
        const World& w = m.world();
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && !u.m.can_interact && u.nation == w.unit(w.player_id())->nation &&
                m.data().units[std::size_t(u.cls)].code == "ENGLISH_SPY")
                return true;
        return false;
    }
    static int food(const World& w, const Unit& p) {  // F3 food (item types 0x15 / 0x19), as the bot counts it
        int n = 0;
        for (const ItemInstance& it : p.inv.bag)
            if (const int t = w.data().items[std::size_t(it.item)].type; t == 0x15 || t == 0x19) n += it.quantity;
        return n;
    }
    // Buy screen open (a shop the generic bot talks to): food (types 0x15 / 0x19) up to kFood
    // (the click handler's purchase, as Autopilot::shop makes it). Only Joan goes shopping (supplies()): the award-only
    // heroes' food trips (Jean to Jargeau's shop across the English villages, to Orleans' that left him on its wall walkway
    // with no way down for 55000 ticks) cost the run.
    void buy_food(Autopilot& ap, Unit& p) {
        Mission& m = ap.mission();
        if (food_ms_ == m.ui().interface_ms) return;
        food_ms_ = m.ui().interface_ms;
        World& w = m.world();
        int& gold = m.state().gold;
        for (const ShopStock& s : shop_stock(m, m.ui().buy_shop)) {
            const ItemDef& d = w.data().items[std::size_t(s.item)];
            if (d.type != 0x15 && d.type != 0x19) continue;
            while (s.quantity != 0 && food(w, p) < kFood && gold >= d.price && w.buy_item(p, s.item, s.level, std::max(1, d.quantity)))
                gold -= d.price, ap.log.push_back(std::to_string(m.ticks()) + " buy " + d.code + " gold " + std::to_string(gold));
        }
        if (ap.trace) std::printf("[plan %d] shop: food %d gold %d\n", ap.tick(), food(w, p), gold);
    }
    std::uint32_t food_ms_ = ~0u;
};

}  // namespace

std::unique_ptr<MissionPlan> make_plan_S9L1() { return std::make_unique<PlanS9L1>(); }

}  // namespace joa::game
