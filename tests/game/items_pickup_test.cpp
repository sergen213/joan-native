// Unit_auto_pickup 0x49da30: a floor item is taken only by a general (rank > 3) of the player unit's nation
// (0x49da91: UnitArray_get(player) +2 == unit +2) with LOD tier <= 3 (+0x22). The port let enemy generals pick up too.
#include <cstdio>
#include <cstdlib>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    if (!run.init(&err) || !run.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    World& w = run.mission()->world();
    Unit* jean = nullptr;
    Unit* foe = nullptr;
    for (Unit& u : w.units()) {
        if (u.deleted || w.dead(u)) continue;
        if (gd->units[std::size_t(u.cls)].code == "JEAN_DE_METZ") jean = &u;
        else if (!foe && w.hostile_to_player(u)) foe = &u;
    }
    int fails = 0;
    if (!jean || !foe || !w.auto_pickup) return std::printf("FAIL setup\n"), 1;
    const auto lying = [&](int id) { const WorldItem* it = w.item(id); return it && !it->deleted; };

    foe->rank = 4, foe->lod_tier = 1;   // a general next to the player: only the nation refuses
    const int bread = w.add_item("fd_bread_s", 0, 1, foe->pos);
    w.auto_pickup(*foe);
    if (!lying(bread)) std::printf("FAIL an enemy general picked up a floor item\n"), ++fails;

    const int letter = w.add_item("flyletter01", 0, 1, jean->pos);
    jean->lod_tier = 4;   // farther than 8000 from the player
    w.auto_pickup(*jean);
    if (!lying(letter)) std::printf("FAIL picked up at LOD tier 4\n"), ++fails;
    jean->lod_tier = 1;
    w.auto_pickup(*jean);
    if (lying(letter)) std::printf("FAIL Jean (the player's nation) did not pick up the letter\n"), ++fails;

    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
