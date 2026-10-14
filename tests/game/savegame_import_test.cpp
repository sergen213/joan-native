// Import of the shipped original save Save/Mission1-AutoSave.SAV.
#include <cstdio>
#include <cstdlib>

#include "game/campaign_runner.hpp"
#include "game/savegame_import.hpp"

using namespace joa;
using namespace joa::game;

static int fails = 0;
#define CHECK(c) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++fails; \
    } while (0)

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("Save/Mission1-AutoSave.SAV")) return 77;
    const formats::Vfs vfs(dir);
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    auto bytes = vfs.read("Save/Mission1-AutoSave.SAV");
    auto o = import_original_save(*bytes, *gd, &err);
    CHECK(o.has_value());
    if (!o) return std::printf("import: %s\n", err.c_str()), 1;
    std::printf("\"%s\": scenario %d step %d stage %d bits %08x gold %d play %u ms, %zu end conditions, %zu heroes\n",
                o->header.description.c_str(), o->scenario_no, o->step, o->stage, o->bits[0], o->gold, o->play_ms,
                o->end.size(), o->heroes.size());
    for (const auto& h : o->heroes)
        std::printf("  hero %s \"%s\" level %d xp %d hp %d, inventory %d: melee %d ranged %d ammo %d x%d, %zu bag items\n", h.code.c_str(),
                    h.unit.name.c_str(), h.unit.level, h.unit.xp, h.unit.hp, h.inventory, h.unit.inv.melee[0].item, h.unit.inv.ranged.item,
                    h.unit.inv.ammo.item, h.unit.inv.ammo.quantity, h.unit.inv.bag.size());
    CHECK(o->header.description == "Mission 1 Auto Save");
    CHECK(o->scenario_no == 1 && o->step == 1 && o->header.value_c == 1);
    CHECK(o->finish == 0 && o->stage == 2 && o->gold == 100 && o->win_delay_ms == 15000);
    CHECK(o->end.size() == 7 && o->end[0].des == "Safely reach and enter Orl\xe9" "ans." && o->end[2].type == EndCondition::Lose);
    CHECK(o->scenario_name == "The Arrival");
    bool joan = false, jean = false;
    for (const auto& h : o->heroes) {
        const Inventory& inv = h.unit.inv;
        if (h.code == "JOAN_OF_ARC") {   // sword + bow equipped, empty bags
            joan = true;
            CHECK(h.inventory && inv.melee[0].item == 70 && inv.ranged.item == 43 && inv.bag.empty());
        }
        if (h.code != "JEAN_DE_METZ") continue;
        jean = true;
        // Jean's inventory records: weapon / bow / 50 arrows equipped, an enhancer at (0,0) of bag 0 and two items at
        // (0,0) / (1,0) of the personal bag.
        CHECK(h.inventory && inv.melee[0].item == 159 && inv.ranged.item == 78 && inv.ammo.item == 92 && inv.ammo.quantity == 50);
        CHECK(inv.bag.size() == 3);
        if (inv.bag.size() == 3) {
            CHECK(inv.bag[0].item == 86 && inv.bag[0].level == 7 && inv.bag[0].bag == 0 && inv.bag[0].col == 0);
            CHECK(inv.bag[1].bag == 1 && inv.bag[1].col == 0 && inv.bag[1].row == 0);
            CHECK(inv.bag[2].bag == 1 && inv.bag[2].col == 1 && inv.bag[2].row == 0);
        }
    }
    CHECK(joan && jean);

    CampaignRunner run(*gd, vfs);
    CHECK(run.init(&err) && run.start(o->scenario_no, &err));
    if (Mission* m = run.mission()) {
        m->autosave = false;
        apply_original_save(*m, *o);
        CHECK(m->scenario().stage == 2 && m->state().end.size() == 7 && m->state().gold == 100);
        bool applied = false;
        for (const Unit& u : m->world().units())
            applied |= !u.deleted && u.cls >= 0 && m->data().units[std::size_t(u.cls)].code == "JEAN_DE_METZ" && u.inv.bag.size() == 3 &&
                        u.inv.bag[2].col == 1;
        CHECK(applied);
        for (int t = 0; t < 300; ++t) m->tick();
        CHECK(m->vm().errors().empty());
    }
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
