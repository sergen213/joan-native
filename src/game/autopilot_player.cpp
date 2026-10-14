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

void Autopilot::fight(InputFrame& f, Unit& p, const Unit& foe) {
    World& w = m_.world();
    // Bow at range (arrow damage is full up to FALL_S 800): the foe 150..800 away and nobody about to strike the player
    // (within 100, or within 200 and after her).
    if (const float d = d2(p.pos, foe.pos); d >= 150 && d < 800 && std::fabs(foe.pos.z - p.pos.z) < 300 && load_ammo(p, false)) {
        bool threat = false;
        for (const Unit& u : w.units())
            if (alive(w, u) && w.hostile_to_player(u) && !u.no_react)
                threat |= d2(p.pos, u.pos) < (u.target == p.id ? 200.0f : 100.0f);
        if (!threat) return shoot(f, p, foe);
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
    if (eta >= 0 && !swinging) {
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
        if (others) {  // two or more in reach: step back so they bunch up in front
            f.held.set(IN_BACKWARD);
            if (tick_ % 2 == 0) f.press(IN_ATTACK_A);
            return;
        }
        if (tick_ % 2 == 0) f.press(IN_ATTACK_A);
        return;
    }
    // A A A B: queue the next step late in the current step's follow-up window [START, END_TIME),
    // pressed on even ticks so the press follows a release (Player_attackInput phase 2 -> 3).
    if (p.action == ATTACK && p.combo_idx > 0 && !p.follow_queued && !others && p.combo.size() < 4 && tick_ % 2 == 0) {
        const int since = w.now() - p.anim_start;
        if (since >= p.win_close - 140 && since < p.win_close) f.press(p.combo == "AAA" ? IN_ATTACK_B : IN_ATTACK_A);
    }
}

// Ammo slot: the arrows the bow fires (explosive ammo = projectile kind 2, item types 0xf / 0x12). Swapping them is
// the character screen's drag into the ammo slot (World::equip). Returns false without such arrows.
bool Autopilot::load_ammo(Unit& p, bool explosive) {
    World& w = m_.world();
    if (!p.inv.ranged.item) return false;
    const auto kind = [&](int item) {
        const int t = w.data().items[std::size_t(item)].type;
        return t < 0xd || t > 0x14 ? -1 : (t == 0xf || t == 0x12) ? 1 : 0;
    };
    if (p.inv.ammo.item && p.inv.ammo.quantity > 0 && kind(p.inv.ammo.item) == int(explosive)) return true;
    for (std::size_t i = 0; i < p.inv.bag.size(); ++i)
        if (kind(p.inv.bag[i].item) == int(explosive) && p.inv.bag[i].quantity > 0) return w.equip(p, int(i));
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
    if (tick_ % 2 == 0 && (p.action == SHTIDLE || (p.action != SHTLOAD && p.action != SHTOUT && w.can_act(p)))) f.press(IN_ATTACK_A);
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
    int food = 0;
    for (const ItemInstance& it : p.inv.bag) {
        const int t = w.data().items[std::size_t(it.item)].type;
        food += (t == 0x15 || t == 0x19) ? it.quantity : 0;
    }
    if (hp_pct < 50 && food && tick_ - last_heal_ > 30) f.press(IN_HEAL_ITEM), last_heal_ = tick_;
    if (hp_pct < 30 && !food) retreat_ = true;
    if (hp_pct >= 70 || food) retreat_ = false;
    const Unit* ally = nullptr;
    for (const Unit& u : w.units())
        if (u.id != p.id && alive(w, u) && u.nation == p.nation && u.rank >= 4 && u.retreat != -1 &&
            (!ally || d2(p.pos, u.pos) < d2(p.pos, ally->pos)))
            ally = &u;
    // Interact indicator: talk, free prisoners, pick up items, recruit. A unit already talked to is not
    // pestered again for 100 s (scripts replay their dialog on every R).
    const bool fresh_talk = prompt_kind_ != "talk" || !talked_.count(prompt_id_) || tick_ - talked_[prompt_id_] > 3000;
    if (prompt_ && fresh_talk && tick_ - last_interact_ > 20 && prompt_kind_ != "mount" && prompt_kind_ != "bribe" &&
        prompt_kind_ != "recruit_screen" && prompt_kind_ != "workshop") {
        last_interact_ = tick_;
        if (prompt_kind_ == "talk") talked_[prompt_id_] = tick_;
        f.press(IN_INTERACT);
        goal = "interact " + prompt_kind_;
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
    // Enemies: keep the current target while it lives and stays near; else the nearest reachable one
    // close by, or one that is attacking the player. While hurt (< 70 %) an enemy that has not noticed
    // the player yet is left alone: rest first. With a hero to protect (mission plan) only enemies near
    // that hero count, wherever the waypoints point.
    const bool fresh = hp_pct >= 70;
    const Unit* vip = protectee();
    auto near_vip = [&](const Unit& u) { return !vip || d2(u.pos, vip->pos) < 900 || (u.target == p.id && d2(u.pos, p.pos) < 200); };
    const Unit* foe = nullptr;
    const Unit* lurker = nullptr;  // nearest unaware enemy within 600
    if (const Unit* t = w.unit(fight_target_);
        t && alive(w, *t) && t->m.active && w.hostile_to_player(*t) && d2(p.pos, t->pos) < 700 && near_vip(*t))
        foe = t;
    if (!foe || foe->target != p.id) {  // archers close by first, then whoever targets the player, then the nearest
        float best = 1e30f;
        const float reach = wp && !vip ? 450.0f : 1e9f;
        for (const Unit& u : w.units()) {
            if (!alive(w, u) || !u.m.active || !w.hostile_to_player(u) || u.no_react || std::fabs(u.pos.z - p.pos.z) > 150) continue;
            const auto& c = w.data().units[std::size_t(u.cls)];
            if (c.unit_class == 8 || c.special == 2 || !near_vip(u)) continue;  // siege weapons, horses
            float d = d2(p.pos, u.pos);
            const bool aware = u.target == p.id || d < 250;
            if (!aware && !fresh && !vip) {
                if (d < 600 && (!lurker || d < d2(p.pos, lurker->pos))) lurker = &u;
                continue;
            }
            if (u.target == p.id) d *= 0.5f;
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
    if (vip && d2(p.pos, vip->pos) > 300) {
        goal = "guard #" + std::to_string(vip->id);
        steer(f, p, vip->pos, false);
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
            goal = "talk to #" + std::to_string(npc->id);
            if (npc->id != talk_key_) talk_key_ = npc->id, talk_tick_ = tick_;
            if (tick_ - talk_tick_ > 900) talked_[npc->id] = tick_;  // no talk icon in 30 s: give up on it
            if (d2(p.pos, npc->pos) > 60) steer(f, p, npc->pos, false);
            else f.mouse_dx = w.rel_angle(p, npc->pos) / 0.18f;
            return f;
        }
        if (d2(p.pos, *wp) > 150) seen_tick_ = tick_;
        else if (tick_ - seen_tick_ > 300) done_wps_.push_back(*wp), seen_tick_ = tick_;  // stood there 10 s
        goal = "waypoint " + std::to_string(wp_slot) + " " + at(*wp);
        if (d2(p.pos, *wp) > 80) steer(f, p, *wp, false);
        return f;
    }
    for (const EndCondition& c : m_.state().end) {
        if (!c.has_des || !c.enabled || c.type != EndCondition::Win || c.finish == 1 || c.territory_link.empty()) continue;
        const Territory* t = m_.places().territory(m_.places().territory_by_name(c.territory_link));
        if (!t) continue;
        const Vec3 g{float(t->center_x), float(t->center_y), p.pos.z};
        if (d2(p.pos, g) < 300) break;
        goal = "territory " + c.territory_link + " " + at(g);
        steer(f, p, g, false);
        return f;
    }
    const Unit* far = nullptr;
    for (const Unit& u : w.units())
        if (alive(w, u) && u.m.active && w.hostile_to_player(u) && !u.no_react && (!far || d2(p.pos, u.pos) < d2(p.pos, far->pos)))
            far = &u;
    if (far) {
        goal = "hunt #" + std::to_string(far->id) + " " + at(far->pos);
        steer(f, p, far->pos, false);
        return f;
    }
    // Nothing left: everything lying around, else stand.
    if (loot(f, p, 4000)) return f;
    goal = "idle";
    return f;
}

// Mission plan: the friendly hero whose death ends the mission and that the scripts send the enemy for.
// Mission 2 (S3L1): the English storm Orleans for the town hall; the Bastard must live (stages 3-5).
const Unit* Autopilot::protectee() const {
    const World& w = m_.world();
    if (m_.info().stem() != "S3L1" || m_.scenario().stage < 3 || m_.scenario().stage > 5) return nullptr;
    const int cls = w.data().unit_index("ORLEAN");
    for (const Unit& u : w.units())
        if (u.cls == cls && alive(w, u)) return &u;
    return nullptr;
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
        if (trace && e.type == Ev::Action && e.value == ATTACK) atk_start_[e.unit] = tick_;
        if (trace && e.type == Ev::Death)
            if (const Unit* v = tw.unit(e.unit)) {
                const Unit* k = tw.unit(e.other);
                std::printf("[bot %d] death #%d %s at %s by #%d %s\n", tick_, v->id, tw.data().units[std::size_t(v->cls)].code.c_str(),
                            at(v->pos).c_str(), e.other, k ? tw.data().units[std::size_t(k->cls)].code.c_str() : "-");
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
        if (e.kind == K::InteractPrompt) prompt_ = e.a != 0, prompt_id_ = e.b, prompt_kind_ = e.text;
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
            std::printf("[bot %d] %s hp %d/%d lv %d xp %d food %d stage %d kills %d ally %s hp %d | %s\n", tick_, at(p->pos).c_str(), p->hp,
                        w.max_hp(*p), p->level, p->xp, food, m_.scenario().stage, kills, j ? at(j->pos).c_str() : "-", j ? j->hp : 0,
                        goal.c_str());
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
