// Campaign bot plan for mission 5 (S5L1, the Loire: Jargeau). Win (S5L1_des / S5L1_tactic / S5L1_finish): objective 1
// "Clean up Jargeau" = Castle_Jargeau captured, which S5L1_tactic sets once the player stands in Castle_Jargeau or
// castleJargeau2 and number_of_fightable_enemy_in_place is 0 in both; plus 34 "Defeat William de la Pole" (enabled when a
// French unit enters meetSuffolk / the crossBridge cut scene; finished at his retreat -1) and 33 "Kill John de la Pole"
// (enabled in reachLord). The generic bot meets Dunois and d'Illiers (way points 2, 3; d'Illiers' cannons then breach the
// west walls) and clears the outer bailey, but the Jargeau way point / territory centre lies at the shut middle gate and
// the inner bailey is only reached over the wall walk (a ~190-point path round the east walls and down the south stairs),
// which the generic hunt gives up on after 30 s. The plan hunts the English left in the two castle places to the end.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "formats/vfs_fs.hpp"
#include "game/autopilot_player.hpp"

namespace joa::game {

namespace {

float dist(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); }

struct PlanS5L1 final : MissionPlan {
    int target = 0, since = 0;       // castle foe hunted, tick chosen
    float best = 0;                  // shortest walk to it yet
    int headway = 0;                 // tick of the last 300 closer
    std::map<int, int> skip;         // foe -> tick it may be hunted again (no headway)

    // 1 outer bailey (Castle_Jargeau), 2 inner (castleJargeau2 south of the middle gate wall, y < 5960: the gate pivot is at
    // 5910, the heavy infantry behind it stand at 5911-5917; the place's box reaches 350 past it into the outer bailey), 0 neither.
    static int bailey(Mission& m, Vec3 q) {
        if (q.y < 5960 && m.places().inside(m.places().place_by_name("castleJargeau2"), q)) return 2;
        const auto& pl = m.places();
        return pl.inside(pl.place_by_name("Castle_Jargeau"), q) || pl.inside(pl.place_by_name("castleJargeau2"), q) ? 1 : 0;
    }

    // Counted by number_of_fightable_enemy_in_place (script_bind_places.cpp non_combat): not unit class 0/8/9, special 2/3/7.
    static bool castle_foe(Mission& m, const Unit& u) {
        World& w = m.world();
        if (u.deleted || (u.hp < 1 && u.lives == 0) || w.dead(u) || u.cls < 0 || !w.hostile_to_player(u)) return false;
        const UnitClass& c = m.data().units[std::size_t(u.cls)];
        if (c.unit_class == 0 || c.unit_class == 8 || c.unit_class == 9 || c.special == 2 || c.special == 3 || c.special == 7) return false;
        return bailey(m, u.pos) != 0;
    }

    // Path length to q on the mission navmesh (what the walk over the wall takes); far off without a full path.
    static float walk(Mission& m, const Unit& p, Vec3 q, int expand = 4000) {
        PathQuery pq;
        pq.from = {p.pos.x, p.pos.y, p.pos.z};
        pq.to = {q.x, q.y, q.z};
        pq.max_expand = expand;
        const PathResult r = m.nav() ? m.nav()->find_path(pq) : PathResult{};
        if (r.status != PathResult::Full) return dist(p.pos, q) + 1e5f;
        float len = 0;
        Vec3 a = p.pos;
        for (const NavPt& n : r.points) len += dist(a, {n.x, n.y, n.z}) + std::fabs(n.z - a.z), a = {n.x, n.y, n.z};
        return len;
    }

    // Foes on the other side of the shut middle gate are no fight targets: the generic fight walked at the heavy infantry
    // behind it for 40000 ticks. The hunt below reaches them over the wall walk. Archers on a standing guard tower are
    // none either (no path leads up: the bot stood under the West Right tower's for minutes); its fall removes them.
    float foe_weight(Autopilot& ap, const Unit& p, const Unit& u) override {
        const float w = MissionPlan::foe_weight(ap, p, u);
        Mission& m = ap.mission();
        if (w > 0 && tower_of(m, u)) return 0.0f;
        const MapItem* g = m.objects().item(m.objects().find_by_name("JargeauMiddleGate"));
        if (w <= 0 || !g || g->state != 0 || g->hp == 0) return w;
        const int a = bailey(m, p.pos), b = bailey(m, u.pos);
        return (a == 2) != (b == 2) && (a != 0 || b == 2) ? 0.0f : w;
    }

    // Way point 1 (Jargeau) stands at the shut middle gate: once inside the castle the hunt below leads on (standing there
    // 10 s between fights with the heavy infantry behind the gate took 20000 ticks; walking back to it whenever a hunt led
    // out past the north wall, another 40000).
    bool entered = false;  // the player has stood in a castle place
    bool use_waypoint(Autopilot& ap, int slot, Vec3) override {
        World& w = ap.mission().world();
        if (const Unit* p = w.unit(w.player_id()); p && bailey(ap.mission(), p->pos)) entered = true;
        return slot != 1 || !entered;
    }

    // Hurt (< 70 %) the generic bot stands to regenerate, also under the arrows of tower and wall archers it cannot reach
    // (the north towers cover the wall walk into the inner bailey: 20000 ticks "resting" there at 52-69 %). Targeted by
    // one of those within 2500, it eats (F3) while food lasts, else walks 800 straight away from him.
    bool shot_at(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (p.hp * 100 >= w.max_hp(p) * 70) return false;
        const Unit* s = nullptr;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || !w.hostile_to_player(u)) continue;
            if (std::fabs(u.pos.z - p.pos.z) <= 150 && dist(p.pos, u.pos) < 300 && !tower_of(m, u)) return false;  // a fight here
            if (u.target == p.id && dist(p.pos, u.pos) < 2500 && (std::fabs(u.pos.z - p.pos.z) > 150 || tower_of(m, u)) &&
                (!s || dist(p.pos, u.pos) < dist(p.pos, s->pos)))
                s = &u;
        }
        if (!s) return false;
        if (food(m, p)) {
            if (ap.tick() - heal_tick > 30) f.press(IN_HEAL_ITEM), heal_tick = ap.tick();
            return false;
        }
        const float d = std::max(1.0f, dist(p.pos, s->pos));
        ap.goal = "plan: out of #" + std::to_string(s->id) + "'s range";
        ap.travel(f, p, {p.pos.x + (p.pos.x - s->pos.x) / d * 800, p.pos.y + (p.pos.y - s->pos.y) / d * 800, p.pos.z});
        return true;
    }
    int heal_tick = -1000;

    static int food(Mission& m, const Unit& p) {  // F3 food (item types 0x15 / 0x19), as the bot counts it
        int n = 0;
        for (const ItemInstance& it : p.inv.bag)
            if (const int t = m.data().items[std::size_t(it.item)].type; t == 0x15 || t == 0x19) n += it.quantity;
        return n;
    }

    // Out of food below 30 % the generic bot retreats to the nearest French general: Dunois, back at his camp 15000 off.
    // Normal's Joan ate the ~31 food she starts with by t70000 and was killed on that walk, twice. (Orleans' potion seller
    // is no help: the walk to him stopped at the shut Orleans East Gate.) She falls back to the siege camp before the west
    // walls instead and rests there to 60 %, defending only against a foe on her.
    static constexpr Vec3 kCamp{3200, 12600, 195};
    bool resting = false;
    bool first(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        const int pct = p.hp * 100 / std::max(1, w.max_hp(p)), eat = food(m, p);
        resting = entered && !eat && (pct < 30 || (resting && pct < 60));
        if (!resting) return false;
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && u.target == p.id && dist(u.pos, p.pos) < 300 && std::fabs(u.pos.z - p.pos.z) < 150)
                return false;
        ap.goal = "plan: rest at the siege camp hp " + std::to_string(pct) + "%";
        if (dist(p.pos, kCamp) > 200) ap.travel(f, p, kCamp);
        return true;
    }

    // William and John de la Pole regenerate: the generic rest below 70 % HP (unaware foes left alone) let William climb
    // from 548 back to 656 HP while Joan stood beside him. Within 600 on her level, with 40 % HP and at most two others
    // on her, the bot keeps swinging at the nearer one (F3 still eats below half).
    bool before_targets(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        if (shot_at(ap, f, p)) return true;
        if (p.hp * 100 < w.max_hp(p) * 40) return false;
        const Unit* boss = nullptr;
        int others = 0;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || !w.hostile_to_player(u) || std::fabs(u.pos.z - p.pos.z) > 150) continue;
            const std::string& c = m.data().units[std::size_t(u.cls)].code;
            const bool b = formats::iequals(c, "william_de_la_pole") || formats::iequals(c, "John_de_la_pole");
            if (b && dist(p.pos, u.pos) < 600 && (!boss || dist(p.pos, u.pos) < dist(p.pos, boss->pos))) boss = &u;
            if (!b && u.target == p.id && dist(p.pos, u.pos) < 200) ++others;
        }
        if (!boss || others > 2) return false;
        ap.goal = "plan: boss #" + std::to_string(boss->id) + " hp " + std::to_string(boss->hp);
        ap.fight(f, p, *boss);
        return true;
    }

    // Archers on a standing guard tower (Jargeau West Left stands inside Castle_Jargeau; its team 80 counts against the
    // capture) are out of reach: no stair leads up (the bot stood under them for 50000 ticks). The tower item when u is
    // one of them, else 0.
    static int tower_of(Mission& m, const Unit& u) {
        for (const char* n : {"JargeauWestLeft", "JargeauWestRight", "JargeauNorthLeft", "JargeauNorthRight"})
            if (const int id = m.objects().find_by_name(n); const MapItem* t = m.objects().item(id))
                if (t->hp > 0 && u.pos.z > t->pos.z + 400 && dist(u.pos, t->pos) < 400) return id;
        return 0;
    }

    // One RTS round trip (Session::toggle_rts in and out) around `orders`, as the S6L1 / S9L1 plans.
    template <class F> static void rts(Mission& m, Unit& p, F&& orders) {
        World& w = m.world();
        PlayerController& pc = w.controller();
        pc.prev_mode = w.game_mode, w.game_mode = 4, m.ai().rts_enter(p);
        orders(m.ai());
        m.ai().rts_exit(p), m.ai().clear_task(p);
        const int to = pc.prev_mode;
        pc.prev_mode = w.game_mode, w.game_mode = to;
    }

    // The French siege and troops near Jargeau are the player's to command in RTS mode, as the mission tells ("Assign
    // units to siege machines in RTS mode ... command the siege machines in the same way as you would with troops"):
    // F2, select, right-click the target (Rts_cmd_attack_item 0x44f1f0 / Rts_cmd_attack_unit 0x44eb80), F2 back.
    // - The crewed cannons nearest it at a tower holding castle foes (its fall deletes its archers: S5L1_tactic
    //   get_wall_hp == 0 -> del_unit_team), else at the middle gate while foes hold the inner bailey ("Jargeau has a
    //   second line of defense. We must penetrate these walls"; Joan alone walked ~190 points round the wall walk).
    // - Every other troop at the castle foe on the ground nearest it that a path reaches (alone, Joan cleared the outer
    //   bailey's 72 in ~100000 ticks). No hero (La Hire's, Jean's or Alencon's death loses the mission) and not Dunois or
    //   d'Illiers (sent along, both were killed by t30000; S9L1 needs the Bastard's cannons for Meung).
    // A unit under fire drops its order (AI_cannon_rts_attack_item 0x544690, the melee / archer RTS_ATTACK_UNIT
    // routines): orders are re-given every 20 s. Only once Joan is in the castle: ordered straight after d'Illiers'
    // talk the crews walked into the garrison's fire (cannons lost, d'Illiers killed, objective 19 failed).
    int order_tick = -100000;
    std::map<int, int> sent;                          // troop -> castle foe it was sent at
    std::map<int, std::pair<int, Vec3>> sent_at;      // troop -> (left alone until, where it stood when sent)
    void after_tick(Autopilot& ap) override {
        Mission& m = ap.mission();
        World& w = m.world();
        const auto& end = m.state().end;
        if (end.empty() || end[0].finish == 1 || ap.tick() - order_tick < 600 || m.ui().game_cut_scene) return;
        Unit* p = w.unit(w.player_id());
        if (!p || w.dead(*p) || !(entered = entered || bailey(m, p->pos))) return;
        order_tick = ap.tick();
        std::vector<const Unit*> foes;
        int item = 0;
        for (const Unit& u : w.units())
            if (castle_foe(m, u)) foes.push_back(&u), item = item ? item : tower_of(m, u);
        if (const int g = m.objects().find_by_name("JargeauMiddleGate"); !item && m.objects().item(g) && m.objects().item(g)->hp > 0 &&
                                                                         std::any_of(foes.begin(), foes.end(), [&](const Unit* u) { return bailey(m, u->pos) == 2; }))
            item = g;
        std::vector<int> guns;
        std::map<int, std::vector<int>> troops;  // castle foe -> troops sent at it
        for (const Unit& u : w.units()) {
            const UnitClass& c = m.data().units[std::size_t(u.cls)];
            if (u.deleted || w.dead(u) || u.id == p->id || u.nation != p->nation || c.hero || formats::iequals(c.code, "DUNOIS") ||
                formats::iequals(c.code, "DLLLIER") || c.unit_class == 0 || c.unit_class == 9 ||
                !m.ai().rts_selectable(u))
                continue;
            if (c.unit_class == 8) {
                if (item && !(m.ai().curr_task(u) == task::RTS_ATTACK_ITEM && m.ai().curr_task_para(u, 1) == item)) guns.push_back(u.id);
                continue;
            }
            const int tk = m.ai().curr_task(u);
            if (const Unit* t = w.unit(sent[u.id]); t && castle_foe(m, *t) &&
                                                      (tk == task::RTS_ATTACK_MOVE || tk == task::RTS_MOVE_TO || tk == task::RTS_ATTACK_UNIT))
                continue;
            // One that dropped the last order without moving (no route from its level) is left for 5 min.
            if (const auto s = sent_at.find(u.id); s != sent_at.end()) {
                if (ap.tick() < s->second.first) continue;
                if (s->second.first == 0 && dist(s->second.second, u.pos) < 200) {
                    s->second.first = ap.tick() + 9000;
                    continue;
                }
            }
            std::vector<const Unit*> near;
            for (const Unit* e : foes)
                if (e->pos.z < 450) near.push_back(e);
            const auto by_dist = [&](const Unit* a, const Unit* b) { return dist(u.pos, a->pos) < dist(u.pos, b->pos); };
            std::partial_sort(near.begin(), near.begin() + std::min<std::ptrdiff_t>(4, std::ssize(near)), near.end(), by_dist);
            for (std::size_t i = 0; i < near.size() && i < 4; ++i)
                if (walk(m, u, near[i]->pos, 20000) < 1e5f) {
                    troops[near[i]->id].push_back(u.id), sent[u.id] = near[i]->id, sent_at[u.id] = {0, u.pos};
                    if (ap.trace) std::printf("[plan %d] #%d %s (%.0f,%.0f,%.0f) task %d -> #%d (%.0f,%.0f,%.0f)\n", ap.tick(), u.id, c.code.c_str(), u.pos.x, u.pos.y, u.pos.z, tk, near[i]->id, near[i]->pos.x, near[i]->pos.y, near[i]->pos.z);
                    break;
                }
        }
        if (item) {
            const Vec3 at = m.objects().item(item)->pos;
            std::sort(guns.begin(), guns.end(), [&](int a, int b) { return dist(w.unit(a)->pos, at) < dist(w.unit(b)->pos, at); });
            if (guns.size() > 3) guns.resize(3);
        }
        if (guns.empty() && troops.empty()) return;
        rts(m, *p, [&](AiSystem& ai) {
            if (!guns.empty()) ai.rts_attack_item(guns, item);
            for (const auto& [t, ids] : troops) ai.rts_move(ids, w.unit(t)->pos, true);
        });
        std::size_t n = 0;
        for (const auto& [t, ids] : troops) n += ids.size();
        ap.log.push_back(std::to_string(m.ticks()) + " plan: " + std::to_string(guns.size()) + " cannons at " +
                         (item ? m.objects().item(item)->name : std::string("-")) + ", " + std::to_string(n) + " troops at " +
                         std::to_string(troops.size()) + " foes");
    }

    // The castle foe the shortest walk reaches (straight-line picks sent the bot back and forth between the wall walk and
    // the gatehouse archers above it), kept 3 min; one it walks no closer to for 60 s is left for 3 min.
    bool before_hunt(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        const auto& end = m.state().end;
        if (end.empty() || end[0].finish == 1) return false;
        const Unit* t = w.unit(target);
        if (!t || !castle_foe(m, *t) || tower_of(m, *t) || ap.tick() - since > 5400) {
            t = nullptr;
            float bd = 1e30f;
            for (const Unit& u : w.units())
                if (castle_foe(m, u) && !tower_of(m, u) && ap.tick() >= skip[u.id])
                    if (const float d = walk(m, p, u.pos); d < bd) bd = d, t = &u;
            if (!t) return false;
            target = t->id, since = headway = ap.tick(), best = 1e30f;
        }
        if (ap.tick() % 150 == 0 || best > 1e29f)
            if (const float d = walk(m, p, t->pos); d < best - 300) best = d, headway = ap.tick();
        if (best >= 1e5f) headway = -100000;  // no full path: skip at once
        if (ap.tick() - headway > 1800) {  // 60 s without walking 300 closer: another one for 3 min
            skip[t->id] = ap.tick() + 5400, target = 0;
            return false;
        }
        ap.goal = "plan: Jargeau #" + std::to_string(t->id) + " " + m.data().units[std::size_t(t->cls)].code;
        ap.travel(f, p, t->pos);
        return true;
    }
};

}  // namespace

std::unique_ptr<MissionPlan> make_plan_S5L1() { return std::make_unique<PlanS5L1>(); }

}  // namespace joa::game
