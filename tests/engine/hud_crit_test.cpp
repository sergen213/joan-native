// HudGaps crit (headless, NullRenderer + Session mission 1): World::crit sequence -> HudModel::crit keys and
// lit icons (Crit_drawHUD 0x45fae0 positions), CRITATT / CRITKILL -> CA / CK banner geometry, alpha and glow
// timing on the GAMEPLAY.RES {CRITICAL} durations, and the kill interface panels (FUN_006df180).
#include "check.hpp"
#include "engine/game_ui_bridge.hpp"
#include "engine/hud_crit.hpp"
#include "engine/renderer_null.hpp"
#include "engine/session.hpp"

#include <cstdio>
#include <cstdlib>

using namespace joa::engine;

namespace {
constexpr glm::ivec4 kViewport{0, 0, 1024, 768};

std::vector<UiQuad> draw(Ui& ui, HudModel& hud, float dt) {
    std::vector<UiQuad> q;
    ui.begin(kViewport, q, {}, dt);
    hud_crit(ui, hud);
    return q;
}

// Quads drawn from INTERFAC.SET record set/name (same texture + uv0).
std::vector<UiQuad> quads_of(Ui& ui, const std::vector<UiQuad>& q, const char* set, const char* name) {
    std::vector<UiQuad> r;
    if (const UiSprite* s = ui.sprite(set, name))
        for (const UiQuad& x : q)
            if (x.texture.id == s->texture.id && x.uv0 == s->uv0) r.push_back(x);
    return r;
}

bool at(const std::vector<UiQuad>& q, float x, float y, float w, float h) {
    return q.size() == 1 && q[0].position == glm::vec2(x, y) && q[0].size == glm::vec2(w, h);
}
}  // namespace

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
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
    for (int t = 0; t < 30; ++t) s.tick({});
    s.take_world_events();
    joa::game::World& w = m.world();
    const auto fill = [&] { b.fill(m, s.current(), glm::mat4(1.0f), kViewport, hud); };

    // GAMEPLAY.RES {CRITICAL} durations reach the model.
    fill();
    CHECK(hud.crit.fade_in1 == 200 && hud.crit.fade_in2 == 200 && hud.crit.glow_in[0] == 200 && hud.crit.glow_in[2] == 200);

    // Disabled (mission 1 before 600 s): nothing drawn even with a sequence.
    w.critical_attack_enabled = false;
    w.crit = {true, "AAB", 0, w.now()};
    fill();
    CHECK(!hud.crit.enabled && draw(ui, hud, 0.033f).empty());

    // Sequence start: three dark icons at (0x12 + 0x2d i, H - 0xa1), 40x40.
    w.critical_attack_enabled = true;
    fill();
    CHECK(hud.crit.enabled && hud.crit.keys == "AAB" && hud.crit.progress == 0);
    auto q = draw(ui, hud, 0.033f);
    CHECK(q.size() == 3);
    auto off_l = quads_of(ui, q, "CRITICAL", "LightOFF"), off_h = quads_of(ui, q, "CRITICAL", "HeavyOFF");
    CHECK(off_l.size() == 2 && off_h.size() == 1);
    CHECK(at(off_h, 108, 607, 40, 40));

    // Light pressed: first icon lit; wrong key resets (Crit_key 0x462810).
    w.crit_key('A', true);
    fill();
    CHECK(hud.crit.progress == 1);
    q = draw(ui, hud, 0.033f);
    CHECK(at(quads_of(ui, q, "CRITICAL", "Light"), 18, 607, 40, 40) && quads_of(ui, q, "CRITICAL", "LightOFF").size() == 1);
    w.crit_key('B', false);
    fill();
    CHECK(hud.crit.progress == 0);
    w.crit_key('A', true), w.crit_key('A', false);
    fill();
    q = draw(ui, hud, 0.033f);
    CHECK(quads_of(ui, q, "CRITICAL", "Light").size() == 2 && at(quads_of(ui, q, "CRITICAL", "HeavyOFF"), 108, 607, 40, 40));

    // Completed: CRITATT -> icons gone, CA banner on W/2, H/2 - 100.
    w.take_events();
    w.crit_key('B', false);
    b.on_world_events(w.take_events(), m, hud);
    fill();
    CHECK(hud.crit.keys.empty() && hud.crit.banner == 1);
    // t = 100 ms (FADE_IN1): scale r*1.1 = 0.55, alpha 255 r -> (371,266)-(652,301) inclusive, alpha 127.
    q = draw(ui, hud, 0.1f);
    auto ca = quads_of(ui, q, "CRITICAL", "CA");
    CHECK(at(ca, 371, 266, 282, 36));
    CHECK(!ca.empty() && std::abs(ca[0].color.a - 127.0f / 255.0f) < 1e-3f);
    CHECK(quads_of(ui, q, "CRITICAL", "Glow01").empty());
    // t = 300 (FADE_IN2): 1.1 - 0.05 = 1.05 -> (243,250)-(780,317), opaque.
    q = draw(ui, hud, 0.2f);
    ca = quads_of(ui, q, "CRITICAL", "CA");
    CHECK(at(ca, 243, 250, 538, 68) && ca[0].color.a == 1.0f);
    // t = 500 (GLOW_IN1 half): natural size at (256, 252) + Glow01 at alpha 127.
    q = draw(ui, hud, 0.2f);
    CHECK(at(quads_of(ui, q, "CRITICAL", "CA"), 256, 252, 512, 64));
    auto g1 = quads_of(ui, q, "CRITICAL", "Glow01");
    CHECK(at(g1, 256, 220, 512, 128) && std::abs(g1[0].color.a - 127.0f / 255.0f) < 1e-3f);
    // t = 700 (GLOW_IN2 half): Glow01 out + Glow02 in.
    q = draw(ui, hud, 0.2f);
    CHECK(quads_of(ui, q, "CRITICAL", "Glow01").size() == 1 && at(quads_of(ui, q, "CRITICAL", "Glow02"), 256, 252, 512, 64));
    // A crit kill now swaps CA for CK centred on the screen, same clock (t = 900: Glow02 -> Glow03).
    b.on_world_events({{0, joa::game::Ev::Crit, w.player_id(), 0, 0, "CRITKILL"}}, m, hud);
    q = draw(ui, hud, 0.2f);
    CHECK(quads_of(ui, q, "CRITICAL", "CA").empty());
    CHECK(at(quads_of(ui, q, "CRITI_K", "CK"), 256, 352, 512, 64) && quads_of(ui, q, "CRITI_K", "CKGlow03").size() == 1);
    // t = 1100: past FADE_IN1 + FADE_IN2 + GLOW_IN1..3 = 1000 -> drawn once more, then cleared.
    draw(ui, hud, 0.2f);
    CHECK(hud.crit.banner == 0 && draw(ui, hud, 0.033f).empty());

    // Kill interface: counter 1 citizens killed "3/10" at (0x1ac, 8), counter 2 time left at y 0x33.
    m.ui().kill[0] = {true, 0};
    m.ui().kill[1] = {true, 3};
    m.state().citizen_killed = 3, m.state().kill_citizen_limit = 10, m.state().kill_citizen_limit_2 = 125;
    m.scenario().count_time[0] = 5400;
    fill();
    CHECK(hud.kill_panel[0].text == "3/10" && hud.kill_panel[1].text == "2:0");
    q = draw(ui, hud, 0.033f);
    const auto panels = quads_of(ui, q, "KILL", "panel");
    CHECK(panels.size() == 2 && panels[0].position == glm::vec2(428, 8) && panels[1].position == glm::vec2(428, 51));
    CHECK(at(quads_of(ui, q, "KILL", "icon"), 428, 8, 176, 40) && at(quads_of(ui, q, "KILL", "ticon"), 428, 51, 176, 40));
    m.ui().kill[0].active = false;
    fill();
    q = draw(ui, hud, 0.033f);
    CHECK(at(quads_of(ui, q, "KILL", "panel"), 428, 8, 176, 40));
    return test_result();
}
