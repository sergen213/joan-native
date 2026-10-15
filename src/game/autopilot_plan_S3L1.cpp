// Campaign bot plan for mission 2 (S3L1, Orléans). Stage 5 (S3L1_tactic): three English cannons and six reinforcement
// teams appear outside the shut Castle Gate; the cannons break it (1500 HP) and every team then attacks the Bastard's town
// hall place (ATTACK_PLACE orlean_building_place), John and William de la Pole after their battle places. Win = John and
// William defeated (objectives 3, 4: they retreat at 1 HP) with the Bastard alive (8). Opening the gate sends them in just
// the same (stage 5 checks get_gate_state 1), and stage 5 only starts with Joan inside, so the gate is not held: from stage 5
// on, the bot guards the Bastard (only foes near him or on her are fought) and takes John and William as they come.
// Explosive arrows are what holds the crowd off him: the stage-4 ones are kept, and before the stage-5 talk (Bourges and
// Blois freed) the blacksmith Achard's thanks (20 explosive arrows, interact stage 2) are fetched, his buy screen spends the
// stage-4 gold on more, and Gilles' thanks bring food.
#include <cmath>
#include <cstdlib>

#include "game/autopilot_player.hpp"

namespace joa::game {

namespace {

float dist(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); }

struct PlanS3L1 final : MissionPlan {
    bool bought = false;  // Achard's buy screen opened in stage 4

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
    // The Bastard guarded: stage 5, and stage 3 while Jean is played (S3J=2: Joan too, until she switches).
    bool guarding(Mission& m) const {
        const int st = m.scenario().stage;
        return st == 5 || (st == 3 && (jean_phase == 1 || (jean_phase == 0 && env("S3J", 1) == 2)));
    }
    static float env(const char* k, float d) { const char* v = std::getenv(k); return v ? float(std::atof(v)) : d; }
    static bool is(const Unit& u, std::string_view code, Mission& m) { return m.world().data().units[std::size_t(u.cls)].code == code; }

    // Stage 3 with Jean as the player: 0 not yet, 1 playing Jean, 2 back to Joan.
    int jean_phase = 0;
    int calm_since = -1;

    // Stage 3: once the wave's cut scene is over (Jean controllable), Joan is put on hold (Shift+1) and the bot plays Jean
    // (key 2) by the Bastard; back to Joan once nobody hostile is within 2500 of him for 10 s (or Jean is spent).
    bool jean_turn(Autopilot& ap, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        const Unit* b = bastard(m);
        Unit* joan = nullptr;
        Unit* jean = nullptr;
        for (Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && u.hp > 0) {
                if (is(u, "JOAN_OF_ARC", m)) joan = &u;
                if (is(u, "JEAN_DE_METZ", m)) jean = &u;
            }
        if (!env("S3J", 1) || !joan) return false;
        if (jean_phase == 0 && m.scenario().stage == 3 && jean && jean->m.control == 0 && jean->retreat != -1 && b) {
            if (env("S3J", 1) == 2 && dist(joan->pos, b->pos) > 600) return false;  // Joan first runs to the Bastard
            if (p.id != joan->id) return false;
            if (!m.switch_hero(2)) return false;
            if (env("S3H", 1) && !m.ai().is_holding(*joan)) m.ai().toggle_hold(*joan, true);
            jean_phase = 1;
            return true;
        }
        if (jean_phase != 1) return false;
        bool calm = b != nullptr;
        if (b)
            for (const Unit& u : w.units())
                if (!u.deleted && !w.dead(u) && u.hp > 0 && u.m.active && w.hostile_to_player(u) && dist(u.pos, b->pos) < env("S3R", 2500)) calm = false;
        if (!calm) calm_since = -1;
        else if (calm_since < 0) calm_since = ap.tick();
        const bool spent = !jean || jean->retreat == -1 || p.id != jean->id;
        if (m.scenario().stage != 3 || spent || (calm_since >= 0 && ap.tick() - calm_since > 300)) {
            if (p.id != joan->id && !m.switch_hero(1)) return false;
            if (m.ai().is_holding(*joan)) m.ai().toggle_hold(*joan, true);
            jean_phase = 2;
            return true;
        }
        return false;
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

    bool first(Autopilot& ap, InputFrame&, Unit& p) override {
        Mission& m = ap.mission();
        if (jean_turn(ap, p)) return true;
        ap.explosive_reserve = m.scenario().stage == 4 ? 1000 : 0;
        ap.shop_explosive_max = m.scenario().stage == 4 ? 100 : 20;
        ap.gate_delay = jean_phase == 1 && env("S3C", 1) ? 1 << 30 : 0;  // stage 3: closed as soon as Joan gets there; not Jean's errand
        const Unit* n = errand(m);
        ap.plan_talk = n ? n->id : 0;
        // Stage 3: Jean and his men stay on hold by the Bastard (the script's ATTACK_PLACE sent them east and left him to
        // the first wave): held again whenever a script task replaced the hold.
        if (Unit* j = named(m, "Jean de Metz"); j && m.scenario().stage == 3 && jean_phase != 1 && j->id != p.id && j->m.control == 0 && !m.ai().is_holding(*j))
            if (const Unit* b = bastard(m); b && dist(j->pos, b->pos) < 1500) m.ai().toggle_hold(*j, true);
        return false;
    }

    void shop(Autopilot& ap, Unit&) override { bought |= ap.mission().scenario().stage == 4; }

    // Stage 5: foes more than 1500 from the Bastard are left alone unless they are on the player.
    float foe_weight(Autopilot& ap, const Unit& p, const Unit& u) override {
        const float w = MissionPlan::foe_weight(ap, p, u);
        const Unit* b = guarding(ap.mission()) ? bastard(ap.mission()) : nullptr;
        if (w <= 0 || !b) return w;
        return dist(u.pos, b->pos) < 1500 || (u.target == p.id && dist(u.pos, p.pos) < 300) ? w : 0.0f;
    }

    // Stage 3 (the first wave sprints through the gate the spy opened, 40+ units, and kills whoever is nearest; 18 dead
    // citizens or the Bastard lose the mission): Jean holds by the Bastard, Joan closes the gate at once and fights at it
    // (where the script posts her), scoring foes by distance to her + 3x distance to the gate, those after a citizen or
    // the player at 0.5, after the Bastard at 0.3. ponytail: wins ~4 of 24 seeds after the original arrow hit model
    // (ArrowHit); Bastard-anchored / later-closing variants measured worse. A stronger answer (Jean as the player with the
    // explosive arrows, as the walkthrough plays it) is untried.
    static constexpr Vec3 kGate{8454, 11100, 0};  // just inside the Castle Gate (the script's defend_gate_place for Joan)
    bool defend(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        const Unit* b = bastard(m);
        if (!b) return false;
        const int here = ap.region(p.pos);
        const Unit* foe = nullptr;
        float best = 1e30f;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || u.hp <= 0 || !u.m.active || !w.hostile_to_player(u) || u.no_react || std::fabs(u.pos.z - p.pos.z) > 150 ||
                foe_weight(ap, p, u) <= 0 || ap.unreached(u) || !ap.linked(here, ap.unit_region(u)))
                continue;
            float d = dist(p.pos, u.pos) + 3 * dist(kGate, u.pos);
            const Unit* t = w.unit(u.target);
            if (t && !t->deleted && !w.dead(*t) && w.data().units[std::size_t(t->cls)].code.rfind("CITIZEN", 0) == 0) d *= 0.5f;
            else if (t == b) d *= 0.3f;
            else if (u.target == p.id) d *= 0.5f;
            if (d < best) best = d, foe = &u;
        }
        if (foe) {
            ap.goal = "defend: fight #" + std::to_string(foe->id);
            ap.fight(f, p, *foe);
        } else {
            ap.goal = "defend: at the gate";
            if (dist(p.pos, kGate) > 300) ap.travel(f, p, kGate);
        }
        return true;
    }

    // Stage 3: defend(). Stage 4: the shop errand (the talk itself is the generic interact prompt, plan_talk). Stage 5: the
    // Bastard's attackers first (he falls to a handful within seconds), then anyone near him or on the player; nobody: back
    // to his side.
    bool before_targets(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        if (m.scenario().stage == 3 && !guarding(m)) return jean_phase == 2 && env("S3G", 1) ? false : defend(ap, f, p);
        if (const Unit* n = errand(m)) {
            ap.goal = "thanks / shop " + n->name;
            if (dist(p.pos, n->pos) > 80) ap.travel(f, p, n->pos);
            return true;
        }
        const Unit* b = guarding(m) ? bastard(m) : nullptr;
        if (!b) return false;
        World& w = m.world();
        const Unit* foe = nullptr;
        float best = 1e30f;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || u.hp <= 0 || !u.m.active || !w.hostile_to_player(u) || u.no_react || std::fabs(u.pos.z - p.pos.z) > 150 ||
                foe_weight(ap, p, u) <= 0)
                continue;
            float d = dist(p.pos, u.pos);
            if (u.target == b->id) d *= 0.15f;
            else if (dist(u.pos, b->pos) < 600) d *= 0.4f;
            else if (u.target == p.id && d < 200) d *= 0.5f;
            if (d < best) best = d, foe = &u;
        }
        if (foe) {
            ap.goal = "guard: fight #" + std::to_string(foe->id);
            ap.fight(f, p, *foe);
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
