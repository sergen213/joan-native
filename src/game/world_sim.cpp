// World core: unit creation (Unit_init 0x4f8550), SetAction 0x501190, the unit scheduler
// UnitArray_process 0x51e280 / Unit_process 0x52c490, anim-end handler 0x7c7ab0, Unit_move 0x4f0b50,
// regeneration, the trigger dispatcher 0x4ef090, physics, snapshot and serialization.
#include "game/world_sim.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <utility>

#include "game/mount_ride.hpp"
#include "game/rng_lcg.hpp"
#include "game/siege_crew.hpp"

namespace joa::game {

namespace {
constexpr float kDegToRad = 0.017453292f;
float wrap360(float d) {
    d = std::fmod(d, 360.0f);
    return d < 0 ? d + 360.0f : d;
}
// IDLE2/IDLE3 play once: 0x7c7ab0 ends them on Unit_animRemainingMs == 0, which a looping actor
// (0x5045f0: elapsed % length) never reports.
bool loops(int a) {
    return a == IDLE || a == IDLE_BAT || a == IDLE_T || is_locomotion(a) || a == SHTIDLE ||
           a == SHTIDLEH || a == GUARD1 || a == H_IDLE || a == H_WALK || a == H_RUN || a == UP;
}
}  // namespace

const char* ev_name(Ev e) {
    static const char* n[] = {"action", "hit", "blocked", "knockdown", "death", "levelup", "xp", "sound", "animevent",
                              "effect", "crit", "projectile", "projectile_hit", "item_drop", "custom"};
    return n[int(e)];
}

World::World(GameData& gd, const ITerrainQuery* terrain, std::uint32_t seed) : gd_(gd), terrain_(terrain) {
    own_rng_.seed = seed;
    cosmetic_.seed = seed ^ 0x9e3779b9u;
    for (int a = 0; a < 8; ++a)
        for (int b = 0; b < 8; ++b) hostile_[a][b] = a != b && a != 0 && b != 0;
}

// ---------------------------------------------------------------------------------------------
// Units

int World::spawn_unit(std::string_view class_code, int nation, Vec3 pos, float yaw_deg, int level) {
    const int cls = gd_.unit_index(class_code);
    if (cls < 0) return 0;
    gd_.moves(cls);  // load the move table + clips now; later access is const
    const UnitClass& rec = gd_.units[cls];
    Unit u;
    u.id = int(units_.size()) + 1;
    u.cls = cls;
    u.nation = nation;
    u.pos = pos;
    if (terrain_) u.pos.z = terrain_->height(pos.x, pos.y);
    u.yaw_deg = u.yaw_goal = wrap360(yaw_deg);
    u.name = rec.name, u.short_name = rec.short_name, u.title = rec.title;
    u.rank = rec.ranking;  // creator 0x5089e0 passes rec+0x149
    u.kingdom = rec.kingdom;
    u.max_lives = u.lives = rec.life;
    u.level = std::max(1, rec.level);
    u.str = rec.str, u.dex = rec.dex, u.def = rec.def, u.lead = rec.lead;
    u.archery = rec.archery, u.horse_riding = rec.horse_riding, u.swimming = rec.swimming;
    u.elephant_riding = rec.elephant_riding, u.wide_animal = rec.wide_animal, u.preaching = rec.preaching;
    u.power_jump = rec.power_jump, u.disguise = rec.disguise, u.persuade = rec.persuade;
    u.lock_picking = rec.lock_picking, u.mountain = rec.mountain;
    u.armor_level = rec.earmor_id;
    u.armor_item = rec.earmor_item;
    u.loyalty = rec.loyalty, u.morale = rec.morale, u.reputation = rec.reputation;
    if (rec.behavior >= 1 && rec.behavior <= 4) u.mode = rec.behavior;
    u.attack_behaviour = 1;
    u.balance = rec.falling;
    u.m.vision = gd_.c.vision_r;
    u.m.crew.assign(rec.attach_points.size(), 0);
    u.m.reserved.assign(rec.attach_points.size(), 0);
    u.m.can_attach = rec.attach_points.empty() ? 0 : 1;
    // AI combo buffer: 'A' x random_range(0,3) (first RNG draw of Unit_init).
    u.idle_variant.assign(size_t(random_range(rng(), 0, 3)), 'A');
    // inventory (0x4970c0 / 0x497270 / 0x4973a0)
    auto inst = [&](int item, int qty) { return ItemInstance{item, qty, gd_.items[item].min_level}; };
    if (rec.pweapon) u.inv.melee[0] = inst(rec.pweapon, 1), u.inv.weapon_mode = 1;
    if (rec.sweapon) u.inv.melee[1] = inst(rec.sweapon, 1);
    if (rec.prange) {
        u.inv.ranged = inst(rec.prange, 1);
        if (u.inv.weapon_mode == 0) u.inv.weapon_mode = 2;
    }
    if (rec.srange) u.inv.ammo = inst(rec.srange, gd_.items[rec.srange].quantity);
    units_.push_back(std::move(u));
    Unit& w = units_.back();
    roll_drop(w);
    w.base_max_hp = rec.hp;
    w.base_max_ep = rec.ep;
    unlock_upgrades(w, 0);
    for (auto& e : w.upgrades)
        if (e.applied < e.unlocked) ++e.applied;  // 0x531520: level-0 abilities known for free
    if (w.level >= 2)
        if (const LevelRow* r = gd_.level_row(w.level - 1)) w.xp = r->exp;
    w.hp = max_hp(w);
    w.hp_trail = w.hp;
    w.ep = max_ep(w);
    w.ts_hp_regen = w.ts_ep_regen = w.ts_balance = now_;
    set_action(w, IDLE);
    if (level > w.level) set_level(w, level);
    return w.id;
}

void World::roll_drop(Unit& u) {
    const UnitClass& rec = gd_.units[u.cls];
    auto list = gd_.drop_lists.find(rec.drop_list);
    if (rec.drop_list.empty() || rec.drop_list == "NONE" || list == gd_.drop_lists.end()) return;
    bool drop = false;
    if (rec.ranking >= 4) drop = true;
    else if (rec.unit_class >= 1 && rec.unit_class <= 6) {
        const int k = rec.unit_class - 1;
        const int r = rng().next(100);
        drop = r <= (rec.ranking == 3 ? gd_.c.drop_cap_pct[k] : gd_.c.drop_pct[k]);
    }
    if (!drop) return;
    // DropList_roll 0x464030: r = RNG(total)+1; walk last -> first, r -= weight; first r < 1 wins.
    int total = 0;
    for (auto& [item, w] : list->second) total += w;
    if (total <= 0) return;
    int r = rng().next(total) + 1;
    for (auto it = list->second.rbegin(); it != list->second.rend(); ++it) {
        r -= it->second;
        if (r < 1) {
            u.drop_item = it->first;
            u.drop_level = gd_.items[it->first].min_level;
            return;
        }
    }
}

void World::delete_unit(int id) {
    Unit* u = unit(id);
    if (!u) return;
    // Unit_deinit 0x461f10: the ridden horse loses its rider (0x482930(0)); a horse with a rider in an
    // active scene dismounts it (0x4fc220(0)).
    if (Unit* h = unit(u->mount)) horse_set_rider(*this, *h, 0);
    if (Unit* r = unit(u->m.rider); r && u->m.active && r->mount == id) ride_horse(*this, *r, 0);
    u->deleted = true;
}
Unit* World::unit(int id) {
    return id >= 1 && id <= int(units_.size()) && !units_[id - 1].deleted ? &units_[id - 1] : nullptr;
}
const Unit* World::unit(int id) const {
    return id >= 1 && id <= int(units_.size()) && !units_[id - 1].deleted ? &units_[id - 1] : nullptr;
}
int World::add_item(std::string_view code, int level, int quantity, Vec3 pos) {
    const int it = gd_.item_index(code);
    if (!it) return 0;
    items_.push_back({int(items_.size()) + 1, it, level ? level : gd_.items[it].min_level, quantity, pos, false});
    return items_.back().id;
}
void World::del_item(int id) {
    if (WorldItem* i = item(id)) i->deleted = true;
}
WorldItem* World::item(int id) {
    return id >= 1 && id <= int(items_.size()) && !items_[id - 1].deleted ? &items_[id - 1] : nullptr;
}
void World::set_player(int id) {
    if (Unit* old = unit(player_)) old->ai_controlled = true;
    player_ = id;
    pc_.unit = id;
    if (Unit* u = unit(id)) u->ai_controlled = false;
}
void World::set_hostile(int a, int b, bool h) {
    if (a >= 0 && a < 8 && b >= 0 && b < 8) hostile_[a][b] = hostile_[b][a] = h;
}
bool World::hostile(int a, int b) const { return a >= 0 && a < 8 && b >= 0 && b < 8 && hostile_[a][b]; }
bool World::hostile_to_player(const Unit& u) const {
    const Unit* p = unit(player_);
    return p && p->nation != 0 && hostile(u.nation, p->nation);
}
void World::attach_pad(int id, std::function<InputFrame(World&, const Unit&)> source) {
    pads_[id] = std::move(source);
    pad_ctrl_[id].unit = id;
}

// ---------------------------------------------------------------------------------------------
// Stats (getters add item effects 1..6; setters clamp)

int World::max_hp(const Unit& u) const { return u.base_max_hp + bonus(u, MAX_HIT_POINT); }
int World::max_ep(const Unit& u) const { return u.base_max_ep + bonus(u, MAX_ENERGY_POINT); }
int World::stat_str(const Unit& u) const { return u.str + bonus(u, STRENGTH); }
int World::stat_dex(const Unit& u) const { return u.dex + bonus(u, DEXTERITY); }
int World::stat_def(const Unit& u) const { return u.def + bonus(u, DEFENSE); }
int World::stat_lead(const Unit& u) const { return u.lead + bonus(u, LEADERSHIP); }

void World::set_hp(Unit& u, int v) {
    u.hp = std::clamp(v, 0, max_hp(u));
    if (u.hp == 0 && u.lives > 0) {
        --u.lives;
        u.hp = max_hp(u);
        u.hp_trail = u.hp;
    }
}
void World::set_ep(Unit& u, int v) { u.ep = std::clamp(v, 0, max_ep(u)); }
void World::set_balance(Unit& u, int v) {
    u.balance = std::clamp(v, 0, 100);
    if (u.balance == 0) {
        if (u.mount) set_action(u, H_DROP);
        else knockdown(u);
    }
}
void World::set_morale(Unit& u, int v) {
    // 0x4fe8c0 (asm 0x4fe90a..0x4fe936): Inv_bonus runs on THIS unit's inventory with the recno of
    // Unit_get_leader (self for a leader), so enhancer requirements are tested on the leader; no leader -> clamp.
    const Unit* l = v < u.morale && u.team != 0 ? (u.leader ? unit(u.leader) : &u) : nullptr;
    if (l) {
        if (bonus(u, MORALE_DROP, l) == 0) return;
        const int base = bonus(u, MORALE_BASE, l);
        if (base < u.morale) return;   // quirk: drop ignored whenever morale > MORALE_BASE
        if (v < base) { u.morale = base; return; }
    }
    u.morale = std::clamp(v, 0, 100);
}

// ---------------------------------------------------------------------------------------------
// Animation records

const MoveRecord* World::move(const Unit& u) const {
    const MoveTable* mt = gd_.moves(u.cls);
    return mt && u.move_index >= 0 && u.move_index < int(mt->rows.size()) ? &mt->rows[u.move_index] : nullptr;
}
const AnimScript* World::script_of(const MoveRecord* r) const {
    return r && r->script >= 0 ? &gd_.scripts[r->script] : nullptr;
}
// Unit_findCurrentAnimRec 0x504010: scans the table from the LAST record to the first. A record matches
// posture, action and ITEM_USING with SPECIAL equal to the unit's (+0x21d; 4 counts as 0 outside
// WALK/RUN) or 0. An exact-SPECIAL match returns at once (highest index); SPECIAL-0 fallbacks keep the
// lowest index. ATTACK / H_ATTACK need the exact combo; for other actions a combo match is preferred
// (IDLE AI buffer rows).
const MoveRecord* World::find_move(const Unit& u, int action, std::string_view combo, int posture) const {
    const MoveTable* mt = gd_.moves(u.cls);
    if (!mt) return nullptr;
    if (posture < 0) posture = u.posture;
    const int sp = u.special == 4 && action != WALK && action != RUN ? 0 : u.special;
    const bool attack = action == ATTACK || action == H_ATTACK;
    const MoveRecord* best[4] = {};  // [special fallback][combo mismatch]
    for (auto it = mt->rows.rbegin(); it != mt->rows.rend(); ++it) {
        const MoveRecord& r = *it;
        if (r.position != posture || r.movement != action || r.item_using != u.item_using) continue;
        if (r.special != sp && r.special != 0) continue;
        const bool cm = r.combo == combo;
        if (!cm && attack) continue;
        const int k = (r.special == sp ? 0 : 2) + (cm ? 0 : 1);
        if (k < 2 && best[k]) continue;  // exact SPECIAL: first found from the end
        best[k] = &r;                    // fallback: keeps the lowest index
    }
    for (const MoveRecord* r : best)
        if (r) return r;
    return nullptr;
}
int World::anim_length(const Unit& u) const {
    const MoveRecord* r = move(u);
    const ClipInfo* c = r ? gd_.clip(*r) : nullptr;
    return c ? int(c->length_ms) : 0;
}
int World::anim_remaining(const Unit& u) const { return std::max(0, anim_length(u) - (now_ - u.anim_start)); }

bool World::can_act(const Unit& u) const {
    return now_ >= u.ts_state + gd_.c.clb_idle && can_act_state(u.action);
}

float World::rel_angle(const Unit& self, Vec3 t) const {
    if (t.x == self.pos.x && t.y == self.pos.y) return 0.0f;
    // Sign: positive = target clockwise (to the right) of the facing; yaw is CCW-positive (cos, sin).
    float a = self.yaw_deg - std::atan2(t.y - self.pos.y, t.x - self.pos.x) / kDegToRad;
    a = std::fmod(a, 360.0f);
    if (a > 180.0f) a -= 360.0f;
    if (a < -180.0f) a += 360.0f;
    return a;
}
void World::set_yaw(Unit& u, float deg) { u.yaw_goal = wrap360(deg); }

// Unit_set_yaw_rad 0x4ff870 rotates the ridden horse too, except during mount/dismount (0x4f..0x52).
static void set_body_yaw(World& w, Unit& u, float deg) {
    u.yaw_deg = wrap360(deg);
    if (Unit* h = w.unit(u.mount); h && !h->deleted && !(u.action >= H_MNT_L && u.action <= H_DMNT_R)) h->yaw_deg = u.yaw_deg;
}
void World::set_yaw_now(Unit& u, float deg) {
    u.yaw_goal = wrap360(deg);
    set_body_yaw(*this, u, deg);
}

// FUN_00429ec0 (CONFIRMED asm): one Misc_rand_unit_incl r (stored as float): r <= IDLE2_CH -> IDLE2,
// r <= IDLE3_CH -> IDLE3, else IDLE. Only Unit_stop calls it, for horses. Cosmetic stream (deviation 3).
void World::idle_pick(Unit& u) {
    const float r = float(rand_unit_incl(cosmetic_));
    set_action(u, r <= gd_.c.idle2_ch ? IDLE2 : r <= gd_.c.idle3_ch ? IDLE3 : IDLE);
}

// Unit_stop 0x508b90 (CONFIRMED): idle-class states are kept as they are. A rider stands H_IDLE and
// its horse picks IDLE/IDLE2/IDLE3 (0x429ec0); a horse with a rider (+0x1e6) picks for itself (asm
// 0x508c10..0x508c28: ECX = the horse); a cart with a driver
// stands and the driver gets CA_IDLE; a cart driver CA_IDLE and the cart IDLE; a climber
// C_IDLE_A/B by climb stage; UP lands as IDLEDN; units in ATTACK/ACTIVE mode stand IDLE_BAT, bound
// prisoners IDLE_T, everyone else IDLE. Deviation: JATTACKB (excluded in the original, which ends it
// elsewhere) stands too; set_action turns that IDLE into JATTBI.
void World::stop(Unit& u) {
    const int a = u.action;
    if (is_idle_class(a)) return;
    if (Unit* h = unit(u.mount); h && !h->deleted) {
        set_action(u, H_IDLE);
        idle_pick(*h);
        return;
    }
    if (u.m.rider) return idle_pick(u);
    const int stand = a == UP ? IDLEDN : (u.mode == 1 || u.mode == 2) ? IDLE_BAT : (u.no_react && u.m.bound) ? IDLE_T : IDLE;
    if (Unit* d = unit(u.m.cart_driver); d && !d->deleted) {
        set_action(u, stand);
        set_action(*d, CA_IDLE);
        return;
    }
    if (Unit* c = unit(u.m.cart); c && !c->deleted) {
        set_action(u, CA_IDLE);
        set_action(*c, IDLE);
        return;
    }
    if (u.m.climb) {
        switch (a) {
        case C_DN_BA: case C_DN_CA: case C_UP_BA: case C_UP_SA: set_action(u, C_IDLE_A); break;
        case C_DN_AB: case C_DN_CB: case C_UP_AB: set_action(u, C_IDLE_B); break;
        }
        return;
    }
    set_action(u, stand);
}

// 0x4f2190 (Unit_process phase 1, after Unit_updateAction): step the body facing (+0x1a) toward the
// desired heading (+0x1e). Degrees per 33 ms by class Special: siege/tower/bombard/trebuchet/ram/cart
// 1, fauconneau 2, siege crossbow 5, everyone else 20; capped at 25 (LOD < 2) or 90. A siege weapon
// turns only with every required slot (usage bit 1, or any slot for crossbow/trebuchet) manned; the
// crew is carried to the rotated attach points. Returns true while the remaining angle >= the step:
// the caller then skips/cancels the move (CONFIRMED decomp + asm: ftol tests are on diff / 360-diff).
bool World::turn_step(Unit& u) {
    if (u.m.vehicle >= 1) return false;   // +0x384: attached units are placed by their vehicle
    const float cur = u.yaw_deg;
    const int a = u.action;
    if ((a >= ATTACKED && a <= BATTKED) || a == DIE || (a >= H_HITED && a <= H_RHITED)) {
        u.yaw_goal = cur;
        return false;
    }
    const UnitClass& rec = gd_.units[u.cls];
    const int n = int(rec.attach_points.size());
    if (rec.unit_class == 8 && n > 0) {
        bool manned = false;
        for (int i = 0; i < n; ++i) {
            if (!((rec.attach_points[i].usage & 1) || rec.special == 6 || rec.special == 10)) continue;
            const Unit* c = i < int(u.m.crew.size()) ? unit(u.m.crew[i]) : nullptr;
            if (!c || c->deleted) continue;
            manned = c->m.vehicle == u.id;
            if (!manned) break;
        }
        if (!manned) {
            u.yaw_goal = cur;
            return false;
        }
    }
    const float diff = wrap360(u.yaw_goal - cur);
    if (diff <= 0 || diff >= 360) return false;
    float rate = 20;
    switch (rec.special) {
    case 4: case 8: case 9: case 10: case 11: case 13: rate = 1; break;
    case 5: rate = 2; break;
    case 6: rate = 5; break;
    }
    const float step = std::min(float(unsigned(now_ - u.last_phase1)) / 33.0f * rate, u.lod_tier < 2 ? 25.0f : 90.0f);
    const bool neg = diff >= 180;
    const float rem = neg ? 360 - diff : diff;
    set_body_yaw(*this, u, neg ? cur - std::min(step, rem) : cur + std::min(step, rem));
    const bool rotating = int(diff) != 0 && int(360 - diff) != 0;
    if (rec.unit_class == 8) {
        const int push = rec.special == 4 ? PUSH_L : PUSH;
        const float yaw = u.yaw_deg * kDegToRad, cs = std::cos(yaw), sn = std::sin(yaw);
        for (int i = 0; i < n && i < int(u.m.crew.size()); ++i) {
            Unit* c = u.m.crew[i] > 0 ? unit(u.m.crew[i]) : nullptr;
            if (!c || c->deleted) continue;
            const UnitClass::AttachPoint& p = rec.attach_points[i];
            set_body_yaw(*this, *c, u.yaw_deg + p.dir);
            c->pos = {p.x * cs - p.y * sn + u.pos.x, p.x * sn + p.y * cs + u.pos.y, u.pos.z + p.z};
            if (terrain_) c->pos.z = terrain_->height(c->pos.x, c->pos.y) + p.z;
            const bool pusher = p.usage & 1;
            const bool firing = c->action == SHTLOADC || c->action == SHTIDLEC || c->action == SHTOUTC;
            if (!rotating) {
                if (!is_idle_class(c->action) && (pusher || !firing)) stop(*c);
            } else if (pusher) {
                if (c->action != push) set_action(*c, push);
            } else if (rec.special == 11) {
                if (!is_idle_class(c->action)) stop(*c);
            } else if (!firing && c->action != WALK) {
                set_action(*c, WALK);
            }
        }
    } else if (rec.unit_class == 9 && a != WALK && a != RUN) {
        if (!rotating) stop(u);
        else set_action(u, neg ? T_RIGHT : T_LEFT);
    }
    return step <= rem;
}

// Unit_set_action 0x501190.
bool World::set_action(Unit& u, int a, char letter) {
    siege_on_set_action(*this, u, a);
    const int cur = u.action;
    const bool hit_action = (a >= ATTACKED && a <= DNATKED) || is_knocked(a) || a == H_DROP || a == DIE;
    // While flinching, non-hit actions wait for the flinch animation (hit-stun); siege exempt.
    if (is_flinch(cur) && anim_remaining(u) > 0 && !hit_action && gd_.units[u.cls].unit_class != 8) return false;
    // Knocked AI units refuse other actions while the lying timer runs.
    if (is_knocked_ext(cur) && u.ai_controlled && u.knock_ms > 0 && !hit_action) return false;
    // Remaps.
    if (is_knocked_ext(cur) && a != DIE && hit_action && find_move(u, DNATKED)) a = DNATKED;
    if (a == DOWN && (cur == JATTACKB || cur == JATTBI)) a = JATTBD;
    if (a == IDLE && cur == JATTACKB) a = JATTBI;
    if (a == ATTACKED && find_move(u, ATTKED2)) {
        if (cur == ATTACKED) a = ATTKED2;
        else if (cur != ATTKED2 && rng().next(2) == 0) a = ATTKED2;
    }
    std::string want;
    if ((a == ATTACK || a == H_ATTACK) && letter) want = (u.combo_idx > 0 ? u.combo : std::string()) + letter;
    const MoveRecord* r = nullptr;
    if (a == IDLE && !u.idle_variant.empty()) r = find_move(u, a, u.idle_variant);
    if (!r) r = find_move(u, a, want);
    if (!r && u.posture != 0) r = find_move(u, a, want, 0);
    if (!r) return false;
    const MoveTable* mt = gd_.moves(u.cls);

    u.prev_action = cur;
    u.action = a;
    u.ts_state = now_;
    u.move_index = int(r - mt->rows.data());
    u.anim_start = now_;
    u.shot_spawned = false;
    if (a != ATTACK && a != H_ATTACK) {  // ClearCombo 0x50d0d0
        u.combo.clear();
        if (u.combo_idx != -1) u.combo_idx = 0;
        u.follow_queued = false;
        if (u.id == player_ && a != CHAIN) pc_.chain = 1;   // 0x503fb0
    } else if (letter) {
        if (u.combo_idx < 0) u.combo_idx = 0, u.combo.clear();
        u.combo += letter;
        u.combo_idx = int(u.combo.size());
    }
    u.win_open = r->start_time;
    u.win_close = r->end_time;
    const AnimScript* s = script_of(r);
    u.gravity_scale = int(s ? s->gravity : 3.0f);
    if (is_knocked(a) || a == H_DROP) {
        u.knock_ms = gd_.c.fell_dur / 5 + rng().next(gd_.c.fell_dur);
        if (u.rank >= 4) u.invuln_ms = std::max(u.invuln_ms, gd_.c.invc_dur);
    }
    // Attack triggers (asm 0x502a8b..0x502bb1).
    if (s && (a == ATTACK || a == JATTACKB || a == JATTBI || a == JATTBD || (a >= ENGATK && a <= ENGATK4) ||
              a == H_ATTACK || a == RUSH || a == H_RUN || a == H_WALK)) {
        int reach = 0;
        for (const auto& m : u.inv.melee)
            if (m.item) reach = std::max(reach, gd_.items[m.item].range);
        Trigger t;
        t.type = 1;
        t.sender = u.id;
        t.pos = u.pos;
        t.radius = float(s->range + reach);
        t.start = t.end = now_ + s->start_time;
        triggers_.push_back(t);
        t.target_kind = 2;   // map-object twin (0x5029d1)
        triggers_.push_back(t);
    } else if (s && a == DOWNATK) {
        Trigger t;
        t.type = 0x400;
        t.sender = u.id;
        t.pos = u.pos;
        t.radius = float(s->range);   // quirk: weapon reach not added
        t.start = t.end = now_ + s->start_time;
        triggers_.push_back(t);
    }
    schedule_cues(u, *r);
    emit(Ev::Action, u.id, 0, a, action_name(a));
    return true;
}

void World::schedule_cues(Unit& u, const MoveRecord& r) {
    std::erase_if(u.cues, [](const PendingCue& c) { return c.kind == 1; });
    u.cues.clear();
    if (!r.sound.empty()) u.cues.push_back({now_ + std::max(0, r.sound_time), u.action, 0, r.sound});
    if (!r.sound2.empty()) u.cues.push_back({now_ + std::max(0, r.sound_time2), u.action, 0, r.sound2});
    if (const ClipInfo* c = gd_.clip(r))
        for (const auto& e : c->events) u.cues.push_back({now_ + int(e.time), u.action, 1, e.name});
}
void World::emit_cues(Unit& u) {
    for (size_t i = 0; i < u.cues.size();) {
        PendingCue& c = u.cues[i];
        if (c.action != u.action) { u.cues.erase(u.cues.begin() + long(i)); continue; }
        if (now_ >= c.at_ms) {
            emit(c.kind ? Ev::AnimEvent : Ev::Sound, u.id, 0, 0, c.name);
            u.cues.erase(u.cues.begin() + long(i));
            continue;
        }
        ++i;
    }
}

void World::emit(Ev type, int unit, int other, int value, std::string name) {
    events_.push_back({now_, type, unit, other, value, std::move(name)});
}

// ---------------------------------------------------------------------------------------------
// Tick

void World::tick_units(const InputFrame& in) {
    // Sys_process_frame_input: the controller runs before the clock advances (Part B §1.2). ViewCtrl_process_key
    // 0x42e1a0 switches on +0x10 (= game_mode): 0 keyboard, 1 joystick, 2 point&click, 3 keyboard+mouse; in RTS
    // (mode 4) it dispatches to PlayerCtrl_rts 0x440b30 (engine GameCamera) instead: the hero is AI. Pads keep mode 3.
    if (unit(pc_.unit) && game_mode != 4) {
        pc_.mode = game_mode;
        pc_.process(*this, in);
    }
    for (auto& [id, src] : pads_)
        if (Unit* u = unit(id)) {
            const InputFrame f = src(*this, *u);
            pad_ctrl_[id].process(*this, f);
        }
    now_ += kTickMs;

    // UnitArray_process 0x51e280 (normal mode). Pass 0: recno N..1.
    if (stop_unit_process) {   // cut-scene flag 0xa596ab: the scheduler does nothing
        process_projectiles();
        process_triggers();
        return;
    }
    if (cut_scene) {   // special mode (Game 0x4301c0): only cut-scene units run fully
        for (int id = int(units_.size()); id >= 1; --id) {
            Unit& u = units_[id - 1];
            if (u.deleted) continue;
            if (u.m.cutscene_process) {
                u.lod_tier = 1;
                for (int ph = 0; ph < 4; ++ph) unit_process(u, ph);
            } else if (!u.no_react) {
                if (u.action != IDLE && !dead(u)) stop(u);
                unit_process(u, 0), unit_process(u, 1);
            }
        }
        process_projectiles();
        process_triggers();
        return;
    }
    const Unit* pl = unit(player_);
    for (int id = int(units_.size()); id >= 1; --id) {
        Unit& u = units_[id - 1];
        if (u.deleted) continue;
        if (!pl) u.lod_tier = 4;
        else if (u.id == player_) u.lod_tier = 1;
        else {
            const float dx = u.pos.x - pl->pos.x, dy = u.pos.y - pl->pos.y, d2 = dx * dx + dy * dy;
            u.lod_tier = d2 <= 4e6f ? 1 : d2 <= 1.6e7f ? 2 : d2 <= 6.4e7f ? 3 : 4;
        }
        const bool busy = !u.ai_controlled || u.hp > 0 || u.lives != 0 || u.retreat == -2 ||
                          (u.action == DIE && now_ - u.ts_state < anim_length(u) + 3000);
        if (!busy) { delete_unit(id); continue; }
        if (u.lod_tier <= 1) unit_process(u, 0), unit_process(u, 1);
    }
    // Passes 1..12 (budgets are never exhausted here: every unit, ascending recno).
    auto pass = [&](auto pred, int phase) {
        for (size_t i = 0; i < units_.size(); ++i)
            if (!units_[i].deleted && pred(units_[i])) unit_process(units_[i], phase);
    };
    auto tier = [](int lo, int hi) { return [=](const Unit& u) { return u.lod_tier >= lo && u.lod_tier <= hi; }; };
    pass(tier(2, 2), 0), pass(tier(2, 2), 1);  // pass 1 (phase 0 then 1 per unit in the original; equivalent)
    pass(tier(3, 99), 0), pass(tier(3, 99), 1);
    pass([](const Unit& u) { return u.lod_tier <= 1 && u.rank >= 4; }, 2);
    pass([](const Unit& u) { return u.lod_tier <= 1 && u.rank < 4; }, 2);
    pass(tier(2, 2), 2), pass(tier(3, 3), 2), pass(tier(4, 99), 2);
    pass([](const Unit& u) { return u.lod_tier <= 1 && u.rank >= 4; }, 3);
    pass(tier(0, 1), 3);   // generals/kings near the player think twice per tick (quirk)
    pass(tier(2, 2), 3), pass(tier(3, 3), 3), pass(tier(4, 99), 3);

    process_projectiles();   // #9 ThrowItem manager
    process_triggers();      // #10 Trigger_process
    if (combo_hits && now_ - combo_hits_ts > 1999) combo_hits = 0;   // HUD 0x702740
    // ViewCtrl_process 0x42f700 (camera manager 0x5634e0, after the tick's objects; skipped in cut scenes and range
    // mode): the point & click update 0x439090. Leaving mode 2 (exit 0x438b00) drops its auto action.
    if (game_mode != 2) pc_.auto_target = pc_.auto_kind = pc_.auto_state = pc_.auto_fury = 0;
    else if (!pc_.range_mode) pc_.update_click(*this);
}

void World::tick_physics() {
    integrate_projectiles(float(kTickMs));
    if (physics_) physics_->integrate(*this, kTickMs / 1000.0f);
    else integrate_units(kTickMs / 1000.0f);
    siege_after_physics(*this);
    for (Unit& u : units_)   // seated riders follow the horse body (actor attach)
        if (const Unit* h = unit(u.mount); !u.deleted && h && u.ride_ready && !(u.action >= H_MNT_L && u.action <= H_DMNT_R))
            u.pos = h->pos;
}

void World::unit_process(Unit& u, int phase) {
    if (u.deleted) return;
    switch (phase) {
    case 0:
        if (auto_pickup) auto_pickup(u);   // Unit_auto_pickup 0x49da30 (phase-0 head)
        if (u.id == player_ && critical_attack_enabled) crit_check(u);
        if (u.no_react && u.action != IDLE_T && u.action != IDLE && u.action != DIE) set_action(u, u.m.bound ? IDLE_T : IDLE);
        break;
    case 1: {
        mount_sync(*this, u);   // 0x5174e0 rider/horse sync (Unit_process phase 1 head)
        if (u.level_arrow && now_ - u.ts_level > gd_.c.levelarr) u.level_arrow = false;
        // Landing (controller 0x4352f0 landing detection, generalised to every unit).
        if (u.posture == 1 && !u.airborne) {
            u.posture = 0;
            if (!is_knocked_ext(u.action) && u.action != DIE) set_action(u, DOWN);
        }
        // Unit_process 0x52c490: Unit_updateAction / 0x4f2190 / Unit_process_movement need the body
        // actor (+0x2f5), which Unit_deactivate 0x41fcc0 destroys: inactive units freeze (CONFIRMED).
        // Movement runs unless turning (AI or cut scene); a turning body has its velocity and move
        // request cleared (body +0x142/+0x146/+0x260).
        u.root_vel = {};
        if (u.m.active) {
            update_action(u);
            const bool turning = turn_step(u);
            if (!turning || (!u.ai_controlled && !cut_scene)) siege_vehicle_move(*this, u), unit_move(u), root_motion(u);
            if (turning) u.vel.x = u.vel.y = 0;
        }
        regen(u);
        u.invuln_ms -= kTickMs;
        if (u.upgrade_ts != 0 && now_ - u.upgrade_ts >= gd_.c.upg_time && !gd_.units[u.cls].hero) {
            ai_auto_upgrade(u);
            u.upgrade_ts = 0;
        }
        u.last_phase1 = now_;
        emit_cues(u);
        break;
    }
    case 2:
        if (brain_) brain_->execute_orders(*this, u);   // all units: leaders' formations, orders to the player too
        break;
    case 3:
        if (!u.ai_controlled) break;
        if (brain_) { brain_->think(*this, u); break; }   // brain owns the 0x49d810 timer (order queue)
        if (u.think_ts < 0) u.think_ts = now_ + rng().next(500);
        if (now_ > u.think_ts + gd_.units[u.cls].script_duration) u.think_ts = now_;
        break;
    }
}

// Unit_updateAction 0x7c7ab0 (animation-end transitions).
void World::update_action(Unit& u) {
    const int a = u.action;
    const int rem = anim_remaining(u);
    if (a == GUARD1 || a == GUARDR || a == DIE) return;
    if (is_knocked_ext(a)) {
        if (rem > 0) return;
        if (u.ai_controlled) {
            u.knock_ms -= kTickMs;
            if (u.knock_ms >= 0) return;
        }
        u.balance = 100;   // SetBalance(100)
        u.knock_ms = 0;
        if (dead(u)) set_action(u, DIE);
        else {
            set_action(u, STANDUP);
            u.down_attacked = false;
        }
        return;
    }
    // Firing state machine (Part D §5).
    const int special = gd_.units[u.cls].special;
    const bool crossbow = u.inv.melee[1].item && gd_.items[u.inv.melee[1].item].type == 0xc;
    if (a == SHTOUT || a == SHTOUTH) {
        int release = 650;
        if (special == 4 || special == 9 || special == 5) release = 1667;
        else if (special == 10) release = 0;
        else if (u.ai_controlled && crossbow) release = 1300;
        if (!u.shot_spawned && rem <= release) {
            u.shot_spawned = true;
            spawn_projectile(u);
        }
        if (rem == 0) {
            if (special == 10 && find_move(u, SHTOUT2)) set_action(u, SHTOUT2);
            else stop(u);
        }
        return;
    }
    if (a == IDLE) {   // 0x7c7ab0 case 1, IDLE_CC (cosmetic stream, deviation 3); r drawn every update
        float r = float(rand_unit_incl(cosmetic_));
        if (cut_scene) r *= 4.0f;   // Game_is_cutscene: a quarter of the chance
        const int len = anim_length(u);
        const int left = len > 0 ? len - (now_ - u.anim_start) % len : 0;   // looping actor (0x5045f0)
        if (left > 100 || !gd_.moves(u.cls) || u.posture != 0 || r > gd_.c.idle_cc) return;   // +0x2f1, +0x213
        const float r2 = float(rand_unit_incl(cosmetic_));
        const bool i2 = find_move(u, IDLE2) != nullptr;
        const bool i3 = find_move(u, IDLE3) != nullptr && !u.m.rider;   // FUN_004f2170: ridden horses skip IDLE3
        if (i2 && (r2 < 0.5f || !i3)) set_action(u, IDLE2);
        else if (i3) set_action(u, IDLE3);
        return;
    }
    if (rem > 0) return;
    switch (a) {
    case IDLE2: case IDLE3: set_action(u, IDLE); break;   // 0x7c7ab0 cases 2/3
    case ATTACK: case JATTACKB: case DOWNATK: case CHAIN: case RUSH: case ENGATK4: case H_ATTACK:
    finish:
        u.combo_idx = -1;
        u.combo_lock = now_;
        stop(u);
        break;
    case ENGATK: case ENGATK2: case ENGATK3: {
        const MoveRecord* n = find_move(u, a + 1);
        if (n && (n->combo_avail == 'Y' || skill_level(u, 0xd, {}) > 0)) set_action(u, a + 1);
        else goto finish;
        break;
    }
    case SHTLOAD:
        if (!u.ai_controlled) break;   // player holds until drawn (controller)
        ai_reaim(u, 1);
        if (crossbow && find_move(u, SHTLOAD2)) set_action(u, SHTLOAD2);
        else set_action(u, u.aim_dir.z < 0.1f ? SHTIDLE : SHTIDLEH);
        break;
    case SHTLOAD2:
        if (u.ai_controlled) ai_reaim(u, 0);
        set_action(u, u.aim_dir.z < 0.1f ? SHTIDLE : SHTIDLEH);
        break;
    case SHTIDLE:
        if (u.ai_controlled) set_action(u, SHTOUT);
        break;
    case SHTIDLEH:
        if (u.ai_controlled) set_action(u, SHTOUTH);
        break;
    case IDLEDN:   // idle-class: Unit_stop would keep it (HYPOTHESIS: the landing idle ends in IDLE)
        if (dead(u)) set_action(u, DIE);
        else set_action(u, IDLE);
        break;
    case SHTOUT2: case STANDUP: case GUARD2: case DOWN: case JATTBD: case JUMP_B: case JUMP_R:
    case JUMP_L: case THROW: case STOP: case STOP_B: case DIZZY: case BACK:
    case ATTACKED: case ATTKED2: case ATTKEDR: case BATTKED: case LATTKED: case RATTKED:
    case H_HITED: case H_BHITED: case H_LHITED: case H_RHITED:
        if (dead(u)) set_action(u, DIE);
        else stop(u);
        break;
    case H_STOP: case H_STOP_B: set_action(u, H_IDLE); break;   // 0x7c7ab0 cases 0x47/0x48
    case H_MNT_L: case H_MNT_R:   // seated: rider takes the horse heading, +0x1ea = 1, stand
        if (Unit* h = unit(u.mount)) set_yaw_now(u, h->yaw_deg);
        u.ride_ready = true;
        stop(u);
        break;
    case H_DMNT_L: case H_DMNT_R: {   // step 30 to the side, heading -90 (L) / -270 (R)
        const float y = u.yaw_rad(), s = a == H_DMNT_L ? 30.0f : -30.0f;
        u.pos.x += -std::sin(y) * s, u.pos.y += std::cos(y) * s;
        set_yaw_now(u, u.yaw_deg - (a == H_DMNT_L ? 90.0f : 270.0f));
        if (dead(u)) set_action(u, DIE);
        else stop(u);
        break;
    }
    case THROWN:   // 0x7c7ab0 case 0x3b: balance 100, STANDUP unless dead
        if (!dead(u)) u.balance = 100, set_action(u, STANDUP);
        break;
    default: break;   // looping states
    }
}

// Unit_move 0x4f0b50: action -> horizontal velocity (world units / s).
void World::unit_move(Unit& u) {
    const int a = u.action;
    if (!is_locomotion(a)) return;
    const UnitClass& rec = gd_.units[u.cls];
    float s = 0, off = 0;
    switch (a) {
    case TIPTOE: s = 1.0f; break;
    case WALK: s = rec.walk; break;
    case RUN: s = rec.run; break;
    case MOVE_L: s = rec.shift_l, off = 90; break;
    case MOVE_FL: s = rec.shift_l, off = 45; break;
    case MOVE_FR: s = rec.shift_l, off = -45; break;
    case MOVE_BL: s = rec.shift_l, off = 135; break;
    case MOVE_BR: s = rec.shift_l, off = -135; break;
    case MOVE_R: s = rec.shift_r, off = -90; break;
    case MOVE_B: s = rec.shift_b, off = 180; break;
    }
    // A ridden horse on WALK runs at its body speed cap (+0x2a4, 5 or 10 m/s, set by the gear
    // functions 0x470430 / 0x466d50 via 0x6b1d80). HYPOTHESIS: the cap is reached at once.
    if (u.m.rider && u.gear_speed > 0 && a == WALK) {
        const float d = u.yaw_deg * kDegToRad;
        u.vel.x = std::cos(d) * u.gear_speed * kUnitsPerMetre;
        u.vel.y = std::sin(d) * u.gear_speed * kUnitsPerMetre;
        return;
    }
    // DEX bonus from the raw +0x157 DEX (no item effects) of the team leader, own DEX for leaders
    // and loners; never for MOVE_L/MOVE_R/MOVE_B (CONFIRMED 0x4f0b50).
    if (a != MOVE_L && a != MOVE_R && a != MOVE_B) {
        const Unit* L = u.leader && u.leader != u.id ? unit(u.leader) : nullptr;
        const int dex = (L && !L->deleted ? L : &u)->dex;
        if (dex > gd_.c.mindexs)
            s *= float(std::min(dex, gd_.c.maxdexs) - gd_.c.mindexs) / float(gd_.c.maxdexs - gd_.c.mindexs) + 1.0f;
    }
    if (u.ai_controlled) s *= gd_.c.ai_move;
    s *= 1.3f;
    if (!u.mount) {   // distant LOD tiers catch up: tier 2 x1.4 (speed < 20), tier >= 3 x1.8
        if (u.lod_tier >= 3) s *= 1.8f;
        else if (u.lod_tier == 2 && s < 20.0f) s *= 1.4f;
    }
    const float speed = s * kUnitsPerMetre;   // HYPOTHESIS: 1 speed point = 46.1 u/s
    const float d = (u.yaw_deg + off) * kDegToRad;
    u.vel.x = std::cos(d) * speed;
    u.vel.y = std::sin(d) * speed;
}

// Unit_process_movement 0x52daa0, root-motion actions (root_motion_action): the move record's root
// track (FUN_00417f20) is sampled at t and t - dt (t = now - anim start, ms, clamped at 0); the delta,
// rotated by the body heading ((dx cos - dy sin, dy cos + dx sin)), becomes the body's move request
// for this tick (x 21.691975 / dt = 1000 / 46.1 / dt m/s, Body_set_move). Skipped while airborne
// (+0x355) and for dead units except THROWN.
void World::root_motion(Unit& u) {
    if (!root_motion_action(u.action) || u.airborne || (dead(u) && u.action != THROWN)) return;
    const MoveRecord* r = move(u);
    const ClipInfo* c = r ? gd_.clip(*r) : nullptr;
    if (!c || c->root.empty()) return;
    const int t = now_ - u.anim_start, tp = std::max(0, t - kTickMs);
    float x1, y1, x0, y0;
    root_offset(*c, float(t), x1, y1);
    root_offset(*c, float(tp), x0, y0);
    const float y = u.yaw_rad(), cs = std::cos(y), sn = std::sin(y), dx = x1 - x0, dy = y1 - y0;
    const float k = 1000.0f / float(kTickMs);
    u.root_vel = {(dx * cs - dy * sn) * k, (dy * cs + dx * sn) * k, 0.0f};
}

// Regeneration, Unit_process phase 1 (asm 0x52ccd2..0x52cf3c).
void World::regen(Unit& u) {
    const Consts& c = gd_.c;
    if (u.hp == 0 || u.ep >= max_ep(u)) u.ts_ep_regen = now_;
    else if (unsigned(now_ - u.ts_ep_regen) >= unsigned(c.recov_t)) {
        u.ts_ep_regen = now_;
        set_ep(u, u.ep + c.recov_mp);
    }
    if (u.balance < 100 && now_ - u.ts_balance >= 1000) {
        u.ts_balance = now_;
        int g = u.balance;
        if (is_knocked(u.action) || u.action == DNATKED) g = int(float(g) + (float(stat_def(u)) * c.def_fall + 1.0f) * 10.0f);
        else g = int(float(g + 1) + 10.0f * c.def_fall);
        u.balance = std::clamp(g, 0, 100);   // regen never reaches 0
    }
    if (u.hp == 0 || u.hp >= max_hp(u) || u.retreat == -1) u.ts_hp_regen = now_;
    else if (now_ - u.ts_hp_regen >= c.recov_hp) {
        u.ts_hp_regen = now_;
        float inc = float(stat_def(u)) * c.def_hp_r;
        if (!(inc > 1.0f)) inc = 1.0f;
        set_hp(u, int(float(u.hp) + inc));
    }
}

void World::integrate_units(float dt) {
    for (Unit& u : units_) {
        // Bodies disabled while climbing (0x4ac910), driving a cart (0x454b20) or riding (0x5174e0).
        if (u.deleted || u.m.climb || u.m.cart || u.mount) continue;
        if (!u.airborne && !is_locomotion(u.action)) {
            // Ground friction for knock-back / stopped locomotion (HYPOTHESIS; the original body
            // only moves for the duration of the last move request or force pulse).
            u.vel.x *= 0.5f, u.vel.y *= 0.5f;
            if (std::fabs(u.vel.x) < 1 && std::fabs(u.vel.y) < 1) u.vel.x = u.vel.y = 0;
        }
        if (u.airborne) u.vel.z -= 9.81f * float(u.gravity_scale) * kUnitsPerMetre * dt;
        Vec3 n{u.pos.x + (u.vel.x + u.root_vel.x) * dt, u.pos.y + (u.vel.y + u.root_vel.y) * dt, u.pos.z + u.vel.z * dt};
        if (terrain_ && !terrain_->walkable(n.x, n.y)) n.x = u.pos.x, n.y = u.pos.y, u.vel.x = u.vel.y = 0;
        const float ground = terrain_ ? terrain_->height(n.x, n.y) : 0.0f;
        if (u.airborne) {
            if (n.z <= ground && u.vel.z <= 0) n.z = ground, u.airborne = false, u.vel.z = 0;
        } else {
            n.z = ground;
            u.vel.z = 0;
        }
        u.pos = n;
    }
}

// Trigger_process 0x4ef090: deliver once on the first tick with now >= start, insertion order.
void World::process_triggers() {
    std::vector<Trigger> due;
    std::vector<Trigger> keep;
    for (auto& t : triggers_) (now_ >= t.start ? due : keep).push_back(t);
    triggers_ = std::move(keep);
    for (const auto& t : due) deliver_trigger(t);
}

void World::deliver_trigger(const Trigger& t) {
    if (t.target_kind == 2) {   // map objects: mission layer receiver
        if ((t.type == 1 || t.type == 0x400) && t.src_kind == 1) {
            const Unit* s = unit(t.sender);
            if (!s || !is_attacking(s->action)) return;
        }
        if (object_trigger) object_trigger(t);
        return;
    }
    if (t.target_kind != 1) return;
    Unit* src = t.src_kind == 1 ? unit(t.sender) : nullptr;
    if (t.type == 1 || t.type == 0x400) {
        if (!src || !is_attacking(src->action)) return;
    }
    const bool area_cyl = t.type == 0x40 || t.type == 0x2000 || t.type == 0x4000;
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& d = units_[i];
        if (d.deleted || (t.src_kind == 1 && d.id == t.sender)) continue;
        if (t.target_id > 0 && d.id != t.target_id) continue;
        if (!(gd_.units[d.cls].trigger_mask & unsigned(t.type))) continue;
        const float dx = d.pos.x - t.pos.x, dy = d.pos.y - t.pos.y, dz = d.pos.z - t.pos.z;
        if (t.target_id <= 0) {
            if (area_cyl) {
                if (dx * dx + dy * dy > t.radius * t.radius || std::fabs(dz) > t.radius) continue;
            } else if (dx * dx + dy * dy + dz * dz > t.radius * t.radius) continue;
        }
        if (on_attack_message(d, t) == 2) break;   // down attack / throw stop the scan
    }
}

// ---------------------------------------------------------------------------------------------
// Snapshot / serialization

Snapshot World::snapshot() const {
    Snapshot s;
    s.time_ms = now_;
    for (const Unit& u : units_) {
        if (u.deleted) continue;
        UnitView v;
        v.id = u.id;
        v.class_code = gd_.units[u.cls].code;
        v.model = gd_.units[u.cls].model;
        v.pos = u.pos;
        v.yaw_deg = u.yaw_deg;
        v.action = u.action;
        if (const MoveRecord* r = move(u)) v.clip = r->upper_name, v.clip_file = r->upper_file;
        const int len = anim_length(u);
        const int t = now_ - u.anim_start;
        v.clip_time_ms = float(len > 0 ? (loops(u.action) ? t % len : std::min(t, len)) : t);
        v.hp = u.hp, v.max_hp = max_hp(u), v.ep = u.ep, v.max_ep = max_ep(u);
        v.nation = u.nation, v.team = u.team, v.level = u.level;
        v.dead = dead(u);
        s.units.push_back(std::move(v));
    }
    for (const Projectile& p : projectiles_)
        s.projectiles.push_back({p.id, p.type, {p.pos_m.x * kUnitsPerMetre, p.pos_m.y * kUnitsPerMetre, p.pos_m.z * kUnitsPerMetre},
                                 {p.vel_m.x * kUnitsPerMetre, p.vel_m.y * kUnitsPerMetre, p.vel_m.z * kUnitsPerMetre}, p.item});
    return s;
}

namespace {
template <class T> concept Visitable = requires(T& t, int& v) { t.visit(v); };

struct Writer {
    std::vector<std::uint8_t>& o;
    template <class T> void operator()(T& x) {
        if constexpr (std::is_arithmetic_v<T>) {
            const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&x);
            o.insert(o.end(), p, p + sizeof(T));
        } else if constexpr (std::is_same_v<T, std::string>) {
            std::uint32_t n = std::uint32_t(x.size());
            (*this)(n);
            o.insert(o.end(), x.begin(), x.end());
        } else if constexpr (requires { x.size(); x.resize(0); x[0]; }) {
            std::uint32_t n = std::uint32_t(x.size());
            (*this)(n);
            for (auto& e : x) (*this)(e);
        } else {
            x.visit(*this);
        }
    }
};
struct Reader {
    std::span<const std::uint8_t> in;
    size_t pos = 0;
    bool ok = true;
    template <class T> void operator()(T& x) {
        if constexpr (std::is_arithmetic_v<T>) {
            if (pos + sizeof(T) > in.size()) { ok = false; return; }
            std::memcpy(&x, in.data() + pos, sizeof(T));
            pos += sizeof(T);
        } else if constexpr (std::is_same_v<T, std::string>) {
            std::uint32_t n = 0;
            (*this)(n);
            if (!ok || pos + n > in.size()) { ok = false; return; }
            x.assign(reinterpret_cast<const char*>(in.data() + pos), n);
            pos += n;
        } else if constexpr (requires { x.size(); x.resize(0); x[0]; }) {
            std::uint32_t n = 0;
            (*this)(n);
            if (!ok || n > in.size()) { ok = false; return; }
            x.resize(n);
            for (auto& e : x) (*this)(e);
        } else {
            x.visit(*this);
        }
    }
};
}  // namespace

struct World::Impl {
    template <class V> static void visit(World& w, V& v) {
        v(w.now_), v(w.player_), v(w.own_rng_.seed), v(w.cosmetic_.seed), v(w.difficulty), v(w.game_mode);
        v(w.stop_unit_process), v(w.critical_attack_enabled), v(w.cut_scene);
        std::uint32_t shared = w.rng_->seed;
        v(shared);
        w.rng_->seed = shared;
        v(w.units_), v(w.items_), v(w.triggers_), v(w.projectiles_), v(w.next_projectile_);
        for (auto& row : w.hostile_)
            for (int& h : row) v(h);
        v(w.pc_), v(w.command_points), v(w.combo_hits), v(w.combo_hits_ts), v(w.crit);
        std::vector<int> pad_ids;
        for (auto& [id, c] : w.pad_ctrl_) pad_ids.push_back(id);
        v(pad_ids);
        for (int id : pad_ids) v(w.pad_ctrl_[id]);
    }
};

void World::serialize(std::vector<std::uint8_t>& out) const {
    Writer wr{out};
    Impl::visit(const_cast<World&>(*this), wr);
}
bool World::deserialize(std::span<const std::uint8_t> in) {
    Reader rd{in};
    Impl::visit(*this, rd);
    return rd.ok && rd.pos == in.size();
}
std::uint64_t World::hash() const {
    std::vector<std::uint8_t> b;
    serialize(b);
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint8_t c : b) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

}  // namespace joa::game
