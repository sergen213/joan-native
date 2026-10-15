// Campaign mission 6 (S9L1, "The Sieges of Meung and Beaugency"): S9L1 continues in S5L1's world (Jargeau taken).
// Win (S5L1_des records, enabled by S9L1_tactic): 2 Meung, 9 every north village + Meung + Beaugency, 27 every south
// village + Jargeau, then 3 Beaugency (after Meung), 32 Talbot, 16 / 24 / 25 Glasdale, Fastolf, Scales. A place is
// liberated once Joan stands in it with no enemy left there (number_of_enemy_in_place; Meung / Beaugency count fightable
// enemies only); Meung, Loches and Montargis only once Jargeau is held, Janville, Patay and Tours once Beaugency is.
// The plan walks the bot to the nearest open place, hunts what stands in it, then stands inside.
#include <cmath>
#include <map>
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
    bool first(Autopilot& ap, InputFrame&, Unit& p) override { return army(ap, p), pick(ap, p), false; }

    void shop(Autopilot& ap, Unit& p) override { buy_food(ap, p); }

    void after_tick(Autopilot& ap) override {
        if (!ap.trace || ap.tick() % ap.trace) return;
        Mission& m = ap.mission();
        const Place* t = m.places().place(target_);
        int caps = 0;
        for (const Cap& c : kCaps) caps += m.places().place(m.places().place_by_name(c.place))->captured;
        std::printf("[plan %d] target %s captured %d/%zu gold %d food %d\n", ap.tick(), t ? t->name.c_str() : "-", caps, std::size(kCaps),
                    m.state().gold, food(m.world(), *m.world().unit(m.world().player_id())));
    }

    // Only the spy's house (8, Selles-en-Berry) and the final camp (9) lead; 4 / 5 mark Meung / Beaugency behind shut English
    // gates all mission (the places below lead there), the others are side errands (Dunois, d'Illiers, the child, Chateauneut).
    bool use_waypoint(Autopilot&, int slot, Vec3) override { return slot == 8 || slot == 9; }

    float foe_weight(Autopilot& ap, const Unit&, const Unit& u) override {
        Mission& m = ap.mission();
        const auto& c = m.data().units[std::size_t(u.cls)];
        if (c.special == 2) return 0;  // horses
        if (spy_walking(m) && in_box(m, "attackMeung", u.pos)) return 0;  // the spy only gets in with no French there
        const bool in = target_ && in_place(m, target_, u);
        if (c.unit_class == 8) return in && !cap(target_).fightable ? 0.5f : 0.0f;  // siege weapons hold a town too
        return in ? 0.3f : 1.0f;
    }

    bool before_hunt(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        pick(ap, p);
        const Place* pl = m.places().place(target_);
        if (!pl) return false;
        // An enemy still holding the place: hunt the nearest the bot can get at; the generic fight takes over in reach.
        const Unit* foe = nullptr;
        int left = 0;
        for (const Unit& u : w.units())
            if (counts(m, cap(target_), u) && (++left, !ap.unreached(u)) && (!foe || d2(p.pos, u.pos) < d2(p.pos, foe->pos))) foe = &u;
        if (foe) {
            ap.goal = "capture " + pl->name + ": #" + std::to_string(foe->id) + " (" + std::to_string(left) + " left)";
            walk(ap, f, p, foe->pos);
            return true;
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
        const Unit* best = nullptr;
        for (Unit& h : w.units()) {
            if (!commandable(m, p, h) || is_joan(m, h) || w.max_hp(h) < kSturdy || resting_.count(h.id)) continue;
            if (ap.hero_key(h) && (!best || h.hp > best->hp)) best = &h;
            if (d2(h.pos, p.pos) <= kRally && (ai.curr_task(h) != task::FOLLOW_UNIT || ai.curr_task_para(h, 1) != p.id)) {
                rts(m, p, [&](AiSystem& a) { a.rts_assign(a.rts_select_with_team(h.id), p.id, false); });
                if (ap.trace) std::printf("[plan %d] %s follows %s\n", ap.tick(), h.name.c_str(), p.name.c_str());
            }
        }
        if (is_joan(m, p) && best && m.switch_hero(ap.hero_key(*best)) && ap.trace)
            std::printf("[plan %d] switch to %s to lead the army\n", ap.tick(), best->name.c_str());
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
            // Meung: once one of its gates is open or down (the spy opens the south gate, S9L1_tactic l.4485).
            if (std::string_view(kCaps[i].place) == "Castle_Meung" && !gate_open(ap.mission(), "MeungSouthGate") &&
                !gate_open(ap.mission(), "MeungNorthGate"))
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
    static bool gate_open(Mission& m, std::string_view name) {
        for (int r = 1; r <= m.objects().size(); ++r)
            if (const MapItem* it = m.objects().item(r); it && it->cls == 1 && it->name == name) return it->state != 0 || it->hp <= 0;
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
    // Buy screen open (a shop the generic bot talks to): food (types 0x15 / 0x19) up to 40, keeping the spy's 3000 gold
    // (the click handler's purchase, as Autopilot::shop makes it). The plan sends no one shopping: every food trip (Joan and
    // Jean to Jargeau's shop across the English villages, Jean to Orleans' that left him on its wall walkway with no way
    // down for 55000 ticks) cost the run.
    void buy_food(Autopilot& ap, Unit& p) {
        Mission& m = ap.mission();
        if (food_ms_ == m.ui().interface_ms) return;
        food_ms_ = m.ui().interface_ms;
        World& w = m.world();
        int& gold = m.state().gold;
        for (const ShopStock& s : shop_stock(m, m.ui().buy_shop)) {
            const ItemDef& d = w.data().items[std::size_t(s.item)];
            if (d.type != 0x15 && d.type != 0x19) continue;
            while (s.quantity != 0 && food(w, p) < 40 && gold - d.price >= kSpyGold && w.give_item(p, d.code, s.level, std::max(1, d.quantity)))
                gold -= d.price, ap.log.push_back(std::to_string(m.ticks()) + " buy " + d.code + " gold " + std::to_string(gold));
        }
        if (ap.trace) std::printf("[plan %d] shop: food %d gold %d\n", ap.tick(), food(w, p), gold);
    }
    static constexpr int kSpyGold = 3000;  // Elya Tinchor's price (S9L1_tactic l.4409)
    std::uint32_t food_ms_ = ~0u;
};

}  // namespace

std::unique_ptr<MissionPlan> make_plan_S9L1() { return std::make_unique<PlanS9L1>(); }

}  // namespace joa::game
