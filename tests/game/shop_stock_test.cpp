// Shop stock (f_shop.cpp / i_buy.cpp) on mission 2 (S3L1): the script's shop_set_sell_item_list copies the SELLITEM.RES
// record verbatim, purchases (game::shop_buy: the engine buy screen and the campaign bot alike) come off the facility's
// stock and stay sold between visits, the hourly restock of a POTION / can-regen shop adds back half the listed
// quantity, prices and sale values follow the exe's expressions, and the stock survives a native save round trip.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

static int fails = 0;
#define CHECK(c) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++fails; \
    } while (0)

static const ShopStock* entry(Mission& m, int fac, int item) {
    const Facility* f = m.places().facility(fac);
    for (const ShopStock& s : f->stock)
        if (s.item == item) return &s;
    return nullptr;
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    if (!run.init(&err) || !run.start(2, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    Mission& m = *run.mission();
    m.autosave = false;
    int potion = 0, weapon = 0;
    for (int t = 0; t < 3000 && !(potion && weapon); ++t) {
        m.tick();
        for (const Facility& f : m.places().facilities())
            if (f.kind == Facility::Shop && !f.stock.empty()) (f.shop_type == 0 ? potion : weapon) = f.recno;
    }
    CHECK(potion && weapon);
    if (!potion || !weapon) return std::printf("%d failures\n", fails), 1;

    const int bread = gd->item_index("fd_bread_S"), fish = gd->item_index("fd_fish"), bkstar = gd->item_index("Bkstar");
    const int arrow = gd->item_index("ARROW"), letter = gd->item_index("flyletter02");
    // FUN_004697c0: S3L1_POTION verbatim (fd_bread_S 10, fd_fish 5); S3L1_WEAPON Bkstar at level 7, ARROW unlimited.
    CHECK(entry(m, potion, bread) && entry(m, potion, bread)->quantity == 10 && entry(m, potion, fish)->quantity == 5);
    CHECK(entry(m, weapon, bkstar) && entry(m, weapon, bkstar)->level == 7 && entry(m, weapon, arrow)->quantity == -1);
    CHECK(m.places().facility(potion)->can_regen == 1 && m.places().facility(potion)->selling_ratio == 0.5f);

    // 0x6c5ac6: Price x factor x (level - MinLevel + 1).
    const ItemDef& bk = gd->items[std::size_t(bkstar)];
    CHECK(shop_price(*gd, *entry(m, weapon, bkstar)) == bk.price * (7 - bk.min_level + 1));
    CHECK(shop_price(*gd, *entry(m, potion, bread)) == gd->items[std::size_t(bread)].price);
    CHECK(shop_price(*gd, ShopStock{bread, 1.5f, 1, 3}) == int(gd->items[std::size_t(bread)].price * 1.5 * 3));

    // Buying: one shared state. Every purchase is one less for every later reader (shop_stock = what the buy screen
    // lays out and the bot plans from); a sold-out entry refuses (-1), short gold refuses with BUY 8 and keeps the stock.
    World& w = m.world();
    Unit& joan = *w.unit(w.player_id());
    joan.inv.bag.clear();
    int& gold = m.state().gold;
    gold = 100000;
    const ShopStock b = *entry(m, potion, bread);
    for (int i = 0; i < 10; ++i) CHECK(shop_buy(m, potion, b, joan) == 0);
    CHECK(gold == 100000 - 10 * shop_price(*gd, b));
    CHECK(shop_buy(m, potion, b, joan) == -1);
    const auto seen = shop_stock(m, potion);
    CHECK(std::find_if(seen.begin(), seen.end(), [&](const ShopStock& s) { return s.item == bread; })->quantity == 0);
    const ShopStock fi = *entry(m, potion, fish);
    for (int i = 0; i < 4; ++i) CHECK(shop_buy(m, potion, fi, joan) == 0);
    gold = 0;
    CHECK(shop_buy(m, potion, fi, joan) == 8 && entry(m, potion, fish)->quantity == 1);
    gold = 100000;
    CHECK(shop_buy(m, weapon, *entry(m, weapon, arrow), joan) == 0 && entry(m, weapon, arrow)->quantity == -1);

    // Selling (FUN_006c6550): the whole stack, Price x SellingRatio x quantity / Quantity x level factor; food only to a
    // POTION shop, the rest only to a WEAPON one, CanSell 0 never.
    ItemInstance loaves{bread, 3, gd->items[std::size_t(bread)].min_level};
    const ItemDef& bd = gd->items[std::size_t(bread)];
    CHECK(shop_sell_value(m, potion, loaves) == int(bd.price * 0.5 * 3.0 / std::max(1, bd.quantity)));
    CHECK(shop_sell_value(m, potion, loaves, true) == int(bd.price * 0.5));   // double click: quantity / quantity
    CHECK(shop_sell_value(m, weapon, loaves) == -1);
    CHECK(shop_sell_value(m, potion, {arrow, 10, gd->items[std::size_t(arrow)].min_level}) == -1);
    CHECK(!gd->items[std::size_t(letter)].can_sell && shop_sell_value(m, weapon, {letter, 1, 1}) == -1);

    // Native save round trip: stock, restock clock and record fields come back.
    m.places().facility(potion)->regen_ms = 1234;
    {
        CampaignRunner b2(*gd, vfs);
        auto f = decode_save(encode_save(run.save()), &err);
        CHECK(f && b2.init(&err) && b2.load(*f, &err) && b2.mission());
        if (b2.mission()) {
            const Facility* a = m.places().facility(potion);
            const Facility* c = b2.mission()->places().facility(potion);
            CHECK(c && c->stock.size() == a->stock.size() && c->regen_ms == 1234 && c->shop_type == 0 && c->selling_ratio == 0.5f);
            for (std::size_t i = 0; c && i < c->stock.size(); ++i)
                CHECK(c->stock[i].item == a->stock[i].item && c->stock[i].quantity == a->stock[i].quantity &&
                      c->stock[i].level == a->stock[i].level);
        }
    }
    // A save from before the shop stock (no PLCS trailer) still loads; its shops get the listed stock back.
    {
        SaveFile old = run.save();
        std::size_t trailer = 0;
        for (const Facility& f : m.places().facilities()) trailer += 4 + f.stock.size() * 16 + 12;
        for (SaveSection& s : old.sections)
            if (std::string_view(s.tag.data(), 4) == "PLCS") s.data.resize(s.data.size() - trailer);
        CampaignRunner b3(*gd, vfs);
        auto f = decode_save(encode_save(old), &err);
        CHECK(f && b3.init(&err) && b3.load(*f, &err) && b3.mission());
        if (b3.mission()) CHECK(entry(*b3.mission(), potion, bread)->quantity == 10 && entry(*b3.mission(), potion, fish)->quantity == 5);
    }

    // Hourly restock (FUN_00469710 / FUN_004699f0) of the can-regen POTION shop: round(listed / 2) + left, at most the
    // listed quantity (bread 0 -> 5, fish 1 -> 4); the WEAPON shop without can-regen keeps its clock at 0.
    m.places().facility(potion)->regen_ms = 3600000 - World::kTickMs;
    m.tick();
    CHECK(entry(m, potion, bread)->quantity == 5 && entry(m, potion, fish)->quantity == 4);
    CHECK(m.places().facility(potion)->regen_ms == 0 && m.places().facility(weapon)->regen_ms == 0);

    // A UniqueItem stored by Unit_add_item 0x495f40 (World::give_item: floor pickups, scripts; transfer_item) joins
    // Game+0x71c (MissionState::item_hints_a, FUN_00472620); a fresh list then leaves it out. The bot's probe
    // (can_give) records nothing.
    const int armor = gd->item_index("JOAN_ARMOR_LV2");
    CHECK(gd->items[std::size_t(armor)].unique && entry(m, weapon, armor));
    const auto& owned = m.state().item_hints_a;
    joan.inv.bag.clear();
    CHECK(w.can_give(joan, "JOAN_ARMOR_LV2", 0, 1) == 1 && std::find(owned.begin(), owned.end(), armor) == owned.end());
    CHECK(w.give_item(joan, "JOAN_ARMOR_LV2", 0, 1) == 1 && std::count(owned.begin(), owned.end(), armor) == 1);
    CHECK(w.give_item(joan, "JOAN_ARMOR_LV2", 0, 1) == 1 && std::count(owned.begin(), owned.end(), armor) == 1);
    CHECK(entry(m, weapon, armor));   // the stock already listed stays until the list is set again / restocked
    m.places().set_sell_list(weapon, m.places().facility(weapon)->sell_list);
    CHECK(!entry(m, weapon, armor) && entry(m, weapon, bkstar));

    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
