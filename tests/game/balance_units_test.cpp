// Balance audit: English vs French light infantry (game.ai_battle ends ~20 English dead vs 1 French).
// 1. GameData / Unit_init values == raw unit/unit.res + item/item.res text (independent line parse).
// 2. Per-hit melee damage both ways (0x5104f0 / 0x509950, GAMEPLAY_CHARACTER_COMBAT.md Part C §4)
//    against hand-computed values.
// 3. Effective reach (animscr Range + weapon Range) and a traced 20v20: the outcome is original data
//    (French AA reach 132 vs English 87 inside the AI's 60..120 approach band), not a port bug.
#include <cmath>
#include <cstring>


#include "ai_fixture.hpp"
#include "formats/res_archive.hpp"
#include "game/ai_system.hpp"
#include "game/world_sim.hpp"

using namespace joa::game;

namespace {
// "Key : value" lookup in a raw res record (first match, key compared case-sensitively as written).
std::string raw(const joa::formats::ResArchive& res, const char* rec, const char* key) {
    const std::string t(res.text(rec).value_or(""));
    for (std::size_t p = 0; p < t.size();) {
        std::size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(p, e - p);
        p = e + 1;
        const std::size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k != key) continue;
        std::string v = line.substr(c + 1);
        const std::size_t b = v.find_first_not_of(" \t"), z = v.find_last_not_of(" \t\r");
        return b == std::string::npos ? std::string() : v.substr(b, z - b + 1);
    }
    return {};
}
int ri(const joa::formats::ResArchive& r, const char* rec, const char* key) { return std::atoi(raw(r, rec, key).c_str()); }

struct Trace { int dead[3] = {}, hits[3] = {}, beyond_100[3] = {}; float max_hit_dist[3] = {}, dmg[3] = {}; };

Trace battle(fixture::Env& env, float cx, float cy) {
    World w(*env.gd, env.terrain.get(), 1234);
    CharPhysics phys(env.nav.get());
    w.set_physics(&phys);
    AiSystem ai(w, env.nav.get());
    w.set_hostile(3, 1, false), w.set_hostile(3, 2, false);
    w.set_player(w.spawn_unit("ENGLISH_LIGHT_INFANTRY", 3, {cx, cy + 600, 0}, 270));
    const char* code[3][2] = {{}, {"ENGLISH_LIGHT_INFANTRY_CAPTAIN", "ENGLISH_LIGHT_INFANTRY"},
                              {"FRENCH_LIGHT_INFANTRY_CAPTAIN", "FRENCH_LIGHT_INFANTRY"}};
    for (int side = 1; side <= 2; ++side) {   // same layout as game.ai_battle
        const float x0 = side == 1 ? cx - 450 : cx + 450, yaw = side == 1 ? 0.0f : 180.0f, dir = side == 1 ? -1.0f : 1.0f;
        for (int q = 0; q < 2; ++q) {
            const float y0 = cy - 100.0f + 200.0f * float(q);
            const int L = w.spawn_unit(code[side][0], side, {x0, y0, 0}, yaw);
            ai.set_unit_team(*w.unit(L), side * 100 + q);
            for (int i = 0; i < 9; ++i) {
                const float fx = 60.0f + 50.0f * float(i / 3), fy = -50.0f + 50.0f * float(i % 3);
                const int m = w.spawn_unit(code[side][1], side, {x0 + dir * fx, y0 + fy, 0}, yaw);
                ai.set_unit_team(*w.unit(m), side * 100 + q);
            }
        }
    }
    Trace r;
    InputFrame in{};
    for (int t = 0; t < 3000; ++t) {
        w.tick(in);
        for (const GameEvent& e : w.take_events()) {
            const Unit* d = w.unit(e.unit);
            const Unit* a = w.unit(e.other);
            if (e.type != Ev::Hit || !a || !d || a->nation < 1 || a->nation > 2) continue;
            const float dx = d->pos.x - a->pos.x, dy = d->pos.y - a->pos.y, dist = std::sqrt(dx * dx + dy * dy);
            ++r.hits[a->nation], r.dmg[a->nation] += float(e.value);
            r.max_hit_dist[a->nation] = std::max(r.max_hit_dist[a->nation], dist);
            if (dist > 100.0f) ++r.beyond_100[a->nation];
        }
    }
    for (const Unit& u : w.units())
        if (u.hp < 1 && u.nation >= 1 && u.nation <= 2) ++r.dead[u.nation];
    return r;
}
}  // namespace

int main() {
    auto env = fixture::load();
    if (!env) return 77;
    GameData& gd = *env->gd;
    const std::filesystem::path dir = std::getenv("JOA_GAME_DIR");
    auto ures = joa::formats::ResArchive::open(dir / "unit/unit.res");
    auto ires = joa::formats::ResArchive::open(dir / "item/item.res");
    CHECK(ures && ires, "unit.res / item.res");

    // ---- 1. stats: raw text == GameData == spawned Unit (Unit_init 0x4f8550) -----------------------
    struct Want { const char* code; int hp, ep, str, dex, def, lead, rank; const char* weapon; int wdmg, winc, wrange; };
    const Want want[] = {   // literal raw-file values (unit.res / item.res)
        {"ENGLISH_LIGHT_INFANTRY", 65, 20, 4, 1, 4, 0, 0, "ENG_L_INF_SWORD", 40, 5, 45},
        {"ENGLISH_LIGHT_INFANTRY_CAPTAIN", 100, 10, 5, 2, 5, 4, 3, "ENG_L_INF_CAP_SWORD", 40, 5, 45},
        {"FRENCH_LIGHT_INFANTRY", 32, 10, 4, 5, 4, 0, 0, "f_lisword", 40, 10, 45},
        {"FRENCH_LIGHT_INFANTRY_CAPTAIN", 50, 20, 5, 5, 5, 4, 3, "f_lisword_c", 40, 10, 45},
    };
    World w(gd, env->terrain.get(), 1);
    AiSystem ai(w, env->nav.get());
    int id[4] = {};
    for (int k = 0; k < 4; ++k) {
        const Want& x = want[k];
        const int cls = gd.unit_index(x.code);
        CHECK(cls >= 0, x.code);
        const UnitClass& c = gd.units[std::size_t(cls)];
        CHECK(ri(*ures, x.code, "HP") == x.hp && c.hp == x.hp, std::string(x.code) + " HP");
        CHECK(ri(*ures, x.code, "EP") == x.ep && c.ep == x.ep, std::string(x.code) + " EP");
        CHECK(ri(*ures, x.code, "Strength") == x.str && c.str == x.str, std::string(x.code) + " STR");
        CHECK(ri(*ures, x.code, "Dexterity") == x.dex && c.dex == x.dex, std::string(x.code) + " DEX");
        CHECK(ri(*ures, x.code, "Defense") == x.def && c.def == x.def, std::string(x.code) + " DEF");
        CHECK(ri(*ures, x.code, "Leadership") == x.lead && c.lead == x.lead, std::string(x.code) + " LEAD");
        CHECK(c.level == ri(*ures, x.code, "Level") && c.level == 1, "Level 1");
        CHECK(c.morale == ri(*ures, x.code, "Morale") && c.morale == 70, "Morale 70");
        CHECK(c.falling == ri(*ures, x.code, "Falling") && c.falling == 20, "Falling 20");
        CHECK(c.earmor_id == ri(*ures, x.code, "EArmorId") && c.earmor_id == 1, "EArmorId 1");
        CHECK(c.script_duration == ri(*ures, x.code, "ScriptDuration") && c.script_duration == 500, "ScriptDuration 500");
        CHECK(c.walk == float(std::atof(raw(*ures, x.code, "WalkSpeed").c_str())) && c.walk == 3.5f, "WalkSpeed 3.5");
        CHECK(c.run == float(std::atof(raw(*ures, x.code, "RunSpeed").c_str())) && c.run == 6.0f, "RunSpeed 6");
        CHECK(c.unit_class == 1 && raw(*ures, x.code, "UnitClass") == "LIGHT_INFANTRY", "LIGHT_INFANTRY");
        CHECK(c.ranking == x.rank, std::string(x.code) + " Ranking");
        CHECK(raw(*ures, x.code, "PWeaponId") == x.weapon && c.pweapon == gd.item_index(x.weapon) && c.pweapon > 0,
              std::string(x.code) + " PWeaponId (case-insensitive item lookup)");
        CHECK(c.sweapon == 0, "no SWeaponId");
        const ItemDef& it = gd.items[std::size_t(c.pweapon)];
        const char* irec = gd.items[std::size_t(c.pweapon)].code.c_str();
        std::string rec(irec);
        for (char& ch : rec) ch = char(std::toupper((unsigned char)ch));   // res record names are upper case
        CHECK(ri(*ires, rec.c_str(), "Damage") == x.wdmg && it.damage == x.wdmg, rec + " Damage");
        CHECK(ri(*ires, rec.c_str(), "IncDamage") == x.winc && it.inc_damage == x.winc, rec + " IncDamage");
        CHECK(ri(*ires, rec.c_str(), "Range") == x.wrange && it.range == x.wrange, rec + " Range");
        CHECK(ri(*ires, rec.c_str(), "MinLevel") == 1 && it.min_level == 1, rec + " MinLevel");

        id[k] = w.spawn_unit(x.code, k < 2 ? 1 : 2, {0, 0, 0}, 0);
        Unit& u = *w.unit(id[k]);
        ai.st(u);   // AI init (ensure_init): ai_class from ScriptFile, threat = base attack chance 0x511e70
        CHECK(u.hp == x.hp && w.max_hp(u) == x.hp, std::string(x.code) + " spawned HP");
        CHECK(u.str == x.str && u.dex == x.dex && u.def == x.def && u.lead == x.lead && u.armor_level == 1 && u.morale == 70,
              std::string(x.code) + " spawned stats");
        CHECK(u.inv.melee[0].item == c.pweapon && u.inv.melee[0].level == 1 && u.inv.melee[1].item == 0,
              std::string(x.code) + " weapon slot 0 at MinLevel");
        CHECK(u.ai_class == (x.rank == 3 ? 2 : 1), std::string(x.code) + " AI class (light_infantry 1 / light_general 2)");
        CHECK(ai.attack_chance(u) == 50 && ai.block_chance(u) == 50, std::string(x.code) + " aggression symmetric");
    }
    // English soldiers also carry BOW + ARROW (melee mode 1 kept: PWeaponId set first in 0x4f8550).
    CHECK(gd.units[std::size_t(gd.unit_index("ENGLISH_LIGHT_INFANTRY"))].prange == gd.item_index("BOW"), "English bow");
    CHECK(w.unit(id[0])->inv.weapon_mode == 1, "English soldier weapon mode stays melee");

    // ---- 2. per-hit damage, attacker -> defender, no leader / player / items (morale 70 both) --------
    // P = (STR*STR_DAM 3 + wpn 40 + 100) * DMG / 100; atk = skill 1.0 * morale 1.2 * P, skill = (lvl 1 + 10)/11:
    // every combo used is a level-0 NEW_COMBO row of UpgradeList ENGLISH_INFANTRY(_CAPTAIN) (E_INFUP/E_INFUPC),
    // which the French classes use too; def = 1.2 * (DEF*0 (DEF_RAT atoi 0) + armor 1);
    // dmg = atk/2 + max(0,(atk-def)/2) = 1.2P - 0.6.
    auto hand = [](int str, int dmgcol) {
        const float p = (float(str * 3) + 140.0f) * float(dmgcol) / 100.0f;
        const float atk = 1.2f * p, def = 1.2f;
        return atk / 2.0f + std::max(0.0f, (atk - def) / 2.0f);
    };
    struct Hit { int a, d; const char* combo; int dmgcol; float lit; };
    const Hit hits[] = {   // DAMAGE column of STD.SET ELINFANT / ELINFCAP / FLINFANT / FLINFCAP
        {0, 2, "A", 3, 4.872f}, {0, 2, "AA", 4, 6.696f},  {2, 0, "A", 2, 3.048f},    {2, 0, "AA", 4, 6.696f},
        {1, 3, "A", 3, 4.98f},  {1, 3, "AA", 4, 6.84f},   {1, 3, "AAA", 4, 6.84f},   {1, 3, "AAB", 6, 10.56f},
        {3, 1, "A", 4, 6.84f},  {3, 1, "AA", 8, 14.28f},  {3, 1, "AAA", 12, 21.72f}, {3, 1, "AAB", 20, 36.6f},
    };
    for (const Hit& h : hits) {
        Unit& a = *w.unit(id[h.a]);
        Unit& d = *w.unit(id[h.d]);
        w.stop(a);
        for (const char* p = h.combo; *p; ++p) CHECK(w.set_action(a, ATTACK, *p), std::string("set_action ") + h.combo);
        CHECK(w.move(a) && w.move(a)->combo == h.combo && w.move(a)->damage == h.dmgcol, std::string("DAMAGE column ") + h.combo);
        CHECK(w.attack_skill_mult(a) == 1.0f, "level-0 NEW_COMBO upgrade -> skill multiplier 1.0");
        d.hp = w.max_hp(d);
        const float got = w.apply_melee_damage(d, a, 0);
        const float exp = hand(want[h.a].str, h.dmgcol);
        std::printf("%-30s %-3s -> %-30s dmg %.4f (hand %.4f)\n", want[h.a].code, h.combo, want[h.d].code, got, exp);
        CHECK(std::fabs(got - exp) < 1e-4f && std::fabs(got - h.lit) < 1e-3f, std::string("per-hit damage ") + h.combo);
    }
    // HP = ftol(HP - dmg) => a 6.696 AA hit costs 7 HP: an English soldier (65) dies to 10 French AA hits, a
    // French soldier (32) to 5 English AA hits -> per exchange the English soldier is 2x stronger.

    // ---- 3. reach = animscr Range + weapon Range (trigger radius 0x502a8b / hit test 0x5048b0) ------
    struct Reach { int k; const char* combo; int r; };
    const Reach reach[] = {{0, "A", 83}, {0, "AA", 87}, {2, "A", 74}, {2, "AA", 132},
                           {1, "AAB", 94}, {3, "AA", 132}, {3, "AAB", 132}};
    for (const Reach& x : reach) {
        const int cls = gd.unit_index(want[x.k].code);
        const MoveRecord* m = nullptr;
        for (const MoveRecord& r : gd.moves(cls)->rows)
            if (r.movement == ATTACK && r.combo == x.combo) { m = &r; break; }
        CHECK(m && m->script >= 0, "attack script");
        const int got = gd.scripts[std::size_t(m->script)].range + gd.items[std::size_t(gd.units[std::size_t(cls)].pweapon)].range;
        CHECK(got == x.r, std::string(want[x.k].code) + " reach " + x.combo);
    }

    // ---- traced 20v20 (game.ai_battle layout): where the hits land --------------------------------------
    float cx = 0, cy = 0;
    CHECK(fixture::open_area(*env->nav, 1200, 600, cx, cy), "open area");
    const Trace t = battle(*env, cx, cy);
    std::printf("20v20: dead english %d french %d; hits eng %d (max dist %.0f, >100: %d) fr %d (max dist %.0f, >100: %d); "
                "dmg eng %.0f fr %.0f\n", t.dead[1], t.dead[2], t.hits[1], t.max_hit_dist[1], t.beyond_100[1], t.hits[2],
                t.max_hit_dist[2], t.beyond_100[2], t.dmg[1], t.dmg[2]);
    // English reach tops out at 94 (captain AAB; soldiers 83/87), French AA/AAB reach 132.
    CHECK(t.beyond_100[1] * 10 <= t.beyond_100[2] && t.beyond_100[2] > t.hits[2] / 4, "French hits land beyond English reach");
    return 0;
}
