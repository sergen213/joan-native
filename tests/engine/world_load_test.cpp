// Headless: loads S1L1 (terrain, objects, lighting, Joan) through a renderer stub and checks counts.
// Exit 77 (skip) without the original data.
#include "check.hpp"
#include "engine/character_actor.hpp"
#include "engine/world_scene.hpp"
#include "engine/world_view.hpp"

#include <cstdlib>
#include <string>

using namespace joa::engine;

namespace {

struct NullRenderer final : IRenderer {
    std::uint32_t next = 1, meshes = 0, textures = 0;
    MeshData last;
    const char* name() const override { return "null"; }
    MeshHandle create_mesh(const MeshData& m) override { return last = m, ++meshes, MeshHandle{next++}; }
    void destroy_mesh(MeshHandle) override {}
    TextureHandle create_texture(const TextureData& t) override {
        return t.pixels.size() >= std::size_t(t.width) * t.height * 4 ? (++textures, TextureHandle{next++}) : TextureHandle{};
    }
    void destroy_texture(TextureHandle) override {}
    void begin_frame(glm::ivec2) override {}
    void submit(const FrameData&) override {}
    void end_frame() override {}
    Image read_back() override { return {}; }
};

}  // namespace

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    const joa::formats::Vfs vfs(dir ? dir : ".");
    if (!vfs.exists("map/scenario1/S1L1.gri")) {
        std::fprintf(stderr, "SKIP: no original data at %s\n", dir ? dir : ".");
        return 77;
    }
    CHECK(find_scenario(vfs, "S1L1").has_value());
    CHECK(find_scenario(vfs, "s1l1")->dir == "map/scenario1");
    CHECK(find_scenario(vfs, "map/scenario1/S1L1.stg").has_value());
    CHECK(!find_scenario(vfs, "S9L9").has_value());

    NullRenderer r;
    AssetCache assets(vfs, r);
    WorldScene world(assets, "S1L1");
    const WorldStats& s = world.stats();
    std::fprintf(stderr, "S1L1: stage=%s objects=%d placed=%d missing=%d terrain_tris=%d groups=%d lightmaps=%d textures=%u missing_tex=%zu\n",
                 s.stage.c_str(), s.objects, s.placed, s.missing_models, s.terrain_triangles, s.terrain_groups, s.lightmaps,
                 r.textures, assets.missing_textures());
    for (const auto& m : s.missing) std::fprintf(stderr, "  missing model %s\n", m.c_str());
    for (const auto& t : assets.missing_texture_names()) std::fprintf(stderr, "  missing texture %s\n", t.c_str());

    CHECK(s.stage == "Normal_12");           // S1L1w.day stage running at the default 12:00 clock
    CHECK(s.objects == 762);                 // CreateObject blocks in S1L1.stg
    CHECK(s.placed + s.missing_models == s.objects);
    CHECK(s.placed >= 750);
    CHECK(s.terrain_triangles > 100000 && s.terrain_triangles <= 256 * 256 * 2);
    CHECK(s.lightmaps == 256);               // texture/terrain/S1l1_scene_sector_1..256.dds

    // Every placed object stands near the terrain (WorldFormats: 82% within 40 units).
    int near = 0;
    for (const PlacedObject& o : world.objects()) {
        const glm::vec3 p(o.transform[3]);
        near += std::abs(p.y - world.ground_height(p)) < 40.0f;
    }
    std::fprintf(stderr, "objects within 40 units of the ground: %d / %zu\n", near, world.objects().size());
    CHECK(near * 10 >= int(world.objects().size()) * 7);

    // Scripted start (S1L1.sce set_unit_pos 15021, 12684; set_unit_dir 115).
    CHECK_NEAR(world.start_position().x, 15021.0f, 0.5f);
    CHECK_NEAR(world.start_position().z, -12684.0f, 0.5f);
    CHECK_NEAR(glm::degrees(world.start_yaw()), 115.0f, 1e-3f);
    const Lighting l = world.lighting();
    CHECK(l.sun_direction.y < -0.99f);       // Normal_12: VDir 90 = straight down
    CHECK(l.ambient.r > 0.4f && l.fog_end > l.fog_start);
    // RE'd fog (FUN_005634e0): 900 .. view distance 3996; colour = Normal_12 inscattering at 2000 units,
    // level view: hand-computed (106, 146, 193) from scatter constants 0x617ee0 + common.sca.
    CHECK_NEAR(l.fog_start, 900.0f, 1e-3f);
    CHECK_NEAR(l.fog_end, 3996.0f, 1e-3f);
    CHECK_NEAR(l.fog_color.r * 255.0f, 106.0f, 2.0f);
    CHECK_NEAR(l.fog_color.g * 255.0f, 146.0f, 2.0f);
    CHECK_NEAR(l.fog_color.b * 255.0f, 193.0f, 2.0f);
    CHECK_NEAR(l.sun_color.r, 227.0f / 255.0f, 1e-4f);  // Normal_12 Type 1, raw (material diffuse applies per draw)
    // Point lights: 135 CreateLight (all light_type 3) + item .pro HasLight lights (torches).
    std::fprintf(stderr, "point lights=%d\n", s.lights);
    CHECK(s.lights >= 135 && int(l.point_lights.size()) == s.lights);
    int lit_objects = 0;
    for (const PlacedObject& o : world.objects()) {
        lit_objects += o.light_count > 0;
        for (int i = 0; i < o.light_count; ++i) CHECK(o.lights[std::size_t(i)] < l.point_lights.size());
    }
    std::fprintf(stderr, "objects with point lights=%d\n", lit_objects);
    CHECK(lit_objects > 0);
    // "Light 11" (8370.4, 482.7, 1424) is listed in scene_sector_113 and _129; the point lies in _129.
    DrawItem sector129;
    world.sector_lights(to_engine(8370.0f, 482.0f, 0.0f), kMaxDrawLights, sector129);
    bool light11 = false;
    for (int i = 0; i < sector129.light_count; ++i)
        light11 |= glm::distance(l.point_lights[sector129.lights[std::size_t(i)]].position, to_engine(8370.42f, 482.69f, 1424.0f)) < 1.0f;
    CHECK(light11);
    CHECK(world.units().size() == 2 + 71);  // Joan + Jean de Metz (S1L1.sce), then the S1L1.uni CreateUnit blocks
    CHECK(s.map_units == 71);
    bool jean = false;
    for (const auto& u : world.units())
        if (u.code == "JEAN_DE_METZ") jean = std::abs(u.position.x - 14934.0f) < 0.5f && std::abs(glm::degrees(u.heading) - 295.0f) < 1e-3f;
    CHECK(jean);
    CHECK(WorldScene(assets, "S1L1", 60.0f).stats().stage == "Normal_Night");  // 1400 + 160 min wraps past midnight
    Character joan(assets, "unit/Joan_of_Arc.AM1", "anim/joan_of_arc.am2");
    CHECK(joan.play("f_run"));
    CHECK(!joan.play("no_such_clip"));
    CHECK(joan.clip().name == "f_run");
    joan.pose(0.0f);
    CHECK(joan.height() > 60.0f && joan.height() < 100.0f);
    const float t0 = joan.time_ms(1.0f);
    joan.tick(1.0f / 30.0f);
    CHECK_NEAR(joan.time_ms(1.0f) - t0, 1000.0f / 30.0f, 1e-3f);
    CHECK_NEAR(joan.time_ms(0.5f) - t0, 500.0f / 30.0f, 1e-3f);
    for (int i = 0; i < 30; ++i) joan.tick(1.0f / 30.0f);  // f_run is 800 ms: wraps
    CHECK(joan.time_ms(1.0f) < joan.clip().end - joan.clip().start);
    CHECK(joan.time_ms(0.0f) >= 0.0f);
    std::vector<DrawItem> draws;
    joan.build(glm::mat4(1.0f), draws);
    CHECK(draws.size() == 6);  // six submeshes

    // unit.res + STD.SET ANIMATE + item.res look-ups.
    const UnitDb db(vfs);
    const auto archer = db.look("ENGLISH_ARCHER");
    CHECK(archer && archer->model == "unit/english_archer.am1" && archer->anim == "anim/english_archer_share.am2" &&
          archer->idle == "F_IDLE" && archer->shadow == "model/shadow_bitmap.smo");
    CHECK(archer && archer->items.size() == 1 && archer->items[0].model == "item/e_bmlb.am1" && archer->items[0].bone == "bip01 L Hand");
    const auto joan_look = db.look("JOAN_OF_ARC");
    CHECK(joan_look && joan_look->model == "unit/joan_armorC.am1" && joan_look->items.size() == 1 &&
          joan_look->items[0].model == "item/sw_Warbrand.smo" && joan_look->items[0].bone == "bip01 R Hand");
    CHECK(!db.look("NO_SUCH_UNIT"));

    // Sword on the right hand: drawn with the bone's transform, shadow (#S body + sword #s) on the ground.
    CHECK(joan.attach(assets.model("item/sw_Warbrand.smo"), "Bip01 R Hand", assets.static_mesh("item/sw_Warbrand#s.SMO")));
    CHECK(!joan.attach(assets.model("item/sw_Warbrand.smo"), "no such bone"));
    joan.pose(0.0f);
    draws.clear();
    joan.build(glm::mat4(1.0f), draws);
    CHECK(draws.size() > 6 && draws.back().model != glm::mat4(1.0f));
    const glm::vec3 hand(draws.back().model[3]);
    CHECK(hand.y > 20.0f && hand.y < 60.0f);  // at hand height, not at the feet or the origin
    for (const DrawItem& d : draws) CHECK(!d.material.receive_shadow);  // units: stencil 0, no self-shadowing
    draws.clear();
    joan.build_shadow(glm::mat4(1.0f), glm::vec3(0, -1, 0), Character::ShadowKind::Volume, draws);
    CHECK(draws.size() == 1 && draws[0].material.blend == BlendMode::ShadowVolume);
    // Vertices come in pairs: caster point, then the point extruded height + 300 along the light.
    float worst = 0;
    for (std::size_t i = 0; i + 1 < r.last.vertices.size(); i += 2)
        worst = std::max(worst, glm::length(r.last.vertices[i + 1].position - r.last.vertices[i].position - glm::vec3(0, -(joan.height() + 300.0f), 0)));
    CHECK(!r.last.indices.empty() && r.last.vertices.size() % 2 == 0 && worst < 1e-2f);
    // An upward quad lit from above: 2 cap triangles + 4 silhouette quads, sides wound outwards.
    const glm::vec3 quad[4] = {{0, 0, 0}, {1, 0, 0}, {1, 0, -1}, {0, 0, -1}};
    const std::uint32_t quad_tris[6] = {0, 1, 2, 0, 2, 3};
    MeshData vol;
    append_shadow_volume(ShadowCaster(quad, quad_tris), quad, glm::vec3(0, -1, 0), 10.0f, vol);
    CHECK(vol.vertices.size() == 8 && vol.indices.size() == 30);
    bool outward = true;
    for (std::size_t i = 6; i + 2 < vol.indices.size(); i += 3) {
        const glm::vec3 a = vol.vertices[vol.indices[i]].position, b = vol.vertices[vol.indices[i + 1]].position,
                        c = vol.vertices[vol.indices[i + 2]].position;
        const glm::vec3 out = (a + b + c) / 3.0f - glm::vec3(0.5f, (a.y + b.y + c.y) / 3.0f, -0.5f);
        outward = outward && glm::dot(glm::cross(b - a, c - a), out) > 0.0f;
    }
    CHECK(outward);

    // Whole client world: every start unit placed with its idle clip and equipment.
    Options o;
    o.game_dir = dir ? dir : ".";
    o.scenario = "S1L1";
    const WorldView view(r, o);
    CHECK(view.water().count() == 5 && view.water().bump_count() == 2 && view.water().bump_frames() == 65);
    // Emitters: 2 .stg CreateParticle (waterfall mist) + 9 TorchStand .pro HasEmitter slots.
    CHECK(view.particles().emitter_count() == 11 && view.particles().stage_emitters() == 2 &&
          view.particles().item_emitters() == 9 && view.particles().missing() == 0);
    const auto& u = view.unit_stats();
    std::fprintf(stderr, "units placed=%d missing=%d items=%d items_missing=%d\n", u.placed, u.missing, u.items, u.items_missing);
    CHECK(u.placed == 72 && u.missing == 0);  // Jean de Metz + 71 .uni units
    CHECK(u.items >= 73 && u.items_missing == 0);
    return test_result();
}
