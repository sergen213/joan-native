// XP / levels (Part A §4): LEVEL.csv thresholds, multi-level LEVELUP loop, kill XP formula,
// set-level + AI auto-upgrade from the unit's UPGRADE table, manual attribute allocation.
#include "sim_fixture.hpp"

using namespace joa::game;

int main() {
    auto gd = load_data_or_skip();
    CHECK(gd->levels.size() == 30, "LEVEL rows %zu", gd->levels.size());
    if (gd->levels.size() == 30) {
        CHECK(gd->levels[0].exp == 1000 && gd->levels[0].att == 5 && gd->levels[0].ski == 1, "row 1");
        CHECK(gd->levels[3].exp == 10400 && gd->levels[3].att == 6, "row 4");
        CHECK(gd->levels[29].exp == 4065260 && gd->levels[29].att == 12 && gd->levels[29].ski == 2, "row 30");
    }
    FlatTerrain flat;
    {
        World w(*gd, &flat, 0);
        const int j = w.spawn_unit("JOAN_OF_ARC", gd->nation_id("FRENCH"), {0, 0, 0}, 0);
        w.set_player(j);
        Unit& joan = *w.unit(j);
        CHECK(joan.level == 1 && joan.xp == 0 && joan.hp == 60 && joan.ep == 30, "Joan init lvl %d xp %d hp %d ep %d", joan.level, joan.xp, joan.hp, joan.ep);
        CHECK(joan.str == 9 && joan.def == 8 && joan.dex == 10 && joan.lead == 15, "Joan stats");
        joan.hp = 10;
        w.add_xp(joan, 999, false, false);
        CHECK(joan.level == 1, "level up below threshold");
        w.add_xp(joan, 1, false, false);   // 1000 = LEVEL[1].EXPERIENCE
        CHECK(joan.level == 2 && joan.attr_points == 5 && joan.skill_points == 1, "L2 pts %d %d", joan.attr_points, joan.skill_points);
        CHECK(joan.hp == 60 && joan.level_arrow, "level-up full heal %d", joan.hp);
        CHECK(w.command_points == 1000, "command points %d", w.command_points);
        // LEVELUP unlocks the rows for the level being left: JOANUP "1 NEW_COMBO_BB".
        bool bb = false;
        for (const auto& e : joan.upgrades) bb |= gd->upitems[size_t(e.upitem)].code == "NEW_COMBO_BB" && e.unlocked == 1 && e.applied == 0;
        CHECK(bb, "BB not unlocked at level 2");
        // 4800 more -> 5800 total: 2 -> 3 (3200) -> 4 (5800) in one call.
        w.add_xp(joan, 4800, false, false);
        CHECK(joan.level == 4 && joan.attr_points == 15 && joan.skill_points == 3, "multi level %d pts %d %d", joan.level, joan.attr_points, joan.skill_points);
        // Manual allocation 0x50d640: HP +HP_PER(4) heals the delta; EP +EP_PER(3); undo restores.
        CHECK(w.allocate_point(joan, 6, false) && w.max_hp(joan) == 64 && joan.hp == 64 && joan.attr_points == 14, "HP point");
        CHECK(w.allocate_point(joan, 7, false) && w.max_ep(joan) == 33, "EP point");
        CHECK(w.allocate_point(joan, 7, true) && w.max_ep(joan) == 30 && joan.attr_points == 14, "EP undo");
        CHECK(!w.allocate_point(joan, 4, false), "charisma allowed");
    }
    {   // Kill XP: Experience(50) * victim level * m, m = 1/2^(kLvl - vLvl), min 10; EP_NOR +6.
        auto kill_xp = [&](int joan_level) {
            World w(*gd, &flat, 0);
            const int j = w.spawn_unit("JOAN_OF_ARC", gd->nation_id("FRENCH"), {0, 0, 0}, 0, joan_level);
            const int e = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", gd->nation_id("ENGLISH"), {50, 0, 0}, 180);
            w.set_player(j);
            Unit& joan = *w.unit(j);
            joan.ep = 0;
            const int xp0 = joan.xp;
            w.unit(e)->hp = 0;
            w.unit_die(*w.unit(e), j);
            CHECK(joan.ep == 6 && joan.kills == 1 && joan.medals == 1, "kill counters ep %d", joan.ep);
            CHECK(w.unit(e)->action == DIE, "victim not dying");
            return joan.xp - xp0;
        };
        CHECK(kill_xp(0) == 50, "kill xp same level %d", kill_xp(0));
        CHECK(kill_xp(3) == 12, "kill xp lvl 3 %d", kill_xp(3));     // 50/4 = 12.5 -> 12
        CHECK(kill_xp(4) == 10, "kill xp lvl 4 %d", kill_xp(4));     // 6.25 -> minimum 10
    }
    {   // Spawn at level 3: set-level then AI auto-upgrade (E_INFUP: HP+5, STR+1, DEF+1 at L1 and L2).
        World w(*gd, &flat, 0);
        Unit& e = *w.unit(w.spawn_unit("ENGLISH_LIGHT_INFANTRY", gd->nation_id("ENGLISH"), {0, 0, 0}, 0, 3));
        CHECK(e.level == 3 && e.xp == 3200, "level %d xp %d", e.level, e.xp);
        CHECK(w.max_hp(e) == 75 && e.hp == 75 && e.str == 6 && e.def == 6, "upgrades hp %d str %d def %d", w.max_hp(e), e.str, e.def);
        CHECK(e.attr_points == 4 && e.skill_points == 0, "points left %d %d", e.attr_points, e.skill_points);
    }
    return finish();
}
