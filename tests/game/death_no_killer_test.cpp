// Unit_dieNoKiller 0x64f320 (World::unit_die with killer 0) against Unit_die 0x7c6240: morale of the
// victim's nation only (same troop -= W1*MOR_CH_T, everyone else of the nation -= W1*MOR_CH_U, no min 1;
// W1 0x527dd0: INFANTRY 1, CAPTAIN 2), no EP / XP / medals / kills / troop counters, death push 3000/500
// at angle 0 (+x). The explosive-keg hit (0x75d540 case 0x4000 from an item) dispatches to it.
// Troop members (+0x31 != 0) without MORALE_BASE items never lose morale (SetMorale 0x4fe8c0 quirk).
#include "sim_fixture.hpp"

using namespace joa::game;

namespace {
struct Scene {
    FlatTerrain flat{0};
    World w;
    int joan, victim, loner, mate, other;
    Scene(GameData& gd, const char* victim_cls, int victim_team) : w(gd, &flat, 0) {
        const int fr = gd.nation_id("FRENCH"), en = gd.nation_id("ENGLISH");
        joan = w.spawn_unit("JOAN_OF_ARC", fr, {0, 0, 0}, 0);
        victim = w.spawn_unit(victim_cls, en, {400, 0, 0}, 180);
        loner = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", en, {400, 300, 0}, 180);   // no troop
        mate = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", en, {400, -300, 0}, 180);   // victim's troop
        other = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", en, {700, 0, 0}, 180);     // another troop
        w.set_player(joan);
        for (Unit& u : w.units()) u.morale = 70;
        V().team = victim_team;
        V().leader = mate;
        M().team = victim_team;
        w.unit(other)->team = 77;
    }
    Unit& J() { return *w.unit(joan); }
    Unit& V() { return *w.unit(victim); }
    Unit& L() { return *w.unit(loner); }
    Unit& M() { return *w.unit(mate); }
    Unit& O() { return *w.unit(other); }
    int deaths(int killer) {
        int n = 0;
        for (const auto& e : w.take_events()) n += e.type == Ev::Death && e.unit == victim && e.other == killer;
        return n;
    }
};
}  // namespace

int main() {
    auto gd = load_data_or_skip();
    const int cap = gd->unit_index("ENGLISH_LIGHT_INFANTRY_CAPTAIN"), inf = gd->unit_index("ENGLISH_LIGHT_INFANTRY");
    CHECK(cap >= 0 && gd->units[size_t(cap)].ranking == 3, "captain ranking");
    CHECK(inf >= 0 && gd->units[size_t(inf)].ranking == 0, "infantry ranking");
    CHECK(gd->c.mor_ch_u == 1.0f && NEAR(gd->c.mor_ch_t, 0.3, 1e-6), "MOR_CH_U %f MOR_CH_T %f", gd->c.mor_ch_u, gd->c.mor_ch_t);
    const float push = 3000.0f * World::kUnitsPerMetre / 1000.0f, lift = 500.0f * World::kUnitsPerMetre / 1000.0f;

    {   // 1. Killer-less captain (W1 2) in troop 5.
        Scene s(*gd, "ENGLISH_LIGHT_INFANTRY_CAPTAIN", 5);
        const Unit j0 = s.J();
        const Vec3 v0 = s.V().vel;
        s.w.take_events();
        s.w.set_hp(s.V(), 0);
        s.w.unit_die(s.V(), 0);
        CHECK(s.deaths(0) == 1, "Death event with killer 0");
        CHECK(s.L().morale == 68, "no-troop English 70 - 2*MOR_CH_U -> %d", s.L().morale);
        CHECK(s.M().morale == 70, "troop mate (MOR_CH_T path, SetMorale troop immunity) %d", s.M().morale);
        CHECK(s.O().morale == 70, "other troop (MOR_CH_U path, troop immunity) %d", s.O().morale);
        CHECK(s.J().morale == 70, "other nation untouched %d", s.J().morale);
        CHECK(s.J().xp == j0.xp && s.J().ep == j0.ep && s.J().level == j0.level && s.J().medals == j0.medals &&
                  s.J().kills == j0.kills && s.J().officer_kills == j0.officer_kills && s.J().troop_kills == j0.troop_kills,
              "no rewards to anyone");
        CHECK(s.M().troop_lost == 0 && s.M().troop_kills == 0, "leader counters %d %d", s.M().troop_lost, s.M().troop_kills);
        CHECK(s.V().action == DIE, "victim DIE %d", s.V().action);
        CHECK(NEAR(s.V().vel.x - v0.x, push, 1e-3) && NEAR(s.V().vel.y - v0.y, 0, 1e-4) && NEAR(s.V().vel.z - v0.z, lift, 1e-3),
              "push %f %f %f", s.V().vel.x - v0.x, s.V().vel.y - v0.y, s.V().vel.z - v0.z);
    }
    {   // 2. Same death with Joan as the killer (0x7c6240): rewards, troop-only morale, push away from Joan.
        Scene s(*gd, "ENGLISH_LIGHT_INFANTRY_CAPTAIN", 5);
        s.w.take_events();
        s.w.set_hp(s.V(), 0);
        s.w.unit_die(s.V(), s.joan);
        CHECK(s.deaths(s.joan) == 1, "Death event with killer");
        CHECK(s.J().kills == 1 && s.J().medals == 10, "Joan kills %d medals %d", s.J().kills, s.J().medals);
        CHECK(s.M().troop_lost == 1, "victim leader troop_lost %d", s.M().troop_lost);
        CHECK(s.L().morale == 70, "killer case leaves other troops alone %d", s.L().morale);
    }
    {   // 3. Killer-less infantry (W1 1) without a troop: the whole nation -= 1 * MOR_CH_U.
        Scene s(*gd, "ENGLISH_LIGHT_INFANTRY", 0);
        s.M().team = 0;
        s.w.set_hp(s.V(), 0);
        s.w.unit_die(s.V(), 0);
        CHECK(s.L().morale == 69 && s.M().morale == 69, "nation -1: %d %d", s.L().morale, s.M().morale);
        CHECK(s.O().morale == 70, "troop member immune %d", s.O().morale);
        CHECK(s.J().morale == 70, "French untouched %d", s.J().morale);
    }
    {   // 4. Nation 0 victim: nobody's morale changes.
        Scene s(*gd, "ENGLISH_LIGHT_INFANTRY", 0);
        s.V().nation = 0;
        s.L().nation = 0;
        s.w.set_hp(s.V(), 0);
        s.w.unit_die(s.V(), 0);
        CHECK(s.L().morale == 70 && s.M().morale == 70, "nation 0: %d %d", s.L().morale, s.M().morale);
    }
    {   // 5. Explosive keg (item hit 0x4000) kills killer-less.
        Scene s(*gd, "ENGLISH_LIGHT_INFANTRY", 0);
        s.M().team = 0;
        const Unit j0 = s.J();
        Trigger t;
        t.type = 0x4000;
        t.target_kind = 1;
        t.target_id = s.victim;
        t.src_kind = 2;
        t.param = 100000;
        t.pos = s.V().pos;
        t.radius = 100;
        t.start = t.end = s.w.now();
        s.w.take_events();
        s.w.post_trigger(t);
        s.w.tick({});
        CHECK(s.w.dead(s.V()), "keg victim dead hp %d", s.V().hp);
        CHECK(s.deaths(0) == 1, "keg death without killer");
        CHECK(s.L().morale == 69, "keg death morale %d", s.L().morale);
        CHECK(s.J().xp == j0.xp && s.J().medals == j0.medals && s.J().kills == j0.kills, "keg gives no rewards");
    }
    return finish();
}
