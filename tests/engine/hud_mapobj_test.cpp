// HudGaps mapobj headless (NullRenderer + Session mission 1): minimap vision sources -> HudModel, enemy dots only
// inside the mask (FUN_0070fbe0), the 200x200 mask raster, objective panel fade timing (FUN_00704810 / O key),
// list events (New! / COMPLETED! highlight and removal) and the "(Time left ...)" suffix.
#include "check.hpp"
#include "engine/game_ui_bridge.hpp"
#include "engine/hud_mapobj.hpp"
#include "engine/renderer_null.hpp"
#include "engine/session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace joa::engine;

namespace {
void draw(Ui& ui, HudModel& hud, float dt) {
    std::vector<UiQuad> quads;
    ui.begin({0, 0, 1024, 768}, quads, {}, dt);
    hud_objectives(ui, hud);
}
}  // namespace

int main() {
    // ---- pure parts ----
    CHECK(hud_format_count(" (Time left: %1d %1p{minute|minutes})", 1, 0) == " (Time left: 1 minute)");
    CHECK(hud_format_count(" (Time left: %1d %1p{minute|minutes})", 4, 0) == " (Time left: 4 minutes)");
    CHECK(hud_format_count(" (%1d/%2d rescured)", 3, 7) == " (3/7 rescured)");
    {
        HudModel h;
        h.minimap_fog = true;
        h.minimap_vision.push_back({1, {{1000.0f, 1000.0f}}, 400.0f, true});
        h.minimap_vision.push_back({0, {{3000.0f, 3000.0f}, {3500.0f, 2000.0f}}, 0.0f, false});
        // Counter-clockwise in the world (y up): the winding FUN_00719e60 fills (down edges = left bound).
        h.minimap_vision.push_back({2, {{100.0f, 4000.0f}, {100.0f, 3600.0f}, {600.0f, 3600.0f}, {600.0f, 4000.0f}}, 0.0f, false});
        CHECK(hud_vision_visible(h, {1000, 1390}) && !hud_vision_visible(h, {1000, 1410}));
        CHECK(hud_vision_visible(h, {3200, 2500}) && !hud_vision_visible(h, {3600, 2500}));
        CHECK(hud_vision_visible(h, {300, 3800}) && !hud_vision_visible(h, {700, 3800}));
        // View 200 px = 8192 world units from (0, 8192): 1 px = 40.96 units.
        VisionMask mask;
        hud_vision_mask(h, {0.0f, 8192.0f}, glm::vec2(200.0f / 8192.0f), mask);
        auto at = [&](glm::vec2 w) { return mask[std::size_t(int((8192.0f - w.y) / 40.96f) * 200 + int(w.x / 40.96f))]; };
        CHECK(at({1000, 1000}) == 1 && at({1000, 1300}) == 1 && at({1000, 1500}) == 0 && at({1500, 1000}) == 0);
        CHECK(at({3200, 2500}) == 1 && at({3700, 2500}) == 0);
        CHECK(at({300, 3800}) == 1 && at({800, 3800}) == 0);
        int lit = 0;
        for (auto v : mask) lit += v;
        std::fprintf(stderr, "mask lit %d px\n", lit);
        CHECK(lit > 300 && lit < 1000);  // circle r ~9.8 px (~300) + rect 12x24 + quad 12x10
        h.minimap_fog = false;
        CHECK(hud_vision_visible(h, {9000, 9000}));
    }
    {  // panel fade: show -> 2 s fade in -> 12 s after the show -> 3 s fade out -> hidden
        HudModel h;
        h.objective_list.resize(1);
        hud_objective_show(h);
        CHECK(h.show_objective && h.objective_fade == 1);
        const int a1 = hud_objective_alpha(h, 1000.0f);
        CHECK(a1 == 127);
        CHECK(hud_objective_alpha(h, 1000.0f) == 255 && h.objective_fade == 3);
        CHECK(hud_objective_alpha(h, 9999.0f) == 255 && h.objective_fade == 3);
        CHECK(hud_objective_alpha(h, 2.0f) == 255 && h.objective_fade == 2);
        CHECK(hud_objective_alpha(h, 1500.0f) == 127);
        hud_objective_show(h);  // fade-in resumes at the current alpha
        CHECK(h.objective_fade == 1 && hud_objective_alpha(h, 0.0f) == 127);
        CHECK(hud_objective_alpha(h, 1100.0f) == 255);
        hud_objective_toggle(h);  // O: fade out from shown, pinned
        CHECK(h.objective_fade == 2 && h.objective_pinned);
        CHECK(hud_objective_alpha(h, 3000.0f) == 0 && !h.show_objective && h.objective_fade == 0);
        hud_objective_toggle(h);  // O again: fade in, pinned -> no 12 s auto-hide
        hud_objective_alpha(h, 2000.0f);
        CHECK(hud_objective_alpha(h, 60000.0f) == 255 && h.objective_fade == 3);
        h.objective_paused = true;  // cut scene: the panel clock stands still
        hud_objective_toggle(h);
        CHECK(hud_objective_alpha(h, 5000.0f) == 255 && h.objective_fade == 2);
    }

    // ---- live mission ----
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return test_result() ? test_result() : 77;
    }
    joa::formats::Vfs vfs(game);
    Session s(vfs);
    s.seed = 12345;
    std::string err;
    if (!s.new_game(1, 1, &err)) {
        std::fprintf(stderr, "new_game: %s\n", err.c_str());
        return 1;
    }
    joa::game::Mission& m = *s.mission();
    m.autosave = false;
    NullRenderer r;
    Ui ui(vfs, r);
    GameUiBridge b(vfs);
    b.auto_advance = true;
    HudModel hud;
    b.reset(m, hud);
    b.on_events(s.take_mission_events(), m, hud);
    for (int t = 0; t < 60; ++t) {
        s.tick({});
        s.take_world_events();
        b.on_events(s.take_mission_events(), m, hud);
    }
    b.fill(m, s.current(), glm::mat4(1.0f), {0, 0, 1024, 768}, hud);
    auto& w = m.world();
    const joa::game::Unit* joan = w.unit(w.player_id());
    CHECK(joan && hud.minimap_fog);
    const auto own = std::find_if(hud.minimap_vision.begin(), hud.minimap_vision.end(), [&](const HudModel::VisionArea& a) {
        return a.shape == 1 && a.p[0] == glm::vec2(joan->pos.x, joan->pos.y);
    });
    CHECK(own != hud.minimap_vision.end() && own->r == float(m.data().c.vision_r) && own->r == 2000.0f);
    // Orleans (French, the player's nation) is a territory rectangle (2000,0)-(14384,1500) in S1L1w.ter.
    CHECK(std::any_of(hud.minimap_vision.begin(), hud.minimap_vision.end(),
                      [](const HudModel::VisionArea& a) { return a.shape == 0 && a.p[1] == glm::vec2(14384, 1500); }));
    std::fprintf(stderr, "vision areas %zu\n", hud.minimap_vision.size());

    // Enemy dots only inside the mask; a script circle over a hidden enemy reveals its dot.
    const joa::game::Unit* far = nullptr;
    for (const auto& u : w.units())
        if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && !hud_vision_visible(hud, {u.pos.x, u.pos.y})) far = &u;
    CHECK(far != nullptr);
    auto red_at = [&](const joa::game::Unit& u) {
        return std::any_of(hud.map_icons.begin(), hud.map_icons.end(), [&](const HudMapIcon& i) {
            return i.dot == HudMapIcon::Dot::Red && i.world == glm::vec2(u.pos.x, u.pos.y);
        });
    };
    if (far) {
        CHECK(!red_at(*far));
        auto& v = m.ui().vision[3];
        v.enabled = true, v.shape = 1, v.c[0] = int(far->pos.x), v.c[1] = int(far->pos.y), v.radius = 300;
        b.fill(m, s.current(), glm::mat4(1.0f), {0, 0, 1024, 768}, hud);
        CHECK(red_at(*far));
        v.enabled = false;
    }

    // Objective list: mission 1's main objective, "Time left" numbers from para1 1.
    CHECK(!hud.objective_list.empty());
    auto& ends = m.state().end;
    int main_rec = 0;
    for (const auto& l : hud.objective_list)
        if (l.type == 0 && l.enabled && !main_rec) main_rec = l.recno;
    CHECK(main_rec > 0);
    if (main_rec <= 0) return test_result();
    ends[std::size_t(main_rec - 1)].para1 = 1;
    m.scenario().objective_count_time = 300;  // 5 min limit, 61 s elapsed -> 3 whole minutes left
    m.scenario().count_time[0] = 61000;
    b.fill(m, s.current(), glm::mat4(1.0f), {0, 0, 1024, 768}, hud);
    const auto* ml = &*std::find_if(hud.objective_list.begin(), hud.objective_list.end(), [&](const auto& l) { return l.recno == main_rec; });
    CHECK(ml->suffix == 1 && ml->a == 3);
    CHECK(hud_format_count(ui.text("MAINGAME", 5), ml->a, 0) == " (Time left: 3 minutes)");

    // COMPLETED!: panel shows, the prefix types in after the fade-in, the row then fades and leaves the list.
    hud.show_objective = false, hud.objective_fade = 0;
    hud.objective_paused = false;  // tick 60 is inside the intro cut scene (the panel clock would stand still)
    joa::game::MissionEvent fin;
    fin.kind = joa::game::MissionEvent::Kind::ObjectiveFinish;
    fin.a = main_rec;
    b.on_events({fin}, m, hud);
    CHECK(hud.show_objective && hud.objective_fade == 1);
    const std::size_t n = hud.objective_list.size();
    float t = 0.0f;
    for (; t < 20.0f && hud.objective_list.size() == n; t += 0.033f) draw(ui, hud, 0.033f);
    std::fprintf(stderr, "COMPLETED! line removed after %.2f s\n", t);
    // 2 s fade in + strlen("COMPLETED!") = 10 steps of > 100 ms + 2 s row fade.
    CHECK(hud.objective_list.size() == n - 1 && t > 4.9f && t < 5.6f);

    // New! on an update; the list keeps the line, the highlight clears 2 s after the prefix completes.
    int side = 0;
    for (const auto& l : hud.objective_list)
        if (l.type == 2) side = l.recno;
    if (side) {
        joa::game::MissionEvent up;
        up.kind = joa::game::MissionEvent::Kind::ObjectiveUpdate;
        up.a = side;
        b.on_events({up}, m, hud);
        auto* sl = &*std::find_if(hud.objective_list.begin(), hud.objective_list.end(), [&](const auto& l) { return l.recno == side; });
        CHECK(sl->status == 2);
        for (int i = 0; i < 300; ++i) draw(ui, hud, 0.033f);
        CHECK(sl->status == -1 && sl->reveal == 0);
    }
    // Hide removes, Show (enabled again) inserts first with New!.
    joa::game::MissionEvent hide;
    hide.kind = joa::game::MissionEvent::Kind::ObjectiveHide;
    hide.a = hud.objective_list.back().recno;
    const int moved = hide.a;
    b.on_events({hide}, m, hud);
    CHECK(std::none_of(hud.objective_list.begin(), hud.objective_list.end(), [&](const auto& l) { return l.recno == moved; }));
    joa::game::MissionEvent show = hide;
    show.kind = joa::game::MissionEvent::Kind::ObjectiveShow;
    b.on_events({show}, m, hud);
    CHECK(hud.objective_list.front().recno == moved && hud.objective_list.front().status == 2);
    return test_result();
}
