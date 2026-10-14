// Ranged combat and siege projectiles: base damage 0x510bf0, hit 0x5051e0, damage 0x509e90, spread
// 0x512130, ThrowItem spawn 0x4ea700/0x507fc0, launch 0x5133e0, aim solver 0x496d40, integrator
// 0x6a8060, hit posting 0x514c40 / ground 0x4fcc00, manager 0x4913c0. GAMEPLAY_CHARACTER_COMBAT.md Part D.
#include <algorithm>
#include <cmath>

#include "game/world_sim.hpp"

namespace joa::game {

namespace {
float len3(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 norm(Vec3 a) { const float l = len3(a); return l > 0 ? scale(a, 1.0f / l) : Vec3{1, 0, 0}; }

int proj_type_for(int item_type) {
    switch (item_type) {
    case 0xd: case 0x10: return 0;
    case 0xe: case 0x11: return 1;
    case 0xf: case 0x12: return 2;
    case 0x14: return 4;
    case 0x13: return 5;
    default: return -1;
    }
}

// 0x496d40: returns M * v(m/s) * 1000 (a 1 ms force pulse that sets v exactly).
Vec3 solve_aim(Vec3 p0, Vec3 t, float m, float mxd, float av, float gl, float min_sin) {
    float d = len3(sub(t, p0));
    if (d > mxd) {
        t = {p0.x + (t.x - p0.x) / d * mxd, p0.y + (t.y - p0.y) / d * mxd, p0.z + (t.z - p0.z) / d * mxd};
        d = mxd;
    }
    auto vel = [&](float tt) {
        return Vec3{((t.x - p0.x) / 46.1f) / tt, ((t.y - p0.y) / 46.1f) / tt, (gl * 9.81f * tt * tt / 2 + (t.z - p0.z) / 46.1f) / tt};
    };
    Vec3 v = vel(av > 0 ? d / av : 1.0f);
    const float dh = std::sqrt((t.x - p0.x) * (t.x - p0.x) + (t.y - p0.y) * (t.y - p0.y));
    const float min_s = dh >= 800 ? min_sin : dh * 1.5f / 800 - 1.5f;
    float tt = 0.1f;
    for (int guard = 0; norm(v).z < min_s && guard < 500; ++guard) {
        tt += 0.2f;
        v = vel(tt);
    }
    return scale(v, m * 1000.0f);
}
}  // namespace

float World::ranged_base_damage(const Unit& a) const {
    float w = 0;
    bool has_ammo = false;
    if (a.inv.ranged.item) w = float(gd_.items[a.inv.ranged.item].base_damage(a.inv.ranged.level));
    if (a.inv.ammo.item) w += float(gd_.items[a.inv.ammo.item].base_damage(a.inv.ammo.level)), has_ammo = true;
    w = float(bonus(a, WEAPON_DAMAGE) + 100) * 0.01f * w;
    const float div = has_ammo ? 200.0f : 100.0f;
    float r = float(stat_dex(a) * gd_.c.dex_dam + 100) * w / div;   // DEX_DAM is atoi("1.5") = 1
    r = float(bonus(a, WEAPON_DAMAGE_MULTIPLIER) + 100) * 0.01f * r;
    if (hostile_to_player(a)) r *= gd_.c.diff_d[std::clamp(difficulty, 0, 3)];
    return r;
}

float World::apply_ranged_damage(Unit& d, Unit& a, int type, int p) {
    const Consts& c = gd_.c;
    float atk = ranged_base_damage(a);
    atk = float(a.morale + 100 - c.mor_nor) * atk / 100.0f;
    atk = (float(skill_level(a, 0xe, {})) + 10.0f) / 11.0f * atk;
    const float arm = float(d.armor_level * c.amr_def);
    const float def = float(d.morale + 100 - c.mor_nor) * (float(stat_def(d)) * float(100 - p) * float(c.def_rat) / 100.0f + arm) / 100.0f;
    float dmg = atk / 2.0f + std::max(0.0f, (atk - def) / 2.0f);
    dmg *= float(100 - bonus(d, DAMAGE_SUSTAINED)) * 0.01f;
    if (gd_.units[d.cls].unit_class != 8) {
        const int dist = int(len3(sub(d.pos, a.pos)));
        if (dist >= c.fall_e) dmg *= c.dist_p;
        else if (dist > c.fall_s) dmg *= 1.0f - float(dist - c.fall_s) * c.dist_p / float(c.fall_e - c.fall_s);
    }
    if (d.armor_level == 3 && (type == 0x40 || type == 0x2000)) dmg *= c.amrexdef;
    if (type == 0x40) dmg *= c.sie_udam;
    if (const Unit* pl = unit(player_)) {
        if (a.nation == pl->nation) dmg *= float(c.f_attack) / 100.0f;
        if (d.nation == pl->nation) dmg *= float(c.f_defend) / 100.0f;
    }
    if (const Unit* l = unit(a.leader); l && l->id != a.id) dmg *= float(std::min(l->lead, 100) + 100) / 100.0f;
    set_hp(d, int(float(d.hp) - dmg));
    return dmg;
}

float World::try_ranged_hit(Unit& d, Unit& a, int type, bool ignore_down) {
    if (d.action == DIE || dead(d) || &d == &a) return 0;
    if ((d.action >= ENGATK && d.action <= ENGATK4) || d.action == RUSH) return 0;
    if (!ignore_down && is_knocked_ext(d.action)) return 0;
    if (d.invuln_ms > 0 || !a.m.active || !hostile(d.nation, a.nation)) return 0;
    const float ang = rel_angle(d, a.pos);
    const float ba = float(gd_.c.blockang);
    if ((d.action == GUARD1 || d.action == GUARDR) && -ba <= ang && ang <= ba) {
        emit(Ev::Blocked, d.id, a.id, 0, "GUARDARR");
        set_action(d, GUARDR);
        return 0;
    }
    const int q = std::abs(int(ang));
    const float dmg = apply_ranged_damage(d, a, type, q <= 45 ? 0 : q <= 135 ? 20 : 40);
    if (dmg != 0 && d.ai_controlled) d.threat = std::int16_t(d.threat + 5);
    return dmg;
}

bool World::ai_shoot(Unit& u, int target) {
    Unit* t = unit(target);
    if (!t || !can_act(u) || !u.inv.ranged.item || !find_move(u, SHTLOAD)) return false;
    u.target = u.range_target = target;
    u.aim_dir = norm(sub(t->pos, u.pos));
    return set_action(u, SHTLOAD);
}

int World::spawn_projectile(Unit& s) {
    const UnitClass& cls = gd_.units[s.cls];
    const int ammo = s.inv.ammo.item ? s.inv.ammo.item : cls.srange;
    if (!ammo) return 0;
    const int type = proj_type_for(gd_.items[ammo].type);
    if (type < 0) return 0;
    const ThrowConsts& tc = gd_.throwi[type];
    Projectile p;
    p.id = next_projectile_++;
    p.type = type;
    p.shooter = s.id;
    p.tl = tc.tl;
    p.dl = type == 0 ? bonus(s, RANGE_DISTANCE) * tc.dl / 100 : tc.dl;   // integer imul/idiv
    p.m = tc.m, p.tr = tc.tr, p.gl = tc.gl;
    p.launch_ms = now_;
    // Launch point: model bone origin (HYPOTHESIS: 3/4 of the class cylinder height above the feet).
    const Vec3 p0{s.pos.x, s.pos.y, s.pos.z + cls.cyl_height * 0.75f};
    p.launch = {p0.x, p0.y, 0.0f};   // +0x20 overwritten with 0 (CONFIRMED quirk)
    p.pos_m = scale(p0, 1.0f / kUnitsPerMetre);
    const Unit* tgt = unit(s.range_target ? s.range_target : (s.ai_controlled ? s.target : 0));
    if (s.ai_controlled && tgt) {   // 0x50aca0 AI aim
        s.aim_dir = norm(sub(tgt->pos, s.pos));
        if (cls.special == 4 || cls.special == 5) s.aim_dir.z += 0.15f;
    }
    Vec3 f;
    float dur = 1.0f;
    if (type == 0) {
        f = scale(norm(s.aim_dir), tc.fc);
        dur = 100.0f;
    } else if (tgt) {
        Vec3 t = tgt->pos;
        t.z += float(gd_.units[tgt->cls].height) * 0.8f;
        if ((type == 1 || type == 2) && len3(sub(t, s.pos)) > 300 && s.id != player_ && cls.unit_class != 8) {
            auto spread = [&] { const int v = rng().next(gd_.c.arr_accu); return rng().next(2) != 0 ? -v : v; };
            t.x += float(spread());
            t.y += float(spread());
        }
        f = solve_aim(p0, t, tc.m, tc.mxd, tc.av, tc.gl, type <= 2 ? 0.25f : 0.1f);
    } else {
        Vec3 dir = s.aim_dir;
        if (type == 1 || type == 2) dir.z += 0.1f;
        f = scale(dir, tc.fc);
    }
    p.force = f;
    p.force_start = now_;
    p.force_left_ms = dur;
    if (s.id == player_ && s.inv.ammo.item && --s.inv.ammo.quantity < 1) s.inv.ammo = {};
    if (cls.special != 12) {
        const Vec3 hv = f;
        if (hv.x != 0 || hv.y != 0) set_yaw(s, std::atan2(hv.y, hv.x) / 0.017453292f);
    }
    projectiles_.push_back(p);
    emit(Ev::ProjectileSpawn, s.id, p.id, type, gd_.items[ammo].code);
    return p.id;
}

// ThrowItem manager 0x4913c0 (+ ThrowItem_update 0x4fcc00 ground explosions).
void World::process_projectiles() {
    for (Projectile& p : projectiles_) {
        if (p.destroyed) continue;
        const float dx = p.pos_m.x * kUnitsPerMetre - p.launch.x, dy = p.pos_m.y * kUnitsPerMetre - p.launch.y;
        if (float(p.dl) < std::sqrt(dx * dx + dy * dy)) { p.destroyed = true; continue; }
        if (p.hit_id == 0 && (p.collided || p.launch_ms + p.tl < now_)) { p.destroyed = true; continue; }
        if (p.ground && !p.exploded) projectile_ground(p);
        if (p.hit_id || p.ground) p.destroyed = true;
    }
    std::erase_if(projectiles_, [](const Projectile& p) { return p.destroyed; });
}

void World::projectile_ground(Projectile& p) {
    p.exploded = true;
    Trigger t;
    t.src_kind = 1;
    t.sender = p.shooter;
    t.pos = scale(p.pos_m, kUnitsPerMetre);
    t.radius = 150.0f;   // hard-coded ground blast radius
    t.start = t.end = now_;
    if (p.type == 2) t.type = 0x2000, emit(Ev::Sound, 0, p.id, 0, "EXPLOSIVEHIT");
    else if (p.type == 3 || p.type == 4) t.type = 0x40, emit(Ev::Sound, 0, p.id, 0, "CANNONHL");
    else return;
    triggers_.push_back(t);
    t.target_kind = 2;
    triggers_.push_back(t);
}

void World::projectile_hit_unit(Projectile& p, Unit& v) {
    p.collided = true;
    const Vec3 hit = scale(p.pos_m, kUnitsPerMetre);
    if (!(hit.z < float(gd_.units[v.cls].height) + v.pos.z)) return;   // above the head: no event
    p.hit_id = v.id;
    Trigger t;
    t.sender = p.shooter;
    t.start = t.end = now_;
    t.radius = p.tr;
    t.param = p.type;
    if (p.type <= 1) t.type = 2, t.target_id = v.id, t.pos = v.pos;
    else {
        t.type = p.type == 2 ? 0x2000 : 0x40;
        t.pos = hit;
        emit(Ev::Sound, v.id, p.id, 0, p.type == 2 ? "EXPLOSIVEHIT" : "CANNONHL");
    }
    triggers_.push_back(t);
    if (p.type >= 2) t.target_kind = 2, triggers_.push_back(t);
    emit(Ev::ProjectileHit, v.id, p.id, p.type);
}

// 0x6ad660 -> 0x6a8060: fixed sub-steps, trapezoid integration, force pulse, swept unit test, terrain.
void World::integrate_projectiles(float dt_ms) {
    for (Projectile& p : projectiles_) {
        if (p.destroyed || p.collided || (p.type != 0 && p.ground)) continue;
        const int max_steps = p.type == 0 ? 2 : 5;
        float h = 0.001f;
        int n = int(dt_ms);
        if (dt_ms > float(max_steps)) h = std::min(dt_ms * 0.001f / float(max_steps), 1.0f), n = max_steps;
        const Unit* shooter = unit(p.shooter);
        for (int i = 0; i < n && !p.collided && !p.ground; ++i) {
            const Vec3 old = p.pos_m;
            float rest = h;
            while (rest > 0) {
                float hh = rest;
                Vec3 a{0, 0, 0};
                if (p.force_left_ms > 0.00015f && now_ >= p.force_start) {
                    if (p.force_left_ms < hh * 1000) hh = p.force_left_ms / 1000;
                    a = scale(p.force, 1.0f / p.m);
                    p.force_left_ms -= hh * 1000;
                }
                a.z += -p.gl * 9.81f;
                const Vec3 nv{p.vel_m.x + a.x * hh, p.vel_m.y + a.y * hh, p.vel_m.z + a.z * hh};
                p.pos_m = {p.pos_m.x + 0.5f * (p.vel_m.x + nv.x) * hh, p.pos_m.y + 0.5f * (p.vel_m.y + nv.y) * hh,
                           p.pos_m.z + 0.5f * (p.vel_m.z + nv.z) * hh};
                p.vel_m = nv;
                rest -= hh;
            }
            // Swept segment vs unit cylinders (shape forced to cylinder for throw items).
            const Vec3 s0 = scale(old, kUnitsPerMetre), s1 = scale(p.pos_m, kUnitsPerMetre);
            for (Unit& v : units_) {
                if (v.deleted || v.id == p.shooter) continue;
                if (shooter && !hostile(shooter->nation, v.nation)) continue;
                if (is_knocked(v.action) || v.action == STANDUP || v.action == DNATKED || v.action == H_DROP) continue;
                const UnitClass& vc = gd_.units[v.cls];
                const Vec3 d = sub(s1, s0);
                const float dd = d.x * d.x + d.y * d.y;
                float u = dd > 0 ? ((v.pos.x - s0.x) * d.x + (v.pos.y - s0.y) * d.y) / dd : 0.0f;
                u = std::clamp(u, 0.0f, 1.0f);
                const Vec3 c{s0.x + d.x * u, s0.y + d.y * u, s0.z + d.z * u};
                const float rx = c.x - v.pos.x, ry = c.y - v.pos.y;
                if (rx * rx + ry * ry > vc.cyl_radius * vc.cyl_radius || c.z < v.pos.z || c.z > v.pos.z + vc.cyl_height) continue;
                p.pos_m = scale(c, 1.0f / kUnitsPerMetre);
                projectile_hit_unit(p, v);
                break;
            }
            if (p.collided) break;
            if (terrain_ && terrain_->height(s1.x, s1.y) > s1.z) p.ground = true;
        }
    }
}

}  // namespace joa::game
