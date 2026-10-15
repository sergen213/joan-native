// Campaign-bot plan for mission 7 (S7L1, La Charité: Gough's Saint-Pierre-le-Moutier, Gressart's La Charité-sur-Loire).
// Objective chain from S7L1_des / S7L1_tactic / S7L1_finish (docs/SAVES_AND_CAMPAIGN.md §8.1 "Mission 7"): every enabled
// WIN record of 1 / 3 / 24 / 4 / 6 / 7 / 14 / 15 / 16 finished opens La Charité (event bit 2:16 deletes the invisible wall
// "inv", enables 2 and 5).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string_view>
#include <vector>

#include "game/autopilot_player.hpp"
#include "game/interact_player.hpp"

namespace joa::game {
namespace {

float dist(Vec3 a, Vec3 b) { return std::hypot(b.x - a.x, b.y - a.y); }

class PlanS7L1 final : public MissionPlan {
public:
    bool first(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        ap.plan_talk = 0;   // set again by before_hunt each tick it leads to the potion seller (a stale one re-talks him forever)
        // Buy screen: explosive bolts for the wall archers, plain ones, and gold kept for recruits and the siege workshop.
        ap.shop_explosive_max = 60, ap.shop_plain_max = 150, ap.shop_gold_keep = 6000;
        if (is(m, p, "JOAN_OF_ARC")) {
            if (ap.tick() >= army_tick_ && p.m.control == 0) army_tick_ = ap.tick() + (call_army(m, p) ? 900 : 30);
            if (!m.world().controller().range_mode) best_launcher(m, p);
        }
        return bribe_job(ap, f, p) || gate_job(ap, f, p);
    }

    // The walkthrough's quiet way into Saint-Pierre: bribe the Gate Guard (2200 gold) right after the gold-cart cut scene.
    // Unbribed, French troops on the bridge (place "st_pierre_bribe") for 6 s, or any damage to the gate, its walls or
    // the guard, sound the alarm: ~36 archers man the walls (event bit 13). Bribed (event bit 14), the gate is French and
    // the first-invasion cut scene is skipped. The bribe prompt shows within 200 at |dz| <= 100 (find_bribable).
    bool bribe_job(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        const auto& b = m.vm().state().bits;
        const Unit* g = named(m, "Gate Guard");
        if (!is(m, p, "JOAN_OF_ARC") || !g || g->m.bribe != 1 || ((b[1] >> 12) & 1u) || ((b[1] >> 13) & 1u) || !((b[0] >> 17) & 1u) ||
            m.state().gold < bribe_cost(m, *g) + 500)
            return false;
        ap.goal = "bribe gate guard";
        if (const InteractTarget t = interact_target(m); t.kind == InteractTarget::Bribe && t.id == g->id) {
            if (ap.tick() % 10 == 0) f.press(IN_INTERACT);
            return true;
        }
        ap.travel(f, p, g->pos);
        return true;
    }

    // The mercenary leaders whose death is an objective first: Matthew / Martin Gough (4 / 24, killable once the
    // monastery hill's cut scene gave them retreat -2), Gressart, Begley, Hudleg (5 / 25 / 26).
    float foe_weight(Autopilot& ap, const Unit& p, const Unit& u) override {
        const float base = MissionPlan::foe_weight(ap, p, u);
        if (u.name == "Gate Guard" && u.m.bribe == 1 && !((ap.mission().vm().state().bits[1] >> 12) & 1u)) return 0;   // bribed, not fought
        const std::string& code = ap.mission().data().units[std::size_t(u.cls)].code;
        for (const auto& [c, n] : {std::pair<std::string_view, int>{"MATTHEWGOUGH", 4}, {"MARTINGOUGH", 24}, {"Perrinet", 5}, {"BEGLEY", 25},
                                   {"HUDLEG", 26}})
            if (code == c)
                if (const EndCondition* e = objective(ap.mission(), n); e && e->enabled && e->finish != 1) return base * 0.05f;
        return base;
    }

    // With no marker, talk or nearby fight left: the Goughs while objective 4 / 24 is open (Matthew idles on the monastery
    // hill, place "st pierre firework", where walking in starts the cut scene that makes both killable), then the gold
    // (objective 6: 100 S7_GOLDBAR barrels, minimap yellow dots once the cart cut scene ran), nearest barrel first.
    bool before_hunt(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        const int here = ap.region(p.pos);
        for (const Unit& u : w.units()) {
            const std::string& code = m.data().units[std::size_t(u.cls)].code;
            const int n = code == "MATTHEWGOUGH" ? 4 : code == "MARTINGOUGH" ? 24 : 0;
            if (!n || u.deleted || w.dead(u) || ap.unreached(u) || !ap.linked(here, ap.unit_region(u))) continue;
            if (const EndCondition* e = objective(m, n); e && e->enabled && e->finish != 1) {
                ap.goal = "boss #" + std::to_string(u.id) + " " + code;
                ap.travel(f, p, u.pos);
                return true;
            }
        }
        // St-Thomas-sur-le-pont's defence (objective 7, enabled on entering the town): three mercenary squads come 5 min
        // later (or at once on the priest's road); it is won once they are dead with a French general in the town and the
        // priest and councillor back at their places. Wait at the councillor's place; the fight logic meets the squads.
        if (const EndCondition* v = objective(m, 7); v && v->enabled && v->finish != 1 && dist(p.pos, kCouncillor) > 600) {
            ap.goal = "defend St-Thomas";
            ap.travel(f, p, kCouncillor);
            return true;
        }
        // The gold back to St-Thomas (objective 16, enabled by the defence): the councillor's talk with 100 barrels found.
        if (const EndCondition* r = objective(m, 16); r && r->enabled && r->finish != 1 && m.state().gold_crates >= 100)
            if (const Unit* c = named(m, "Philippe Barrillier"); c && dist(p.pos, c->pos) > 60) {
                ap.goal = "gold to the councillor";
                ap.travel(f, p, c->pos);
                return true;
            }
        // The inner gate to break: Joan clears its surroundings and waits there for La Hire (gate_job switches to him).
        if (gate_at_.x != 0 && dist(p.pos, gate_at_) > 800) {
            ap.goal = "to inner gate";
            ap.travel(f, p, gate_at_);
            return true;
        }
        // Food: the potion shop opens with Saint-Pierre (objective 1); the HP food the bot lives on is bought there. The trip
        // from La Charité is long and crosses enemy ground: it starts while 10 are left.
        if (const EndCondition* c = objective(m, 1); c && c->finish == 1 && food(m, p) < 10 && m.state().gold > 4000)
            if (const Unit* s = potion_seller(m); s && !ap.unreached(*s)) {
                ap.goal = "to the potion seller";
                ap.plan_talk = s->id;
                ap.travel(f, p, s->pos);
                return true;
            }
        if (recruit_job(ap, f, p) || barrels(ap, f, p)) return true;
        // Capture objectives, each place's fightable enemies hunted down: Saint-Pierre (1, once Gough is dead; stragglers
        // outside the walls, arbalest crews), Varzy (3), des champs du nord (14).
        if (const EndCondition* c = objective(m, 1); c && c->finish != 1 && ((m.vm().state().bits[1] >> 20) & 1u) &&
                                                     hunt_in(ap, f, p, {"St Pierre", "St Pierre2", "St Pierre3"}))
            return true;
        if (const EndCondition* c = objective(m, 3); c && c->enabled && c->finish != 1 && hunt_in(ap, f, p, {"eng camp"})) return true;
        if (const EndCondition* c = objective(m, 14); c && c->enabled && c->finish != 1 && hunt_in(ap, f, p, {"eng 2 trigger"})) return true;
        return charite(ap, f, p);
    }

    // Joan's troop refilled at a French barracks (the start camp's "french recruit"; Saint-Pierre's "StPierre Recruit" once the
    // town is captured) when it is down to half, or at once when La Charité's trebuchet lacks crew (crew_short_) and there is
    // room: R at the facility opens the recruit screen (FacilityOrder, as the engine's), light infantry up to three in the
    // troop (the only class that mans a siege weapon), then crossbowmen and heavy infantry alternately while gold, citizens
    // and the troop limit allow, RECRUIT.
    bool recruit_job(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        const int cap = m.ai().max_troop_size(p);
        const bool urgent = crew_short_ && int(p.members.size()) < cap;
        if (!is(m, p, "JOAN_OF_ARC") || ap.tick() < recruit_tick_ || (!urgent && int(p.members.size()) * 2 > cap) ||
            m.state().gold < ap.shop_gold_keep)
            return false;
        Places& pl = m.places();
        int fac = 0;
        for (int i = 1; i <= int(pl.facilities().size()); ++i) {
            const Facility* fc = pl.facility(i);
            const Place* own = fc ? pl.place(fc->place) : nullptr;
            if (fc && fc->kind == Facility::Recruit && own && own->nation == p.nation && FacilityOrder::barracks(m, i, p.id).citizens > 0 &&
                (!fac || dist(p.pos, fc->pos) < dist(p.pos, pl.facility(fac)->pos)))
                fac = i;
        }
        if (!fac) return false;
        ap.goal = "recruit at " + pl.facility(fac)->name;
        if (find_recruit_facility(m, p) != fac) {
            ap.travel(f, p, pl.facility(fac)->pos);
            return true;
        }
        FacilityOrder order = FacilityOrder::barracks(m, fac, p.id);
        int light = 0;
        for (int id : p.members)
            if (const Unit* u = m.world().unit(id); u && m.data().units[std::size_t(u->cls)].code.find("LIGHT_INFANTRY") != std::string::npos) ++light;
        int rows[3] = {-1, -1, -1};   // light infantry (slot 0), crossbowman (slot 5), heavy infantry (slot 1)
        for (int r = 0; r < 6; ++r) {
            if (order.slot(m, r) == 0) rows[0] = r;
            if (order.slot(m, r) == 5) rows[1] = r;
            if (order.slot(m, r) == 1) rows[2] = r;
        }
        const auto afford = [&] { return m.state().gold - order.cost - 500 >= ap.shop_gold_keep; };
        for (; light < 3 && rows[0] >= 0 && afford() && order.more(m, rows[0]); ++light) {}
        for (bool more = true; more;) {
            more = false;
            for (int r : {rows[1], rows[2]}) more |= r >= 0 && afford() && order.more(m, r);
        }
        ap.log.push_back(std::to_string(m.ticks()) + " recruit " + std::to_string(order.recruited) + " at " + pl.facility(fac)->name);
        order.commit(m);
        recruit_tick_ = ap.tick() + 3000;
        return true;
    }

    // Objective 6: the nearest S7_GOLDBAR barrel in a linked region (80 s going for one at most).
    bool barrels(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        const EndCondition* gold = objective(m, 6);
        if (!gold || !gold->enabled || gold->finish == 1) return false;
        // St-Thomas-sur-le-pont's barrels are left alone while objective 7 is off: walking into the town starts the villagers'
        // defence (7) and with it the gold return (16).
        const EndCondition* villagers = objective(m, 7);
        const bool avoid_town = villagers && !villagers->enabled;
        const MapObjects& o = m.objects();
        const int bar = m.data().item_index("S7_GOLDBAR");
        const int here = ap.region(p.pos);
        int best = 0;
        float best_d = 1e30f;
        for (int r = 1; r <= o.size(); ++r)
            if (const MapItem* it = o.item(r); it && it->cls == 10 && it->stage > 0 && it->content == bar && barrel_spent_[r] < 2400 &&
                                               !(avoid_town && it->pos.x > 58000 && it->pos.y > 17400 && it->pos.y < 21800) &&
                                               ap.linked(here, ap.region(it->pos))) {
                const float d = dist(p.pos, it->pos) + std::fabs(it->pos.z - p.pos.z) * 4;
                if (d < best_d) best_d = d, best = r;
            }
        if (!best) return false;
        ++barrel_spent_[best];  // 80 s going for one barrel in all: out of reach, left
        ap.goal = "gold barrel " + std::to_string(best);
        ap.travel(f, p, o.item(best)->pos);
        return true;
    }

    // La Charité (objectives 2 / 5, enabled with event_2 bit 16 once the invisible wall "inv" is gone). The walkthrough's way:
    // take the siege house ("workshop1": its cut scene, scenario bit 30, then every mercenary in "workshop1 place" dead,
    // bit 31, makes it and its weapons French), build a trebuchet there (gold workshop) and fire it at the eastern wall:
    // any damage to "wall" / "wall2" opens both gates (event bit 26) and the garrison comes out. Then every fightable
    // mercenary in "St La Charite" (x < 15262) and Gressart.
    bool charite(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        const auto& b = m.vm().state().bits;
        if (!((b[2] >> 15) & 1u) || !is(m, p, "JOAN_OF_ARC")) return false;
        if (!((b[0] >> 29) & 1u)) {
            ap.goal = "to the siege house";
            ap.travel(f, p, kWorkshopScene);
            return true;
        }
        if (!((b[0] >> 30) & 1u)) return hunt_in(ap, f, p, {"workshop1 place"});
        MapObjects& o = m.objects();
        const bool short_before = crew_short_;
        crew_short_ = false;   // only while a trebuchet waits for crew
        const int wall = o.find_by_name("wall2");
        if (!((b[1] >> 25) & 1u) && o.item(wall)) {
            const Unit* treb = nullptr;
            for (const Unit& u : m.world().units())
                if (!u.deleted && !m.world().dead(u) && u.nation == p.nation && formats::iequals(m.data().units[std::size_t(u.cls)].code, "TREBUCHET"))
                    treb = &u;
            if (treb) {
                ap.goal = "trebuchet at the wall";
                crew_short_ = short_before;
                if (ap.tick() >= siege_tick_ && rts_ok(m, p)) {
                    siege_tick_ = ap.tick() + 300;
                    bool fresh = false;
                    const std::vector<int> crew = trebuchet_crew(m, p, *treb, fresh);
                    if (!crew.empty()) ap.log.push_back(std::to_string(m.ticks()) + " crew " + std::to_string(crew.size()) + " to the trebuchet");
                    const int sel[] = {treb->id};
                    rts(m, p, [&](AiSystem& ai) {
                        // Nobody of an earlier crew near: its attach points may still be reserved by men stuck on the way
                        // (nearest_attach_point skips reserved points), so the weapon is double-clicked (dismiss) first.
                        if (fresh) ai.rts_dismiss_crew(treb->id);
                        if (!crew.empty()) ai.rts_assign(crew, treb->id, true);
                        ai.rts_attack_item(sel, wall);
                    });
                }
                if (dist(p.pos, treb->pos) > 400) ap.travel(f, p, treb->pos);
                return true;
            }
            const int fac = m.places().facility_by_name("workshop1");
            const Facility* fc = m.places().facility(fac);
            if (fc && m.state().gold >= 2000 + 500) {
                ap.goal = "build a trebuchet";
                if (find_workshop(m, p) != fac) {
                    ap.travel(f, p, fc->pos);
                    return true;
                }
                if (ap.tick() >= siege_tick_) {   // the workshop screen: one TREBUCHET, BUILD
                    siege_tick_ = ap.tick() + 300;
                    FacilityOrder order = FacilityOrder::siege_workshop(m, fac);
                    for (int row = 0; row < 4; ++row)
                        if (const int t = order.row_type(m, row); t && formats::iequals(m.data().units[std::size_t(t - 1)].code, "TREBUCHET"))
                            order.more(m, row);
                    order.commit(m);
                }
                return true;
            }
        }
        return hunt_in(ap, f, p, {"St La Charite"});
    }

    // A fight / hunt target the player gets nowhere with (a poleman on a wall walkway she backs away from for a bow
    // angle that never comes): 30 s on it without its HP moving, it is given up on (Autopilot::give_up).
    void after_tick(Autopilot& ap) override {
        int id = 0;
        for (const char* k : {"fight #", "hunt #", "boss #", "volley at #"})
            if (ap.goal.rfind(k, 0) == 0) id = std::atoi(ap.goal.c_str() + std::strlen(k));
        const Unit* u = ap.mission().world().unit(id);
        if (!u || id != stall_id_ || u->hp != stall_hp_) {
            stall_id_ = id, stall_hp_ = u ? u->hp : 0, stall_tick_ = ap.tick();
            return;
        }
        if (ap.tick() - stall_tick_ > 900) ap.give_up(id), stall_id_ = 0;
    }

    // Potion shop (buy screen open): HP food (item types 0x15 / 0x19) up to 60 carried, never below the plan's gold keep.
    void shop(Autopilot& ap, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        const GameData& gd = m.data();
        for (const ShopStock& s : shop_stock(m, m.ui().buy_shop)) {
            const ItemDef& d = gd.items[std::size_t(s.item)];
            while ((d.type == 0x15 || d.type == 0x19) && s.quantity != 0 && food(m, p) < 60 && m.state().gold - d.price >= ap.shop_gold_keep &&
                   w.give_item(p, d.code, s.level, std::max(1, d.quantity)))
                m.state().gold -= d.price;
        }
    }

private:
    static const EndCondition* objective(Mission& m, int n) {
        const auto& e = m.state().end;
        return n >= 1 && n <= int(e.size()) ? &e[std::size_t(n - 1)] : nullptr;
    }

    static const Unit* hero(Mission& m, std::string_view code) {
        const World& w = m.world();
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && m.data().units[std::size_t(u.cls)].code == code) return &u;
        return nullptr;
    }

    static int food(Mission& m, const Unit& p) {
        int n = 0;
        for (const ItemInstance& it : p.inv.bag) {
            const int t = m.data().items[std::size_t(it.item)].type;
            n += (t == 0x15 || t == 0x19) ? it.quantity : 0;
        }
        return n;
    }
    static const Unit* potion_seller(Mission& m) { return named(m, "Alfonse Potier"); }

    // A capture objective's place cleared by hand: the nearest fightable enemy (as fightable_enemy_exist_in_place: not unit
    // class 0 / 8 / 9, not special 2 / 3 / 7) in one of `places`, walked up to and shot at when in a clear line within
    // 1500 (wall walkways, arbalest crews); 80 s on one without its death and it is given up on for a while.
    bool hunt_in(Autopilot& ap, InputFrame& f, Unit& p, std::initializer_list<const char*> places) {
        Mission& m = ap.mission();
        World& w = m.world();
        const Unit* best = nullptr;
        float best_d = 1e30f;
        const int here = ap.region(p.pos);
        for (const char* name : places) {
            const int pr = m.places().place_by_name(name);
            for (const Unit& u : w.units()) {
                if (u.deleted || w.dead(u) || !w.hostile_to_player(u) || ap.unreached(u) || !m.places().inside(pr, u.pos)) continue;
                const UnitClass& c = m.data().units[std::size_t(u.cls)];
                if (c.unit_class == 0 || c.unit_class == 8 || c.unit_class == 9 || c.special == 2 || c.special == 3 || c.special == 7) continue;
                const float d = dist(p.pos, u.pos) + std::fabs(u.pos.z - p.pos.z) * 2;
                if (d < best_d && (ap.linked(here, ap.unit_region(u)) || (dist(p.pos, u.pos) < 1500 && ap.clear_shot(p, u)))) best_d = d, best = &u;
            }
        }
        if (!best) return false;
        if (++hunt_spent_[best->id] > 2400) {
            ap.give_up(best->id), hunt_spent_[best->id] = 0;
            return false;
        }
        ap.goal = "hunt #" + std::to_string(best->id) + " " + m.data().units[std::size_t(best->cls)].code;
        if (dist(p.pos, best->pos) < 1500 && ap.clear_shot(p, *best)) ap.shoot(f, p, *best);
        else ap.travel(f, p, best->pos);
        return true;
    }

    // One RTS session (F12 in, the commands, F12 out), as Session::toggle_rts: Rts_init hands the player to the AI,
    // Rts_deinit takes her back with her task parked. The commands are the RTS click handlers (Rts_cmd_*).
    template <class F> static void rts(Mission& m, Unit& p, F&& commands) {
        World& w = m.world();
        PlayerController& pc = w.controller();
        pc.prev_mode = w.game_mode;
        w.game_mode = 4;
        m.ai().rts_enter(p);
        commands(m.ai());
        m.ai().rts_exit(p);
        m.ai().clear_task(p);
        w.game_mode = pc.prev_mode;
        pc.prev_mode = 4;
    }
    static bool rts_ok(Mission& m, const Unit& p) {
        const World& w = m.world();
        return !m.ui().game_cut_scene && !w.cut_scene && !w.dead(p) && m.ticks() >= 60;
    }

    // The trebuchet shoots only with its F (front) attach points manned, and the RTS assign onto a siege weapon
    // (Rts_cmd_assign 0x44f750 branch A) takes light infantry only (the walkthrough: "assigning three infantrymen"). Joan's
    // light infantry within 2000 not yet manning or walking to it, one per free F point (French light infantry elsewhere
    // are La Charité's prisoners or troops a map away; crew sent from afar got stuck at (30425,15345)). `fresh`: nobody
    // near mans it or walks to it. Short of Joan's light infantry, crew_short_ sends her to recruit. Empty when manned.
    std::vector<int> trebuchet_crew(Mission& m, const Unit& p, const Unit& treb, bool& fresh) {
        World& w = m.world();
        AiSystem& ai = m.ai();
        crew_short_ = false;
        if (ai.all_attached(treb, 'F') != 0) return {};
        int total = 0;
        for (const auto& a : m.data().units[std::size_t(treb.cls)].attach_points) total += (a.usage & 2) != 0;
        int need = total, mine = 0;   // mine: Joan's light infantry anywhere
        std::vector<int> sel;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || u.nation != p.nation || m.data().units[std::size_t(u.cls)].code.find("LIGHT_INFANTRY") == std::string::npos)
                continue;
            mine += u.leader == p.id;
            if (dist(u.pos, treb.pos) > 2000) continue;
            if (u.m.vehicle == treb.id || (ai.curr_task(u) == task::LARGE_WEAPON_PUSH && ai.curr_task_para(u, 1) == treb.id)) --need;
            else if (u.leader == p.id) sel.push_back(u.id);
        }
        fresh = need == total;
        if (int(sel.size()) > need) sel.resize(std::size_t(std::max(need, 0)));
        crew_short_ = mine < need;
        return sel;
    }

    // The Bastard follows Joan (Rts_cmd_assign 0x44f750 branch B, commander on commander: FOLLOW_UNIT), the walkthrough's
    // "assign everyone to follow Joan"; he withdraws when beaten (retreat 1). Jean and Alencon, who must live (imperative
    // 10 / 11), stay at the French camp; La Hire stays there for the inner gate. True when nobody is left to call.
    static bool call_army(Mission& m, Unit& p) {
        if (!rts_ok(m, p)) return false;
        const Unit* b = hero(m, "ORLEAN");
        if (!b || (m.ai().curr_task(*b) == task::FOLLOW_UNIT && m.ai().curr_task_para(*b, 1) == p.id)) return true;
        const int sel[] = {b->id};
        rts(m, p, [&](AiSystem& ai) { ai.rts_assign(sel, p.id, true); });
        return true;
    }

    static bool is(Mission& m, const Unit& u, std::string_view code) { return m.data().units[std::size_t(u.cls)].code == code; }
    static const Unit* named(Mission& m, std::string_view name) {
        const World& w = m.world();
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && u.name == name) return &u;
        return nullptr;
    }

    // Character screen: the strongest bow / crossbow in the bag into the ranged slot (Martin Gough drops the Dragon
    // crossbow, the walkthrough's best ranged weapon of the game).
    static void best_launcher(Mission& m, Unit& p) {
        const GameData& gd = m.data();
        auto power = [&](const ItemInstance& i) { return i.item > 0 ? gd.items[std::size_t(i.item)].base_damage(i.level) : -1; };
        int best = -1, best_power = power(p.inv.ranged);
        for (std::size_t i = 0; i < p.inv.bag.size(); ++i) {
            const ItemDef& d = gd.items[std::size_t(p.inv.bag[i].item)];
            if ((d.type == 0xb || d.type == 0xc) && (d.usable_unit < 0 || d.usable_unit == p.cls) && power(p.inv.bag[i]) > best_power)
                best = int(i), best_power = power(p.inv.bag[i]);
        }
        if (best >= 0) m.world().equip(p, best);
    }

    // Saint-Pierre's inner wooden gate ("StPierre gate 2", 300 HP) shuts the monastery hill off once Gough's gate cut scene
    // ran. A gate takes melee only from La Hire (x3, Gate_take_melee 0x48c8c0) or a ram, and the AI attacks nothing but boxes
    // on an RTS attack-item (is_item_box in every RTS_ATTACK_ITEM routine), so, as the walkthrough's "switch to La Hire and
    // beat down the wooden gate": with the gate's surroundings clear and Joan near, switch to La Hire (key 3), walk him
    // to the gate, swing at it until it falls, switch back to Joan (key 1). True when this tick's input is set.
    bool gate_job(Autopilot& ap, InputFrame& f, Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        const EndCondition* e = objective(m, 4);
        const Unit* gough = hero(m, "MATTHEWGOUGH");
        const Unit* joan = hero(m, "JOAN_OF_ARC");
        const Unit* lahire = hero(m, "LAHIRE");
        const int gate = m.objects().find_by_name("StPierre gate 2");
        const MapItem* g = m.objects().item(gate);
        const bool need = e && e->enabled && e->finish != 1 && gough && joan && lahire && g && g->state == 0 && g->hp > 0 &&
                          gate_spent_ < 9000 && !ap.linked(ap.region(joan->pos), ap.unit_region(*gough));
        gate_at_ = need ? g->pos : Vec3{};
        if (is(m, p, "LAHIRE")) {
            if (!need) return m.switch_hero(1);
            ++gate_spent_;
            for (const Unit& u : w.units())  // under attack: the generic fight first
                if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && !u.no_react && dist(u.pos, p.pos) < 250 && std::fabs(u.pos.z - p.pos.z) < 150)
                    return false;
            ap.goal = "la hire at gate " + std::to_string(gate);
            if (dist(p.pos, g->pos) > 110) {
                ap.travel(f, p, g->pos);
                return true;
            }
            const float a = w.rel_angle(p, g->pos);
            f.mouse_dx = a / 0.18f;
            if (std::fabs(a) < 20 && ap.tick() % 2 == 0) f.press(IN_ATTACK_A);
            return true;
        }
        if (!need || !is(m, p, "JOAN_OF_ARC") || ap.tick() < switch_tick_ || dist(p.pos, g->pos) > 3000) return false;
        const int here = ap.region(p.pos);  // the gate's near side (the polemen behind it wait for La Hire)
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && !u.no_react && dist(u.pos, g->pos) < 1200 &&
                std::fabs(u.pos.z - g->pos.z) < 300 && ap.linked(here, ap.unit_region(u)))
                return false;
        switch_tick_ = ap.tick() + 900;
        return m.switch_hero(3);
    }

    static constexpr Vec3 kCouncillor{58900, 20020, 410};  // place "councillor stand"
    static constexpr Vec3 kWorkshopScene{23280, 7300, 0};  // place "workshop cutscene"
    int siege_tick_ = 0, recruit_tick_ = 0;  // next workshop order / trebuchet RTS order; next recruit visit
    bool crew_short_ = false;                 // the trebuchet lacks crew Joan's troop could give: recruit light infantry
    int army_tick_ = 0, switch_tick_ = 0, gate_spent_ = 0;
    Vec3 gate_at_{};  // the inner gate La Hire is to break (0 none)
    std::map<int, int> barrel_spent_;  // gold barrel -> ticks spent going for it
    std::map<int, int> hunt_spent_;    // capture-hunt target -> ticks spent on it
    int stall_id_ = 0, stall_hp_ = 0, stall_tick_ = 0;
};

}  // namespace

std::unique_ptr<MissionPlan> make_plan_S7L1() { return std::make_unique<PlanS7L1>(); }

}  // namespace joa::game
