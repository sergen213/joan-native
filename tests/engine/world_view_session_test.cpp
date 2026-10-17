// WorldView against a headless Session (NullRenderer): one actor per snapshot unit with the snapshot's
// clip, despawn, attack-script particles, and the two-node S3L1 map (terrain/objects per node, ground
// height = game terrain). Exit 77 without the original data.
#include "check.hpp"
#include "engine/renderer_null.hpp"
#include "engine/session.hpp"
#include "engine/world_view.hpp"

#include <algorithm>
#include <cstdlib>

using namespace joa;
using namespace joa::engine;

namespace {

CameraPose behind(const game::World& w) {
    CameraPose pose;
    if (const game::Unit* p = w.unit(w.player_id())) {
        pose.target = to_engine(p->pos.x, p->pos.y, p->pos.z + 55.0f);
        pose.eye = pose.target + glm::vec3(0, 60, 190);
    }
    return pose;
}

const game::UnitView* find(const game::Snapshot& s, int id) {
    for (const auto& v : s.units)
        if (v.id == id) return &v;
    return nullptr;
}

}  // namespace

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    const formats::Vfs vfs(dir ? dir : ".");
    if (!vfs.exists("map/scenario1/S1L1.gri") || !vfs.exists("map/scenario3/S3L1_2.gri")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    NullRenderer r;
    Session s(vfs);
    s.seed = 7;
    std::string err;
    CHECK(s.new_game(1, 1, &err));
    game::Mission& m = *s.mission();
    m.autosave = false;
    game::World& w = m.world();
    WorldView view(r, vfs, m);
    CHECK(view.node_count() == 1);
    CHECK(view.water().count() == 5 && view.water().bump_count() == 2 && view.water().bump_frames() == 65);
    // Emitters: 2 .stg CreateParticle (waterfall mist) + 9 TorchStand .pro HasEmitter slots.
    CHECK(view.particles().emitter_count() == 11 && view.particles().stage_emitters() == 2 && view.particles().item_emitters() == 9 &&
          view.particles().missing() == 0);

    std::vector<DrawItem> draws;
    const auto tick = [&](const game::InputFrame& f) {
        if (m.ui().cut_scene_status()) m.ui().skip_cut_scene(m);  // the opening cut scene ignores the pad
        s.tick(f);
        view.sync(w, s.previous(), s.current());
        view.on_events(s.take_world_events(), s.current());
        s.take_mission_events();
        FrameView fv;
        draws.clear();
        view.frame(0.5f, behind(w), float(kGameTickSeconds), fv, draws);
    };
    const auto clip_matches = [&](int id) {
        const Character* c = view.actor(id);
        const game::UnitView* v = find(s.current(), id);
        return c && v && lower(c->clip().name) == lower(v->clip);
    };

    // 60 ticks running forward, then attacks.
    std::string run_clip;
    for (int i = 0; i < 60; ++i) {
        game::InputFrame f;
        f.held.set(game::IN_FORWARD);
        tick(f);
        if (const game::UnitView* v = find(s.current(), w.player_id())) run_clip = v->clip;
    }
    const auto& st = view.unit_stats();
    std::fprintf(stderr, "units=%zu actors=%d missing=%d items=%d items_missing=%d draws=%zu joan clip=%s\n", s.current().units.size(),
                 st.actors, st.missing, st.items, st.items_missing, draws.size(), run_clip.c_str());
    CHECK(st.actors == int(s.current().units.size()) && st.missing == 0);
    CHECK(st.items >= int(s.current().units.size()) && st.items_missing == 0);
    CHECK(clip_matches(w.player_id()));
    bool all = true;
    for (const auto& v : s.current().units) all = all && clip_matches(v.id);
    CHECK(all);
    CHECK(view.shadow_stats().volumes >= 1);
    // Attack B (JOAN_ATK_B: EmitterFlag 1, slots Joan_B / Joan_B1) spawns its script emitters on Joan.
    std::string attack_clip;
    std::size_t max_effects = 0, max_particles = 0;
    for (int i = 0; i < 90; ++i) {
        game::InputFrame f;
        if (i % 30 == 0) f.press(game::IN_ATTACK_B);
        tick(f);
        CHECK(clip_matches(w.player_id()));
        if (const game::UnitView* v = find(s.current(), w.player_id()); v && v->clip != run_clip) attack_clip = v->clip;
        max_effects = std::max(max_effects, view.effects().emitter_count());
        max_particles = std::max(max_particles, view.effects().particle_count());
    }
    std::fprintf(stderr, "joan attack clip=%s effect emitters max=%zu particles max=%zu now=%zu missing=%d\n", attack_clip.c_str(),
                 max_effects, max_particles, view.effects().emitter_count(), view.effects().missing());
    CHECK(!attack_clip.empty() && attack_clip != run_clip);
    CHECK(max_effects >= 2 && max_particles > 0 && view.effects().missing() == 0);

    // Despawn: a deleted unit's actor is gone after the next sync.
    int victim = 0;
    for (const auto& v : s.current().units)
        if (v.id != w.player_id()) victim = v.id;
    CHECK(victim && view.actor(victim));
    w.delete_unit(victim);
    tick({});
    CHECK(!find(s.current(), victim) && !view.actor(victim));
    CHECK(view.unit_stats().actors == int(s.current().units.size()));

    // Interactive water waves. S1L1 "Water 4" pond: CreateWater X 10687, Y 14139, 250 x 473, Z 242, bed ~215.
    CHECK(view.water_height(to_engine(10687.0f, 14139.0f, 0.0f)) == 242.0f && !view.water_height(to_engine(10687.0f, 14500.0f, 0.0f)));
    CHECK(view.waves().data().height.empty());  // Joan ran and fought on dry ground: nothing kicked the water
    const auto wave = [&](float x, float y, float r) {  // strongest |height| within r of original x, y
        float m = 0.0f;
        for (float dx = -r; dx <= r; dx += 4.0f)
            for (float dy = -r; dy <= r; dy += 4.0f) m = std::max(m, std::abs(view.waves().height({x + dx, -(y + dy)})));
        return m;
    };
    game::Unit& joan = *w.unit(w.player_id());
    joan.pos = {10680.0f, 14160.0f, w.terrain()->height(10680.0f, 14160.0f)};
    for (int i = 0; i < 20; ++i) tick({});
    const float standing = wave(joan.pos.x, joan.pos.y, 80.0f);
    for (int i = 0; i < 8; ++i) {
        game::InputFrame f;
        f.held.set(game::IN_FORWARD);
        tick(f);
    }
    const float wading = wave(joan.pos.x, joan.pos.y, 80.0f);
    // The pond's banks are walls: the dry ground north of it never moves; its middle is open water.
    const glm::vec2 dry(10687.0f, -14500.0f), pond(10687.0f, -14139.0f);
    std::fprintf(stderr, "pond: bed %.0f waves standing=%.2f wading=%.2f bank=%.2f\n", joan.pos.z, standing, wading, view.waves().height(dry));
    CHECK(standing > 0.05f && wading > 0.3f && view.waves().wall(dry) && view.waves().height(dry) == 0.0f && !view.waves().wall(pond));
    // An item dropped into the pond: a 0.8 kick (a ~5-unit crater) where it lands.
    const float before = wave(10650.0f, 14000.0f, 0.0f);
    CHECK(w.add_item("SMALLRED", 0, 1, {10650.0f, 14000.0f, w.terrain()->height(10650.0f, 14000.0f)}) != 0);
    tick({});
    std::fprintf(stderr, "drop: %.2f -> %.2f\n", before, wave(10650.0f, 14000.0f, 0.0f));
    CHECK(wave(10650.0f, 14000.0f, 0.0f) > before + 2.0f);

    // Mission 2: S3L1 = S3L1_1 + S3L1_2 (.wor CreateNode), node 2 at Y 14080.
    CHECK(s.new_game(2, 1, &err));
    game::Mission& m2 = *s.mission();
    m2.autosave = false;
    CHECK(m2.info().step.world_file.find("S3L1") != std::string::npos);
    WorldView v2(r, vfs, m2);
    CHECK(v2.node_count() == 2);
    const auto& nodes = v2.scene().nodes();
    for (const auto& n : nodes) {
        std::fprintf(stderr, "%s origin=(%.0f,%.0f) objects=%d placed=%d tris=%d lightmaps=%d lights=%d water=%d emitters=%zu\n",
                     n.map.name.c_str(), n.map.origin.x, n.map.origin.y, n.stats.objects, n.stats.placed, n.stats.terrain_triangles,
                     n.stats.lightmaps, n.stats.lights, v2.water(std::size_t(&n - nodes.data())).count(),
                     v2.particles(std::size_t(&n - nodes.data())).emitter_count());
        CHECK(n.stats.objects > 0 && n.stats.placed > 0 && n.stats.terrain_triangles > 0);
    }
    CHECK(nodes.size() == 2 && nodes[1].map.name == "S3L1_2" && nodes[1].map.origin == glm::vec2(0.0f, 14080.0f));
    CHECK(v2.scene().stats().objects == nodes[0].stats.objects + nodes[1].stats.objects);
    // Objects of node 2 stand at Y >= 14080 (original).
    int in_node2 = 0;
    for (const PlacedObject& o : v2.scene().objects()) in_node2 += -o.transform[3].z >= 14080.0f;
    CHECK(in_node2 == nodes[1].stats.placed);
    const game::ITerrainQuery* terrain = m2.world().terrain();
    float worst = 0;
    for (int i = 1; i < 10; ++i)
        for (int j = 1; j < 10; ++j) {
            const float x = 1408.0f * float(i), y = 14080.0f + 1408.0f * float(j);
            worst = std::max(worst, std::abs(v2.ground_height(to_engine(x, y, 0.0f)) - terrain->height(x, y)));
        }
    std::fprintf(stderr, "S3L1 node 2 ground height worst diff %.3f\n", worst);
    CHECK(worst < 1.0f);
    game::World& w2 = m2.world();
    for (int i = 0; i < 30; ++i) {
        s.tick({});
        v2.sync(w2, s.previous(), s.current());
        v2.on_events(s.take_world_events(), s.current());
        FrameView fv;
        draws.clear();
        v2.frame(0.0f, behind(w2), float(kGameTickSeconds), fv, draws);
    }
    std::fprintf(stderr, "S3L1 units=%zu actors=%d missing=%d\n", s.current().units.size(), v2.unit_stats().actors, v2.unit_stats().missing);
    CHECK(v2.unit_stats().actors + v2.unit_stats().missing == int(s.current().units.size()));
    CHECK(v2.unit_stats().actors > 0);

    // Mission 3: S2L1's river is the world's own CreateWater (S2L1w.wor X 16384, Y 10900, 32768 x 6200, Z -150), not a
    // node's .stg one.
    CHECK(s.new_game(3, 1, &err));
    WorldView v3(r, vfs, *s.mission());
    CHECK(v3.world_water() && v3.world_water()->count() == 1 && v3.world_water()->bump_count() == 1);
    const std::optional<float> river = v3.water_height(to_engine(16384.0f, 10900.0f, 0.0f));
    CHECK(river && *river == -150.0f);
    return test_result();
}
