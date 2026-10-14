// Melee combat: attack power 0x5104f0, skill multiplier 0x510750, damage 0x509950, hit validation
// 0x5048b0, hit reaction / balance / knock-back (Unit_onMessage 0x75d540), knockdown 0x509000,
// death 0x7c6240, critical sequence 0x416680/0x462810. GAMEPLAY_CHARACTER_COMBAT.md Part C.
// All damage arithmetic is float32 (the game runs the x87 in 24-bit mode); ftol truncates.
#include <algorithm>
#include <climits>
#include <cmath>

#include "game/rng_lcg.hpp"
#include "game/world_sim.hpp"

namespace joa::game {

int World::skill_level(const Unit& u, int type, std::string_view str) const {
    int lvl = 0;
    for (const auto& e : u.upgrades) {
        const UpItem& up = gd_.upitems[e.upitem];
        if (up.type != type || e.unlocked != e.applied || e.applied == 0) continue;
        if (!str.empty() && up.para1 != str) continue;
        lvl = e.combo_level;
    }
    return lvl;
}

bool World::combo_learned(const Unit& u, const MoveRecord& r) const {
    return r.combo_avail == 'Y' || skill_level(u, 9, r.combo) > 0;
}

// 0x500d40: special move usable (anim exists, not scripted, free / AI / skill learned).
bool World::can_use_special(const Unit& u, int action) const {
    const MoveRecord* r = find_move(u, action);
    if (!r || u.no_react) return false;
    int skill = 0;
    switch (action) {
    case RUSH: skill = 0xc; break;
    case DOWNATK: skill = 0xb; break;
    case ENGATK: skill = 0xd; break;
    case JATTACKB: skill = 10; break;
    }
    const bool learned = r->combo_avail == 'Y' || u.ai_controlled || (skill && skill_level(u, skill, {}) > 0);
    if (action == RUSH && !(r->energy_pt < u.ep)) return false;
    return learned;
}

float World::attack_skill_mult(const Unit& a) const {
    int type = 0, item = 0;
    std::string_view str;
    const char last = a.combo.empty() ? 0 : a.combo.back();
    switch (a.action) {
    case ATTACK: type = 9, str = a.combo, item = last == 'A' ? A_ATTACK_DAMAGE : B_ATTACK_DAMAGE; break;
    case DOWNATK: type = 0xb, item = DOWN_ATTACK_DAMAGE; break;
    case JATTACKB: case JATTBI: case JATTBD: type = 10, item = JUMP_ATTACK_B_DAMAGE; break;
    case RUSH: type = 0xc, item = RUSH_ATTACK_DAMAGE; break;
    case ENGATK: case ENGATK2: case ENGATK3: case ENGATK4: type = 0xd; break;
    case H_ATTACK: type = last == 'A' ? 0xf : 0x10, str = a.combo, item = last == 'A' ? A_ATTACK_DAMAGE : B_ATTACK_DAMAGE; break;
    default: return 1.0f;
    }
    float m = (float(skill_level(a, type, str)) + 10.0f) / 11.0f;
    if (item) m = float(bonus(a, item) + 100) * 0.01f * m;
    return m;
}

float World::melee_attack_power(const Unit& a) const {
    const MoveRecord* r = move(a);
    const float dmg = float(r ? r->damage : 0);
    float wpn = 0;
    for (int s = 0; s < 2; ++s)
        if (const ItemInstance* it = a.inv.slot(s); it && it->item) wpn += float(gd_.items[it->item].base_damage(it->level));
    wpn = float(bonus(a, WEAPON_DAMAGE) + 100) * 0.01f * wpn;
    const float t = (wpn - float(gd_.c.wp_damg)) + 100.0f;
    float atk = (float(stat_str(a) * gd_.c.str_dam) + t) * dmg / 100.0f;
    atk = float(bonus(a, WEAPON_DAMAGE_MULTIPLIER) + 100) * 0.01f * atk;
    if (hostile_to_player(a)) atk *= gd_.c.diff_d[std::clamp(difficulty, 0, 3)];
    return atk;
}

float World::apply_melee_damage(Unit& d, Unit& a, int def_ignore) {
    const Consts& c = gd_.c;
    float dmg;
    const bool fatal = rng().next(100) < bonus(a, FATAL_KILL);   // every unit has an inventory
    if (fatal) {
        dmg = float(d.hp);
    } else {
        const float p = melee_attack_power(a);
        const float ma = float(a.morale + 100 - c.mor_nor) / 100.0f;
        const float atk = attack_skill_mult(a) * ma * p;
        const float arm = float(d.armor_level * c.amr_def);
        const float def = float(d.morale + 100 - c.mor_nor) *
                          (float(stat_def(d)) * (100.0f - float(def_ignore)) * float(c.def_rat) / 100.0f + arm) / 100.0f;
        const float pen = std::max(0.0f, (atk - def) / 2.0f);
        const float taken = float((100 - bonus(d, REDUCE_MELEE_DAMAGE_SUSTAINED)) * (100 - bonus(d, DAMAGE_SUSTAINED))) * 0.0001f;
        dmg = taken * (atk / 2.0f + pen);
        if (a.id == player_) {
            if (a.crit_flag) dmg = dmg + dmg;
            dmg = float(pc_.chain) * dmg;
        }
    }
    if (const Unit* pl = unit(player_)) {   // friendly bonuses (game type 8 branch not modelled)
        if (a.nation == pl->nation) dmg *= float(c.f_attack) / 100.0f;
        if (d.nation == pl->nation) dmg *= float(c.f_defend) / 100.0f;
    }
    if (const Unit* l = unit(a.leader); l && l->id != a.id) dmg *= float(std::min(int(l->lead), 100) + 100) / 100.0f;
    set_hp(d, int(float(d.hp) - dmg));
    if (a.id == player_ && a.crit_flag) {
        ++a.crit_count;
        if (dead(d)) {
            emit(Ev::Crit, a.id, d.id, 0, "CRITKILL");
            ++a.crit_kills;
        }
    }
    return dmg;
}

// Melee_tryHit 0x5048b0 (this = d).
float World::try_melee_hit(Unit& d, Unit& a) {
    if (d.action == DIE || dead(d) || &a == &d) return 0;
    if ((d.action >= ENGATK && d.action <= ENGATK4) || d.action == RUSH || (d.action >= H_MNT_L && d.action <= H_DMNT_R)) return 0;
    const bool knocked = is_knocked_ext(d.action);
    if (knocked && a.action != DOWNATK && a.combo_idx > 0 && !a.combo.empty() && a.combo.back() == 'A') return 0;
    if (a.action == DOWNATK && !knocked) return 0;
    if (d.invuln_ms > 0 && a.action != DOWNATK) return 0;
    if (a.action == DOWNATK && d.down_attacked) return 0;
    if (!a.m.active) return 0;
    int reach = a.inv.melee[1].item ? gd_.items[a.inv.melee[1].item].range
                                    : a.inv.melee[0].item ? gd_.items[a.inv.melee[0].item].range : 0;   // quirk: no max
    reach = std::max(reach, 0);
    if (!hostile(d.nation, a.nation)) return 0;
    const AnimScript* s = script_of(move(a));
    if (!s) return 0;
    const float dx = d.pos.x - a.pos.x, dy = d.pos.y - a.pos.y, dz = d.pos.z - a.pos.z;
    const float r = float(s->range + reach);
    if (dx * dx + dy * dy + dz * dz > r * r) return 0;
    float ang = rel_angle(a, d.pos);
    const float lo = float(std::min(s->h0, s->h1)), hi = float(std::max(s->h0, s->h1));
    if (ang < lo || ang > hi) {
        if (ang >= 0) { ang -= 360; if (ang < lo) return 0; }
        else { ang += 360; if (ang > hi) return 0; }
    }
    const float ang2 = rel_angle(d, a.pos);
    const float ba = float(gd_.c.blockang);
    if ((d.action == GUARD1 || d.action == GUARDR) && -ba <= ang2 && ang2 <= ba) {
        emit(Ev::Blocked, d.id, a.id, 0, "DEFEND");
        emit(Ev::Effect, d.id, 0, 0, "GUARD");
        if (find_move(d, GUARDR) && (d.action != GUARDR || anim_remaining(d) == 0)) set_action(d, GUARDR);
        return 0;
    }
    int ignore;
    if (a.action == DOWNATK) ignore = random_range(rng(), 50, 100);
    else {
        const int q = std::abs(int(ang2));
        ignore = q <= 45 ? 0 : q <= 135 ? 20 : 40;
    }
    const float dmg = apply_melee_damage(d, a, ignore);
    if (dmg != 0 && d.ai_controlled) d.threat = std::int16_t(d.threat + 10);
    return dmg;
}

// Knockdown 0x509000.
void World::knockdown(Unit& u) {
    if (is_knocked(u.action)) return;
    const int uc = gd_.units[u.cls].unit_class;
    if (uc == 8 || uc == 9 || !gd_.moves(u.cls) || u.mount) return;
    const int n = 1 + (find_move(u, KNOCKDN2) ? 1 : 0) + (find_move(u, KNOCKDN3) ? 1 : 0);
    const int r = rng().next(n);
    if (set_action(u, KNOCKDN + r)) emit(Ev::Knockdown, u.id, 0, u.action, action_name(u.action));
    u.threat = std::int16_t(u.threat << 1);
}

namespace {
void retarget(World& w, Unit& d, Unit& a) {
    const Unit* dt = w.unit(d.target);
    if ((!dt || w.dead(*dt)) && !is_attacking(d.action) && !(d.action >= SHTLOAD && d.action <= SHTIDLEH)) d.target = a.id;
    const Unit* at = w.unit(a.target);
    if (!at || w.dead(*at)) a.target = d.id;
}
int flinch_for(float a) {
    if (a >= -45 && a <= 45) return ATTACKED;
    if (a > 45 && a < 135) return RATTKED;
    if (a >= -135 && a < -45) return LATTKED;
    return BATTKED;
}
}  // namespace

// Unit_onMessage 0x75d540 for TRIGGER_ATTACK / DOWN_ATTACK / ranged event types.
int World::on_attack_message(Unit& d, const Trigger& t) {
    const Consts& c = gd_.c;
    if (t.type == 0x4000 && t.src_kind == 2) {   // explosive keg (0x505650 -> 0x49d5f0): A = keg item damage
        if (d.action == DIE || dead(d) || d.invuln_ms > 0) return 0;
        const float atk = float(t.param);
        const float def = float(d.morale + 100 - c.mor_nor) * (float(stat_def(d)) * 100.0f * float(c.def_rat) / 100.0f +
                                                              float(d.armor_level * c.amr_def)) / 100.0f;
        float dmg = (atk / 2.0f + std::max(0.0f, (atk - def) / 2.0f)) * float(100 - bonus(d, DAMAGE_SUSTAINED)) * 0.01f;
        if (const Unit* pl = unit(player_); pl && d.nation == pl->nation) dmg *= float(c.f_defend) / 100.0f;
        set_hp(d, int(float(d.hp) - dmg));
        d.threat = std::int16_t(d.threat + 10);
        emit(Ev::Hit, d.id, 0, int(dmg), "KEG");
        if (!d.mount) knockdown(d);   // HYPOTHESIS: same reaction as explosive arrows
        if (dead(d)) unit_die(d, 0);
        return 1;
    }
    Unit* a = t.src_kind == 1 ? unit(t.sender) : nullptr;
    if (!a) return 0;
    const int special = gd_.units[d.cls].special;
    if (t.type == 0x400) {
        const float dmg = try_melee_hit(d, *a);
        if (dmg <= 0) return 0;
        emit(Ev::Hit, d.id, a->id, int(dmg), "DOWNATK");
        d.down_attacked = true;
        d.knock_ms = 0;
        set_action(d, DNATKED);
        if (a->id == player_) ++combo_hits, combo_hits_ts = now_;
        if (dead(d)) unit_die(d, a->id);
        return 2;
    }
    if (t.type == 1) {
        const int a_action = a->action;
        const float dmg = try_melee_hit(d, *a);
        if (dmg <= 0) return 0;
        emit(Ev::Hit, d.id, a->id, int(dmg), a->combo.empty() ? action_name(a_action) : a->combo);
        retarget(*this, d, *a);
        if (a->id == player_) ++combo_hits, combo_hits_ts = now_;
        if (!d.no_react) {
            if ((a_action >= ENGATK && a_action <= ENGATK4) || a_action == RUSH) {
                if (rng().next(2) == 1) knockdown(d);
            } else if (a_action == ATTACK || a_action == H_ATTACK) {
                const float ang = rel_angle(d, a->pos);
                set_action(d, flinch_for(ang));
                if (d.ai_controlled && d.action != DNATKED && special != 12 && special != 13) set_yaw(d, d.yaw_deg - ang);
            }
        }
        // Balance / knock-back (Part C §5.3).
        int h = 3000, v = 500;
        const AnimScript* s = t.p16 ? &gd_.scripts[t.p16] : script_of(move(*a));
        if (s) h = s->force_h, v = s->force_v;
        if (h + v < c.t_fallf && v < c.v_fallf) set_balance(d, d.balance - (h + v) / c.f_fallr);
        else set_balance(d, 0);
        if (!dead(d) && special != 12) {
            float k = d.action == H_DROP ? 2.0f : d.action == DNATKED ? 0.7f : 1.0f;
            const float dir = std::atan2(d.pos.y - a->pos.y, d.pos.x - a->pos.x);
            // HYPOTHESIS: force pulse (100 ms) -> velocity force/1000 m/s (unit body mass not recovered).
            const float f = k * kUnitsPerMetre / 1000.0f;
            d.vel.x += std::cos(dir) * float(h) * f;
            d.vel.y += std::sin(dir) * float(h) * f;
            if (v > 0) d.vel.z += float(v) * f, d.airborne = true;
        }
        if (dead(d)) unit_die(d, a->id);
        return 1;
    }
    if (t.type == 2 || t.type == 0x40 || t.type == 0x2000) {
        if ((t.type == 2 || t.type == 0x2000) && rng().next(100) < bonus(d, ARROW_DEFLECT_OFF)) return 0;
        const float dmg = try_ranged_hit(d, *a, t.type, t.type == 0x2000);
        if (dmg <= 0) return 0;
        emit(Ev::Hit, d.id, a->id, int(dmg), t.type == 2 ? "ARROW" : t.type == 0x40 ? "STONE" : "EXPLOSIVE");
        retarget(*this, d, *a);
        if (is_knocked(d.action)) {
            d.down_attacked = true;
            set_action(d, DNATKED);
        } else if ((t.type == 0x40 || t.type == 0x2000) && !d.mount) {
            knockdown(d);
        } else {
            const float ang = rel_angle(d, a->pos);
            set_action(d, flinch_for(ang));
            if (d.ai_controlled && special != 12 && special != 13) set_yaw(d, d.yaw_deg - ang);
        }
        if (special != 12) {
            float k = t.type == 0x2000 ? (d.action == H_DROP ? 3.0f : 1.0f) : (d.action == H_DROP ? 2.5f : d.action == DNATKED ? 0.7f : 1.0f);
            const float dir = std::atan2(d.pos.y - a->pos.y, d.pos.x - a->pos.x);
            const float f = k * kUnitsPerMetre / 1000.0f;
            d.vel.x += std::cos(dir) * 3000.0f * f, d.vel.y += std::sin(dir) * 3000.0f * f;
            d.vel.z += 500.0f * f, d.airborne = true;
        }
        if (t.type != 0x2000) set_balance(d, d.balance - c.arr_fall);
        if (dead(d)) unit_die(d, a->id);
        return 1;
    }
    return 0;
}

namespace {
int morale_w1(int rank) { return rank <= 2 ? 1 : rank == 3 ? 2 : rank <= 5 ? 3 : 0; }
int morale_w2(int rank, bool joan) { return rank <= 2 ? 1 : rank == 3 ? 2 : rank == 4 ? (joan ? 4 : 3) : 0; }
}  // namespace

// Unit_die 0x7c6240 (victim, killer recno; 0 = environment).
void World::unit_die(Unit& v, int killer) {
    const Consts& c = gd_.c;
    if (v.retreat == 1) {   // mission-critical: survive with 1 HP, defeat event
        v.invuln_ms = INT_MAX;
        v.hp = 1;
        v.retreat = -1;
        emit(Ev::Custom, v.id, killer, 7, "MISSION_EVENT_DEFEAT");
        return;
    }
    if (v.drop_item) {
        const int id = add_item(gd_.items[v.drop_item].code, v.drop_level, 1, v.pos);
        emit(Ev::ItemDrop, v.id, id, v.drop_item, gd_.items[v.drop_item].code);
        v.drop_item = 0;
    }
    if (Unit* k = unit(killer)) {
        set_ep(*k, k->ep + (v.rank < 3 ? c.ep_nor : v.rank == 3 ? c.ep_cap : c.ep_gen));
        k->medals += v.rank < 3 ? 1 : 10;
        ++k->kills;
        if (v.rank > 2) ++k->officer_kills;
        if (Unit* kl = unit(k->leader)) ++kl->troop_kills;
        if (Unit* vl = unit(v.leader)) ++vl->troop_lost;
        float m = 1.0f;
        if (k->crit_flag) m = c.critexp;
        if (k->level > v.level) m *= 1.0f / float(1 << std::min(30, k->level - v.level));
        if (k->id == player_ && game_mode != 4) {
            const int t = std::min(int(std::int16_t(combo_hits)) / c.c_bonusl, c.m_bonusl);
            m *= float(c.e_bonusl) * 0.01f * float(t) + 1.0f;
        }
        const float xv = float(gd_.units[v.cls].experience) * float(v.level) * m;
        add_xp(*k, int(xv > 10.0f ? xv : 10.0f), true, false);
        // Morale.
        const int kk = morale_w1(gd_.units[v.cls].ranking) + morale_w2(gd_.units[k->cls].ranking, gd_.units[k->cls].code == "JOAN_OF_ARC");
        float dlt = float(kk) * c.mor_ch_t;
        if (!(dlt > 1.0f)) dlt = 1.0f;
        for (Unit& u : units_) {
            if (u.deleted) continue;
            if (k->nation != 0 && u.nation == k->nation && k->team != 0 && u.team == k->team) set_morale(u, int(float(u.morale) + dlt));
            else if (v.nation != 0 && u.nation == v.nation && v.team != 0 && u.team == v.team) set_morale(u, int(float(u.morale) - dlt));
        }
    }
    if (v.action != THROWN) set_action(v, DIE);
    if (const Unit* k = unit(killer)) {   // death impulse: killer's script force
        const AnimScript* s = script_of(move(*k));
        const float h = float(s ? s->force_h : 3000), vv = float(s ? s->force_v : 500);
        const float f = (v.action == H_DROP ? 3.0f : 1.0f) * kUnitsPerMetre / 1000.0f;
        const float dir = std::atan2(v.pos.y - k->pos.y, v.pos.x - k->pos.x);
        v.vel.x += std::cos(dir) * h * f, v.vel.y += std::sin(dir) * h * f;
        if (vv > 0) v.vel.z += vv * f, v.airborne = true;
    }
    emit(Ev::Death, v.id, killer);
}

// Player_critCheck 0x416680 + sequence start 0x462970 + timeout 0x45fae0.
void World::crit_check(Unit& u) {
    if (crit.active && now_ - crit.start > gd_.c.cri_time) crit.active = false;
    if (game_mode == 4 || u.hp_trail - u.hp <= gd_.c.cri_diff) return;
    u.hp_trail = u.hp;
    const MoveTable* mt = gd_.moves(u.cls);
    if (!mt) return;
    std::vector<const MoveRecord*> list;
    bool any_free = false;
    for (const auto& r : mt->rows)
        if (r.show_crit) list.push_back(&r), any_free |= r.combo_avail == 'Y';
    if (!any_free) return;
    const MoveRecord* pick;
    do pick = list[size_t(rng().next(int(list.size())))];
    while (pick->combo_avail != 'Y');
    crit = {true, pick->combo, 0, now_};
    emit(Ev::Crit, u.id, 0, 0, "CRITSEQ:" + pick->combo);
}

// Crit_key 0x462810.
void World::crit_key(char key, bool reset) {
    Unit* p = unit(player_);
    if (!p) return;
    p->crit_flag = false;
    if (!crit.active) return;
    if (reset) crit.progress = 0;
    if (crit.progress < int(crit.keys.size()) && crit.keys[size_t(crit.progress)] == key) ++crit.progress;
    else crit.progress = 0;
    if (crit.progress == int(crit.keys.size())) {
        emit(Ev::Crit, p->id, 0, 0, "CRITATT");
        p->crit_flag = true;
        crit.active = false;
    }
}

}  // namespace joa::game
