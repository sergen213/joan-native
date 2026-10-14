// S1L1 map objects: the gameplay items of the .stg load (27: 18 boxes, 5 houses, 2 gates, 2 tents),
// the "Castle Gate" opens through a TRIGGER_GATE (add_gate_trigger path) and closes again, and the
// object state survives save/load.
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
    Mission& m = *run.mission();
    m.autosave = false;
    MapObjects& o = m.objects();
    int fails = 0;
    std::printf("S1L1 objects: %d\n", o.count());
    if (o.count() < 27) std::printf("FAIL expected >= 27 map objects\n"), ++fails;
    const int gate = o.find_by_name("Castle Gate");
    if (!gate || o.item(gate)->cls != 1) return std::printf("FAIL no Castle Gate\n"), 1;
    auto toggle = [&] {
        Trigger t;
        t.type = 0x20, t.target_kind = 2, t.target_id = o.item(gate)->modify, t.sender = m.world().player_id();
        t.start = t.end = m.world().now();
        m.world().post_trigger(t);
    };
    const int s0 = o.item(gate)->state;
    toggle();
    const int t0 = m.world().now();
    for (int i = 0; i < 300 && (o.item(gate)->state != (s0 == 0 ? 1 : 0)); ++i) m.tick();
    std::printf("gate %d: state %d -> %d after %d ms\n", gate, s0, o.item(gate)->state, m.world().now() - t0);
    if (o.item(gate)->state != (s0 == 0 ? 1 : 0)) std::printf("FAIL gate did not toggle\n"), ++fails;
    std::vector<std::uint8_t> a, b;
    ByteWriter wa(a);
    o.save(wa);
    formats::ByteReader r(a);
    if (!o.load_state(r) || !r.at_end()) std::printf("FAIL load_state\n"), ++fails;
    ByteWriter wb(b);
    o.save(wb);
    if (a != b) std::printf("FAIL save/load round trip\n"), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
