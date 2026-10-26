// Ranged combat and siege projectiles: base damage 0x510bf0, hit 0x5051e0, damage 0x509e90, spread
// 0x512130, ThrowItem spawn 0x4ea700/0x507fc0, launch 0x5133e0, aim solver 0x496d40, integrator
// 0x6a8060, hit posting 0x514c40 / ground 0x4fcc00, manager 0x4913c0. GAMEPLAY_CHARACTER_COMBAT.md Part D.
#include <algorithm>
#include <array>
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
}  // namespace

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

namespace {

// FUN_0069e720 (+ the 16-sample sign scan of 0x6a1460): first t in [0, 1] where the moving sphere (centre c + v t,
// radius r) touches the circle of radius R around k in the plane z = k.z: a root of
// (|P|^2 + R^2 - r^2)^2 - 4 R^2 (Px^2 + Py^2), P = c + v t - k. A sphere that does not move reports t 0 (the original's
// degenerate branch). ponytail: bisection instead of the original's Newton step; same root.
bool sweep_sphere_rim(Vec3 c, Vec3 v, Vec3 k, float R, float r, float& t) {
    if (v.x * v.x + v.y * v.y + v.z * v.z == 0) return t = 0, true;
    auto g = [&](float s) {
        const float px = c.x + v.x * s - k.x, py = c.y + v.y * s - k.y, pz = c.z + v.z * s - k.z;
        const float a = px * px + py * py + pz * pz + R * R - r * r;
        return a * a - 4 * R * R * (px * px + py * py);
    };
    float s0 = 0, g0 = g(0);
    for (int i = 1; i <= 16; ++i) {
        float s1 = float(i) / 16, g1 = g(s1);
        if (g1 == 0) return t = s1, true;
        if ((g0 < 0) == (g1 < 0)) { s0 = s1, g0 = g1; continue; }
        for (int j = 0; j < 24; ++j) {
            const float m = (s0 + s1) / 2;
            ((g(m) < 0) == (g0 < 0) ? s0 : s1) = m;
        }
        return t = (s0 + s1) / 2, true;
    }
    return false;
}
}  // namespace

// FUN_00699a20 (CONFIRMED): sphere (centre c, radius r) moving by v this frame vs a vertical cylinder (centre q,
// radius R, height h). The xy circles (r + R, 0x6a0d80) give the first contact t; a hit when the sphere centre's z is
// then within the cylinder's height (not widened by r). Otherwise the top, then the bottom cap: the sphere reaches the
// cap plane (0x69e970; already within r counts as t 0) with its centre inside the cap disc, or it touches the rim.
// So the hit volume is the cylinder grown by r sideways and r above/below the caps, rounded at the rims.
bool sweep_sphere_cylinder(Vec3 c, float r, Vec3 v, Vec3 q, float R, float h, float& t) {
    if (r == 0 || R == 0 || h == 0) return false;
    const float dx = q.x - c.x, dy = q.y - c.y, d2 = dx * dx + dy * dy, rr = r + R;
    if (std::sqrt(d2) <= rr) {
        t = 0;
    } else {
        const float vl = std::sqrt(v.x * v.x + v.y * v.y), proj = vl > 0 ? (v.x * dx + v.y * dy) / vl : 0;
        const float disc = rr * rr - (d2 - proj * proj);
        if (!(proj > 0) || disc < 0 || proj - std::sqrt(disc) > vl) return false;
        t = (proj - std::sqrt(disc)) / vl;
        if (t < 0 || t > 1) return false;
    }
    const float z = c.z + v.z * t;
    if (q.z - h / 2 <= z && z <= q.z + h / 2) return true;
    for (const float side : {1.0f, -1.0f}) {
        const float plane = q.z + side * h / 2, dist = side * (c.z - plane);
        if (dist < 0) continue;
        float tc = 0;
        if (dist > r) {
            if (!(side * v.z < 0)) continue;
            tc = (dist - r) / -(side * v.z);
            if (tc > 1) continue;
        }
        const float ex = c.x + v.x * tc - q.x, ey = c.y + v.y * tc - q.y;
        if (ex * ex + ey * ey <= R * R) return t = tc, true;
        if (sweep_sphere_rim(c, v, {q.x, q.y, plane}, R, r, t)) return true;
    }
    return false;
}

// FUN_006a0f90 / 0x6a09e0 / 0x6a0d80 (CONFIRMED decomp): the point c moving by v vs the sphere (q, R): t 0 when it
// starts inside, else the first contact within this frame's move.
static bool sweep_point_sphere(Vec3 c, Vec3 v, Vec3 q, float R, float& t) {
    const Vec3 d = sub(q, c);
    const float dl = len3(d), vl = len3(v);
    if (dl <= R) return t = 0, true;
    if (vl == 0) return false;
    const float proj = (v.x * d.x + v.y * d.y + v.z * d.z) / vl, disc = R * R - (dl * dl - proj * proj);
    if (!(proj > 0) || disc < 0 || proj - std::sqrt(disc) > vl) return false;
    t = (proj - std::sqrt(disc)) / vl;
    return true;
}

bool sweep_sphere_sphere(Vec3 c, float r, Vec3 v, Vec3 q, float R, float& t) {
    return r + R != 0 && sweep_point_sphere(c, v, q, r + R, t);
}

// FUN_00697140 (CONFIRMED decomp + asm): in the box's frame, the faces grown by r in the order -x +x -y +y -z +z
// (0x69e640: approaching, or exactly on the plane = t 0; the centre then within the face), then the twelve edges as
// radius-r cylinders (0x6a09e0, the edge's own axis ignored; contact within the edge) - z edges, y edges, x edges - then
// the eight corners (0x6a0f90). The first feature that reports a contact wins. Original quirk kept: the four
// vertical edges test the contact's x, not its z, against the box's z extent.
bool sweep_sphere_box(Vec3 c, float r, Vec3 v, Vec3 lo, Vec3 hi, float& t) {
    const float C[3]{c.x, c.y, c.z}, V[3]{v.x, v.y, v.z}, L[3]{lo.x, lo.y, lo.z}, H[3]{hi.x, hi.y, hi.z};
    auto within = [&](int a, int range, float tt) { const float x = C[a] + V[a] * tt; return L[range] <= x && x <= H[range]; };
    for (int a = 0; a < 3; ++a)
        for (const float s : {-1.0f, 1.0f}) {
            const float dist = s < 0 ? L[a] - r - C[a] : C[a] - H[a] - r, vn = s * V[a];
            float tt = 0;
            if (dist != 0) {
                if (dist < 0 || !(vn < 0) || -vn < dist) continue;
                tt = -dist / vn;
            }
            if (within((a + 1) % 3, (a + 1) % 3, tt) && within((a + 2) % 3, (a + 2) % 3, tt)) return t = tt, true;
        }
    if (r == 0) return false;
    auto edge = [&](int axis, float x, float y, float z, int check, int range) {
        float P[3]{x, y, z}, cc[3]{C[0], C[1], C[2]}, vv[3]{V[0], V[1], V[2]}, tt;
        P[axis] = cc[axis] = vv[axis] = 0;
        if (!sweep_point_sphere({cc[0], cc[1], cc[2]}, {vv[0], vv[1], vv[2]}, {P[0], P[1], P[2]}, r, tt) || !within(check, range, tt))
            return false;
        return t = tt, true;
    };
    if (edge(2, L[0], L[1], 0, 0, 2) || edge(2, H[0], L[1], 0, 0, 2) || edge(2, H[0], H[1], 0, 0, 2) || edge(2, L[0], H[1], 0, 0, 2) ||
        edge(1, L[0], 0, L[2], 1, 1) || edge(1, H[0], 0, L[2], 1, 1) || edge(1, H[0], 0, H[2], 1, 1) || edge(1, L[0], 0, H[2], 1, 1) ||
        edge(0, 0, L[1], L[2], 0, 0) || edge(0, 0, H[1], L[2], 0, 0) || edge(0, 0, H[1], H[2], 0, 0) || edge(0, 0, L[1], H[2], 0, 0))
        return true;
    bool hit = false;
    for (int k = 0; k < 8; ++k) {   // ponytail: earliest corner instead of the original's fixed order (same unless two are met)
        float tt;
        if (sweep_point_sphere(c, v, {k & 1 ? H[0] : L[0], k & 2 ? H[1] : L[1], k & 4 ? H[2] : L[2]}, r, tt) && (!hit || tt < t))
            t = tt, hit = true;
    }
    return hit;
}

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
    dmg = side_factors(dmg, a.nation, d.nation);
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
    // Siege weapons carry only their SRangeWeaponId ammo (CANNONBALL / stone), no launcher item.
    if (!t || !can_act(u) || (!u.inv.ranged.item && !u.inv.ammo.item) || !find_move(u, SHTLOAD)) return false;
    u.target = u.range_target = target;
    u.aim_dir = norm(sub(t->pos, u.pos));
    return set_action(u, SHTLOAD);
}

// Unit_updateAction 0x7c7ab0 cases SHTLOAD (0x30, type 1) / SHTLOAD2 (0x32, type 0): an AI shooter whose +0x2db target
// is not deleted re-solves its aim at the end of the draw, Aim_solve_by_type 0x4945c0 from the launch point to the
// target's feet (+0x0e). The normalised z then picks SHTIDLE (< 0.1) or SHTIDLEH. ai_shoot's feet-to-feet direction
// had z 0.15 for a target 40 units up the slope at 260 (mission-1 archer: SHTIDLEH; original SHTIDLE).
void World::ai_reaim(Unit& u, int type) {
    const Unit* t = unit(u.range_target);
    if (!t || t->deleted) return;
    const ThrowConsts& tc = gd_.throwi[type];
    const Vec3 p0 = launch_point(u, u.inv.ammo.item ? u.inv.ammo.item : gd_.units[u.cls].srange);
    u.aim_dir = norm(solve_aim(p0, t->pos, tc.m, tc.mxd, tc.av, tc.gl, type == 0 ? 0.0f : 0.25f));
}

// ThrowItem_launch 0x5133e0 (CONFIRMED asm): spawn point = world origin of the bone the ammo hangs from (item +0x9e
// attach id, else the ItemRes bone: AttachBoneName) in the shooter's current pose, + the caller's offset
// (Unit_updateAction 0x7c8775: AI holder of an item type 0xc crossbow (+0x1bf) adds world (5.337, 0, 7.5), unrotated;
// zero otherwise), + for unit-type special 4/5 (CANNON / FAUCONNEAU) actor row0 (forward) * 68.695 + row1 (left) * 12
// and z + 10.122 (the muzzle). Model x = forward, y = left. Without a skeleton track (unparsed .am3) the old
// HYPOTHESIS: 3/4 of the class cylinder height above the feet.
Vec3 World::launch_point(const Unit& s, int ammo) {
    const UnitClass& cls = gd_.units[s.cls];
    const float c = std::cos(s.yaw_rad()), sn = std::sin(s.yaw_rad());
    Vec3 p{s.pos.x, s.pos.y, s.pos.z + cls.cyl_height * 0.75f};
    std::array<float, 3> b;
    if (const MoveRecord* mv = move(s); mv && ammo > 0 && gd_.bone_origin(*mv, gd_.items[size_t(ammo)].attach_bone, float(now_ - s.anim_start), b))
        p = {s.pos.x + c * b[0] - sn * b[1], s.pos.y + sn * b[0] + c * b[1], s.pos.z + b[2]};
    if (s.id != player_ && s.inv.melee[1].item && gd_.items[size_t(s.inv.melee[1].item)].type == 0xc) p.x += 5.337f, p.z += 7.5f;
    if (cls.special == 4 || cls.special == 5) p = {p.x + c * 68.695f - sn * 12.0f, p.y + sn * 68.695f + c * 12.0f, p.z + 10.122f};
    return p;
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
    p.item = ammo;
    p.tl = tc.tl;
    p.dl = type == 0 ? bonus(s, RANGE_DISTANCE) * tc.dl / 100 : tc.dl;   // integer imul/idiv
    p.m = tc.m, p.tr = tc.tr, p.gl = tc.gl;
    p.launch_ms = now_;
    const Vec3 p0 = launch_point(s, ammo);
    p.launch = {p0.x, p0.y, 0.0f};   // +0x20 overwritten with 0 (CONFIRMED quirk)
    p.pos_m = scale(p0, 1.0f / kUnitsPerMetre);
    const Unit* tgt = unit(s.range_target ? s.range_target : (s.ai_controlled && !s.range_item ? s.target : 0));
    // +0x2dd map-item target (SHOOT order on an item); only without a unit target (0x50aca0, 0x5133e0).
    const StructureHit obj = !tgt && s.range_item && structure_ray ? structure_ray(s.range_item, {}, {}) : StructureHit{};
    if (s.ai_controlled && (tgt || obj.item)) {   // 0x50aca0 AI aim: the target, else the actor's forward
        s.aim_dir = norm(sub(tgt ? tgt->pos : obj.pos, s.pos));
    } else if (s.ai_controlled) {
        s.aim_dir = {std::cos(s.yaw_rad()), std::sin(s.yaw_rad()), 0.0f};
        if (cls.special == 4 || cls.special == 5) s.aim_dir = norm({s.aim_dir.x, s.aim_dir.y, 0.15f});
    }
    Vec3 f;
    float dur = 1.0f;
    auto spread = [&] { const int v = rng().next(gd_.c.arr_accu); return rng().next(2) != 0 ? -v : v; };
    const bool spreads = s.id != player_ && cls.unit_class != 8;   // Ranged_spread_offset 0x512130
    if (type == 0) {
        f = scale(norm(s.aim_dir), tc.fc);
        dur = 100.0f;
    } else if (tgt) {
        Vec3 t = tgt->pos;
        t.z += float(gd_.units[tgt->cls].height) * 0.8f;
        if ((type == 1 || type == 2) && len3(sub(t, s.pos)) > 300 && spreads) {
            t.x += float(spread());
            t.y += float(spread());
        }
        f = solve_aim(p0, t, tc.m, tc.mxd, tc.av, tc.gl, type <= 2 ? 0.25f : 0.1f);
    } else if (obj.item) {
        Vec3 t = obj.pos;
        t.z += type <= 2 ? obj.aim_h : obj.aim_h_siege;
        if (type <= 2 && len3(sub(t, s.pos)) > 300 && spreads) {
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

// Physics_sweep_structures 0x693750, mesh colliders (HIGH): a straight look-ahead ray of T ms
// (THROWI derived T/V0) from the substep start along (vx, vy, vz - V0), re-cast at most every T ms;
// the hit fires on the frame whose (now - dt, now] holds the predicted time, t = 1 - (now - hit)/dt.
// Kind 3 (STM) has no case in the T/V0 switch (stale locals in the original): never tested here.
// This pass's primitive branch (mesh format 7) and its zero-velocity mesh branch never stop a flying
// projectile; the items' CollisionType primitives are met in Physics_sweep_actors (item_sweep).
int World::sweep_structures(Projectile& p, Vec3 from, Vec3 v, float dt_ms, float& t) {
    if (!structure_ray || p.type == 3) return 0;
    const int dt = int(dt_ms), start = now_ - dt;
    if (p.sweep_item != 0 && start < p.sweep_hit && p.sweep_hit <= now_) {
        t = 1.0f - float(now_ - p.sweep_hit) / float(dt);
        return p.sweep_item;
    }
    if (now_ < p.sweep_next || (v.x == 0 && v.y == 0 && v.z == 0)) return 0;
    const ThrowConsts& tc = gd_.throwi[p.type];
    const int T = int(tc.ft);
    p.sweep_next = start + T;
    p.sweep_item = 0;
    const float k = float(T) * kUnitsPerMetre * 0.001f;
    const StructureHit h = structure_ray(0, from, {from.x + v.x * k, from.y + v.y * k, from.z + (v.z - tc.v0) * k});
    if (h.item && h.t <= 0.001f) {   // immediate hit; the prediction is not stored
        t = 0;
        return h.item;
    }
    p.sweep_hit = start + int(float(T) * (h.item ? h.t : 1.0f));
    p.sweep_item = h.item;
    if (p.sweep_item == 0 || now_ <= p.sweep_hit) return 0;
    t = 1.0f - float(now_ - p.sweep_hit) / float(dt);
    return p.sweep_item;
}

// ThrowItem_on_hit_object 0x4fbc90 (CONFIRMED): +0x26 = item; with a live shooter: kinds 0/1 ARROW
// (2) to the item at its position; kind 2 EXPLOSIVE_ARROW (0x2000) to the item, items around, units
// around; kinds 3-5 STONE (0x40) to the item, units around, items around; radius TR, then kill. A
// deleted item: nothing (the integrator keeps the projectile in place).
void World::projectile_hit_object(Projectile& p, int item) {
    const StructureHit h = structure_ray(item, {}, {});
    if (!h.item) return;
    p.hit_id = item;
    p.collided = true;
    const Unit* s = unit(p.shooter);
    if (!s || s->deleted) return;
    Trigger t;
    t.sender = p.shooter;
    t.start = t.end = now_;
    t.radius = p.tr;
    t.param = p.type;
    t.pos = h.pos;
    t.target_kind = 2;
    t.target_id = h.modify;
    t.type = p.type <= 1 ? 2 : p.type == 2 ? 0x2000 : 0x40;
    triggers_.push_back(t);
    if (p.type >= 2) {
        Trigger area = t;
        area.target_id = 0;
        area.target_kind = p.type == 2 ? 2 : 1;
        triggers_.push_back(area);
        area.target_kind = p.type == 2 ? 1 : 2;
        triggers_.push_back(area);
        // EXPLOSIVEHIT1..3 is a Misc_random(3) particle in the original; picked by the audio side here.
        emit(Ev::Sound, 0, p.id, 0, p.type == 2 ? "EXPLOSIVEHIT" : h.struction == 1 || h.struction == 2 ? "CANNONHH" : "CANNONHM");
    }
    emit(Ev::ProjectileHit, 0, p.id, p.type, "item");
}

// 0x6ad660 -> 0x6a8060 (CONFIRMED): all fixed sub-steps first (trapezoid integration, force pulse), then once per frame
// from the frame-start position along the frame's move: the swept actor test (Physics_sweep_actors 0x68de40), the
// structure sweep (0x693750) and, last, the terrain flag at the new position. The body hit wins when it is closer
// than the structure hit; either hit returns before the new state is stored (the body stays at the frame start, and
// ThrowItem_on_hit_actor 0x514c40 tests that position against the head height).
void World::integrate_projectiles(float dt_ms) {
    float max_cyl = 0;   // widest unit cylinder, for the grid query below
    if (!projectiles_.empty())
        for (const UnitClass& uc : gd_.units) max_cyl = std::max(max_cyl, std::fabs(uc.cyl_radius));
    grid_open(true);   // no unit moves during the sweep but in a hit (which reopens the grid)
    for (Projectile& p : projectiles_) {
        if (p.destroyed || p.collided || (p.type != 0 && p.ground)) continue;
        const int max_steps = p.type == 0 ? 2 : 5;
        float h = 0.001f;
        int n = int(dt_ms);
        if (dt_ms > float(max_steps)) h = std::min(dt_ms * 0.001f / float(max_steps), 1.0f), n = max_steps;
        const Vec3 f0 = p.pos_m, fv0 = p.vel_m;
        const float ff0 = p.force_left_ms;
        for (int i = 0; i < n; ++i) {
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
        }
        const Vec3 s0 = scale(f0, kUnitsPerMetre), s1 = scale(p.pos_m, kUnitsPerMetre), mv = sub(s1, s0);
        if (len3(mv) > 0) {
            // The projectile's sphere (0x41e850) vs each unit's cylinder (0x6a1bb0 -> 0x699a20); first hit in unit order,
            // then (Physics_sweep_actors' item pass) the map items' CollisionType primitives.
            const std::array<float, 4>& sp = gd_.item_sphere(p.item);
            const Vec3 c{s0.x + sp[0], s0.y + sp[1], s0.z + sp[2]};   // ponytail: centre offset unrotated (arrows: 0)
            const Unit* shooter = unit(p.shooter);
            Unit* hit = nullptr;
            float tu = 1;   // a missed cylinder's t is never read (every hit path writes it)
            auto test = [&](Unit& v) {
                if (v.deleted || v.id == p.shooter || v.m.climb) return false;   // climbers have no body (0x4ac910)
                if (shooter && !hostile(shooter->nation, v.nation)) return false;
                if (is_knocked(v.action) || v.action == STANDUP || v.action == DNATKED || v.action == H_DROP) return false;
                const UnitClass& vc = gd_.units[v.cls];
                if (!sweep_sphere_cylinder(c, sp[3], mv, {v.pos.x, v.pos.y, v.pos.z + vc.cyl_center_z}, vc.cyl_radius, vc.cyl_height, tu))
                    return false;
                hit = &v;
                return true;
            };
            // 0x699a20 hits only where the xy circles (|r| + |R|) meet along the move: units outside the move's xy box
            // grown by that reach are never hit, and the grid returns the rest in unit order.
            const float reach = std::fabs(sp[3]) + max_cyl;
            if (units_in(std::min(c.x, c.x + mv.x) - reach, std::min(c.y, c.y + mv.y) - reach, std::max(c.x, c.x + mv.x) + reach,
                         std::max(c.y, c.y + mv.y) + reach, hit_scan_)) {
                for (int i : hit_scan_)
                    if (test(units_[std::size_t(i)])) break;
            } else {
                for (Unit& v : units_)
                    if (test(v)) break;
            }
            const int body_item = !hit && item_sweep ? item_sweep(c, sp[3], mv, tu) : 0;
            float tw = 1;
            const int item = sweep_structures(p, s0, fv0, dt_ms, tw);
            if (hit || body_item || item) p.pos_m = f0, p.vel_m = fv0, p.force_left_ms = ff0;
            if ((hit || body_item) && (!item || tu < tw)) {
                // ThrowItem_on_hit_actor 0x514c40: a map item's body (actor +8 == 1) posts the events of 0x4fbc90.
                grid_open(false);   // hits move units: scans inside them see all units, the next sweep a new grid
                if (hit) projectile_hit_unit(p, *hit);
                else projectile_hit_object(p, body_item);
                grid_open(true);
                continue;
            }
            if (item) {
                grid_open(false);
                projectile_hit_object(p, item);
                grid_open(true);
                continue;
            }
        }
        if (terrain_ && terrain_->height(s1.x, s1.y) > s1.z) p.ground = true;
    }
    grid_open(false);
}

}  // namespace joa::game
