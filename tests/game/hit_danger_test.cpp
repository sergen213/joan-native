// Unit_onMessage 0x75d540 calls Unit_is_in_danger 0x4fd980(1) after every hit, before its death check: a non-player
// hero below INDANGER eats the bag food closest to his missing HP, even when the hit took him to 0 (0x50b4f0's
// allow-dead argument), so a lethal blow on an AI hero carrying food leaves him alive. The player's hero never eats
// this way (F3 only). Mission 1 start: Jean de Metz (AI), a spawned English captain and Joan (player), explosive-keg
// hits (item trigger 0x4000).
#include <cstdio>
#include <cstdlib>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

static int fails = 0;
#define CHECK(c, ...) (void)((c) || (std::printf("FAIL " __VA_ARGS__), std::printf("\n"), ++fails))

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
    const int bread = gd->item_index("fd_bread_s");
    Unit* jean = nullptr;
    for (Unit& u : w.units())
        if (u.name == "Jean de Metz") jean = &u;
    if (!jean || !bread) return std::printf("FAIL setup\n"), 1;
    auto keg = [&](Unit& u) {
        Trigger t;
        t.type = 0x4000, t.target_kind = 1, t.target_id = u.id, t.src_kind = 2, t.param = 100000, t.pos = u.pos, t.radius = 100;
        t.start = t.end = w.now();
        u.lives = 0, u.invuln_ms = 0;
        w.post_trigger(t);
        w.tick({});
    };

    // Jean withdraws at 1 HP instead of dying; below INDANGER he then eats.
    jean->inv.bag = {{bread, 1, 1}};
    keg(*jean);
    CHECK(jean->hp == std::min(w.max_hp(*jean), 1 + gd->items[size_t(bread)].damage) && jean->inv.bag.empty(), "AI hero ate: hp %d", jean->hp);
    // An English captain (rank 3) the hit kills outright eats on the way down and lives; without food he dies. His
    // soldier eats only from the captain's troop supply (bag grid 2), never from his personal food.
    const int en = gd->nation_id("ENGLISH");
    const Vec3 at{jean->pos.x + 3000, jean->pos.y, jean->pos.z};
    const int cid = w.spawn_unit("ENGLISH_LIGHT_INFANTRY_CAPTAIN", en, at, 0), sid = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", en, {at.x + 200, at.y, at.z}, 0);
    Unit &cap = *w.unit(cid), &sol = *w.unit(sid);
    sol.leader = cid;
    cap.inv.bag = {{bread, 1, 1}};
    keg(sol);
    CHECK(w.dead(sol) && cap.inv.bag.size() == 1, "member ate the leader's personal food: hp %d", sol.hp);
    keg(cap);
    CHECK(!w.dead(cap) && cap.hp == std::min(w.max_hp(cap), gd->items[size_t(bread)].damage) && cap.inv.bag.empty(), "lethal hit, ate: hp %d", cap.hp);
    keg(cap);
    CHECK(w.dead(cap), "no food left: dead, hp %d", cap.hp);
    const int sid2 = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", en, {at.x - 200, at.y, at.z}, 0);
    Unit& cap2 = *w.unit(cid);   // the spawn may have moved the unit array
    Unit& sol2 = *w.unit(sid2);
    sol2.leader = cid;
    ItemInstance troop{bread, 1, 1};
    troop.bag = 2;
    cap2.inv.bag = {troop};
    keg(sol2);
    CHECK(!w.dead(sol2) && cap2.inv.bag.empty(), "member ate the leader's troop supply: hp %d", sol2.hp);

    Unit& pl = *w.unit(w.player_id());   // the spawn may have moved the unit array
    pl.inv.bag = {{bread, 1, 1}};
    keg(pl);
    CHECK(w.dead(pl) && pl.inv.bag.size() == 1, "the player does not eat: hp %d bag %zu", pl.hp, pl.inv.bag.size());

    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
