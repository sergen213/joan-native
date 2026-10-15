// Campaign bot plan for mission 4 (S4L1, Orléans: Saint Augustin and Tourelles). S4L1_tactic.sce: the four French
// cannons shoot the Abtower only once Jean de Metz is rescued (objective 9) and the attack has started (scenario bit 31:
// the Abtower took damage, or the player stands in "Abtower Inside", whose door needs Glasdale's key). In scenario 4
// explosive arrows damage walls (Wall_on_event, MapObjects::wall_event), so the bot puts one into the tower. The tower's
// fall kills its garrison and the six arbalests (event bit 24); English reinforcements spawn in its ruin, and Saint
// Augustin is taken once none is left (the bot hunts them first). Tourelles is shut behind its gate (1500 HP); Cannons 2 and
// 3, turned on it once the tower falls, break it within ~3000 ticks (before Augustin's capture drives them off); a gate
// still standing after the capture takes the bot's explosive arrows into the portcullis (Gate_take_ranged:
// ranged_base_damage / 2 for the direct and the area event of each arrow; ~15 HP an arrow from the BOW), then Tourelles is
// cleared.
//
// Shopping: before Talbot, Tours (a stronger sword, food, 100 plain arrows) once on the Loches road; then Fierbois (visited
// for the priest anyway): explosive arrows up to kExplosive, food, plain arrows. Rests stop at 50 % HP and eat from 4 food on
// (Autopilot::rest_pct / rest_food): the 70 % rests at 1 HP / 150 ticks cost 20000+ ticks each.
// In Forêt Fierbois (the councillor's five villagers) only foes near the player are fought: the 32 English the councillor's
// talk spawns there, all counted near the errand's villagers, were chosen in turn from 3000 off and rooted the bot.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "game/autopilot_player.hpp"
#include "game/mission_ui.hpp"

namespace joa::game {

namespace {

float dist(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); }

constexpr int kExplosive = 40;    // carried: the Abtower's arrow, volleys (the cannons break the gate)
constexpr int kFood = 30;         // F3 food bought up to (20 ran out at Tourelles: two deaths with 6000+ gold left)
constexpr int kPlain = 300;       // plain arrows bought up to, out of the gold kept back for them (the archers left on the
                                  // Abtower ruin and walls at z 325..629 are only reached by arrows: 200 ran out there)

struct PlanS4L1 final : MissionPlan {
    int shop_until = -1;               // tick the trip to the weapon seller for the Abtower arrow gives up
    std::uint32_t shop_ms = ~0u;       // buy screen opening already shopped at
    std::map<int, int> keeper_skip;    // shopkeeper -> tick he is tried again
    int keeper = 0, keeper_since = 0;  // shopkeeper walked to, since

    static const Place* place(Mission& m, const char* n) { return m.places().place(m.places().place_by_name(n)); }
    static bool english_in(Mission& m, const char* n, const Unit& u) {
        const Place* a = place(m, n);
        return a && !a->captured && u.nation == m.data().nation_id("ENGLISH") && m.places().inside(a->recno, u.pos);
    }
    static const MapItem* gate(Mission& m) {
        const MapItem* g = m.objects().item(m.objects().find_by_name("Tourelles gate"));
        return g && !g->deleted && g->hp > 0 ? g : nullptr;
    }
    // The Abtower at 0 HP: its wall archers are gone (before that, the English in Augustin stand on its walls, z 325).
    static bool tower_down(Mission& m) {
        const MapItem* t = m.objects().item(m.objects().find_by_name("Abtower"));
        return !t || t->hp == 0;
    }
    // Explosive arrows the standing gate still takes (~15 HP each from the BOW, measured 13..19), 0 once it is down.
    static int gate_arrows(Mission& m) {
        const MapItem* g = gate(m);
        return g ? g->hp / 15 + 5 : 0;
    }
    static bool augustin_taken(Mission& m) { const Place* a = place(m, "Augustin"); return a && a->captured; }
    static int food(const World& w, const Unit& p) {  // F3 food (item types 0x15 / 0x19), as the bot counts it
        int n = 0;
        for (const ItemInstance& it : p.inv.bag)
            if (const int t = w.data().items[std::size_t(it.item)].type; t == 0x15 || t == 0x19) n += it.quantity;
        return n;
    }
    static bool has_item(const World& w, const Unit& p, std::string_view code) {
        for (const ItemInstance& it : p.inv.bag)
            if (w.data().items[std::size_t(it.item)].code == code) return true;
        return false;
    }
    static bool errand_open(Mission& m, std::size_t i) {
        const auto& e = m.state().end;
        return e.size() > i && e[i].enabled && e[i].finish != 1;
    }
    // Talbot not beaten yet: his retreat drops Jean's key (keyS4_02), objective 9 once Jean is talked to with it.
    static bool before_talbot(Mission& m, const Unit& p) {
        const auto& e = m.state().end;
        return !has_item(m.world(), p, "keyS4_02") && !(e.size() > 8 && e[8].finish == 1);
    }

    // English inside the uncaptured Augustin place go first: its capture (no ENGLISH in the place, S4L1_tactic) turns the
    // cannons on the Tourelles gate (the generic hunt walked to Tourelles, the first WIN objective's territory, and left one
    // archer in Augustin for 40000 ticks); once the gate is down, the English in Tourelles.
    float foe_weight(Autopilot& ap, const Unit& p, const Unit& u) override {
        Mission& m = ap.mission();
        const float w = MissionPlan::foe_weight(ap, p, u);
        if (w <= 0) return w;
        // Forêt Fierbois (outside the town): only foes near the player. Its place covers the church, the wagon's track and the
        // councillor's villagers, so every English in it counted as near an errand NPC from up to 13000 off: the bot chased
        // them in turn (rooted for 76000 ticks among the villagers' 32 guards, or dragged the wagon off its track).
        const Place* forest = place(m, "Fierbois_Forest");
        const Place* town = place(m, "Fierbois");
        if (forest && m.places().inside(forest->recno, u.pos) && !(town && m.places().inside(town->recno, u.pos)) &&
            dist(p.pos, u.pos) > 1500 && u.target != p.id)
            return 0;
        // Waiting for the cannons to break the gate (first()): the Augustin English are left to come at her; its capture
        // drives the cannons off (S4L1_tactic RTS_ATTACK_ITEM) and left the bot a 1500 HP gate and 4 explosive arrows.
        if (wait_cannons && english_in(m, "Augustin", u) && u.target != p.id) return 0;
        if (tower_down(m) && english_in(m, "Augustin", u)) return w * 0.3f;
        if (augustin_taken(m) && !gate(m) && english_in(m, "Tourelles", u)) return w * 0.3f;
        return w;
    }

    // No generic talk errands once the priest's and the councillor's are done (Jean de Metz is led to by the plan): the
    // councillor kept his talk icon and was walked to across the map again and again.
    bool use_talker(Autopilot& ap, const Unit&) override {
        const auto& e = ap.mission().state().end;
        return !(e.size() > 3 && e[2].finish == 1 && e[3].finish == 1);
    }

    bool first(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        const World& w = m.world();
        // Before Talbot (the Tours sellers): no explosive arrows, 100 plain; the gold goes on a sword and food first. The 30
        // explosive arrows carried go into the volleys.
        const bool pre = before_talbot(m, p);
        const int eat = food(w, p), pct = p.hp * 100 / std::max(1, w.max_hp(p));
        ap.shop_explosive_max = pre ? 0 : kExplosive;
        ap.shop_plain_max = pre ? 100 : kPlain;
        ap.shop_gold_keep = (eat < kFood ? 2500 : 0) + (ap.arrows(p, false) < kPlain ? 3000 : 0);
        ap.rest_pct = eat ? 50 : 70, ap.rest_food = 3;
        // Five explosive arrows kept for the Abtower once Talbot is beaten (its damage starts the attack): the volleys spent
        // all 40 on the way, and the trip back to Fierbois for one cost 10000+ ticks.
        ap.explosive_reserve = !pre && !tower_down(m) ? 5 : 0;
        ap.plan_talk = 0;
        // The tower down, the gate up, Augustin not taken: Cannons 2 and 3 turn on the gate (their crews attach near Joan) and
        // break it within ~3000 ticks; the bot stands by them (before_targets) for up to 12000 ticks from her arrival (20000
        // in all) before clearing Augustin.
        const bool cannons = tower_down(m) && gate(m) && !augustin_taken(m);
        if (!cannons) cannon_seen = cannon_since = -1;
        else if (cannon_seen < 0) cannon_seen = ap.tick();
        wait_cannons = cannons && ap.tick() - cannon_seen < 20000 && (cannon_since < 0 || ap.tick() - cannon_since < 12000);
        // Out of food below 30 %: rest where she stands until 60 %, fighting only whoever is after her within 300 (the guard
        // comes back below 45 %); the generic retreat ran back to La Hire at the start, 7000 off through the English, and
        // died on the way (a level-10 start without food).
        starving = !eat && (pct < 30 || (starving && pct < 60));
        if (!starving) return false;
        ap.goal = "plan: rest without food hp " + std::to_string(pct) + "%";
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && u.target == p.id && dist(u.pos, p.pos) < 300 && std::fabs(u.pos.z - p.pos.z) < 150)
                return ap.fight(f, p, u), true;
        return true;
    }
    bool starving = false, wait_cannons = false;
    int cannon_seen = -1, cannon_since = -1;  // ticks the cannon wait began, she arrived by them

    // After the generic purchases: a stronger sword (Tours sells JOAN_SWORD_LV4: the chain can reach mission 4 with LV1,
    // a quarter of its damage), food (best heal first: meat and chicken heal fully), plain arrows once Talbot is beaten.
    void shop(Autopilot& ap, Unit& p) override {
        Mission& m = ap.mission();
        if (shop_ms == m.ui().interface_ms) return;
        shop_ms = m.ui().interface_ms;
        World& w = m.world();
        int& gold = m.state().gold;
        const bool pre = before_talbot(m, p);
        served.insert(keeper);
        std::vector<ShopStock> stock = shop_stock(m, m.ui().buy_shop);
        const auto heal = [&](const ShopStock& s) { const ItemDef& d = w.data().items[std::size_t(s.item)]; return d.type == 0x15 || d.type == 0x19 ? d.damage : 0; };
        std::stable_sort(stock.begin(), stock.end(), [&](const ShopStock& a, const ShopStock& b) { return heal(a) > heal(b); });
        ap.make_room(p);  // Tours could not place the sword in 29 scattered loaves, Fierbois no explosive arrows
        const ItemInstance& hand = p.inv.melee[0];
        const int cur = hand.item ? w.data().items[std::size_t(hand.item)].base_damage(hand.level) : 0;
        bool sword = false;
        for (const ShopStock& s : stock) {
            const ItemDef& d = w.data().items[std::size_t(s.item)];
            auto buy = [&] {
                if (gold < d.price || !w.buy_item(p, s.item, s.level, std::max(1, d.quantity))) return false;
                gold -= d.price;
                ap.log.push_back(std::to_string(m.ticks()) + " buy " + d.code + " gold " + std::to_string(gold));
                return true;
            };
            if (d.type == 9 && !sword && (d.usable_unit < 0 || d.usable_unit == p.cls) && d.base_damage(s.level) > cur) sword = buy();
            if (heal(s))
                for (int n = 0; n < s.quantity && food(w, p) < kFood && buy(); ++n) {}
            if (d.type == 0xe && !pre)
                while (ap.arrows(p, false) < kPlain && buy()) {}
        }
        keeper_skip[keeper] = ap.tick() + 9000;  // served: not again for 5 min
    }
    std::set<int> served;  // shopkeepers whose buy screen opened

    // A shopkeeper within 4000 to walk to (the Fierbois sellers, at the priest's errand): a weapon seller for explosive
    // arrows, a potion seller for food. Out of the arrows the way on needs (plain for Augustin's wall archers, explosive for
    // the gate): the nearest weapon seller. Before Talbot, the two Tours sellers once on the Loches road south of them
    // (y > 11000, within 7000: from the north the walk led through Glasdale's camp, 30000 ticks of fighting): his fight
    // with the 14 crossbowmen his wound calls burned 20 food.
    const Unit* shop_errand(Autopilot& ap, const Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        const int gold = m.state().gold;
        const bool pre = before_talbot(m, p) && gold >= 2000;
        const bool expl = ap.arrows(p, true) < kExplosive - 20 && gold >= 2500, eat = food(w, p) < kFood / 2 && gold >= 1000;
        // Out of food after Talbot: the nearest potion seller from anywhere (before him, from the start, the walk to Tours
        // led through Glasdale's camp: four deaths).
        const bool starve = food(w, p) < 3 && gold >= 1000 && !before_talbot(m, p);
        const bool restock = gold >= 1000 && ((ap.arrows(p, true) < gate_arrows(m) && augustin_taken(m)) ||
                                              (!ap.arrows(p, false) && tower_down(m) && !augustin_taken(m)));
        const Unit* best = nullptr;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || !u.m.can_interact || w.hostile_to_player(u)) continue;
            if (const auto s = keeper_skip.find(u.id); s != keeper_skip.end() && ap.tick() < s->second) continue;
            const std::string& code = w.data().units[std::size_t(u.cls)].code;
            const bool weapon = code.starts_with("WEAPON_SHOPKEEPER"), potion = code.starts_with("POTION_SHOPKEEPER");
            const bool want = (weapon && restock) || (potion && starve) || (dist(p.pos, u.pos) < 4000 && ((weapon && expl) || (potion && eat))) ||
                              (pre && (weapon || potion) && !served.count(u.id) && served.size() < 2 &&
                               ((dist(p.pos, u.pos) < 7000 && p.pos.y > 11000) || u.id == keeper));
            if (want && (!best || dist(p.pos, u.pos) < dist(p.pos, best->pos))) best = &u;
        }
        return best;
    }

    // The councillor's errand (objective 4): his talk sends the player for the five villagers (scenario bit 10), then each
    // "Missing Villager" showing the talk icon, nearest first, then the councillor once all five were talked to (bit 23).
    // The talks alone deliver them (S4L1_tactic: the councillor's distance checks are commented out), so the generic
    // escort waits (40000+ ticks of walking back and forth with five slow followers) are skipped. A councillor in retreat
    // (English in the Fierbois town) answers "Not now!": the generic fight clears the town first.
    static const Unit* villager_errand(Mission& m, const Unit& p) {
        const auto& bits = m.vm().state().bits[0];
        if (!errand_open(m, 3)) return nullptr;
        World& w = m.world();
        const bool sent = (bits >> 9) & 1u, found = (bits >> 22) & 1u;
        const Unit* best = nullptr;
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || !u.m.can_interact) continue;
            const bool want = !sent || found ? u.name == "Councillor" && !u.retreat : u.name.starts_with("Missing Villager");
            if (want && (!best || dist(p.pos, u.pos) < dist(p.pos, best->pos))) best = &u;
        }
        return best;
    }

    // The priest's wagon (a cart following the player, FOLLOW_UNIT) that has not moved 100 in 20 s while more than 600 off:
    // the player walks back to within 250 of it (it stalls off its track and the generic escort wait stood at the priest
    // for 150000 ticks); the follow resumes from there.
    std::map<int, std::pair<Vec3, int>> cart_seen;  // cart -> (position, tick) of its last 100 of movement
    int cart_fetch = 0;                             // cart being fetched
    const Unit* stuck_cart(Autopilot& ap, const Unit& p) {
        World& w = ap.mission().world();
        for (const Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || w.data().units[std::size_t(u.cls)].unit_class != 9 || !ap.following(u)) continue;
            auto& [at, since] = cart_seen.try_emplace(u.id, u.pos, ap.tick()).first->second;
            if (dist(at, u.pos) > 100) at = u.pos, since = ap.tick();
            const float d = dist(p.pos, u.pos);
            if (cart_fetch == u.id && d < 250) cart_fetch = 0, since = ap.tick();
            if (cart_fetch == u.id || (d > 600 && ap.tick() - since > 600)) return cart_fetch = u.id, &u;
        }
        cart_fetch = 0;
        return nullptr;
    }

    // The priest's errand (objective 3): the wagon at the church is talked to (it follows on its cart track, "All the goods
    // are in the cart": scenario bit 32), led back to the priest and waited for, standing, while it closes in; the priest
    // only hands over the sword with it within 600 (S4L1_tactic l.2969). A wagon that stopped ("I shall remain here": the
    // player left its track) is talked to again. The generic escort walk took the wagon after far foes and lost it.
    bool wagon_errand(Autopilot& ap, InputFrame& f, const Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        if (!errand_open(m, 2)) return false;
        const Unit *wagon = nullptr, *priest = nullptr;
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u)) {
                if (u.name == "Horse Wagon Driver") wagon = &u;
                if (u.name == "Priest") priest = &u;
            }
        if (!wagon || !priest) return false;
        if (!ap.following(*wagon)) return wagon->m.can_interact && walk_to_npc(ap, f, p, *wagon, "wagon:");
        if (!((m.vm().state().bits[0] >> 31) & 1u)) return false;  // following, but not with the goods
        if (dist(p.pos, wagon->pos) > 600 && dist(p.pos, priest->pos) < 2500) {
            ap.goal = "plan: wait for the wagon #" + std::to_string(wagon->id);
            return true;
        }
        return walk_to_npc(ap, f, p, *priest, "goods to");
    }

    // The priest or the councillor needed (objectives 3 / 4 open) but in retreat: they answer "Not now!" while any English
    // stands in the Fierbois place (S4L1_tactic l.2711). Walk to the nearest of them; the generic fight takes
    // over within 400 (the bot stood at the priest for 190000 ticks with two archers 1900 off in the town).
    static bool clear_town(Autopilot& ap, InputFrame& f, const Unit& p) {
        Mission& m = ap.mission();
        World& w = m.world();
        bool needed = false;
        for (const Unit& u : w.units())
            if (!u.deleted && u.retreat && ((u.name == "Priest" && errand_open(m, 2)) || (u.name == "Councillor" && errand_open(m, 3)))) needed = true;
        if (!needed) return false;
        const Unit* best = nullptr;
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && u.m.active && !ap.unreached(u) && english_in(m, "Fierbois", u) &&
                (!best || dist(p.pos, u.pos) < dist(p.pos, best->pos)))
                best = &u;
        if (!best || dist(p.pos, best->pos) < 400) return false;
        ap.goal = "plan: clear Fierbois #" + std::to_string(best->id);
        go(ap, f, p, best->pos);
        return true;
    }

    bool walk_to_npc(Autopilot& ap, InputFrame& f, const Unit& p, const Unit& u, const std::string& why) {
        World& w = ap.mission().world();
        ap.goal = "plan: " + why + " #" + std::to_string(u.id) + " " + u.name;
        ap.plan_talk = u.id;
        // Bag room before the buy screen's generic purchases.
        if (dist(p.pos, u.pos) < 300 && w.data().units[std::size_t(u.cls)].code.find("SHOPKEEPER") != std::string::npos) ap.make_room(*w.unit(p.id));
        if (dist(p.pos, u.pos) > 60) go(ap, f, p, u.pos);
        else f.mouse_dx = w.rel_angle(p, u.pos) / 0.18f;  // face him: the interact prompt (decide) talks
        return true;
    }

    // Fierbois (nav level 1) and the Loire forts (level 2) meet at x 16384; the forest crossing at y 27264 is the short way
    // between them. Walk to its near side first, then across: the route planner's level detour, re-planned after each fight
    // on the way, went round by Saint Jean le Blanc (28000 ticks from the councillor to Jean de Metz).
    static void go(Autopilot& ap, InputFrame& f, const Unit& p, Vec3 g) {
        const int a = ap.region(p.pos) / 100000, b = ap.region(g) / 100000;
        const Vec3 west{16000, 27264, p.pos.z}, east{16700, 27264, p.pos.z};
        if (a == 1 && b == 2) ap.travel(f, p, dist(p.pos, west) > 150 ? west : east);
        else if (a == 2 && b == 1) ap.travel(f, p, dist(p.pos, east) > 150 ? east : west);
        else ap.travel(f, p, g);
    }

    // Explosive arrows at `aim` from `spot`: walk there first (range mode is left on the way: Autopilot::step), shoot within
    // 150 of it (a range-mode check here once fired 160 arrows from 10000 off).
    static bool shoot_from(Autopilot& ap, InputFrame& f, Unit& p, Vec3 spot, Vec3 aim) {
        if (dist(p.pos, spot) > (ap.mission().world().controller().range_mode ? 150 : 80)) {
            go(ap, f, p, spot);
            return true;
        }
        ap.load_ammo(p, true);
        ap.shoot_at(f, p, aim, p.action == SHTIDLE);
        return true;
    }

    bool before_hunt(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        if (!tower_down(m)) return false;
        const char* where = !augustin_taken(m) ? "Augustin" : !gate(m) ? "Tourelles" : nullptr;
        if (!where) return false;
        const Unit* best = nullptr;
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && u.m.active && !ap.unreached(u) && foe_weight(ap, p, u) > 0 && english_in(m, where, u) &&
                (!best || dist(p.pos, u.pos) < dist(p.pos, best->pos)))
                best = &u;
        if (!best) return false;
        // No 300 nearer in 3000 ticks (a wall walker up a ladder the bot does not mount: 40000 ticks at its foot): given up
        // on for a while (Autopilot::give_up), the next one first.
        if (best->id != hunt_id || ap.tick() - hunt_last > 60 || dist(p.pos, best->pos) < hunt_best - 300)
            hunt_id = best->id, hunt_best = dist(p.pos, best->pos), hunt_since = ap.tick();
        hunt_last = ap.tick();
        if (ap.tick() - hunt_since > 3000) return ap.give_up(best->id), hunt_id = 0, false;
        ap.goal = std::string("plan: clear ") + where + " #" + std::to_string(best->id);
        go(ap, f, p, best->pos);
        return true;
    }
    int hunt_id = 0, hunt_since = 0, hunt_last = -1;  // foe hunted, tick of its last 300 of headway, last tick hunted
    float hunt_best = 0;                              // nearest to it yet

    bool before_targets(Autopilot& ap, InputFrame& f, Unit& p) override {
        Mission& m = ap.mission();
        World& w = m.world();
        // Talbot wounded within 1500 on her level: kept at while Joan has food or 40 % HP and at most two others are on her.
        // He regenerates in his DEFEND_PLACE hill (and eats a full-heal meat below 20 %): the bot gave him up there, rested
        // 600 off and watched him climb from 213 back to 940 HP. Up the hill (z +500) he is left to the generic fight: the
        // walk at him stood under his 14 crossbowmen for 20000 ticks.
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && u.hp < w.max_hp(u) && dist(u.pos, p.pos) < 1500 &&
                std::fabs(u.pos.z - p.pos.z) < 150 && w.data().units[std::size_t(u.cls)].code == "LORD_TALBOT" &&
                (food(w, p) || p.hp * 100 >= 40 * w.max_hp(p))) {
                int others = 0;
                for (const Unit& o : w.units())
                    others += o.id != u.id && !o.deleted && !w.dead(o) && w.hostile_to_player(o) && o.target == p.id && dist(o.pos, p.pos) < 200;
                if (others > 2) break;
                ap.goal = "plan: Talbot hp " + std::to_string(u.hp);
                ap.fight(f, p, u);
                return true;
            }
        // Someone on the player (on her level: the Abtower's wall archers shoot from z 325): the generic fight first.
        for (const Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && u.target == p.id && dist(u.pos, p.pos) < 400 &&
                std::fabs(u.pos.z - p.pos.z) < 150 && (!ap.unreached(u) || dist(u.pos, p.pos) < 150))
                return false;
        // Waiting for the cannons (first()): by them, ahead of any shop trip (the restock walk to Tours left them without
        // crews: they attach near Joan, and the gate stood at 1500 until Augustin fell).
        if (const Place* cp = place(m, "Place Cannon 1"); wait_cannons && cp) {
            ap.goal = "plan: wait for the cannons (gate hp " + std::to_string(gate(m)->hp) + ")";
            if (dist(p.pos, cp->pos) > 300) go(ap, f, p, cp->pos);
            else if (cannon_since < 0) cannon_since = ap.tick();
            return true;
        }
        if (!m.ui().buy_open)
            if (const Unit* k = shop_errand(ap, p)) {
                if (k->id != keeper) keeper = k->id, keeper_since = ap.tick();
                if (ap.tick() - keeper_since > 6000) keeper_skip[keeper] = ap.tick() + 9000;  // 3 min without the buy screen
                return walk_to_npc(ap, f, p, *k, "shop at");
            }
        if (const Unit* v = villager_errand(m, p)) return walk_to_npc(ap, f, p, *v, "villagers:");
        if (clear_town(ap, f, p)) return true;
        if (const Unit* c = stuck_cart(ap, p)) {
            ap.goal = "plan: back to the stuck wagon #" + std::to_string(c->id);
            go(ap, f, p, c->pos);
            return true;
        }
        if (wagon_errand(ap, f, p)) return true;
        // Jean de Metz (objective 9) once the Fierbois errands are done, with Talbot's key (keyS4_02): talked to, he is free
        // and the cannons may shoot the Abtower (the bot otherwise walked into Saint Augustin, the next WIN territory, and
        // spent its arrows on the tower's wall archers).
        const auto& e = m.state().end;
        if (e.size() > 8 && e[8].enabled && e[8].finish != 1 && e[2].finish == 1 && e[3].finish == 1 && has_item(w, p, "keyS4_02"))
            for (const Unit& u : w.units())
                if (u.name == "Jean de Metz" && !u.deleted && !w.dead(u) && u.m.can_interact) return walk_to_npc(ap, f, p, u, "free");
        const auto& end = m.state().end;
        const bool jean_free = end.size() > 8 && end[8].finish == 1;
        const bool attack = (m.vm().state().bits[0] >> 30) & 1u;  // set_scenario_bit_flag(31)
        const MapItem* tower = m.objects().item(m.objects().find_by_name("Abtower"));
        if (jean_free && !attack && tower && tower->hp == tower->max_hp && p.inv.ranged.item) {
            if (!ap.arrows(p, true)) {
                if (shop_until < 0) shop_until = ap.tick() + 9000;
                if (ap.tick() > shop_until || m.state().gold < 500) return false;
                for (const Unit& u : w.units())
                    if (u.name == "Macaire Hougier" && !w.dead(u) && u.m.can_interact) return walk_to_npc(ap, f, p, u, "buy explosive arrows from");
                return false;
            }
            // Stand 650 from the tower's centre on the cannons' side (Place Cannon 1) and shoot at its middle.
            const Place* cp = place(m, "Place Cannon 1");
            if (!cp) return false;
            const float d = dist(cp->pos, tower->pos);
            const Vec3 spot{tower->pos.x + (cp->pos.x - tower->pos.x) / d * 650, tower->pos.y + (cp->pos.y - tower->pos.y) / d * 650, p.pos.z};
            ap.goal = "plan: shoot the Abtower (hp " + std::to_string(tower->hp) + ")";
            return shoot_from(ap, f, p, spot, {tower->pos.x, tower->pos.y, tower->pos.z + 300});
        }
        // Augustin taken, the gate still up: explosive arrows into the portcullis from 350 outside it (the cannons' side),
        // aimed at z 190 (inside its CollisionType box (-15, -200, 0)..(15, 200, 265) across the whole width).
        const MapItem* g = gate(m);
        if (!g || !augustin_taken(m) || !ap.arrows(p, true) || !p.inv.ranged.item) return false;
        const Place* cp = place(m, "Place Cannon 1");
        const float c = std::cos(g->dir), s = std::sin(g->dir);
        const float side = cp && (cp->pos.x - g->pos.x) * c + (cp->pos.y - g->pos.y) * s < 0 ? -1.0f : 1.0f;
        const Vec3 spot{g->pos.x + side * 350 * c, g->pos.y + side * 350 * s, p.pos.z};
        ap.goal = "plan: shoot the Tourelles gate (hp " + std::to_string(g->hp) + ")";
        return shoot_from(ap, f, p, spot, {g->pos.x + 12 * c, g->pos.y + 12 * s, g->pos.z + 190});
    }
};

}  // namespace

std::unique_ptr<MissionPlan> make_plan_S4L1() { return std::make_unique<PlanS4L1>(); }

}  // namespace joa::game
