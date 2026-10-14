// Barracks recruit + siege workshop screens, headless (NullRenderer, mission 7 S7L1): the player is warped into the
// "french recruit" facility, R (World::interact) opens the screen through MissionEvent::Interact and pauses the flow;
// pad left/right on a row adjusts the count and East leaves without a change; mouse clicks on aw_r add recruits,
// Enter commits (gold, population, queue) and the spawn barrack brings the units into the player's troop. Same for
// the NORMAL workshop "workshop1" (one cannon built after its 2 s). Last: bag-grid cells survive Mission::save/load
// and the character window draws the items at them.
#include "check.hpp"
#include "engine/hud_character.hpp"
#include "engine/hud_recruit.hpp"
#include "engine/renderer_null.hpp"
#include "engine/session_flow.hpp"

#include <SDL3/SDL_scancode.h>

#include <cstdlib>
#include <filesystem>

using namespace joa;
using namespace joa::engine;

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !formats::Vfs(game).exists("scenario/scenario_07.scr")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    const char* out = std::getenv("JOA_TEST_OUT");
    const std::filesystem::path xdg = std::filesystem::path(out ? out : ".") / "recruitworkshop-xdg";
    setenv("XDG_DATA_HOME", xdg.c_str(), 1);
    setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);

    const formats::Vfs vfs(game);
    Session s(vfs);
    s.seed = 1;
    MenuModel menu;
    HudModel hud;
    GameFlow flow(s, menu, hud);
    flow.persist_options = false;
    flow.new_game(7, 1);
    CHECK(flow.run_pending() && s.mission());
    if (g_failures) return test_result();
    game::Mission& m = *s.mission();
    m.autosave = false;
    for (int t = 0; t < 30; ++t) s.tick({}), s.take_world_events(), s.take_mission_events();
    m.ui().cs.active = false, m.ui().game_cut_scene = false;
    game::World& w = m.world();
    game::Unit& joan = *w.unit(w.player_id());
    game::Places& pl = m.places();

    NullRenderer r;
    Ui ui(vfs, r);
    FacilityWindow win;
    std::vector<UiQuad> quads;
    auto frame = [&](glm::vec2 mouse, bool press = false, bool release = false, int key = 0, UiPad pad = {}) {
        UiInput in;
        in.mouse = mouse, in.mouse_pressed = press, in.mouse_down = press, in.mouse_released = release, in.key_pressed = key;
        in.pad = pad;
        quads.clear();
        ui.begin({0, 0, 1024, 768}, quads, in, 0.033f);
        win.draw(ui, m, hud);
    };
    auto click = [&](glm::vec2 p) { frame(p), frame(p, true), frame(p, false, true); };
    auto settle = [&](glm::vec2 p) { for (int i = 0; i < 20; ++i) frame(p); };   // slides (rows, ia_ov panels)
    auto open = [&](const char* name) {
        const int f = pl.facility_by_name(name);
        const game::Facility* fc = pl.facility(f);
        CHECK(fc != nullptr);
        if (!fc) return 0;
        joan.pos = {fc->pos.x, fc->pos.y, w.terrain() ? w.terrain()->height(fc->pos.x, fc->pos.y) : fc->pos.z};
        // The interact handler's event (interact_player recruit_screen / workshop_handler); S7L1's nearby talk / bribe
        // targets would win R's earlier handlers here, so the event is emitted as the handler does.
        m.emit(game::MissionEvent::Kind::Interact, joan.id, f, fc->kind == game::Facility::Recruit ? "recruit_screen" : "workshop_screen");
        win.on_mission_events(m.take_events(), m, hud);
        std::fprintf(stderr, "%s: facility %d, recruit %d workshop %d\n", name, f, hud.recruit_screen, hud.workshop_screen);
        return f;
    };
    m.state().gold = 5000;
    // S7L1 starts Joan's troop at its maximum size (BarrUI_handle_input refuses recruits then): free the room.
    std::fprintf(stderr, "troop %zu, max %d\n", joan.members.size(), m.ai().max_troop_size(joan));
    for (int id : std::vector<int>(joan.members))
        if (game::Unit* u = w.unit(id)) m.ai().leave_team(*u);

    // ---- Barracks ----
    const int rf = open("french recruit");
    CHECK(rf > 0 && hud.recruit_screen == rf && flow.paused());
    settle({900, 740});
    CHECK(win.open() && !win.workshop && win.money == 5000 && win.citizens > 0);
    const game::UnitClass& linf = w.data().units[std::size_t(w.data().unit_index("FRENCH_LIGHT_INFANTRY"))];
    const int citizens = win.citizens;

    // Pad: the first input shows the focus (row 0), right adds one, East = Escape leaves without a commit.
    UiPad pad;
    pad.used = pad.connected = true;
    frame({900, 740}, false, false, 0, pad);
    frame({900, 740}, false, false, 0, pad);
    UiPad right = pad;
    right.dir = {1, 0};
    frame({900, 740}, false, false, 0, right);
    std::fprintf(stderr, "pad: add %d cost %d\n", win.add[0], win.cost);
    CHECK(win.add[0] == 1 && win.cost == linf.gold && win.citizens == citizens - 1);
    UiPad back = pad;
    back.back = true;
    frame({900, 740}, false, false, 0, back);
    settle({900, 740});
    CHECK(!win.open() && hud.recruit_screen == 0 && !flow.paused() && m.state().gold == 5000);

    // Mouse: hover row 0 (slides its ia_ov panel out), aw_r (0x238, 0x68 + 0x14) twice, Enter.
    open("french recruit");
    settle({300, 0x68 + 50});
    click({0x238 + 18, 0x68 + 0x14 + 14});
    click({0x238 + 18, 0x68 + 0x14 + 14});
    std::fprintf(stderr, "mouse: add %d cost %d total %d\n", win.add[0], win.cost, win.total);
    CHECK(win.add[0] == 2 && win.cost == 2 * linf.gold);
    click({0x238 + 18, 0x68 + 0x34 + 14});   // aw_l removes one
    CHECK(win.add[0] == 1);
    click({0x238 + 18, 0x68 + 0x14 + 14});
    const game::Place* owner = pl.place(pl.facility(rf)->place);
    const int pop = owner ? owner->pop : 0;
    frame({300, 0x68 + 50}, false, false, SDL_SCANCODE_RETURN);
    settle({900, 740});
    std::fprintf(stderr, "commit: gold %d, pop %d -> %d, queue %zu\n", m.state().gold, pop, owner ? owner->pop : 0,
                 pl.facility(rf)->queue.size());
    CHECK(m.state().gold == 5000 - 2 * linf.gold && owner && owner->pop == pop - 2 && !pl.facility(rf)->queue.empty());
    CHECK(!flow.paused());
    auto count = [&](int cls, int team) {
        int n = 0;
        for (const game::Unit& u : w.units()) n += !u.deleted && u.cls == cls && (team < 0 || u.team == team);
        return n;
    };
    const int linf_cls = w.data().unit_index("FRENCH_LIGHT_INFANTRY");
    const int before = count(linf_cls, joan.team);
    for (int t = 0; t < 600 && count(linf_cls, joan.team) < before + 2; ++t) s.tick({}), s.take_world_events(), s.take_mission_events();
    std::fprintf(stderr, "light infantry in troop %d: %d -> %d\n", joan.team, before, count(linf_cls, joan.team));
    CHECK(count(linf_cls, joan.team) == before + 2);

    // ---- Siege workshop (NORMAL: gold) ----
    const int wf = open("workshop1");
    CHECK(wf > 0 && hud.workshop_screen == wf && flow.paused());
    settle({900, 740});
    const game::Facility* wk = pl.facility(wf);
    CHECK(win.open() && win.workshop && wk && !wk->products.empty());
    if (wk && !wk->products.empty()) {
        const int cannon = w.data().unit_index("CANNON");
        CHECK(wk->products[0].type == cannon + 1);
        const int gold = m.state().gold, price = w.data().units[std::size_t(cannon)].gold;
        settle({300, 0x68 + 50});
        click({0x238 + 18, 0x68 + 0x14 + 14});
        CHECK(win.add[0] == 1 && win.cost == price && win.total == 1);
        click({0x28b + 78, 0x2be + 22});   // BUILD
        settle({900, 740});
        std::fprintf(stderr, "workshop: gold %d -> %d, builds %zu\n", gold, m.state().gold, wk->builds.size());
        CHECK(m.state().gold == gold - price && wk->builds.size() == 1 && !flow.paused());
        const int cannons = count(cannon, -1);
        for (int t = 0; t < 300 && count(cannon, -1) == cannons; ++t) s.tick({}), s.take_world_events(), s.take_mission_events();
        std::fprintf(stderr, "cannons %d -> %d, built %d\n", cannons, count(cannon, -1), wk->products[0].built);
        CHECK(count(cannon, -1) == cannons + 1 && wk->products[0].built == 1 && wk->builds.empty());
    }

    // ---- Bag-grid cells persist (ItemInstance bag/col/row, Mission "INVG"): personal + troop bag, and the carried
    // heroes of the campaign profile (MISN keeps no cells) ----
    joan.inv.bag.clear();
    CHECK(w.give_item(joan, "JOAN_SWORD_LV2", 0, 1) && w.give_item(joan, "fd_bread_s", 0, 1));
    joan.inv.bag[0].bag = 1, joan.inv.bag[0].col = 3, joan.inv.bag[0].row = 4;
    joan.inv.bag[1].bag = 2, joan.inv.bag[1].col = 2, joan.inv.bag[1].row = 3;   // troop bag 6x4
    m.profile().heroes.push_back({"JOAN_OF_ARC", joan});
    const game::SaveFile saved = m.save();
    CHECK(saved.find("INVG") != nullptr);
    joan.inv.bag[0].col = joan.inv.bag[1].col = 5, joan.inv.bag[0].bag = -1;
    m.profile().heroes.back().unit.inv.bag[1].bag = -1;
    std::string err;
    CHECK(m.load(saved, &err));
    CHECK(m.profile().heroes.size() == 1 && m.profile().heroes[0].unit.inv.bag.size() == 2 &&
          m.profile().heroes[0].unit.inv.bag[1].bag == 2 && m.profile().heroes[0].unit.inv.bag[1].col == 2);
    m.profile().heroes.clear();
    game::Unit& j2 = *w.unit(w.player_id());
    CHECK(j2.inv.bag.size() == 2);
    if (j2.inv.bag.size() == 2) {
        CHECK(j2.inv.bag[0].bag == 1 && j2.inv.bag[0].col == 3 && j2.inv.bag[0].row == 4);
        CHECK(j2.inv.bag[1].bag == 2 && j2.inv.bag[1].col == 2 && j2.inv.bag[1].row == 3);
        CharacterWindow cw;
        flow.action(35);
        quads.clear();
        ui.begin({0, 0, 1024, 768}, quads, UiInput{}, 0.033f);
        cw.draw(ui, m, hud);
        CHECK(cw.cells().size() == 2);
        if (cw.cells().size() == 2)   // the troop item stays in the troop grid (hud grid 2)
            CHECK(cw.cells()[0].grid == 0 && cw.cells()[0].col == 3 && cw.cells()[0].row == 4 && cw.cells()[1].grid == 2 &&
                  cw.cells()[1].col == 2 && cw.cells()[1].row == 3);
    }
    return test_result();
}
