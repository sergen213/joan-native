// Mission brief (hud_game.cpp topic menu + hud_brief.cpp pages) without a GPU: topic clicks open pages, BACK returns,
// the Battleground view toggles, and brief_content loads mission 1's file-backed data (work/ui/notes_MissionBrief.md).
#include "check.hpp"
#include "engine/hud_brief.hpp"
#include "engine/ui_runner.hpp"

#include <cstdio>
#include <cstdlib>

using namespace joa::engine;

namespace {
struct NullRenderer final : IRenderer {
    const char* name() const override { return "null"; }
    MeshHandle create_mesh(const MeshData&) override { return {1}; }
    void destroy_mesh(MeshHandle) override {}
    TextureHandle create_texture(const TextureData&) override { return {1}; }
    void destroy_texture(TextureHandle) override {}
    void begin_frame(glm::ivec2) override {}
    void submit(const FrameData&) override {}
    void end_frame() override {}
    Image read_back() override { return {}; }
};

// One frame with a click released at `p` (design = window pixels at 1024x768), then `idle` frames without input.
void click(UiRunner& u, glm::vec2 p, int idle = 20) {
    std::vector<UiQuad> q;
    UiInput in;
    in.mouse = p;
    in.mouse_released = true;
    u.frame({0, 0, 1024, 768}, in, 1.0f / 30.0f, q);
    in.mouse_released = false;
    for (int i = 0; i < idle; ++i) q.clear(), u.frame({0, 0, 1024, 768}, in, 1.0f / 30.0f, q);
}
}  // namespace

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    joa::formats::Vfs vfs(game);
    NullRenderer r;
    UiRunner u(vfs, r, "brief");
    click(u, {512, 234});  // Background
    CHECK(u.hud.brief_page == 1 && u.hud.brief_scroll == -4);
    CHECK(u.hud.brief_text.starts_with("Convinced that she is on a higher mission"));
    click(u, {512, 679});  // BACK
    CHECK(u.hud.brief_page == 0 && u.hud.show_mission_brief);
    click(u, {512, 549});  // Strategic Command: disabled in mission 1
    CHECK(u.hud.brief_page == 0);
    click(u, {512, 486});  // Battleground: BACK at x 0x219, "2D VIEW" at 0x183
    CHECK(u.hud.brief_page == 5 && !u.hud.brief_2d);
    click(u, {437, 679});
    CHECK(u.hud.brief_2d);
    click(u, {587, 679});
    CHECK(u.hud.brief_page == 0);

    HudModel m;
    brief_content(vfs, 1, m);
    CHECK(m.brief_map == "script/scenario/S1L1_map.bm" && m.brief_map_centre == glm::vec2(100.0f));
    CHECK(m.brief_towns.size() == 2);
    if (m.brief_towns.size() == 2) {
        CHECK(m.brief_towns[0].name == "de la belle ombre" && m.brief_towns[0].side == 1 && m.brief_towns[0].type == 0);
        CHECK(m.brief_towns[1].picture == "s1_2" && m.brief_towns[1].side == 0 && m.brief_towns[1].type == 2);
        CHECK(m.brief_towns[1].world == glm::vec2(8196.0f, 750.0f));
    }
    const BriefWeapon& cannon = m.brief_weapon_info[3];  // Dexterity 75 / 20, Defense 10 / 5, RunSpeed 3, 2 crew
    CHECK(cannon.name == "Cannon" && cannon.attack == 4 && cannon.defense == 2 && cannon.mobility == 3 && cannon.crew == 2);
    CHECK(m.brief_weapon_info[0].attack == 2);  // battering ram: no ammo, Dexterity 10 / 5
    return test_result();
}
