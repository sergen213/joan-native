// Save/Player.dat full format: the shipped profile (N = 0) parses and rewrites byte-exact; a
// progressed CampaignProfile (S1L1 won with Joan levelled + bag items, CONTINUE) exported to
// Player.dat round-trips byte-exact and field-equal (copy_persistent_unit subset).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <tuple>

#include "game/campaign_runner.hpp"
#include "game/unitclass_db.hpp"

using namespace joa;
using namespace joa::game;

static int fails = 0;
#define CHECK(c) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++fails; \
    } while (0)

static std::vector<std::uint8_t> persisted(const Unit& u) {  // the fields Player.dat carries
    Unit c;
    copy_persistent_unit(u, c);
    c.cls = u.cls;
    std::sort(c.inv.bag.begin(), c.inv.bag.end(), [](const ItemInstance& a, const ItemInstance& b) {
        return std::tie(a.item, a.level, a.quantity) < std::tie(b.item, b.level, b.quantity);
    });
    std::vector<std::uint8_t> out;
    ByteWriter w(out);
    write_unit(w, c);
    return out;
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);

    // Shipped profile.
    auto shipped = vfs.read("save\\player.dat");
    CHECK(shipped && shipped->size() == 114);
    if (shipped) {
        auto p = formats::parse_player_dat(*shipped, &err);
        CHECK(p && p->name() == "Player" && p->entries.empty() && formats::write_player_dat(*p) == *shipped);
        if (p) {
            const CampaignProfile cp = from_player_dat(*p, *gd);
            CHECK(cp.gold == 100 && cp.heroes.empty() && cp.command_points == 0);
            CHECK(formats::write_player_dat(to_player_dat(cp, *gd)) == *shipped);  // export of a fresh profile
        }
    }

    // Progressed profile: S1L1 with Joan levelled and carrying items, won, CONTINUE.
    CampaignRunner run(*gd, vfs);
    if (!run.init(&err) || !run.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    run.mission()->autosave = false;
    const int joan_cls = gd->unit_index("JOAN_OF_ARC");
    for (int t = 0; t < 60; ++t) run.mission()->tick();
    Unit* joan = nullptr;
    for (Unit& u : run.mission()->world().units())
        if (!u.deleted && u.cls == joan_cls) joan = &u;
    CHECK(joan != nullptr);
    if (!joan) return 1;
    World& w = run.mission()->world();
    w.set_level(*joan, joan->level + 4);
    int enhancer = 0, potion = 0;
    for (int i = 1; i < int(gd->items.size()); ++i) {
        if (!enhancer && gd->items[size_t(i)].item_class == "ENHANCER") enhancer = i;
        if (!potion && gd->items[size_t(i)].item_class == "POTION") potion = i;
    }
    CHECK(enhancer && potion);
    CHECK(w.give_item(*joan, gd->items[size_t(potion)].code, 0, 2));
    CHECK(w.give_item(*joan, gd->items[size_t(enhancer)].code, 0, 1));
    run.mission()->finish_objectives();
    for (int t = 0; run.mission()->finish() == 0 && t < 3000; ++t) run.mission()->tick();
    CHECK(run.mission()->finish() == 1);
    CHECK(run.continue_after_win(&err));
    CampaignProfile prof = run.profile;
    prof.item_hints_a = {3, 7};
    prof.item_hints_b = {12};
    prof.command_points = 42;
    const HeroRecord* hero = prof.find("JOAN_OF_ARC");
    CHECK(hero && hero->unit.level >= 5 && hero->unit.inv.bag.size() >= 2 && !hero->unit.upgrades.empty());
    if (fails) return std::printf("%d failures (%s)\n", fails, err.c_str()), 1;

    const auto bytes = formats::write_player_dat(to_player_dat(prof, *gd));
    auto parsed = formats::parse_player_dat(bytes, &err);
    CHECK(parsed);
    if (!parsed) return std::printf("parse: %s\n", err.c_str()), 1;
    CHECK(parsed->entries.size() == prof.heroes.size());
    CHECK(formats::write_player_dat(*parsed) == bytes);  // byte-exact
    const CampaignProfile back = from_player_dat(*parsed, *gd);
    CHECK(formats::write_player_dat(to_player_dat(back, *gd)) == bytes);  // export is a fixed point
    CHECK(back.gold == prof.gold && back.play_ms == prof.play_ms && back.command_points == prof.command_points);
    CHECK(back.item_hints_a == prof.item_hints_a && back.item_hints_b == prof.item_hints_b);
    CHECK(back.heroes.size() == prof.heroes.size());
    for (size_t i = 0; i < back.heroes.size() && i < prof.heroes.size(); ++i) {
        CHECK(back.heroes[i].code == prof.heroes[i].code);
        CHECK(persisted(back.heroes[i].unit) == persisted(prof.heroes[i].unit));
    }
    const HeroRecord* bj = const_cast<CampaignProfile&>(back).find("JOAN_OF_ARC");
    if (bj && hero)
        std::printf("Player.dat %zu bytes, %zu entries; Joan level %d xp %d str %d upgrades %zu bag %zu\n", bytes.size(),
                    parsed->entries.size(), bj->unit.level, bj->unit.xp, bj->unit.str, bj->unit.upgrades.size(),
                    bj->unit.inv.bag.size());
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
