// Headless: loads S1L1 (terrain, objects, lighting, Joan) through a renderer stub and checks counts.
// Exit 77 (skip) without the original data.
#include "check.hpp"
#include "engine/character_actor.hpp"
#include "engine/world_scene.hpp"

#include <cstdlib>
#include <string>

using namespace joa::engine;

namespace {

struct NullRenderer final : IRenderer {
    std::uint32_t next = 1, meshes = 0, textures = 0;
    const char* name() const override { return "null"; }
    MeshHandle create_mesh(const MeshData&) override { return ++meshes, MeshHandle{next++}; }
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
    std::fprintf(stderr, "S1L1: stage=%s objects=%d placed=%d missing=%d terrain_tris=%d groups=%d lightmaps=%d water=%d textures=%u missing_tex=%zu\n",
                 s.stage.c_str(), s.objects, s.placed, s.missing_models, s.terrain_triangles, s.terrain_groups, s.lightmaps, s.water,
                 r.textures, assets.missing_textures());
    for (const auto& m : s.missing) std::fprintf(stderr, "  missing model %s\n", m.c_str());
    for (const auto& t : assets.missing_texture_names()) std::fprintf(stderr, "  missing texture %s\n", t.c_str());

    CHECK(s.stage == "Normal_12");           // S1L1w.day stage running at the default 12:00 clock
    CHECK(s.objects == 762);                 // CreateObject blocks in S1L1.stg
    CHECK(s.placed + s.missing_models == s.objects);
    CHECK(s.placed >= 750);
    CHECK(s.terrain_triangles > 100000 && s.terrain_triangles <= 256 * 256 * 2);
    CHECK(s.lightmaps == 256);               // texture/terrain/S1l1_scene_sector_1..256.dds
    CHECK(s.water == 5);

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
    CHECK(world.scripted_units().size() == 2);  // Joan + Jean de Metz (S1L1.sce load_unit/set_unit_pos)
    bool jean = false;
    for (const auto& u : world.scripted_units())
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
    return test_result();
}
