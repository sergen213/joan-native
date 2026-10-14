// MGEXP HUD headless (NullRenderer): EXP queue add FUN_006fe520 + roll / hold / fade of FUN_006fe660, level-up
// FUN_006ffed0 states, ComboHUD_show / ComboHUD_update, and the live feed from mission 1 World events through
// GameUiBridge::on_world_events (player only).
#include "check.hpp"
#include "engine/game_ui_bridge.hpp"
#include "engine/hud_exp.hpp"
#include "engine/renderer_null.hpp"
#include "engine/session.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace joa::engine;

namespace {
constexpr float kDt = 1.0f / 30.0f;  // 33.3 ms frames: one roll step each

// n frames of kDt last `ms` (within one frame).
bool lasts(int n, float ms) { return std::fabs(float(n) * kDt * 1000.0f - ms) <= kDt * 1000.0f + 0.5f; }

// One HUD frame; returns the number of quads drawn.
std::size_t frame(Ui& ui, HudModel& hud, float dt = kDt) {
    std::vector<UiQuad> quads;
    ui.begin({0, 0, 1024, 768}, quads, UiInput{}, dt);
    hud_exp(ui, hud);
    return quads.size();
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
    Ui ui(vfs, r);

    {  // queue: digits, cap 9
        HudModel hud;
        hud_exp_add(hud.exp, 247);
        CHECK(hud.exp.queue.size() == 1);
        CHECK(hud.exp.queue[0].digits == 3 && hud.exp.queue[0].roll == 0 && hud.exp.queue[0].state == 0);
        for (int i = 0; i < 12; ++i) hud_exp_add(hud.exp, 5);
        CHECK(hud.exp.queue.size() == 9);
    }
    {  // roll: ones 0,2,4,6,7 then tens 0,2,4 then hundreds 0,2 (+2 while >= 2 short), one step per frame
        HudModel hud;
        hud_exp_add(hud.exp, 247);
        const int expect[] = {2, 4, 6, 7, 27, 47, 247};
        CHECK(frame(ui, hud) > 0);
        for (int v : expect) {
            CHECK(hud.exp.queue[0].roll == v);
            CHECK(hud.exp.queue[0].state == 0);
            frame(ui, hud);
        }
        CHECK(hud.exp.queue[0].state == 1);  // all digits matched in this frame
        frame(ui, hud);
        CHECK(hud.exp.queue[0].state == 2);  // one "_2" flash frame
        frame(ui, hud);                        // hold timer starts
        // 3 digits: state 2 -> 3 at the ones digit, 3 -> 4 at the last one (same frame) after 1500 ms
        int frames = 0;
        while (hud.exp.queue[0].state == 2 && frames < 100) frame(ui, hud), ++frames;
        CHECK(lasts(frames, 1500));
        CHECK(hud.exp.queue[0].state == 4);
        frames = 0;
        while (!hud.exp.queue.empty() && frames < 100) frame(ui, hud), ++frames;
        CHECK(lasts(frames, 300));  // fade / fly-out, then dropped
        CHECK(frame(ui, hud) == 0);
    }
    {  // queued entries: the second rolls right after the first leaves; EXPTEXT stays (no slide-out in between)
        HudModel hud;
        hud_exp_add(hud.exp, 3);
        hud_exp_add(hud.exp, 9);
        for (int i = 0; i < 200 && hud.exp.queue.size() == 2; ++i) frame(ui, hud);
        CHECK(hud.exp.queue.size() == 1 && hud.exp.queue[0].value == 9 && hud.exp.queue[0].state == 0);
    }
    {  // level-up: LEVELUP0 fade-in 500 ms, LEVELUP1/2 one frame, LEVELUP3 2200 ms, cross-fade 300 ms, LEVEL3 2200 ms
        HudModel hud;
        hud_exp_level_up(hud.exp, 12);
        int f = 0;
        auto run_while = [&](int st) {
            int n = 0;
            while (hud.exp.level_state == st && n < 200) frame(ui, hud), ++n, ++f;
            return n;
        };
        CHECK(lasts(run_while(1), 500));
        CHECK(run_while(2) == 1);
        CHECK(run_while(3) == 1);
        CHECK(lasts(run_while(4), 2200));
        CHECK(lasts(run_while(5), 300));
        CHECK(run_while(6) == 1);   // state 7 is a separate `if`: drawn in the same frame state 6 ends
        CHECK(hud.exp.level_state == 7);
        CHECK(lasts(run_while(7), 2200));
        CHECK(run_while(8) == 1);
        CHECK(lasts(run_while(9), 300));
        CHECK(hud.exp.level_state == 0);
        CHECK(frame(ui, hud) == 0);
    }
    {  // combo: fade in 500 ms, count shown, kept while it grows, hidden 2000 ms after the last increase
        HudModel hud;
        hud_exp_combo(hud.exp, 2);
        hud.exp.combo_hits = 2;
        int n = 0;
        while (hud.exp.combo_state != 3 && n < 200) frame(ui, hud), ++n;
        CHECK(lasts(n, 533));  // 500 ms fade-in + the COMBO1 frame
        hud.exp.combo_hits = 4;  // hits counted while the HUD shows (no new ComboHUD_show)
        frame(ui, hud);
        CHECK(hud.exp.combo == 4);
        n = 0;
        while (hud.exp.combo_state == 3 && n < 200) frame(ui, hud), ++n;
        CHECK(lasts(n, 2000));
        CHECK(hud.exp.combo == 0 && hud.exp.combo_state == 4);
        n = 0;
        while (hud.exp.combo_state && n < 200) frame(ui, hud), ++n;
        CHECK(lasts(n, 333));  // COMBO0 frame + 300 ms fade
        hud_exp_combo(hud.exp, 3);
        hud.rts_mode = true;  // DAT_00a5b9f8 == 4: reset, nothing drawn
        CHECK(frame(ui, hud) == 0);
        CHECK(hud.exp.combo_state == 0);
    }
    {  // live feed: mission 1 player XP / level-up / combo through the bridge; other units ignored
        Session s(vfs);
        s.seed = 1;
        std::string err;
        CHECK(s.new_game(1, 1, &err));
        joa::game::Mission& m = *s.mission();
        m.autosave = false;
        joa::game::World& w = m.world();
        GameUiBridge b(vfs);
        HudModel hud;
        b.reset(m, hud);
        joa::game::Unit* pl = w.unit(w.player_id());
        CHECK(pl != nullptr);
        joa::game::Unit* other = nullptr;
        for (joa::game::Unit& u : w.units())
            if (u.id != w.player_id() && !other) other = &u;
        CHECK(other != nullptr);
        w.take_events();
        if (other) w.add_xp(*other, 40, false, false);
        if (pl) w.add_xp(*pl, 25, false, false);
        b.on_world_events(w.take_events(), m, hud);
        CHECK(hud.exp.queue.size() == 1 && hud.exp.queue[0].value == 25 && hud.exp.queue[0].digits == 2);
        CHECK(hud.exp.level_state == 0);
        if (pl) w.add_xp(*pl, w.data().level_row(pl->level)->exp, false, false);  // crosses the next level
        b.on_world_events(w.take_events(), m, hud);
        CHECK(hud.exp.queue.size() == 2);
        CHECK(hud.exp.level_state == 1 && pl && hud.exp.level == pl->level && pl->level >= 2);
        w.combo_hits = 1;
        b.on_world_events({}, m, hud);
        CHECK(hud.exp.combo_state == 0);  // ComboHUD_show only when the count > 1
        w.combo_hits = 2;
        b.on_world_events({}, m, hud);
        CHECK(hud.exp.combo_state == 1 && hud.exp.combo == 2);
    }
    return test_result();
}
