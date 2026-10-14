// F3 Ctrl_use_hp_item 0x43b490 through FUN_0050b4f0 with force 0: refused (and the food kept) at full HP;
// a team leader's potion also gives amount/2 to every live member; a member's surplus goes to its
// leader's most hurt members; red 0x15 is preferred over all-recovery 0x19.
#include <cstdio>
#include <cstdlib>

#include "game/world_sim.hpp"

using namespace joa::game;

static int fails = 0;
#define CHECK(c, ...) (void)((c) || (std::printf("FAIL " __VA_ARGS__), std::printf("\n"), ++fails))

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    std::string err;
    auto gd = dir ? GameData::load(dir, &err) : nullptr;
    if (!gd) return 77;
    const int bread = gd->item_index("fd_bread_s");
    if (!bread || gd->items[size_t(bread)].type != 0x15) return std::printf("FAIL fd_bread_s\n"), 1;
    const int amount = gd->items[size_t(bread)].damage;
    World w(*gd, nullptr);
    const int l = w.spawn_unit("JOAN_OF_ARC", 2, {0, 0, 0}, 0), m1 = w.spawn_unit("FRENCH_LIGHT_INFANTRY", 2, {50, 0, 0}, 0),
              m2 = w.spawn_unit("FRENCH_LIGHT_INFANTRY", 2, {90, 0, 0}, 0);
    Unit &L = *w.unit(l), &A = *w.unit(m1), &B = *w.unit(m2);
    L.members = {m1, m2}, A.leader = B.leader = l;

    L.inv.bag = {{bread, 2, 1}};
    CHECK(!w.use_recovery_item(L, false) && L.inv.bag.size() == 1 && L.inv.bag[0].quantity == 2, "F3 at full HP must keep the food");

    w.set_hp(L, 1), w.set_hp(A, 1), w.set_hp(B, w.max_hp(B) - 1);
    CHECK(w.use_recovery_item(L, false) && L.inv.bag[0].quantity == 1, "F3 when hurt uses one");
    CHECK(L.hp == std::min(w.max_hp(L), 1 + amount), "leader healed %d", L.hp);
    CHECK(A.hp == std::min(w.max_hp(A), 1 + amount / 2) && B.hp == w.max_hp(B), "members get amount/2: %d %d", A.hp, B.hp);

    // Member: the surplus over its own max HP goes to the most hurt member of the leader's team.
    w.set_hp(A, w.max_hp(A) - 1), w.set_hp(B, 1);
    A.inv.bag = {{bread, 1, 1}};
    CHECK(w.use_recovery_item(A, false) && A.hp == w.max_hp(A), "member healed");
    CHECK(B.hp == std::min(w.max_hp(B), 1 + amount - 1), "surplus to the hurt member: %d", B.hp);

    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
