// Campaign bot (test / diagnostic only). See autopilot_player.hpp.
#include "game/autopilot_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/multimap_nav.hpp"

namespace joa::game {

namespace {
constexpr float kDeg = 0.017453292f;
float d2(Vec3 a, Vec3 b) { return std::hypot(b.x - a.x, b.y - a.y); }
std::string at(Vec3 p) {
    char b[48];
    std::snprintf(b, sizeof b, "(%.0f,%.0f)", p.x, p.y);
    return b;
}
bool alive(const World& w, const Unit& u) { return !u.deleted && !w.dead(u) && u.hp > 0; }
}  // namespace

// --- decisions ----------------------------------------------------------------------------------

void Autopilot::steer(InputFrame& f, const Unit& p, Vec3 g, bool replan) {
    // Path on the mission navmesh (generous expansion budget: a player knows the roads).
    if (replan || d2(path_goal_, g) > 250 || tick_ - path_tick_ > 300 || wp_ >= path_.size()) {
        path_.clear(), wp_ = 0, path_goal_ = g, path_tick_ = tick_;
        if (NavWorld* nav = m_.nav()) {
            PathQuery q;
            q.from = {p.pos.x, p.pos.y, p.pos.z};
            q.to = {g.x, g.y, g.z};
            q.max_expand = 4000;
            for (const NavPt& n : nav->find_path(q).points) path_.push_back({n.x, n.y, n.z});
        }
        if (path_.empty()) path_.push_back(g);
    }
    while (wp_ + 1 < path_.size() && d2(p.pos, path_[wp_]) < 70) ++wp_;
    const Vec3 t = path_[wp_];
    f.camera_yaw_deg = std::atan2(t.y - p.pos.y, t.x - p.pos.x) / kDeg;
    f.held.set(IN_FORWARD);
    // No progress over 90 ticks: strafe and jump for 40, then replan. Stuck again (a house corner the
    // path mesh cuts, a unit in a doorway): run off sideways, alternating sides and further each time.
    if (tick_ - check_tick_ >= 90) {
        if (d2(check_pos_, p.pos) < 40) {
            ++stuck_count_, unstick_dir_ ^= 1, path_tick_ = -1000;
            unstick_until_ = tick_ + (stuck_count_ < 2 ? 40 : 30 + 20 * std::min(stuck_count_, 8));
        } else {
            stuck_count_ = 0;
        }
        check_pos_ = p.pos, check_tick_ = tick_;
    }
    if (tick_ < unstick_until_) {
        if (stuck_count_ >= 2) f.camera_yaw_deg += unstick_dir_ ? 90.0f : -90.0f;
        else f.held.set(unstick_dir_ ? IN_STRAFE_LEFT : IN_STRAFE_RIGHT);
        if (tick_ + 30 == unstick_until_) f.press(IN_JUMP);
    }
}

// Where the player's people are: the centroid of the units not hostile to the player (the town's people).
Vec3 Autopilot::home() const {
    const World& w = m_.world();
    float sx = 0, sy = 0;
    int n = 0;
    for (const Unit& u : w.units())
        if (alive(w, u) && u.id != w.player_id() && !w.hostile_to_player(u)) sx += u.pos.x, sy += u.pos.y, ++n;
    return n ? Vec3{sx / float(n), sy / float(n), 0} : Vec3{};
}

// The friendly side of a gate: towards home().
Vec3 Autopilot::gate_inside(const MapItem& g) const {
    const Vec3 h = home();
    const float dx = h.x - g.pos.x, dy = h.y - g.pos.y, l = std::max(1.0f, std::hypot(dx, dy));
    return {g.pos.x + dx / l * 100, g.pos.y + dy / l * 100, g.pos.z};
}

// Walk to g. The path mesh shuts a closed gate's passage, so a path ending well short of g with a closed
// interactable gate near its end is blocked by that gate: walk up to it and open it (R at its indicator).
void Autopilot::travel(InputFrame& f, const Unit& p, Vec3 g) {
    const MapObjects& o = m_.objects();
    travel_tick_ = tick_;
    if (const MapItem* it = o.item(open_gate_); it && it->state == 0 && d2(open_goal_, g) < 500) {
        goal += " via gate";
        if (d2(p.pos, it->pos) > 100) steer(f, p, it->pos, false);
        return;
    }
    open_gate_ = 0;
    steer(f, p, g, false);
    if (path_.empty() || (d2(path_.back(), g) < 300 && std::fabs(path_.back().z - g.z) < 150)) return;
    float best = 1000;
    for (int r = 1; r <= o.size(); ++r)
        if (const MapItem* it = o.item(r); it && it->cls == 1 && it->state == 0 && it->interact == 0x200 && d2(it->pos, path_.back()) < best)
            best = d2(it->pos, path_.back()), open_gate_ = r, open_goal_ = g;
}

// ms until the hit of u's current melee swing lands (animation start + the move script's StartTime,
// when the attack trigger fires); -1 when u is not swinging or that hit is past.
int Autopilot::swing_eta(const Unit& u) const {
    const World& w = m_.world();
    if (u.action != ATTACK) return -1;
    const AnimScript* s = w.script_of(w.move(u));
    if (!s) return -1;
    const int t = u.anim_start + s->start_time - w.now();
    return t >= 0 ? t : -1;
}

// Ctrl_energy_attack's conditions (PlayerController::energy_attack): learned (ENGATK skill 0xd, Joan's ENGATK record is
// not combo-available) and its price, EP_CONSUME % of EGATK_MP % of max EP and EGATK_HP % of max HP, kept 10 HP clear.
bool Autopilot::energy_ok(const Unit& p) const {
    const World& w = m_.world();
    if (!w.can_use_special(p, ENGATK) && w.skill_level(p, 0xd, {}) == 0) return false;
    const Consts& c = w.data().c;
    const int cost_ep = int(float(w.bonus(p, EP_CONSUME)) * (float(w.max_ep(p) * c.egatk_mp) / 100.0f) / 100.0f);
    const int cost_hp = int(float(w.max_hp(p) * c.egatk_hp) / 100.0f);
    return p.ep >= cost_ep && p.hp > cost_hp + 10;
}

// A shot at t lands: t is on its feet (an arrow aimed at a standing body flies over one knocked down and explodes
// far behind), and the release lock (PlayerController::range_fire: the nearest hostile within d/100 + AIM_R of the
// aim ray from the eye) picks t itself, not another body on the way.
bool Autopilot::clear_shot(const Unit& p, const Unit& t) const {
    const World& w = m_.world();
    if (is_knocked_ext(t.action) || t.action == STANDUP || t.action == DIE) return false;
    const GameData& gd = w.data();
    const Vec3 cam{p.pos.x, p.pos.y, p.pos.z + float(gd.units[std::size_t(p.cls)].height)};
    const auto mid = [&](const Unit& e) {
        return Vec3{e.pos.x - cam.x, e.pos.y - cam.y, e.pos.z + float(gd.units[std::size_t(e.cls)].height) * 0.5f - cam.z};
    };
    const Vec3 a = mid(t);
    const float la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    for (const Unit& e : w.units()) {
        if (e.id == t.id || e.deleted || w.dead(e) || !w.hostile(e.nation, p.nation)) continue;
        const Vec3 v = mid(e);
        const float vv = v.x * v.x + v.y * v.y + v.z * v.z, vf = (v.x * a.x + v.y * a.y + v.z * a.z) / la;
        const float d = std::sqrt(vv);
        if (d < la && std::sqrt(std::max(0.0f, vv - vf * vf)) <= d / 100.0f + float(gd.c.aim_r)) return false;
    }
    return true;
}

// The enemy at the centre of the densest group (enemies within the explosive blast's 150 of it) between 60 and
// `range` away; with `steady`, only one an arrow can be put on now (clear_shot). n = the group's size.
const Unit* Autopilot::densest(const Unit& p, float range, bool steady, int& n) const {
    const World& w = m_.world();
    const Unit* best = nullptr;
    n = 0;
    for (const Unit& u : w.units()) {
        if (!alive(w, u) || !w.hostile_to_player(u) || u.no_react) continue;
        const float d = d2(p.pos, u.pos);
        if (d < 60 || d > range || std::fabs(u.pos.z - p.pos.z) > 700) continue;
        int k = 0;
        for (const Unit& v : w.units()) k += alive(w, v) && w.hostile_to_player(v) && d2(u.pos, v.pos) < 150;
        if (k > n && (!steady || clear_shot(p, u))) n = k, best = &u;
    }
    return best;
}

void Autopilot::fight(InputFrame& f, Unit& p, const Unit& foe) {
    World& w = m_.world();
    // Explosive volley: the densest group of 2+ within 800, also the crowd around the player: the blast hits every
    // enemy within 150 of the impact for ~3x a sword hit, knocks them all down and spares friends and the shooter.
    {
        int n = 0;
        if (const Unit* best = densest(p, 800, true, n); best && n >= 2 && load_ammo(p, true)) return shoot(f, p, *best);
    }
    // Bow at range (arrow damage is full up to FALL_S 800): the foe 150..800 away (also down from a wall) and nobody
    // about to strike the player (within 100, or within 200 and after her).
    const bool other_level = std::fabs(foe.pos.z - p.pos.z) > 150;
    if (const float d = d2(p.pos, foe.pos); d >= 150 && d < 800 &&
                                           std::fabs(foe.pos.z - p.pos.z) < 700 && clear_shot(p, foe) && load_ammo(p, false)) {
        bool threat = false;
        for (const Unit& u : w.units())
            if (alive(w, u) && w.hostile_to_player(u) && !u.no_react)
                threat |= d2(p.pos, u.pos) < (u.target == p.id ? 200.0f : 100.0f) && std::fabs(u.pos.z - p.pos.z) < 150;
        if (!threat || other_level) return shoot(f, p, foe);
    }
    // A foe on another level (a wall top) out of reach of the sword: back off until the bow has an angle on it.
    if (other_level) {
        steer(f, p, {2 * p.pos.x - foe.pos.x, 2 * p.pos.y - foe.pos.y, p.pos.z}, false);
        return;
    }
    f.camera_yaw_deg = std::atan2(foe.pos.y - p.pos.y, foe.pos.x - p.pos.x) / kDeg;
    // Guard (Q) against swings about to land on the player (an English swing lands ~350 ms after it
    // starts). Guarding blocks only +-BLOCKANG around the facing and the controller does not turn while
    // guarding, so the guard is aimed at the middle of the incoming swings first. Our own swing cannot
    // be cancelled: a combo is only continued while no second enemy is ready to strike.
    float lo = 1e9f, hi = -1e9f;
    int eta = -1;
    bool others = false;
    for (const Unit& u : w.units()) {
        if (!alive(w, u) || !w.hostile_to_player(u) || u.no_react) continue;
        const float d = d2(p.pos, u.pos);
        if (d > 140 || std::fabs(w.rel_angle(u, p.pos)) > 70) continue;
        if (u.id != foe.id && !is_flinch(u.action) && !is_knocked_ext(u.action) && u.action != STANDUP) others = true;
        const int e = swing_eta(u);
        if (e < 0 || e > 400) continue;
        const float a = w.rel_angle(p, u.pos);
        lo = std::min(lo, a), hi = std::max(hi, a);
        if (eta < 0 || e < eta) eta = e;
    }
    const bool swinging = is_attacking(p.action);
    // Surrounded (3+ within 110): the energy attack (C) instead of guarding in a ring where swings come from
    // every side; it hits everyone around for ~7 A hits (Joan level 4: 29 vs 4) and knocks them down.
    {
        int ring = 0;
        for (const Unit& u : w.units())
            ring += alive(w, u) && w.hostile_to_player(u) && !u.no_react && d2(p.pos, u.pos) < 110 && !is_knocked_ext(u.action);
        if (ring >= 3 && !swinging && w.can_act(p) && energy_ok(p)) {
            f.press(IN_ENERGY);
            return;
        }
    }
    // Below 45 % HP the guard comes back; above it, attacking beats guarding (every hit flinches its targets and
    // the combo cleaves; a guard in a ring of attackers only blocks the front: mission 2 kills 3 -> 18 per run).
    const bool aggro = p.hp * 100 >= 45 * w.max_hp(p);
    if (eta >= 0 && !swinging && !aggro) {
        const float a = (lo + hi) * 0.5f;
        if (std::fabs(a) > 40 && eta > 120) f.mouse_dx = a / 0.18f;
        else f.held.set(IN_DEFEND);
        return;
    }
    const float d = d2(p.pos, foe.pos);
    // A knocked-down foe in reach: Q is the down attack (0x4363a0 runs before the guard).
    if ((is_knocked(foe.action) || foe.action == DNATKED) && !foe.down_attacked && d < 58 && !swinging) {
        const float a = w.rel_angle(p, foe.pos);
        if (std::fabs(a) > 35) f.mouse_dx = a / 0.18f;
        else f.held.set(IN_DEFEND);
        return;
    }
    if (d > 85) {  // close in
        steer(f, p, foe.pos, false);
        return;
    }
    f.mouse_dx = w.rel_angle(p, foe.pos) / 0.18f;
    if (!swinging) {
        if (others && !w.can_act(p)) return;
        if (others && !aggro) {  // two or more in reach: step back so they bunch up in front
            f.held.set(IN_BACKWARD);
            if (tick_ % 2 == 0) f.press(IN_ATTACK_A);
            return;
        }
        if (tick_ % 2 == 0) f.press(IN_ATTACK_A);
        return;
    }
    // A A A B: queue the next step late in the current step's follow-up window [START, END_TIME),
    // pressed on even ticks so the press follows a release (Player_attackInput phase 2 -> 3).
    if (p.action == ATTACK && p.combo_idx > 0 && !p.follow_queued && (!others || aggro) && p.combo.size() < 4 && tick_ % 2 == 0) {
        const int since = w.now() - p.anim_start;
        if (since >= p.win_close - 140 && since < p.win_close) f.press(p.combo == "AAA" ? IN_ATTACK_B : IN_ATTACK_A);
    }
}

// Ammo kind of an item: 1 explosive (projectile kind 2, item types 0xf / 0x12), 0 other arrows/bolts, -1 not ammo.
static int ammo_kind(const World& w, int item) {
    const int t = w.data().items[std::size_t(item)].type;
    return t < 0xd || t > 0x14 ? -1 : (t == 0xf || t == 0x12) ? 1 : 0;
}

// Arrows of a kind the player carries (ammo slot + bag); 0 without a bow.
int Autopilot::arrows(const Unit& p, bool explosive) const {
    const World& w = m_.world();
    if (!p.inv.ranged.item) return 0;
    int n = p.inv.ammo.item && ammo_kind(w, p.inv.ammo.item) == int(explosive) ? p.inv.ammo.quantity : 0;
    for (const ItemInstance& it : p.inv.bag) n += ammo_kind(w, it.item) == int(explosive) ? it.quantity : 0;
    return n;
}

// Ammo slot: the arrows the bow fires. Swapping them is the character screen's drag into the ammo slot
// (World::equip). Returns false without such arrows.
bool Autopilot::load_ammo(Unit& p, bool explosive) {
    World& w = m_.world();
    if (!p.inv.ranged.item) return false;
    if (p.inv.ammo.item && p.inv.ammo.quantity > 0 && ammo_kind(w, p.inv.ammo.item) == int(explosive)) return true;
    for (std::size_t i = 0; i < p.inv.bag.size(); ++i)
        if (ammo_kind(w, p.inv.bag[i].item) == int(explosive) && p.inv.bag[i].quantity > 0) return w.equip(p, int(i));
    return p.inv.ammo.item && p.inv.ammo.quantity > 0 && !explosive;  // plain wanted, only explosive left: keep shooting
}

// Bow (range mode, Z): the camera aims at the target's middle (the release locks the nearest enemy within
// |v|/100 + AIM_R of the aim ray, 0x42c260), A starts drawing (SHTLOAD), A in SHTIDLE releases. Explosive arrows
// go at groups: their ground blast hits every enemy within 150 (no friendly fire).
void Autopilot::shoot(InputFrame& f, Unit& p, const Unit& foe) {
    World& w = m_.world();
    int group = 0;
    for (const Unit& u : w.units()) group += alive(w, u) && w.hostile_to_player(u) && d2(u.pos, foe.pos) < 150;
    if (!load_ammo(p, group >= 2)) load_ammo(p, false);
    shot_ = true;
    const float dx = foe.pos.x - p.pos.x, dy = foe.pos.y - p.pos.y;
    const float dz = foe.pos.z + float(w.data().units[std::size_t(foe.cls)].height) * 0.5f -
                     (p.pos.z + float(w.data().units[std::size_t(p.cls)].height));
    f.camera_yaw_deg = std::atan2(dy, dx) / kDeg;
    f.camera_pitch_deg = std::atan2(dz, std::hypot(dx, dy)) / kDeg;
    if (!w.controller().range_mode) {
        if (w.can_act(p) && tick_ % 2 == 0) f.press(IN_RANGE_MODE);
        return;
    }
    // Draw whenever possible; release (A in SHTIDLE) only while the shot lands.
    if (tick_ % 2 == 0 && ((p.action == SHTIDLE && clear_shot(p, foe)) || (p.action != SHTIDLE && p.action != SHTLOAD && p.action != SHTOUT && w.can_act(p))))
        f.press(IN_ATTACK_A);
}

// Buy screen open (start_buy_interface): the click handler's purchase (FUN_006c5a00 -> World::give_item of the item's
// Quantity at the stock level, gold -= Price), once per opening. A heavier armour than the one worn is bought and
// worn first (inventory right-click, World::use_item); with a bow, explosive arrows up to 20, then plain arrows.
void Autopilot::shop(Unit& p) {
    if (shop_ms_ == m_.ui().interface_ms) return;
    shop_ms_ = m_.ui().interface_ms;
    World& w = m_.world();
    const GameData& gd = w.data();
    int& gold = m_.state().gold;
    const std::vector<ShopStock> stock = shop_stock(m_, m_.ui().buy_shop);
    auto buy = [&](const ShopStock& s) {
        const ItemDef& d = gd.items[std::size_t(s.item)];
        if (s.quantity == 0 || gold < d.price || !w.give_item(p, d.code, s.level, std::max(1, d.quantity))) return false;
        gold -= d.price;
        log.push_back(std::to_string(m_.ticks()) + " buy " + d.code + " gold " + std::to_string(gold));
        return true;
    };
    for (const ShopStock& s : stock) {
        const ItemDef& d = gd.items[std::size_t(s.item)];
        if (d.item_class == "ARMOR" && (d.usable_unit < 0 || d.usable_unit == p.cls) && d.defense > p.armor_level && buy(s))
            for (std::size_t i = p.inv.bag.size(); i-- > 0;)
                if (p.inv.bag[i].item == s.item) {
                    w.use_item(p, int(i));
                    break;
                }
    }
    if (!p.inv.ranged.item) return;
    auto count = [&](int item) {
        int n = p.inv.ammo.item == item ? p.inv.ammo.quantity : 0;
        for (const ItemInstance& it : p.inv.bag) n += it.item == item ? it.quantity : 0;
        return n;
    };
    const int bow = gd.items[std::size_t(p.inv.ranged.item)].type;
    for (const bool explosive : {true, false})
        for (const ShopStock& s : stock) {
            const int t = gd.items[std::size_t(s.item)].type;
            if (t != (explosive ? 0xf : bow == 0xb ? 0xe : 0xd)) continue;
            while ((!explosive || count(s.item) < 20) && buy(s)) {}
        }
}

InputFrame Autopilot::decide() {
    InputFrame f;
    World& w = m_.world();
    Unit* pp = w.unit(w.player_id());
    goal.clear();
    if (pp && alive(w, *pp) && m_.ui().buy_open) shop(*pp);
    if (!pp || !alive(w, *pp) || m_.ui().game_cut_scene || w.cut_scene) return f;
    Unit& p = *pp;
    // Character screen: every attribute point into HIT_POINT (the walkthroughs' rule; STR/DEF buy little).
    while (p.attr_points > 0 && w.allocate_point(p, 6, false)) {}
    // F3 below half HP while food lasts; below 30 % with no food left, fall back to the nearest friendly
    // hero (Jean...) and only guard until regen / a level-up brings HP back to 70 %.
    const int hp_pct = p.hp * 100 / std::max(1, w.max_hp(p));
    int food = 0, ep_food = 0;
    for (const ItemInstance& it : p.inv.bag) {
        const int t = w.data().items[std::size_t(it.item)].type;
        food += (t == 0x15 || t == 0x19) ? it.quantity : 0;
        ep_food += t == 0x17 ? it.quantity : 0;
    }
    if (hp_pct < 50 && food && tick_ - last_heal_ > 30) f.press(IN_HEAL_ITEM), last_heal_ = tick_;
    // F4 (apples, EP) when the energy attack is short of EP and an enemy is near.
    else if (ep_food && (w.can_use_special(p, ENGATK) || w.skill_level(p, 0xd, {})) && !energy_ok(p) && tick_ - last_heal_ > 30)
        for (const Unit& u : w.units())
            if (alive(w, u) && w.hostile_to_player(u) && d2(p.pos, u.pos) < 300) {
                f.press(IN_EP_ITEM), last_heal_ = tick_;
                break;
            }
    if (hp_pct < 30 && !food) retreat_ = true;
    if (hp_pct >= 70 || food) retreat_ = false;
    const Unit* ally = nullptr;
    for (const Unit& u : w.units())
        if (u.id != p.id && alive(w, u) && u.nation == p.nation && u.rank >= 4 && u.retreat != -1 &&
            (!ally || d2(p.pos, u.pos) < d2(p.pos, ally->pos)))
            ally = &u;
    // Interact indicator: talk, free prisoners, pick up items, recruit. A unit already talked to is not
    // pestered again for 100 s (scripts replay their dialog on every R). The indicator is computed before
    // the physics step moves the player on, so she stops first and presses R once it stayed lit (a player
    // stops at the icon too); R pressed while still walking lands out of reach.
    const bool fresh_talk = prompt_kind_ != "talk" || !talked_.count(prompt_id_) || tick_ - talked_[prompt_id_] > 3000;
    // A gate is toggled only on purpose, never in passing: closed (gate_ below) or opened to get through (travel).
    const int prompt_item = prompt_kind_ == "item" ? m_.objects().modify_item(prompt_id_) : 0;
    const MapItem* pit = m_.objects().item(prompt_item);
    const bool gate_prompt = pit && pit->cls == 1;
    const bool open_it = gate_prompt && prompt_item == open_gate_ && pit->state == 0 && tick_ - travel_tick_ <= 1;
    if (prompt_ && fresh_talk && tick_ - last_interact_ > 20 && prompt_kind_ != "mount" && prompt_kind_ != "bribe" &&
        (!gate_prompt || prompt_item == gate_ || open_it) && prompt_kind_ != "recruit_screen" && prompt_kind_ != "workshop") {
        goal = "interact " + prompt_kind_;
        if (tick_ - prompt_tick_ < 3) return f;
        last_interact_ = tick_;
        if (prompt_kind_ == "talk") talked_[prompt_id_] = tick_;
        if (open_it) opened_gate_ = prompt_item;
        f.press(IN_INTERACT);
        return f;
    }
    // Objective marker: the nearest active minimap way point (scripts point the player with them) that was not
    // stood at for 10 s already without the script moving it (mission 2 shows the shop and the blacksmith at once).
    const Vec3* wp = nullptr;
    Vec3 wp_pos;
    int wp_slot = 0, slot = 0;
    for (const auto& x : m_.ui().waypoints) {
        ++slot;
        if (x.type == 0) continue;
        const Unit* fu = x.follow ? w.unit(x.follow) : nullptr;
        const Vec3 at_pos = fu ? fu->pos : Vec3{x.pos[0], x.pos[1], x.pos[2]};
        if (std::any_of(done_wps_.begin(), done_wps_.end(), [&](Vec3 d) { return d2(d, at_pos) < 1; })) continue;
        if (wp && d2(p.pos, at_pos) >= d2(p.pos, wp_pos)) continue;
        wp_pos = at_pos, wp = &wp_pos, wp_slot = slot;
    }
    // A way point on an open gate the player may close: the script asks for it (mission 2: "Someone must
    // approach and close the gate"). Walk up to it from the friendly side and press R at its indicator (the
    // passage closes unless a rider / siege weapon stands within 300). A gate the bot opened itself to get at
    // enemies outside stays open while any of them lives within 4000 outside.
    int gate = 0;
    if (wp)
        for (int r = 1; r <= m_.objects().size(); ++r)
            if (const MapItem* it = m_.objects().item(r); it && it->cls == 1 && it->state == 1 && it->interact == 0x200 && d2(it->pos, *wp) < 150)
                gate = r;
    if (const MapItem* g = m_.objects().item(gate); g && gate == opened_gate_) {
        const Vec3 in = gate_inside(*g);
        for (const Unit& u : w.units())
            if (alive(w, u) && u.m.active && w.hostile_to_player(u) && !u.no_react && std::fabs(u.pos.z - g->pos.z) < 150 &&
                d2(u.pos, g->pos) < 4000 && (u.pos.x - g->pos.x) * (in.x - g->pos.x) + (u.pos.y - g->pos.y) * (in.y - g->pos.y) < 0)
                gate = 0;
    }
    if (!gate) gate_seen_ = 0;
    else if (!gate_seen_) gate_seen_ = tick_;
    constexpr int gate_at = 30 * 30;  // 30 s: mission 2, after John's 10 s mark (closing sends him off), well before the 180 s reinforcements
    gate_ = gate && tick_ - gate_seen_ >= gate_at ? gate : 0;
    if (const MapItem* g = m_.objects().item(gate_)) {
        goal = "close gate " + at(g->pos);
        const Vec3 in = gate_inside(*g);
        if (d2(p.pos, in) > 60) steer(f, p, in, false);
        else f.mouse_dx = w.rel_angle(p, g->pos) / 0.18f;
        return f;
    }
    if (gate) wp = nullptr;  // not yet: the marker does not pull the player
    // Enemies: keep the current target while it lives and stays near; else the nearest reachable one
    // close by, or one that is attacking the player. While hurt (< 70 %) an enemy that has not noticed
    // the player yet is left alone: rest first. (Mission 2: leashing Joan to the Bastard lost on citizens in
    // 6/6 seeds; roaming with the citizen-killer priority below kept 4-13 of 18 alive.)
    const bool fresh = hp_pct >= 70;
    const Unit* foe = nullptr;
    const Unit* lurker = nullptr;  // nearest unaware enemy within 600
    if (const Unit* t = w.unit(fight_target_);
        t && alive(w, *t) && t->m.active && w.hostile_to_player(*t) && d2(p.pos, t->pos) < 700)
        foe = t;
    if (!foe || foe->target != p.id) {  // archers close by first, then whoever targets the player, then the nearest
        float best = 1e30f;
        const float reach = wp ? 450.0f : 3000.0f;  // beyond: "hunt" walks there
        const bool bow = p.inv.ranged.item && p.inv.ammo.item && p.inv.ammo.quantity > 0;
        for (const Unit& u : w.units()) {
            if (!alive(w, u) || !u.m.active || !w.hostile_to_player(u) || u.no_react) continue;
            float d = d2(p.pos, u.pos);
            // Same level, or another (a wall top, the ground below one) within bow range.
            if (const float dz = std::fabs(u.pos.z - p.pos.z); dz > 150 && !(bow && dz < 700 && d < 800)) continue;
            const auto& c = w.data().units[std::size_t(u.cls)];
            if (c.unit_class == 8 || c.special == 2) continue;  // siege weapons, horses
            const bool aware = u.target == p.id || d < 250;
            if (!aware && !fresh) {
                if (d < 600 && (!lurker || d < d2(p.pos, lurker->pos))) lurker = &u;
                continue;
            }
            if (u.target == p.id) d *= 0.5f;
            // An enemy after a citizen first (mission 2 is lost at 18 dead citizens).
            if (const Unit* t = w.unit(u.target); t && alive(w, *t) && w.data().units[std::size_t(t->cls)].unit_class == 0 && !w.hostile_to_player(*t))
                d *= 0.4f;
            if ((c.unit_class == 5 || c.unit_class == 6) && d < 350) d *= 0.3f;
            if (d < reach && d < best) best = d, foe = &u;
        }
    }
    if (retreat_) {
        fight_target_ = 0;
        const Unit* near = foe && d2(p.pos, foe->pos) < 170 ? foe : nullptr;
        if (near && is_attacking(near->action)) {
            f.mouse_dx = w.rel_angle(p, near->pos) / 0.18f;
            f.held.set(IN_DEFEND);
        } else if (ally && d2(p.pos, ally->pos) > 120) {
            steer(f, p, ally->pos, false);
        } else if (foe && d2(p.pos, foe->pos) < 400) {  // away from the enemy
            steer(f, p, {2 * p.pos.x - foe->pos.x, 2 * p.pos.y - foe->pos.y, p.pos.z}, false);
        }
        goal = "retreat hp " + std::to_string(hp_pct) + "%";
        return f;
    }
    // Explosive arrows are worth most on a big group (a squad gathered at its rally point): with some left and
    // nobody on the player, walk up to the densest group of 5+ within 3000 (the volley starts at 800).
    if (int n = 0; arrows(p, true) && !(foe && (d2(p.pos, foe->pos) < 300 || foe->target == p.id)))
        if (const Unit* g = densest(p, 3000, false, n); g && n >= 5 && d2(p.pos, g->pos) > 700) {
            goal = "volley at #" + std::to_string(g->id) + " group " + std::to_string(n) + " " + at(g->pos);
            travel(f, p, g->pos);
            return f;
        }
    fight_target_ = foe ? foe->id : 0;
    if (foe) {
        goal = "fight #" + std::to_string(foe->id) + " " + w.data().units[std::size_t(foe->cls)].code + " " + at(foe->pos);
        fight(f, p, *foe);
        return f;
    }
    if (lurker) {  // hurt: back away from an enemy that has not noticed the player
        goal = "avoid #" + std::to_string(lurker->id);
        steer(f, p, {2 * p.pos.x - lurker->pos.x, 2 * p.pos.y - lurker->pos.y, p.pos.z}, false);
        return f;
    }
    // Loot: crates and barrels hold food and arrows (the mission-1 tutorial says so), dropped items lie
    // where units died. Break / walk over the nearest within reach, skipping ones that took too long.
    if (loot(f, p)) return f;
    // Rest: Joan regenerates (1 HP / 10 s early on); nothing is gained by walking into the next fight hurt.
    // An ally hero told to follow the player (FOLLOW_UNIT, Jean in mission 1) travels with her; heroes
    // that guard a place are not waited for.
    const bool follows = ally && m_.ai().curr_task(*ally) == task::FOLLOW_UNIT && m_.ai().curr_task_para(*ally, 1) == p.id;
    if (!fresh && p.retreat != -1) {
        goal = "rest hp " + std::to_string(hp_pct) + "%";
        if (follows && d2(p.pos, ally->pos) > 250) steer(f, p, ally->pos, false);
        return f;
    }
    // Travel together: a following ally left more than 700 behind is waited for (walked back to).
    if (follows && d2(p.pos, ally->pos) > 700 && d2(p.pos, ally->pos) < 3000) {
        goal = "regroup with #" + std::to_string(ally->id);
        steer(f, p, ally->pos, false);
        return f;
    }
    // A marker often stands at a door with the person to see inside (the shops in mission 2): near the
    // marker, walk up to an NPC showing the talk icon (can_interact) not talked to yet. A reached marker
    // the script does not move on within 10 s means "continue from here": the next marker, the open WIN
    // objective's territory (the minimap shows towns), else the nearest enemy.
    if (wp) {
        const Unit* npc = nullptr;
        if (d2(p.pos, *wp) < 900)
            for (const Unit& u : w.units())
                if (u.id != p.id && alive(w, u) && u.m.can_interact && !w.hostile_to_player(u) && !talked_.count(u.id) &&
                    d2(u.pos, *wp) < 600 && (!npc || d2(p.pos, u.pos) < d2(p.pos, npc->pos)))
                    npc = &u;
        if (npc) {
            goal = "talk to #" + std::to_string(npc->id) + " " + npc->name;
            if (npc->id != talk_key_) talk_key_ = npc->id, talk_tick_ = tick_;
            if (tick_ - talk_tick_ > 900) talked_[npc->id] = tick_;  // no talk icon in 30 s: give up on it
            if (d2(p.pos, npc->pos) > 60) steer(f, p, npc->pos, false);
            else f.mouse_dx = w.rel_angle(p, npc->pos) / 0.18f;
            return f;
        }
        if (d2(p.pos, *wp) > 150) seen_tick_ = tick_;
        else if (tick_ - seen_tick_ > 300) done_wps_.push_back(*wp), seen_tick_ = tick_;  // stood there 10 s
        goal = "waypoint " + std::to_string(wp_slot) + " " + at(*wp);
        if (d2(p.pos, *wp) > 80) travel(f, p, *wp);
        return f;
    }
    for (const EndCondition& c : m_.state().end) {
        if (!c.has_des || !c.enabled || c.type != EndCondition::Win || c.finish == 1 || c.territory_link.empty()) continue;
        const Territory* t = m_.places().territory(m_.places().territory_by_name(c.territory_link));
        if (!t) continue;
        const Vec3 g{float(t->center_x), float(t->center_y), p.pos.z};
        if (d2(p.pos, g) < 300) break;
        goal = "territory " + c.territory_link + " " + at(g);
        travel(f, p, g);
        return f;
    }
    // Hunt the enemy left nearest to the player and to her people (home(): the town under attack before the
    // garrisons of the next ones), those on the player's level (not a wall top above / the ground below) first.
    const Unit* far = nullptr;
    float far_d = 0;
    const Vec3 h = home();
    for (const Unit& u : w.units())
        if (alive(w, u) && u.m.active && w.hostile_to_player(u) && !u.no_react) {
            const float d = d2(p.pos, u.pos) + d2(h, u.pos) + (std::fabs(u.pos.z - p.pos.z) > 150 ? 5000.0f : 0.0f);
            if (!far || d < far_d) far = &u, far_d = d;
        }
    if (far) {
        goal = "hunt #" + std::to_string(far->id) + " " + at(far->pos);
        travel(f, p, far->pos);
        return f;
    }
    // Nothing left: everything lying around, else stand.
    if (loot(f, p, 4000)) return f;
    goal = "idle";
    return f;
}

bool Autopilot::loot(InputFrame& f, const Unit& p, float radius) {
    World& w = m_.world();
    MapObjects& o = m_.objects();
    const GameData& gd = w.data();
    Vec3 best_pos{};
    int best_key = 0;
    float best_d = radius;
    bool box = false;
    auto consider = [&](int key, Vec3 pos, bool is_box) {
        if (std::fabs(pos.z - p.pos.z) > (is_box ? 60.0f : 150.0f) || skip_.count(key)) return;
        const float d = d2(p.pos, pos);
        if (d >= best_d) return;
        for (const Unit& u : w.units())  // left alone while an enemy stands next to it
            if (alive(w, u) && u.m.active && w.hostile_to_player(u) && !u.no_react && d2(u.pos, pos) < 500) return;
        best_d = d, best_pos = pos, best_key = key, box = is_box;
    };
    for (int r = 1; r <= o.size(); ++r) {
        const MapItem* it = o.item(r);
        if (!it) continue;
        if (it->cls == 10 && it->stage > 0 && it->content) consider(-r, it->pos, true);  // BOX with something inside
        else if (it->on_floor && it->res > 0 && it->res < int(gd.items.size()) && gd.items[std::size_t(it->res)].pickable)
            consider(-r, it->pos, false);
    }
    for (const WorldItem& it : w.items())
        if (!it.deleted && it.item > 0 && it.item < int(gd.items.size()) && gd.items[std::size_t(it.item)].pickable)
            consider(it.id, it.pos, false);
    if (!best_key) return false;
    if (best_key != loot_key_) loot_key_ = best_key, loot_tick_ = tick_;
    if (tick_ - loot_tick_ > 900) return skip_.insert(best_key), false;  // not reached / not taken in 30 s
    goal = std::string(box ? "break box " : "pick up ") + at(best_pos);
    if (box && d2(p.pos, best_pos) < 75) {
        const float a = w.rel_angle(p, best_pos);
        f.mouse_dx = a / 0.18f;
        if (std::fabs(a) < 20 && tick_ % 20 == 0) f.press(IN_ATTACK_A);
        return true;
    }
    steer(f, p, best_pos, false);
    return true;
}

void Autopilot::step() {
    ++tick_;
    shot_ = false;
    InputFrame in = decide();
    if (m_.world().controller().range_mode && !shot_) in = InputFrame{}, in.press(IN_RANGE_MODE);  // Z again: leave range mode
    const int pid = m_.world().player_id();
    static const int dbg_from = std::getenv("JOA_BOT_DBG") ? std::atoi(std::getenv("JOA_BOT_DBG")) : -1;
    if (dbg_from >= 0 && tick_ >= dbg_from && tick_ < dbg_from + 200) {
        World& w = m_.world();
        if (const Unit* p = w.unit(pid)) {
            std::printf("t%d me %s act %d yaw %.0f hp %d in:%s%s%s%s mdx %.0f |", tick_, at(p->pos).c_str(), p->action, p->yaw_deg, p->hp,
                        in.held[IN_DEFEND] ? " Q" : "", in.pressed[IN_ATTACK_A] ? " A" : "", in.pressed[IN_ATTACK_B] ? " B" : "",
                        in.held[IN_FORWARD] ? " W" : "", in.mouse_dx);
            for (const Unit& u : w.units())
                if (alive(w, u) && w.hostile_to_player(u) && d2(p->pos, u.pos) < 250)
                    std::printf(" #%d d%.0f act %d face %.0f rel %.0f;", u.id, d2(p->pos, u.pos), u.action, w.rel_angle(u, p->pos),
                                w.rel_angle(*p, u.pos));
            std::printf(" %s\n", goal.c_str());
        }
    }
    m_.tick(in);
    World& tw = m_.world();
    for (const GameEvent& e : tw.take_events()) {
        kills += e.type == Ev::Death && e.other == pid && pid;
        hits += e.type == Ev::Hit && e.other == pid && pid;
        hurt += e.type == Ev::Hit && e.unit == pid && pid;
        if (e.type == Ev::Hit && pid) dealt += e.other == pid ? e.value : 0, taken += e.unit == pid ? e.value : 0;
        player_deaths += e.type == Ev::Death && e.unit == pid && pid;
        if (trace && e.type == Ev::ProjectileSpawn && e.unit == pid && pid)
            std::printf("[bot %d] shot #%d %s at #%d %s\n", tick_, e.other, e.name.c_str(), fight_target_, goal.c_str());
        if (trace && e.type == Ev::ProjectileHit && e.value >= 2)
            std::printf("[bot %d] projectile #%d type %d hit %s #%d\n", tick_, e.other, e.value, e.name.empty() ? "unit" : e.name.c_str(), e.unit);
        if (trace && e.type == Ev::Hit && e.unit != pid)  // hits taken by the other heroes / the Bastard
            if (const Unit* v = tw.unit(e.unit); v && v->rank >= 4 && !tw.hostile_to_player(*v))
                if (const Unit* a = tw.unit(e.other))
                    std::printf("[bot %d] %s hit -%d (hp %d) by #%d %s team %d %s\n", tick_, v->name.c_str(), e.value, v->hp, a->id,
                                tw.data().units[std::size_t(a->cls)].code.c_str(), a->team, e.name.c_str());
        if (trace && e.type == Ev::Action && e.value == ATTACK) atk_start_[e.unit] = tick_;
        if (trace && e.type == Ev::Death)
            if (const Unit* v = tw.unit(e.unit)) {
                const Unit* k = tw.unit(e.other);
                std::printf("[bot %d] death #%d %s at %s by #%d %s team %d\n", tick_, v->id, tw.data().units[std::size_t(v->cls)].code.c_str(),
                            at(v->pos).c_str(), e.other, k ? tw.data().units[std::size_t(k->cls)].code.c_str() : "-", k ? k->team : 0);
            }
        if (trace && e.type == Ev::Hit && e.unit == pid && pid)
            if (const Unit* a = tw.unit(e.other))
                std::printf("[bot %d] hit -%d by #%d %s action %d (my action %d) windup %d dist %.0f\n", tick_, e.value, a->id,
                            tw.data().units[std::size_t(a->cls)].code.c_str(), a->action, tw.unit(pid)->action,
                            tick_ - atk_start_[a->id], d2(a->pos, tw.unit(pid)->pos));
        if (trace && e.type == Ev::Hit && e.other == pid && pid)
            if (const Unit* d = tw.unit(e.unit); d && tw.unit(pid))
                std::printf("[bot %d] dealt -%d to #%d %s hp %d (my action %d combo %s morale %d; its armor %d morale %d)\n", tick_, e.value, d->id,
                            tw.data().units[std::size_t(d->cls)].code.c_str(), d->hp, tw.unit(pid)->action, tw.unit(pid)->combo.c_str(),
                            tw.unit(pid)->morale, d->armor_level, d->morale);
    }
    for (const MissionEvent& e : m_.take_events()) {
        using K = MissionEvent::Kind;
        if (e.kind == K::InteractPrompt) {
            prompt_ = e.a != 0, prompt_id_ = e.b, prompt_kind_ = e.text, prompt_tick_ = tick_;
            if (trace) std::printf("[bot %d] prompt %d #%d %s\n", tick_, e.a, e.b, e.text.c_str());
        }
        if (e.kind == K::ObjectiveFinish || e.kind == K::ObjectiveFail || e.kind == K::Minimap || e.kind == K::ObjectiveAdd)
            last_progress_tick = tick_;
        if (e.kind == K::ObjectiveFinish || e.kind == K::ObjectiveFail || e.kind == K::Victory || e.kind == K::Defeat)
            log.push_back(std::to_string(e.tick) + " " + event_kind_name(e.kind) + " " + std::to_string(e.a) + " " + e.text);
    }
    if (trace)
        for (std::size_t i = seen_units_; i < tw.units().size(); ++i, ++seen_units_)
            if (const Unit& u = tw.units()[i]; !u.deleted)
                std::printf("[bot %d] spawn #%d %s at %s\n", tick_, u.id, tw.data().units[std::size_t(u.cls)].code.c_str(), at(u.pos).c_str());
    if (trace && tick_ % trace == 0) {
        World& w = m_.world();
        if (const Unit* p = w.unit(pid)) {
            int food = 0;
            for (const ItemInstance& it : p->inv.bag) {
                const int t = w.data().items[std::size_t(it.item)].type;
                food += (t == 0x15 || t == 0x19) ? it.quantity : 0;
            }
            const Unit* j = nullptr;
            for (const Unit& u : w.units())
                if (!u.deleted && u.id != p->id && u.nation == p->nation && u.rank >= 4) j = &u;
            std::printf("[bot %d] %s hp %d/%d ep %d/%d lv %d xp %d food %d stage %d kills %d ally %s hp %d | %s\n", tick_, at(p->pos).c_str(),
                        p->hp, w.max_hp(*p), p->ep, w.max_ep(*p), p->level, p->xp, food, m_.scenario().stage, kills,
                        j ? at(j->pos).c_str() : "-", j ? j->hp : 0, goal.c_str());
        }
    }
}

// --- campaign driver ----------------------------------------------------------------------------

MissionReport play_mission(CampaignRunner& run, int max_ticks, int retries) {
    MissionReport r;
    r.no = run.mission()->scenario_no();
    r.stem = run.mission()->info().stem();
    for (int attempt = 0; attempt <= retries; ++attempt) {
        // RETRY: the original reseeds from time(NULL); the bot uses seed + attempt (deterministic, but a
        // retry does not replay the identical loss).
        if (attempt) {
            const std::uint32_t base = run.seed;
            run.seed = base + std::uint32_t(attempt);
            run.retry();
            run.seed = base;
        }
        Mission& m = *run.mission();
        m.autosave = false;
        ++r.attempts;
        Autopilot ap(m);
        if (const char* t = std::getenv("JOA_BOT_TRACE")) ap.trace = std::atoi(t);
        int t = 0;
        // The game-over box (call_gameover_msg: a hero that must live died, too many citizens killed...)
        // freezes every unit until the player picks RETRY, also when no LOSE record ends the mission.
        while (t < max_ticks && m.finish() == 0 && !m.state().game_over) ap.step(), ++t;
        r.ticks += t;
        r.kills += ap.kills;
        r.dealt += ap.dealt, r.taken += ap.taken;
        r.result = m.finish() ? m.finish() : m.state().game_over ? 2 : 0;
        r.hash = m.hash();
        r.objectives_done = r.objectives_total = 0;
        r.log = ap.log;
        std::string blocking;
        for (std::size_t i = 0; i < m.state().end.size(); ++i) {
            const EndCondition& c = m.state().end[i];
            if (!c.has_des || !c.enabled || (c.type != EndCondition::Win && c.type != EndCondition::Side)) continue;
            ++r.objectives_total;
            if (c.finish == 1) ++r.objectives_done;
            else if (blocking.empty() && c.type == EndCondition::Win) blocking = "#" + std::to_string(i + 1) + " " + c.des;
        }
        if (r.result == 1) {
            r.stuck.clear();
            break;
        }
        if (r.result == 2) ++r.deaths;
        const Unit* p = m.world().unit(m.world().player_id());
        r.stuck = (r.result == 2 ? "lost: " + m.state().game_over_text : std::string("timeout")) + " | objective " + blocking +
                  " | bot " + ap.goal + " | player " + (p ? at(p->pos) : std::string("none")) + " | last progress tick " +
                  std::to_string(ap.last_progress_tick);
        if (r.result != 2) break;  // timeout: no retry
    }
    return r;
}

}  // namespace joa::game
