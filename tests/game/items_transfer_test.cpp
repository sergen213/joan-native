// Hero-to-hero item hand-over (character window FUN_006d8ac0 0x6db54a + Unit_add_item 0x495f40): World::transfer_item
// rules on mission 1 (Joan, Jean): only to a general (rank >= 4) that is controllable, not task-locked and inside the
// minimap view; empty melee / ammo slots are filled first, stacks merge, the bags are first-fit grids (no room =
// refused, nothing moves); a hero can hand an item to himself (auto-placement).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
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
    Unit& joan = *w.unit(w.player_id());
    Unit* jean = nullptr;
    Unit* soldier = nullptr;
    for (Unit& u : w.units()) {
        if (u.deleted || w.dead(u)) continue;
        if (gd->units[std::size_t(u.cls)].code == "JEAN_DE_METZ") jean = &u;
        else if (!soldier && u.rank < 4 && u.nation == joan.nation) soldier = &u;
    }
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) std::printf("FAIL %s\n", what), ++fails;
    };
    // First item of an ITEMTYPE usable by anyone.
    auto find = [&](int type) {
        for (std::size_t i = 1; i < gd->items.size(); ++i)
            if (gd->items[i].type == type && gd->items[i].usable_unit < 0) return gd->items[i].code;
        return std::string();
    };
    const std::string explosive = "ARROW_EXPLOSIVE", sword = find(9);
    if (!jean || !gd->item_index(explosive) || sword.empty()) return std::printf("FAIL setup\n"), 1;
    const int explosive_id = gd->item_index(explosive), sword_id = gd->item_index(sword);
    joan.m.control = jean->m.control = 0, joan.no_react = jean->no_react = false;   // opening cut scene
    auto count = [&](const Unit& u, int id) {
        int n = u.inv.ammo.item == id ? u.inv.ammo.quantity : 0;
        for (const ItemInstance& it : u.inv.bag) n += it.item == id ? it.quantity : 0;
        return n;
    };

    // Mission 2 walkthrough: Joan's explosive arrows to Jean. All of them arrive (ammo slot / stacks), none stay.
    joan.inv.bag.clear(), jean->inv.bag.clear();
    check(w.give_item(joan, explosive, 0, 20), "give explosive arrows");
    const int before = count(*jean, explosive_id);
    check(w.transfer_item(joan, joan.inv.ammo.item == explosive_id ? 3 : 10, 0, *jean), "explosive arrows to Jean");
    check(count(*jean, explosive_id) == before + 20 && count(joan, explosive_id) == 0, "arrows moved intact");

    // Empty melee slots are filled first (Inventory_set_melee_slot).
    jean->inv.melee[0] = {}, jean->inv.melee[1] = {};
    check(w.give_item(joan, sword, 0, 1), "give sword");
    check(w.transfer_item(joan, 10, int(joan.inv.bag.size()) - 1, *jean), "sword to Jean");
    check(jean->inv.melee[0].item == sword_id && jean->inv.weapon_mode == 1, "sword equipped on Jean");

    // Equipment slot as source: Jean hands the sword back; Joan's melee is taken, so it lands in a bag cell.
    check(joan.inv.melee[0].item != 0, "Joan has a sword");
    check(w.transfer_item(*jean, 0, -1, joan), "sword back to Joan");
    check(!jean->inv.melee[0].item && !joan.inv.bag.empty() && joan.inv.bag.back().item == sword_id &&
              joan.inv.bag.back().bag == 1, "sword in Joan's personal bag");

    // Rules: not to a soldier; not to (or from) a controlled / task-locked hero; not outside the minimap view.
    const int last = int(joan.inv.bag.size()) - 1;
    if (soldier) check(!w.transfer_item(joan, 10, last, *soldier), "refused to a soldier (rank < 4)");
    jean->m.control = 1;
    check(!w.transfer_item(joan, 10, last, *jean), "refused to a controlled hero");
    jean->m.control = 0, jean->no_react = true;
    check(!w.transfer_item(joan, 10, last, *jean), "refused to a task-locked hero");
    jean->no_react = false;
    const Vec3 keep = jean->pos;
    jean->pos.x = joan.pos.x + 5000;
    check(!w.transfer_item(joan, 10, last, *jean, 0.03f, 0.03f), "refused outside the minimap view (150 px)");
    check(w.transfer_item(joan, 10, last, *jean, 0.01f, 0.01f) && count(joan, sword_id) == 0, "inside the view (50 px)");
    jean->pos = keep;

    // No room: fill Jean's personal and troop bags with swords, then one more is refused and stays with Joan.
    jean->inv.melee[0] = {jean->inv.melee[0].item ? jean->inv.melee[0].item : sword_id, 1, gd->items[std::size_t(sword_id)].min_level};
    for (int i = 0; i < 100; ++i) {
        jean->inv.bag.push_back({sword_id, 1, gd->items[std::size_t(sword_id)].min_level});
        jean->inv.layout(*gd);
        if (jean->inv.bag.back().bag < 0) { jean->inv.bag.pop_back(); break; }
    }
    check(w.give_item(joan, sword, 0, 1), "give another sword");
    const std::size_t jean_bag = jean->inv.bag.size(), joan_bag = joan.inv.bag.size();
    check(!w.transfer_item(joan, 10, int(joan_bag) - 1, *jean), "refused without room");
    check(jean->inv.bag.size() == jean_bag && joan.inv.bag.size() == joan_bag, "nothing moved");

    // To himself: the detached sword fills Joan's empty melee slot.
    joan.inv.melee[0] = {}, joan.inv.melee[1] = {};
    check(w.transfer_item(joan, 10, int(joan_bag) - 1, joan) && joan.inv.melee[0].item == sword_id &&
              joan.inv.bag.size() == joan_bag - 1, "self hand-over equips");

    // ---- Unit_add_item 0x495f40 for everyone (World::give_item), the shop (buy_item), the floor (drop_item) ----
    const std::string bread = "SMALLBLUE";   // a stacking (MaxQuantity 9) 1x1 POTION; food (fd_bread_s) is MaxQuantity 1
    const int bread_id = gd->item_index(bread), bread_max = std::max(1, gd->items[std::size_t(bread_id)].max_quantity);
    const int bread_level = gd->items[std::size_t(bread_id)].min_level;
    jean->inv = {};
    check(w.give_item(*jean, explosive, 0, 5) == 5 && jean->inv.ammo.item == explosive_id, "arrows fill an empty ammo slot");
    check(w.give_item(*jean, sword, 0, 1) == 1 && jean->inv.melee[0].item == sword_id, "a sword fills empty melee slots");
    jean->inv.melee[0] = {};
    check(w.buy_item(*jean, sword_id, 0, 1) && !jean->inv.melee[0].item && jean->inv.bag.size() == 1 &&
              jean->inv.bag[0].bag == 1, "a bought sword goes to the personal grid, not the hand");
    // Fill both bags with full potion stacks (after the bought sword): more is refused everywhere, a floor one stays.
    for (int i = 0; i < 100; ++i) {
        jean->inv.bag.push_back({bread_id, bread_max, bread_level});
        jean->inv.layout(*gd);
        if (jean->inv.bag.back().bag < 0) { jean->inv.bag.pop_back(); break; }
    }
    check(count(*jean, bread_id) > 0, "bags filled");
    check(w.give_item(*jean, bread, 0, 1) == 0, "give_item refused with full bags");
    check(!w.buy_item(*jean, bread_id, 0, 1), "buy refused with a full personal grid (BUY 9)");
    jean->lod_tier = 1, jean->m.active = true;
    w.set_hp(*jean, w.max_hp(*jean));   // a red potion would be eaten at once below full HP
    const int floor_bread = w.add_item(bread, 0, 5, jean->pos);
    if (w.auto_pickup) w.auto_pickup(*jean);
    check(w.item(floor_bread) && w.item(floor_bread)->quantity == 5, "full Jean leaves the floor bread");
    // Stack room 2 < 5 and no free cell: 0x495f40 refuses all of it.
    for (auto& it : jean->inv.bag)
        if (it.item == bread_id) { it.quantity = bread_max - 2; break; }
    w.auto_pickup(*jean);
    check(w.item(floor_bread) && w.item(floor_bread)->quantity == 5, "stack room short and no cell: refused");
    // One free cell + 2 stack room: max + 2 fit, the rest (3) stays on the floor.
    for (std::size_t i = jean->inv.bag.size(); i-- > 0;)
        if (jean->inv.bag[i].item == bread_id && jean->inv.bag[i].quantity == bread_max) {
            jean->inv.bag.erase(jean->inv.bag.begin() + std::ptrdiff_t(i));
            break;
        }
    const int had = count(*jean, bread_id);
    w.item(floor_bread)->quantity = bread_max + 5;
    w.auto_pickup(*jean);
    std::printf("partial: floor %d, Jean %d -> %d (max %d)\n", w.item(floor_bread) ? w.item(floor_bread)->quantity : -1, had,
                count(*jean, bread_id), bread_max);
    check(w.item(floor_bread) && w.item(floor_bread)->quantity == 3 && count(*jean, bread_id) == had + bread_max + 2,
          "partial pickup leaves the rest on the floor");
    w.del_item(floor_bread);
    // 0x512830: beside the unit, just outside its pickup reach (class radius + 15).
    const float radius = gd->units[std::size_t(jean->cls)].cyl_radius;
    const int dropped = w.drop_item(*jean, {bread_id, 1, bread_level});
    const WorldItem* di = w.item(dropped);
    check(di != nullptr, "drop_item found a floor");
    if (di) {
        const float d = std::hypot(di->pos.x - jean->pos.x, di->pos.y - jean->pos.y);
        std::printf("dropped %.1f from Jean (radius %.1f)\n", d, radius);
        check(d > radius + 15.0f && d < radius + 31.0f, "drop distance = radius + 30");
        w.auto_pickup(*jean);
        check(w.item(dropped) != nullptr, "not picked up again at once");
    }
    // Window drop (discard_item): CanDrop 0 items stay, others land beside the shown unit.
    joan.inv.bag.clear();
    joan.inv.bag.push_back({gd->item_index("flyletter01"), 1, 0});
    check(!gd->items[std::size_t(joan.inv.bag[0].item)].can_drop && !w.discard_item(joan, 10, 0, joan) &&
              joan.inv.bag.size() == 1, "a CanDrop 0 letter stays");
    joan.inv.bag[0] = {bread_id, 3, bread_level};
    const std::size_t floor_before = w.items().size();
    check(w.discard_item(joan, 10, 0, joan) && joan.inv.bag.empty() && w.items().size() == floor_before + 1 &&
              w.items().back().item == bread_id && w.items().back().quantity == 3, "a bread lands on the floor");

    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
