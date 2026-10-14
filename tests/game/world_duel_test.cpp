// Scripted duel on S1L1 terrain: Joan (player controller fed by a DuelPad) vs an English light
// infantryman (virtual pad). The outcome and the World hash must be identical across runs, and a
// save/load round trip in the middle of the fight must not change the result.
#include "sim_fixture.hpp"

#include <vector>

using namespace joa::game;

namespace {
struct Duel {
    World w;
    DuelPad pj, pe;
    int joan = 0, eng = 0;
    Duel(GameData& gd, const ITerrainQuery* t) : w(gd, t, 1234) {
        joan = w.spawn_unit("JOAN_OF_ARC", gd.nation_id("FRENCH"), {8096, 4839, 0}, 0);
        eng = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", gd.nation_id("ENGLISH"), {8246, 4839, 0}, 180, 3);
        w.set_player(joan);
        pj.target = eng, pj.combo = "AAAB", pj.approach = 90;
        pe.target = joan, pe.combo = "AA";
        attach();
    }
    void attach() { w.attach_pad(eng, [this](World& ww, const Unit& u) { return pe.next(ww, u); }); }
    std::string run(int ticks) {
        std::string log;
        for (int i = 0; i < ticks; ++i) {
            const Unit* j = w.unit(joan);
            w.tick(j ? pj.next(w, *j) : InputFrame{});
            for (const auto& e : w.take_events())
                if (e.type == Ev::Hit || e.type == Ev::Death || e.type == Ev::Knockdown || e.type == Ev::Blocked)
                    log += std::to_string(e.time) + " " + ev_name(e.type) + " " + std::to_string(e.unit) + " " + std::to_string(e.value) + "\n";
        }
        return log;
    }
};
}  // namespace

int main() {
    auto gd = load_data_or_skip();
    std::string err;
    auto terrain = GriTerrain::load_scenario(game_dir(), "S1L1", &err);
    CHECK(terrain, "S1L1 terrain: %s", err.c_str());
    if (!terrain) return finish();

    Duel a(*gd, terrain.get());
    CHECK(NEAR(a.w.unit(a.joan)->pos.z, terrain->height(8096, 4839), 1e-3), "spawn not on terrain");
    const std::string log_a = a.run(900);
    Duel b(*gd, terrain.get());
    const std::string log_b = b.run(900);
    CHECK(log_a == log_b, "event logs differ");
    CHECK(a.w.hash() == b.w.hash(), "hash differs %llx %llx", (unsigned long long)a.w.hash(), (unsigned long long)b.w.hash());
    const Unit* e = a.w.unit(a.eng);
    const Unit* j = a.w.unit(a.joan);
    CHECK((e && a.w.dead(*e)) || !e || (j && a.w.dead(*j)), "duel reached no decision in 900 ticks");
    CHECK(log_a.find(" hit ") != std::string::npos, "no hits");
    std::fputs(log_a.c_str(), stdout);

    // Save at tick 150, restore into a fresh world, continue: same end state as an uninterrupted run.
    Duel c(*gd, terrain.get());
    c.run(150);
    std::vector<std::uint8_t> blob;
    c.w.serialize(blob);
    Duel d(*gd, terrain.get());
    CHECK(d.w.deserialize(blob), "deserialize failed");
    d.pj = c.pj, d.pe = c.pe;
    CHECK(d.w.hash() == c.w.hash(), "hash after load differs");
    c.run(750);
    d.run(750);
    CHECK(c.w.hash() == d.w.hash() && c.w.hash() == a.w.hash(), "continued hash differs");
    return finish();
}
