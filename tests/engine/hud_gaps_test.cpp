// HudGaps headless (NullRenderer + Session mission 1): interact indicator state + texts from InteractPrompt events
// (talk / bribe / prisoner / gate item), the one-shot bribe_no_gold attention message, the target HUD (unit hit by
// the player, gate item_hit, 3000 ms timeout, shown-HP chase), party hit flash / danger / morale, ui_format.
#include "check.hpp"
#include "engine/game_ui_bridge.hpp"
#include "engine/hud_effects.hpp"
#include "engine/renderer_null.hpp"
#include "engine/session.hpp"
#include "engine/ui_screens.hpp"
#include "game/interact_player.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace joa::engine;
namespace game = joa::game;

int main() {
    const char* gd = std::getenv("JOA_GAME_DIR");
    if (!gd || !joa::formats::Vfs(gd).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    CHECK(ui_format("Press %%1s to talk to %1s.", {"Jean"}) == "Press %1s to talk to Jean.");
    CHECK(ui_format("Press %1s to %2?{close|open} the gate.", {"R", "0"}) == "Press R to open the gate.");
    CHECK(ui_format("Press %1s to %2?{close|open} the gate.", {"R", "1"}) == "Press R to close the gate.");
    CHECK(ui_format("%1s needs %2d (%3d%%)", {"a", "5"}) == "a needs 5 (%)");

    joa::formats::Vfs vfs(gd);
    Session s(vfs);
    s.seed = 12345;
    std::string err;
    if (!s.new_game(1, 1, &err)) {
        std::fprintf(stderr, "new_game: %s\n", err.c_str());
        return 1;
    }
    game::Mission& m = *s.mission();
    m.autosave = false;
    game::World& w = m.world();
    NullRenderer r;
    Ui ui(vfs, r);
    GameUiBridge b(vfs);
    b.auto_advance = true;
    HudModel hud;
    b.reset(m, hud);
    const glm::ivec4 vp{0, 0, 1024, 768};
    auto step = [&](int n = 1) {
        for (int i = 0; i < n; ++i) {
            s.tick({});
            b.on_world_events(s.take_world_events(), m, hud);
            b.on_events(s.take_mission_events(), m, hud);
        }
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
    };
    auto ui_frame = [&] {
        std::vector<UiQuad> quads;
        ui.begin(vp, quads, UiInput{}, 1.0f / 30.0f);
        std::vector<UiCommand> out;
        b.draw(ui, m, hud, out);
        hud_game(ui, hud, out);
        return quads.size();
    };
    step(5);  // opening cut scene state settles; the prompt needs no cut scene
    m.ui().game_cut_scene = false;

    game::Unit& p = *w.unit(w.player_id());
    auto put_front = [&](game::Unit& u, float d) {
        u.pos = {p.pos.x + std::cos(p.yaw_rad()) * d, p.pos.y + std::sin(p.yaw_rad()) * d, p.pos.z};
        u.vel = {};
    };
    auto park = [&](game::Unit& u) { u.pos = {p.pos.x - 5000.0f, p.pos.y - 5000.0f, p.pos.z}; };
    int enemy = 0, friend_ = 0;
    for (game::Unit& u : w.units()) {
        if (u.deleted || u.id == p.id || w.dead(u)) continue;
        if (!enemy && w.hostile_to_player(u) && u.rank < 3) enemy = u.id;
        else if (!friend_ && !w.hostile_to_player(u) && u.nation == p.nation && !u.mount && u.rank < 3 && u.members.empty())
            friend_ = u.id;
        if (u.rank <= 3) park(u);  // party heroes (rank > 3) stay: the party strip is tested below
        u.m.bribe = 0, u.m.can_interact = 0, u.no_react = false;
    }
    CHECK(enemy && friend_);
    step(2);
    CHECK(hud.interact.line == -1 && hud.interact.bribe_line == 0);

    // ---- prisoner: GMSG 4 ----
    game::Unit& f = *w.unit(friend_);
    put_front(f, 60.0f);
    f.no_react = true;
    step();
    put_front(f, 60.0f);
    step();
    CHECK(hud.interact.line == 4);
    CHECK(interact_text(ui, hud.interact).starts_with("Press "));
    CHECK(interact_text(ui, hud.interact).find("untie the French soldier") != std::string::npos);
    CHECK(!hud.interact.key.empty() && interact_text(ui, hud.interact).find(hud.interact.key) != std::string::npos);
    const std::size_t quads_with_box = ui_frame();
    f.no_react = false;
    park(f);
    step(2);
    CHECK(hud.interact.line == -1);
    CHECK(ui_frame() < quads_with_box);  // GMSG BG + BORDER + text gone

    // ---- bribe: GMSG 5 under the minimap when gold is short, GMSG 3 otherwise ----
    game::Unit& e = *w.unit(enemy);
    put_front(e, 80.0f);
    e.m.bribe = 1;
    m.state().gold = 0;
    step();
    put_front(e, 80.0f);
    step();
    CHECK(hud.interact.bribe_line == 5 && hud.interact.bribe_cost == game::bribe_cost(m, e));
    CHECK(bribe_text(ui, hud.interact).starts_with("You cannot bribe "));
    CHECK(bribe_text(ui, hud.interact).find(std::to_string(hud.interact.bribe_cost)) != std::string::npos);
    m.state().gold = hud.interact.bribe_cost + 5;
    put_front(e, 80.0f);
    step();
    CHECK(hud.interact.bribe_line == 3);
    CHECK(bribe_text(ui, hud.interact).find("(you have " + std::to_string(m.state().gold) + " in gold)") != std::string::npos);
    // Interact with no gold: one attention message MESSAGE 2 (FUN_004e8500 -> FUN_004ad710), posted by the bridge.
    m.state().gold = 0;
    put_front(e, 80.0f);
    w.interact(p);
    step();
    const std::size_t before = m.ui().messages.size();
    ui_frame();
    CHECK(m.ui().messages.size() == before + 1 && m.ui().messages.back().text == ui.text("MESSAGE", 2));
    e.m.bribe = 0;

    // ---- target HUD: the player hits the enemy (FUN_006fe280: a new target snaps the shown HP) ----
    const int full = e.hp;
    b.on_world_events({{w.now(), game::Ev::Hit, enemy, p.id, 10, "SLASH"}}, m, hud);
    e.hp = std::max(1, w.max_hp(e) - 120);
    b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
    CHECK(hud.target.active && hud.target.unit && !hud.target.siege);
    CHECK(!hud.target.icon.empty());  // English / mercenary class portrait (FUN_00703880)
    CHECK(hud.target.hp == full);
    b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
    CHECK(hud.target.hp == full);  // FUN_006fd8b0 runs per game tick, not per frame
    b.on_world_events({}, m, hud);
    b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
    CHECK(hud.target.hp == full - 40 || hud.target.hp == e.hp);  // chases by <= 40 per tick
    for (int i = 0; i < 10; ++i) b.on_world_events({}, m, hud);
    b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
    CHECK(hud.target.hp == e.hp);
    ui_frame();
    park(e);
    step(95);  // > 3000 ms without another hit
    CHECK(!hud.target.active);

    // ---- target HUD: gate hit by the player (Gate_on_event -> FUN_006fe340) ----
    if (const int gate = m.objects().find_by_name("Castle Gate")) {
        b.on_events({{game::MissionEvent::Kind::Ui, 0, gate, p.id, "item_hit"}}, m, hud);
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
        CHECK(hud.target.active && !hud.target.unit && hud.target.icon == "GATE");
    } else {
        std::printf("note: no Castle Gate in S1L1\n");
    }
    // ---- player panel bars: FUN_006fd8b0 chases HP by <= 9, EP by <= 3 per tick; the texts are the real values ----
    {
        const int hp0 = p.hp, ep0 = p.ep;
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
        const float bar0 = hud.player.hp_bar;
        CHECK(std::lround(bar0 * float(hud.player.hp_max)) == hp0);
        p.hp = hp0 - 50, p.ep = std::max(0, ep0 - 20);
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
        CHECK(hud.player.hp_bar == bar0 && hud.player.hp_value == p.hp);
        b.on_world_events({}, m, hud);
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
        CHECK(std::lround(hud.player.hp_bar * float(hud.player.hp_max)) == hp0 - 9);
        CHECK(std::lround(hud.player.energy_bar * float(hud.player.energy_max)) == ep0 - std::min(3, ep0 - p.ep));
        for (int i = 0; i < 10; ++i) b.on_world_events({}, m, hud);
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
        CHECK(std::lround(hud.player.hp_bar * float(hud.player.hp_max)) == p.hp);
        p.hp = hp0, p.ep = ep0;
        for (int i = 0; i < 10; ++i) b.on_world_events({}, m, hud);
    }

    // ---- RTS mode: crew text (t_interf RTS 1) for the hovered siege weapon without full crew ----
    // S1L1 has no siege weapon: the parked friendly soldier becomes an uncrewed CANNON for the check.
    if (const int cannon = w.data().unit_index("CANNON"); cannon >= 0) {
        const int cls = f.cls, mode = w.game_mode;
        f.cls = cannon, f.m.can_attach = 1;
        const int slots = int(w.data().units[std::size_t(cannon)].attach_points.size());
        f.m.crew.assign(std::size_t(slots), 0);
        w.game_mode = 4, b.rts_siege = f.id;
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
        CHECK(slots > 0 && hud.interact.line == 1 && hud.interact.item_arg == slots);
        CHECK(interact_text(ui, hud.interact).find("required to operate the " + hud.interact.name) != std::string::npos);
        std::printf("RTS crew text: %s\n", interact_text(ui, hud.interact).c_str());
        for (int& id : f.m.crew) id = p.id;  // fully crewed: no box
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
        CHECK(hud.interact.line == -1);
        w.game_mode = mode, b.rts_siege = 0;
        f.cls = cls, f.m.can_attach = 0, f.m.crew.clear();
        b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
    } else {
        CHECK(false);
    }
    CHECK(ui_format("%1d light infantry %1p{unit|units} %1p{is|are} required to operate the %2s.", {"1", "Cannon"}) ==
          "1 light infantry unit is required to operate the Cannon.");
    CHECK(ui_format("%1d light infantry %1p{unit|units} %1p{is|are} required", {"4"}) == "4 light infantry units are required");

    // ---- party strip: hit flash, danger, morale, busy half alpha, CSELECT / MSELECT owner ----
    CHECK(!hud.party.empty());
    if (!hud.party.empty()) {
        int pid = 0;  // the first party slot's unit

        for (const game::Unit& u : w.units())
            if (!u.deleted && u.name == hud.party.front().name && u.rank > 3) pid = u.id;
        CHECK(pid != 0);
        if (game::Unit* pu = w.unit(pid)) {
            b.on_world_events({{w.now(), game::Ev::Hit, pid, enemy, 5, "SLASH"}}, m, hud);
            b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
            CHECK(hud.party.front().hit_ms >= 0 && hud.party.front().hit_ms < 1000);
            step(35);
            CHECK(hud.party.front().hit_ms == -1);
            const int hp = pu->hp;
            pu->hp = std::max(1, int(float(w.max_hp(*pu)) * 0.1f));
            b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
            CHECK(hud.party.front().danger);
            pu->hp = hp;
            b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
            CHECK(!hud.party.front().danger);
            CHECK((hud.party.front().morale >= 0) == (hud.party.front().units > 0));
            pu->m.control = 1;
            b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
            CHECK(hud.party.front().busy);
            pu->m.control = 0;
            b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
            CHECK(!hud.party.front().busy);
            hud.ui_ms = 1234.0f;
            b.char_unit = pid;  // character window open on it: CSELECT + MSELECT, blink restarts
            b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
            CHECK(hud.party.front().commander_selected && hud.party.front().troops_selected && hud.select_ms == 1234.0f);
            b.char_unit = 0, b.rts_commander = pid;  // RTS: CSELECT only, on the +0x1238 commander
            const int mode = w.game_mode;
            w.game_mode = 4;
            b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
            CHECK(hud.party.front().commander_selected && !hud.party.front().troops_selected);
            w.game_mode = mode;
            b.fill(m, s.current(), glm::mat4(1.0f), vp, hud);
            CHECK(!hud.party.front().commander_selected);
        }
    }
    return test_result();
}
