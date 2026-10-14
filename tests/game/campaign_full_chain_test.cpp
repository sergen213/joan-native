// Forced-win chain through all 8 campaign missions: raise the playable heroes (level, STR,
// inventory, upgrades) and the party gold, finish the objectives, wait for the win, CONTINUE.
// Checks the Transition event / next stem, that every carried hero comes back in the next
// mission (load_unit, .uni placement or the kept S5L1 world of S9L1) with level, xp, STR,
// upgrades and inventory intact, gold carried, "campaign complete" + Ending after mission 8,
// and that a mission-4 save restored into a fresh runner stays hash-identical for 300 ticks.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

static int fails = 0;
#define CHECK(c) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++fails; \
    } while (0)

static const char* kHeroes[] = {"JOAN_OF_ARC", "JEAN_DE_METZ", "LAHIRE", "ALENCON"};

static Unit* find_class(World& w, int cls) {
    for (Unit& u : w.units())
        if (!u.deleted && u.cls == cls && !w.dead(u)) return &u;
    return nullptr;
}

// Progression that must survive the profile: level/xp/stats/points + upgrade list + inventory.
// Stacks of 2+ compare as "2+": scripts top arrows up (S3L1_tactic gives Jean 90 - count).
static std::vector<std::uint8_t> carried(const Unit& u) {
    Unit t;
    t.level = u.level, t.xp = u.xp, t.str = u.str, t.dex = u.dex, t.def = u.def, t.lead = u.lead;
    t.skill_points = u.skill_points, t.attr_points = u.attr_points;
    t.upgrades = u.upgrades, t.inv = u.inv;
    std::vector<std::uint8_t> b;
    t.inv.ammo.quantity = std::min(t.inv.ammo.quantity, 2);
    for (auto& it : t.inv.bag) it.quantity = std::min(it.quantity, 2);
    ByteWriter w(b);
    write_unit(w, t);
    return b;
}

static std::string describe(const Unit& u) {
    std::string s = "melee " + std::to_string(u.inv.melee[0].item) + "/" + std::to_string(u.inv.melee[1].item) + " ranged " +
                    std::to_string(u.inv.ranged.item) + " ammo " + std::to_string(u.inv.ammo.item) + " bag";
    for (const auto& it : u.inv.bag) s += " " + std::to_string(it.item) + "x" + std::to_string(it.quantity) + "L" + std::to_string(it.level);
    s += " upgrades";
    for (const auto& e : u.upgrades) s += " " + std::to_string(e.upitem) + ":" + std::to_string(e.unlocked) + "/" + std::to_string(e.applied);
    return s;
}

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner run(*gd, vfs);
    CHECK(run.init(&err) && run.missions.size() == 8);
    if (fails) return 1;
    std::map<std::string, int> cls;
    for (const char* h : kHeroes) CHECK((cls[h] = gd->unit_index(h)) >= 0);
    int potion = 0;
    for (int i = 1; i < int(gd->items.size()) && !potion; ++i)
        if (gd->items[size_t(i)].type == 0x15) potion = i;  // red potion
    CHECK(potion > 0);
    if (!run.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;

    for (int no = 1; no <= 8; ++no) {
        Mission& m = *run.mission();
        m.autosave = false;
        CHECK(m.scenario_no() == no && m.info().stem() == run.missions[size_t(no - 1)].stem());
        for (int t = 0; t < 60; ++t) m.tick();
        // Carried heroes are back with their stored progression (2..8).
        std::string seen;
        for (const char* h : kHeroes) {
            const HeroRecord* rec = run.profile.find(h);
            Unit* u = find_class(m.world(), cls[h]);
            if (!rec || !u) continue;
            seen += std::string(" ") + h;
            Unit now = *u;  // items the mission scripts hand out on top (S4L1 gives Joan arrows) are allowed
            std::erase_if(now.inv.bag, [&](const ItemInstance& it) {
                return std::none_of(rec->unit.inv.bag.begin(), rec->unit.inv.bag.end(), [&](const ItemInstance& r) { return r.item == it.item; });
            });
            if (carried(now) != carried(rec->unit))
                std::printf("FAIL %s %s not carried: level %d/%d xp %d/%d str %d/%d pts %d,%d/%d,%d\n  now  %s\n  rec  %s\n",
                            m.info().stem().c_str(), h, u->level, rec->unit.level, u->xp, rec->unit.xp, u->str, rec->unit.str,
                            u->skill_points, u->attr_points, rec->unit.skill_points, rec->unit.attr_points, describe(*u).c_str(),
                            describe(rec->unit).c_str()),
                    ++fails;
        }
        if (no > 1) CHECK(seen.find("JOAN_OF_ARC") != std::string::npos);
        // Raise every hero present: +2 levels, +1 STR, a potion, every affordable upgrade.
        for (const char* h : kHeroes) {
            Unit* u = find_class(m.world(), cls[h]);
            if (!u) continue;
            World& w = m.world();
            w.set_level(*u, u->level + 2);
            u->str += 1;
            CHECK(w.give_item(*u, gd->items[size_t(potion)].code, 1, 1));
            for (int e = 0; e < int(u->upgrades.size()); ++e) w.apply_upgrade(*u, e);
        }
        m.state().gold += 100 * no;

        if (no == 4) {  // save -> load into a fresh runner reproduces the same hashes
            const SaveFile f = run.save();
            CampaignRunner run2(*gd, vfs);
            CHECK(run2.init(&err) && run2.load(f, &err));
            if (run2.mission()) {
                run2.mission()->autosave = false;
                CHECK(run2.mission()->hash() == m.hash());
                int diverged = -1;
                for (int t = 0; t < 300; ++t) {
                    m.tick(), run2.mission()->tick();
                    if (diverged < 0 && run2.mission()->hash() != m.hash()) {
                        diverged = t;
                        const SaveFile a = m.save(), b = run2.mission()->save();
                        for (const auto& s : a.sections)
                            if (const SaveSection* o = b.find(std::string_view(s.tag.data(), 4)); !o || o->data != s.data)
                                std::printf("FAIL S4L1 save/load: section %.4s differs after %d ticks\n", s.tag.data(), t + 1), ++fails;
                    }
                }
                std::printf("S4L1 save/load: %016llx %016llx\n", static_cast<unsigned long long>(m.hash()),
                            static_cast<unsigned long long>(run2.mission()->hash()));
            }
        }

        m.finish_objectives();
        int t = 0;
        while (m.finish() == 0 && t < 6000) m.tick(), ++t;
        std::printf("%d %s won after %d ticks (finish %d) gold %d heroes:%s\n", no, m.info().stem().c_str(), t, m.finish(), m.state().gold,
                    seen.c_str());
        CHECK(m.finish() == 1);
        if (m.finish() != 1) break;
        std::map<std::string, std::vector<std::uint8_t>> won;  // progression at CONTINUE time
        for (const char* h : kHeroes)
            if (Unit* u = find_class(m.world(), cls[h]); u && u->rank > 3) {
                Unit c = *u;  // store_profile drops quest items (type 0xf) except leaving scenario 5
                if (no != 5) std::erase_if(c.inv.bag, [&](const ItemInstance& it) { return gd->items[size_t(it.item)].type == 0xf; });
                won[h] = carried(c);
            }

        if (no == 8) {
            CHECK(!run.continue_after_win(&err) && err == "campaign complete");
            bool ending = false;
            for (const auto& e : run.mission()->take_events()) ending |= e.kind == MissionEvent::Kind::Ending;
            CHECK(ending);
            break;
        }
        const int gold = m.state().gold;  // after the win's end-condition awards
        CHECK(run.continue_after_win(&err));
        if (fails) return std::printf("%d failures (%s)\n", fails, err.c_str()), 1;
        Mission& next = *run.mission();
        CHECK(run.profile.continued);
        CHECK(next.state().gold == gold);
        bool transition = false;
        for (const auto& e : next.take_events())
            transition |= e.kind == MissionEvent::Kind::Transition && e.a == no && e.b == no + 1 &&
                          e.text == run.missions[size_t(no)].stem();
        CHECK(transition);
        // Profile entries hold the CONTINUE-time progression (quest items dropped except after 5).
        for (const auto& [h, bytes] : won) {
            const HeroRecord* rec = run.profile.find(h);
            CHECK(rec != nullptr);
            if (rec && carried(rec->unit) != bytes) {
                std::printf("FAIL profile %s differs from %s at CONTINUE\n", h.c_str(), run.missions[size_t(no - 1)].stem().c_str()), ++fails;
            }
        }
    }
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
