// AI_energyAttack 0x4bca60 (order ENERGY_ATTACK): the EGATK_MP / EGATK_HP price is paid unless the unit is hostile
// to the player (Unit_isHostileToPlayer 0x5094d0). The player's enemies attack for free, so a retreat-flagged boss at
// low HP never kills itself with it (mission 2: William de la Pole at 116 HP went to 0 HP without Unit_die, was deleted
// and his "defeat" objective never finished). The player's allies pay.
#include "game/ai_system.hpp"
#include "sim_fixture.hpp"

using namespace joa::game;

namespace {
// Runs one ENERGY_ATTACK order for u; returns {hp lost, ep lost}, or {-1, -1} if the attack never started.
std::pair<int, int> energy_attack(World& w, AiSystem& ai, int id) {
    Unit& u = *w.unit(id);
    const int hp0 = u.hp, ep0 = u.ep;
    ai.give_order(u, order::ENERGY_ATTACK, 5, false, 0, 0, 0, 0, 5000);
    for (int i = 0; i < 5 && u.action != ENGATK; ++i) ai.execute_orders(w, u);
    if (u.action != ENGATK) return {-1, -1};
    return {hp0 - u.hp, ep0 - u.ep};
}
}  // namespace

int main() {
    auto gd = load_data_or_skip();
    FlatTerrain flat{0};
    World w(*gd, &flat, 0);
    AiSystem ai(w, nullptr);
    const int fr = gd->nation_id("FRENCH"), en = gd->nation_id("ENGLISH");
    const int joan = w.spawn_unit("JOAN_OF_ARC", fr, {0, 0, 0}, 0);
    const int boss = w.spawn_unit("William_de_la_pole", en, {2000, 0, 0}, 180);
    const int ally = w.spawn_unit("JEAN_DE_METZ", fr, {0, 2000, 0}, 0);
    w.set_player(joan);
    CHECK(boss > 0 && ally > 0, "spawned boss %d ally %d", boss, ally);
    if (boss <= 0 || ally <= 0) return finish();

    Unit& b = *w.unit(boss);
    const int cost_hp = int(float(w.max_hp(b)) * float(gd->c.egatk_hp) / 100.0f);
    CHECK(cost_hp > 0, "EGATK_HP price %d", cost_hp);
    b.retreat = 1;
    w.set_hp(b, cost_hp);   // at the price: paying would leave 0 HP
    const auto [bhp, bep] = energy_attack(w, ai, boss);
    CHECK(bhp == 0 && bep == 0, "hostile boss attacks for free: lost hp %d ep %d", bhp, bep);
    CHECK(b.hp == cost_hp && !b.deleted && b.retreat == 1, "boss alive at %d HP, retreat %d", b.hp, b.retreat);

    Unit& a = *w.unit(ally);
    w.set_ep(a, w.max_ep(a));
    const int ally_hp = int(float(w.max_hp(a)) * float(gd->c.egatk_hp) / 100.0f);
    const auto [ahp, aep] = energy_attack(w, ai, ally);
    CHECK(ahp == ally_hp && aep > 0, "the player's ally pays: lost hp %d (price %d) ep %d", ahp, ally_hp, aep);
    return finish();
}
