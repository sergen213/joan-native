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

#include "game/rng_lcg.hpp"

namespace joa::game {

namespace {
constexpr float kDegToRad = 0.017453292f;
float wrap360(float d) {
    d = std::fmod(d, 360.0f);
    return d < 0 ? d + 360.0f : d;
}
bool loops(int a) {
    return a == IDLE || a == IDLE2 || a == IDLE3 || a == IDLE_BAT || a == IDLE_T || is_locomotion(a) || a == SHTIDLE ||
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
    u.yaw_deg = wrap360(yaw_deg);
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
    if (Unit* u = unit(id)) u->deleted = true;
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
    if (v < u.morale && u.team != 0) {
        const Unit* l = unit(u.leader);
        if (!l) l = &u;
        (void)l;  // the aggregation runs on THIS unit's inventory (with the leader recno; applicability not modelled)
        if (bonus(u, MORALE_DROP) == 0) return;
        const int base = bonus(u, MORALE_BASE);
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
const MoveRecord* World::find_move(const Unit& u, int action, std::string_view combo, int posture) const {
    const MoveTable* mt = gd_.moves(u.cls);
    if (!mt) return nullptr;
    if (posture < 0) posture = u.posture;
    const MoveRecord* any = nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        const int sp = pass == 0 ? u.special : 0;
        for (const auto& r : mt->rows) {
            if (r.position != posture || r.movement != action || r.item_using != u.item_using || r.special != sp) continue;
            if (r.combo == combo) return &r;
            if (!any) any = &r;
        }
        if (any && action != ATTACK && action != H_ATTACK) return any;
        if (sp == 0) break;
    }
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
void World::set_yaw(Unit& u, float deg) { u.yaw_deg = wrap360(deg); }

// Unit_set_action 0x501190.
bool World::set_action(Unit& u, int a, char letter) {
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
    // Sys_process_frame_input: the controller runs before the clock advances (Part B §1.2).
    if (unit(pc_.unit)) pc_.process(*this, in);
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
        if (!busy) { u.deleted = true; continue; }
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
}

void World::tick_physics() {
    integrate_projectiles(float(kTickMs));
    if (physics_) physics_->integrate(*this, kTickMs / 1000.0f);
    else integrate_units(kTickMs / 1000.0f);
}

void World::unit_process(Unit& u, int phase) {
    if (u.deleted) return;
    switch (phase) {
    case 0:
        if (u.id == player_ && critical_attack_enabled) crit_check(u);
        if (u.no_react && u.action != IDLE_T && u.action != IDLE && u.action != DIE) set_action(u, u.m.bound ? IDLE_T : IDLE);
        break;
    case 1: {
        if (u.level_arrow && now_ - u.ts_level > gd_.c.levelarr) u.level_arrow = false;
        // Landing (controller 0x4352f0 landing detection, generalised to every unit).
        if (u.posture == 1 && !u.airborne) {
            u.posture = 0;
            if (!is_knocked_ext(u.action) && u.action != DIE) set_action(u, DOWN);
        }
        update_action(u);
        unit_move(u);
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
    if (rem > 0) return;
    switch (a) {
    case ATTACK: case JATTACKB: case DOWNATK: case CHAIN: case RUSH: case ENGATK4:
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
        if (crossbow && find_move(u, SHTLOAD2)) set_action(u, SHTLOAD2);
        else set_action(u, u.aim_dir.z < 0.1f ? SHTIDLE : SHTIDLEH);
        break;
    case SHTLOAD2: set_action(u, u.aim_dir.z < 0.1f ? SHTIDLE : SHTIDLEH); break;
    case SHTIDLE:
        if (u.ai_controlled) set_action(u, SHTOUT);
        break;
    case SHTIDLEH:
        if (u.ai_controlled) set_action(u, SHTOUTH);
        break;
    case SHTOUT2: case STANDUP: case GUARD2: case DOWN: case JATTBD: case IDLEDN: case JUMP_B: case JUMP_R:
    case JUMP_L: case THROW: case STOP: case STOP_B: case DIZZY: case BACK:
    case ATTACKED: case ATTKED2: case ATTKEDR: case BATTKED: case LATTKED: case RATTKED:
        if (dead(u)) set_action(u, DIE);
        else stop(u);
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
    if (a != MOVE_L && a != MOVE_R && a != MOVE_B) {
        const Unit* rider = &u;
        const int dex = stat_dex(*rider);
        if (dex > gd_.c.mindexs)
            s *= float(std::min(dex, gd_.c.maxdexs) - gd_.c.mindexs) / float(gd_.c.maxdexs - gd_.c.mindexs) + 1.0f;
    }
    if (u.ai_controlled) s *= gd_.c.ai_move;
    s *= 1.3f;
    const float speed = s * kUnitsPerMetre;   // HYPOTHESIS: 1 speed point = 46.1 u/s
    const float d = (u.yaw_deg + off) * kDegToRad;
    u.vel.x = std::cos(d) * speed;
    u.vel.y = std::sin(d) * speed;
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
        if (u.deleted) continue;
        if (!u.airborne && !is_locomotion(u.action)) {
            // Ground friction for knock-back / stopped locomotion (HYPOTHESIS; the original body
            // only moves for the duration of the last move request or force pulse).
            u.vel.x *= 0.5f, u.vel.y *= 0.5f;
            if (std::fabs(u.vel.x) < 1 && std::fabs(u.vel.y) < 1) u.vel.x = u.vel.y = 0;
        }
        if (u.airborne) u.vel.z -= 9.81f * float(u.gravity_scale) * kUnitsPerMetre * dt;
        Vec3 n{u.pos.x + u.vel.x * dt, u.pos.y + u.vel.y * dt, u.pos.z + u.vel.z * dt};
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
                                 {p.vel_m.x * kUnitsPerMetre, p.vel_m.y * kUnitsPerMetre, p.vel_m.z * kUnitsPerMetre}});
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
