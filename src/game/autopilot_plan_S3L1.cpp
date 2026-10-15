// Campaign bot plan for mission 2 (S3L1, Orléans).
// Stage 3 (S3L1_tactic first wave): ~40 English sprint through the Castle Gate the spy opened and kill the nearest French;
// 18 dead citizens, Joan or the Bastard lose the mission. Joan closes the gate as soon as she gets there (fewer get in;
// left open, the 180 s reinforcements spawn: 0 of 48 seeds survived), gives Jean her explosive arrows and holds there;
// the player takes Jean (the walkthrough's route) and fights the Bastard's attackers, then foes near citizens.
// ponytail: survives ~22 of 48 seeds (Joan as the player with Jean held by the Bastard: ~9; docs §8.1).
// Stage 4: the explosive arrows are kept, and before the stage-5 talk (Bourges and Blois freed) the blacksmith Achard's
// thanks (20 explosive arrows, interact stage 2) are fetched, his buy screen spends the gold on plain arrows, and Gilles'
// thanks bring food.
// Stage 5: three English cannons and six reinforcement teams outside the shut Castle Gate; once the cannons break it
// (1500 HP) every team attacks the Bastard's town hall. Win = John and William defeated (objectives 3, 4: they retreat at
// 1 HP) with the Bastard alive (8). The walkthrough's way: Jean, then Joan, shoots from the wall walk by the gatehouse —
// one crewman of every manned cannon (it fires only fully manned), so the gate holds and the reinforcements stay outside,
// then John and William, who come up to the gate once hit. Gate down: the bot guards the Bastard.
#include <algorithm>
#include <cmath>

#include "game/autopilot_player.hpp"
#include "game/siege_crew.hpp"
#include "game/terrain_query.hpp"

namespace joa::game {

namespace {

float dist(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool lord(const Unit& u) { return u.retreat != -1 && (u.name == "John de la Pole" || u.name == "William de la Pole"); }

struct PlanS3L1 final : MissionPlan {
    static constexpr Vec3 kGate{8454, 11100, 0};  // just inside the Castle Gate (the script's defend_gate_place for Joan)
    // The wall walk either side of the gatehouse (inner edge: a knock-back drops the shooter inside; the gatehouse top
    // itself dropped him outside), each with a line past the closed gate to its outer face where the lords stand.
    static constexpr Vec3 kTops[2] = {{7640, 11300, 517}, {9280, 11320, 517}};

    bool bought = false;      // Achard's buy screen opened in stage 4
    bool jean_done = false;   // stage 5: Jean's turn on the wall is over
    int top_ = 0;             // kTops index in use
    int pull_tick = -100000;  // last pull shot at a far lord

    static Unit* named(Mission& m, std::string_view name) {
        World& w = m.world();
        for (Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && u.hp > 0 && u.name == name) return &u;
        return nullptr;
    }
    static const Unit* bastard(Mission& m) {
        World& w = m.world();
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && u.hp > 0 && w.data().units[std::size_t(u.cls)].code == "ORLEAN") return &u;
        return nullptr;
    }
    static const MapItem* castle_gate(Mission& m) {
        const MapItem* best = nullptr;
        for (int r = 1; r <= m.objects().size(); ++r)
            if (const MapItem* it = m.objects().item(r); it && it->cls == 1 && dist(it->pos, kGate) < 400 && (!best || dist(it->pos, kGate) < dist(best->pos, kGate)))
                best = it;
        return best;
    }
    static bool gate_up(Mission& m) {
        const MapItem* g = castle_gate(m);
        return g && g->hp > 0 && g->state == 0;
    }

    // Stage 4 with Bourges (11) and Blois (12) freed: the shop errand's NPC, or nullptr.
    const Unit* errand(Mission& m) const {
        const auto& e = m.state().end;
        if (m.scenario().stage != 4 || e.size() < 12 || e[10].finish != 1 || e[11].finish != 1) return nullptr;
        const Unit* a = named(m, "Achard Gantier");
        if (a && (a->m.interact_stage == 2 || (a->m.interact_stage == 3 && !bought))) return a;
        const Unit* g = named(m, "Gilles Tollot");
        return g && g->m.interact_stage == 2 ? g : nullptr;
    }

    // The wall-walk spot nearer the lords (hysteresis 100).
    Vec3 top(Mission& m) {
        float d[2] = {1e9f, 1e9f};
        for (const Unit& u : m.world().units())
            if (!u.deleted && !m.world().dead(u) && lord(u))
                for (int i = 0; i < 2; ++i) d[i] = std::min(d[i], dist(kTops[i], u.pos));
        if (d[1 - top_] < d[top_] - 100) top_ = 1 - top_;
        return kTops[top_];
    }

    // Stage 5 with the gate up: Jean (key 2; the Rapid-Fire Bow, twice Joan's bow, and his 90 arrows: the walkthrough's
    // "hit '2' as soon as Joan has closed the gate") takes the wall while Joan holds inside (Shift+1); back to Joan once
    // the gate is down, Jean is below 40 % HP (he withdraws at 1 HP) or out of arrows. Then Jean's bow and arrows go to
    // Joan (the walkthrough gives her the L2 bow; 6 -> 11 of 48 seeds won: once Joan ran out of arrows William, back at
    // full HP, waited outside the gate for good).
    bool jean_turn(Autopilot& ap, Unit& p) {
        Mission& m = ap.mission();
        if (m.scenario().stage != 5 || jean_done) return false;
        Unit* joan = named(m, "Joan of Arc");
        Unit* jean = named(m, "Jean de Metz");
        if (!joan) return false;
        const bool jean_ok = jean && jean->m.control == 0 && jean->retreat != -1 && jean->hp * 100 > 40 * m.world().max_hp(*jean) &&
                             ap.arrows(*jean, false) + ap.arrows(*jean, true) > 0;
        if (p.id == joan->id && gate_up(m) && jean_ok) {
            if (!m.switch_hero(2)) return false;
            if (!m.ai().is_holding(*joan)) m.ai().toggle_hold(*joan, true);
            return true;
        }
        if (jean && p.id == jean->id && !(gate_up(m) && jean_ok)) {
            jean_done = true;
            {
                World& w = m.world();
                const float view = minimap_view(w);
                const int bow = jean->inv.ranged.item;
                if (jean->inv.ammo.item) w.transfer_item(*jean, 3, 0, *joan, view, view);
                for (std::size_t i = jean->inv.bag.size(); i-- > 0;)
                    if (const int t = w.data().items[std::size_t(jean->inv.bag[i].item)].type; t == 0xe || t == 0xf)
                        w.transfer_item(*jean, 10, int(i), *joan, view, view);
                if (bow && w.transfer_item(*jean, 2, 0, *joan, view, view))
                    for (std::size_t i = 0; i < joan->inv.bag.size(); ++i)
                        if (joan->inv.bag[i].item == bow) {
                            w.equip(*joan, int(i));
                            break;
                        }
            }
            if (!m.switch_hero(1)) return false;
            if (m.ai().is_holding(*joan)) m.ai().toggle_hold(*joan, true);
            return true;
        }
        return false;
    }

    // World units -> px of the zoomed-out minimap (zoom 0: one 200-px tile for the whole map), whose 100-px reach
    // bounds the character-screen item transfer.
    static float minimap_view(const World& w) {
        const auto* g = dynamic_cast<const GriTerrain*>(w.terrain());
        return g ? 200.0f / (float(std::max(g->grid().cells_x, g->grid().cells_y)) * formats::TerrainGrid::kCellSize) : 0.0f;
    }

    // Stage 3 (the walkthrough's route): once Joan has closed the gate she hands Jean her explosive arrows on the
    // character screen (World::transfer_item; the minimap zoomed out, zoom 0 = one 200-px tile for the whole map, so
    // he is within its 100-px reach), stays on hold there (Shift+1) and the player takes Jean (key 2) to guard the
    // town (guard(): the Bastard's attackers, then foes near citizens). Back to Joan once stage 4 starts.
    int jean3 = 0;  // 0 Joan, 1 Jean playing stage 3, 2 back to Joan
    bool jean_stage3(Autopilot& ap, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        Unit* joan = named(m, "Joan of Arc");
        Unit* jean = named(m, "Jean de Metz");
        if (!joan || !jean) return false;
        if (jean3 == 0 && m.scenario().stage == 3 && p.id == joan->id && jean->m.control == 0 && gate_up(m)) {
            const float view = minimap_view(w);
            auto expl = [&](int item) { return item && w.data().items[std::size_t(item)].type == 0xf; };
            if (expl(joan->inv.ammo.item)) w.transfer_item(*joan, 3, 0, *jean, view, view);
            for (std::size_t i = joan->inv.bag.size(); i-- > 0;)
                if (expl(joan->inv.bag[i].item)) w.transfer_item(*joan, 10, int(i), *jean, view, view);
            if (!m.switch_hero(2)) return false;
            if (!m.ai().is_holding(*joan)) m.ai().toggle_hold(*joan, true);
            jean3 = 1;
            return true;
        }
        if (jean3 == 1 && m.scenario().stage >= 4 && p.id == jean->id) {
            if (!m.switch_hero(1)) return false;
            jean3 = 2;
            if (m.ai().is_holding(*joan)) m.ai().toggle_hold(*joan, true);
            if (!m.ai().is_holding(*jean)) m.ai().toggle_hold(*jean, true);
            return true;
        }
        return false;
    }

    bool first(Autopilot& ap, InputFrame&, Unit& p) override {
        Mission& m = ap.mission();
        if (jean_turn(ap, p) || jean_stage3(ap, p)) return true;
        // Stage 4 keeps the explosive arrows for the stage-5 crews (Achard's 20 are plenty) and spends the gold on plain
        // ones (10 for 100 against 5 for 500) for John and William, before a heavier armour.
        ap.explosive_reserve = m.scenario().stage == 4 ? 1000 : 0;
        ap.shop_plain_first = m.scenario().stage == 4 ? 250 : 0;
        ap.gate_delay = 0;
        const Unit* n = errand(m);
        ap.plan_talk = n ? n->id : 0;
        // Stage 3: Jean and his men stay on hold by the Bastard (the script's ATTACK_PLACE sent them east and left him to
        // the first wave): held again whenever a script task replaced the hold.
        if (Unit* j = named(m, "Jean de Metz"); j && m.scenario().stage == 3 && j->id != p.id && j->m.control == 0 && !m.ai().is_holding(*j))
            if (const Unit* b = bastard(m); b && dist(j->pos, b->pos) < 1500) m.ai().toggle_hold(*j, true);
        return false;
    }

    void shop(Autopilot& ap, Unit&) override { bought |= ap.mission().scenario().stage == 4; }

    // Stage 3: an enemy counts nearer by 1 + the citizens within 1500 of it (they are what loses the mission). Stage 5:
    // foes more than 1500 from the Bastard are left alone unless they are on the player.
    float foe_weight(Autopilot& ap, const Unit& p, const Unit& u) override {
        Mission& m = ap.mission();
        World& w = m.world();
        float wt = MissionPlan::foe_weight(ap, p, u);
        if (wt <= 0) return wt;
        if (m.scenario().stage == 3) {
            int n = 0;
            for (const Unit& c : w.units())
                n += !c.deleted && !w.dead(c) && w.data().units[std::size_t(c.cls)].unit_class == 0 && !w.hostile_to_player(c) && dist(c.pos, u.pos) < 1500;
            return wt / float(1 + n);
        }
        const Unit* b = m.scenario().stage == 5 ? bastard(m) : nullptr;
        return !b || dist(u.pos, b->pos) < 1500 || (u.target == p.id && dist(u.pos, p.pos) < 300) ? wt : 0.0f;
    }

    // Anything solid (a map item's CollisionType primitive: the gatehouse over a lord standing under it) on the line from
    // the eye to u's middle: the arrow stops there (MapObjects::sweep_items); clear_shot only looks at bodies and gates.
    static bool walled(Mission& m, const Unit& p, const Unit& u) {
        const auto& units = m.world().data().units;
        const Vec3 eye{p.pos.x, p.pos.y, p.pos.z + float(units[std::size_t(p.cls)].height)};
        const Vec3 a{u.pos.x - eye.x, u.pos.y - eye.y, u.pos.z + float(units[std::size_t(u.cls)].height) * 0.5f - eye.z};
        const int legs = int(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z) / 900) + 1;  // under the sweep's 1000 window
        for (int i = 0; i < legs; ++i) {
            const float f = float(i) / float(legs);
            float t;
            if (m.objects().sweep_items({eye.x + a.x * f, eye.y + a.y * f, eye.z + a.z * f}, 10, {a.x / float(legs), a.y / float(legs), a.z / float(legs)}, t))
                return true;
        }
        return false;
    }

    // Stage 5 on the wall: crews of manned cannons from up to 2400 (the release lock reaches 2500), then the lords (within
    // 900, plus a pull shot at a farther one every 30 s: once hit he comes to the gate below, in range). Nothing to shoot
    // from there, no arrows or below 35 % HP: just inside the gate, out of the English archers' reach.
    bool gatehouse(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (!gate_up(m)) return false;
        const Vec3 at = top(m);
        const bool up = dist(p.pos, at) <= 120 && p.pos.z >= at.z - 40;
        const bool pull = ap.tick() - pull_tick < 90 || ap.tick() - pull_tick > 900;  // a 90-tick window every 30 s
        const Unit* foe = nullptr;
        float best = 1e30f;
        int manned = 0;
        bool any = false;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || u.hp <= 0 || !w.hostile_to_player(u) || u.no_react) continue;
            const Unit* v = w.unit(u.m.vehicle);
            const bool crew = v && !v->deleted && !w.dead(*v) && siege_slots_full(w, *v, 0xff);
            manned += crew;
            const float d = dist(at, u.pos);
            if (d > (crew ? 2400.0f : lord(u) ? (pull ? 2400.0f : 900.0f) : 0.0f)) continue;
            any = true;
            // A pull shot goes without a clear line too: the release lock takes whoever stands by him, and that brings his
            // men and him over (his escort hid William from every shot for 30000 ticks).
            const bool far = !crew && d > 900;
            if (!up || (!far && (!ap.clear_shot(p, u) || walled(m, p, u)))) continue;
            const float s = d * (crew ? 0.01f : far ? 1.0f : 0.1f);
            if (s < best) best = s, foe = &u;
        }
        if (!any || ap.arrows(p, false) + ap.arrows(p, true) == 0 || p.hp * 100 < 35 * w.max_hp(p)) {
            ap.goal = "below the gatehouse";
            if (dist(p.pos, kGate) > 300 || p.pos.z > 100) ap.travel(f, p, kGate);
            return true;
        }
        if (!up) {
            ap.goal = "to the wall";
            ap.travel(f, p, at);
            return true;
        }
        if (!foe) {
            ap.goal = "on the wall: watch";
            f.camera_yaw_deg = 90;  // outwards (north)
            return true;
        }
        ap.goal = "on the wall: shoot #" + std::to_string(foe->id) + " " + foe->name;
        const Unit* v = w.unit(foe->m.vehicle);
        if (dist(at, foe->pos) > 900) {
            if (ap.tick() - pull_tick > 900) pull_tick = ap.tick();
            ap.explosive_reserve = 1000;
            if (ap.load_ammo(p, false))
                ap.shoot_at(f, p, {foe->pos.x, foe->pos.y, foe->pos.z + float(w.data().units[std::size_t(foe->cls)].height) * 0.5f}, p.action == SHTIDLE);
            return true;
        }
        ap.explosive_reserve = v && !v->deleted ? 0 : 3 * manned;  // explosive arrows: the crews first, then the lords
        ap.shoot(f, p, *foe);
        return true;
    }

    // Stage 4: the shop errand (the talk itself is the generic interact prompt, plan_talk). Stage 3 as Jean: guard the
    // town. Stage 5: the wall while the gate is up; then guard the Bastard.
    bool before_targets(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        if (const Unit* n = errand(m)) {
            ap.goal = "thanks / shop " + n->name;
            if (dist(p.pos, n->pos) > 80) ap.travel(f, p, n->pos);
            return true;
        }
        if (m.scenario().stage == 3 && jean3 == 1) return guard(ap, f, p, 6000);
        if (m.scenario().stage != 5) return false;
        if (gatehouse(ap, f, p)) return true;
        return guard(ap, f, p, 1500);
    }

    // The Bastard's attackers first (he falls to a handful within seconds), anyone within `r` of him or on the player;
    // in stage 3 also by foe_weight (citizens near the foe). Nobody: back to his side.
    bool guard(Autopilot& ap, InputFrame& f, Unit& p, float r) {
        Mission& m = ap.mission();
        const Unit* b = bastard(m);
        if (!b) return false;
        World& w = m.world();
        const Unit* foe = nullptr;
        float best = 1e30f;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || u.hp <= 0 || !u.m.active || !w.hostile_to_player(u) || u.no_react || std::fabs(u.pos.z - p.pos.z) > 150 ||
                MissionPlan::foe_weight(ap, p, u) <= 0 || (dist(u.pos, b->pos) > r && !(u.target == p.id && dist(u.pos, p.pos) < 300)))
                continue;
            float d = dist(p.pos, u.pos);
            if (u.target == b->id) d *= 0.15f;
            else if (dist(u.pos, b->pos) < 600) d *= 0.4f;
            else if (u.target == p.id && d < 200) d *= 0.5f;
            if (m.scenario().stage == 3) d *= foe_weight(ap, p, u) / MissionPlan::foe_weight(ap, p, u);
            if (d < best) best = d, foe = &u;
        }
        if (foe) {
            ap.goal = "guard: fight #" + std::to_string(foe->id);
            ap.fight(f, p, *foe);
        } else if (m.scenario().stage == 3) {
            return false;  // the last attackers (on a wall, out of the town): the generic hunt
        } else {
            ap.goal = "by the Bastard";
            if (dist(p.pos, b->pos) > 250) ap.travel(f, p, b->pos);
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<MissionPlan> make_plan_S3L1() { return std::make_unique<PlanS3L1>(); }

}  // namespace joa::game
