// Hero switching in action mode (PlayerCtrl_process 0x42e1a0 -> Ctrl_switch_commander 0x432300), headless: action 24
// (`) cycles JOAN -> JEAN (mission 1 has neither La Hire nor Alencon) and back, 25 / 26 pick a hero; the controller
// (ai_controlled), camera 0 and the HUD player panel / party selection follow World::player_id. Mission 1 needs the
// +0x79 unlock (scenario > 1 otherwise); range mode blocks the keys. Mission 7: 28 makes Alencon the player, and R at
// an IRON / WOOD workshop then opens the workshop screen (workshop_handler 0x43c080 accepts only Alencon there).
#include "check.hpp"
#include "engine/game_camera.hpp"
#include "engine/game_input.hpp"
#include "engine/game_ui_bridge.hpp"
#include "engine/session.hpp"
#include "game/mission.hpp"

#include <cstdio>
#include <cstdlib>

using namespace joa;
using namespace joa::engine;

namespace {

game::Unit* by_code(game::World& w, const char* code) {
    for (game::Unit& u : w.units())
        if (!u.deleted && u.cls >= 0 && w.data().units[std::size_t(u.cls)].code == code) return &u;
    return nullptr;
}

// Interact events ("workshop_screen" / ...) since the last call.
std::vector<game::MissionEvent> interacts(Session& s) {
    std::vector<game::MissionEvent> out = s.take_mission_events();
    for (auto& e : s.mission()->take_events()) out.push_back(e);
    std::erase_if(out, [](const game::MissionEvent& e) { return e.kind != game::MissionEvent::Kind::Interact; });
    return out;
}

}  // namespace

int main() {
    const char* game_dir = std::getenv("JOA_GAME_DIR");
    if (!game_dir || !formats::Vfs(game_dir).exists("scenario/scenario_07.scr")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    Session s{formats::Vfs(game_dir)};
    s.seed = 1;
    std::string err;
    if (!s.new_game(1, 1, &err)) return std::fprintf(stderr, "new_game: %s\n", err.c_str()), 1;
    GameCamera cam;
    GameInput in;
    const auto start = [&] {
        game::Mission& m = *s.mission();
        m.autosave = false;
        for (int t = 0; t < 30; ++t) s.tick({}), s.take_world_events(), s.take_mission_events();
        m.ui().cs.active = false, m.ui().game_cut_scene = false;
        cam.reset(m);
    };
    const auto tick = [&](int key = 0, int n = 1) {
        for (int i = 0; i < n; ++i) {
            game::InputFrame f;
            if (key && i == 0) f.press(key);
            cam.before_tick(*s.mission(), f);
            s.tick(f);
            cam.after_tick(*s.mission(), in);
        }
    };
    start();
    game::World& w = s.mission()->world();
    game::Unit* joan = by_code(w, "JOAN_OF_ARC");
    game::Unit* jean = by_code(w, "JEAN_DE_METZ");
    CHECK(joan && jean && w.player_id() == joan->id);
    if (!joan || !jean) return test_result();

    // Gate: mission 1 (scenario 1) without the unlock -> nothing.
    tick(24);
    CHECK(w.player_id() == joan->id);
    cam.rts_unlocked = true;
    tick(24);  // ` -> next hero in JOAN, JEAN, LAHIRE, ALENCON order
    std::printf("switch: player %d (jean %d) joan ai %d jean ai %d\n", w.player_id(), jean->id, int(joan->ai_controlled), int(jean->ai_controlled));
    CHECK(w.player_id() == jean->id && w.controller().unit == jean->id && !jean->ai_controlled && joan->ai_controlled);
    tick(0, 5);
    const glm::vec2 piv(cam.state().pivot);
    std::printf("camera pivot %.0f,%.0f jean at %.0f,%.0f\n", piv.x, piv.y, jean->pos.x, jean->pos.y);
    CHECK(glm::distance(piv, glm::vec2(jean->pos.x, jean->pos.y)) < 5.0f);
    GameUiBridge bridge(s.vfs());
    HudModel hud;
    bridge.reset(*s.mission(), hud);
    bridge.fill(*s.mission(), s.current(), glm::mat4(1.0f), {0, 0, 1024, 768}, hud);
    bool party_sel = false;
    for (const HudUnit& h : hud.party) party_sel |= h.selected && h.name == jean->name;
    std::printf("hud player %s, party selected jean %d\n", hud.player.name.c_str(), int(party_sel));
    CHECK(hud.player.name == jean->name && party_sel);
    tick(24);  // only two heroes in mission 1: back to Joan
    CHECK(w.player_id() == joan->id && jean->ai_controlled && !joan->ai_controlled);
    tick(26);  // Switch to Jean de Metz
    CHECK(w.player_id() == jean->id);
    tick(26);  // already Jean: nothing
    CHECK(w.player_id() == jean->id);
    w.controller().range_mode = true;  // +0xac: no switch keys in range mode
    tick(25);
    CHECK(w.player_id() == jean->id);
    w.controller().range_mode = false;
    tick(25);
    CHECK(w.player_id() == joan->id);

    // Mission 7: Alencon and the IRON / WOOD workshops.
    if (!s.new_game(7, 1, &err)) return std::fprintf(stderr, "new_game 7: %s\n", err.c_str()), 1;
    cam.rts_unlocked = false;  // scenario 7 > 1
    start();
    game::Mission& m7 = *s.mission();
    game::World& w7 = m7.world();
    game::Unit* al = by_code(w7, "ALENCON");
    const int joan7 = w7.player_id();
    int iron = 0;
    for (int i = 1; m7.places().facility(i); ++i)
        if (const game::Facility* f = m7.places().facility(i); f->kind == game::Facility::Workshop && f->workshop_type != 0 && !iron) iron = i;
    std::printf("mission 7: alencon %d, iron/wood workshop %d\n", al ? al->id : 0, iron);
    CHECK(al && iron);
    if (!al || !iron) return test_result();
    const game::Facility& fc = *m7.places().facility(iron);
    const auto warp = [&](game::Unit& u) {
        u.pos = {fc.pos.x, fc.pos.y, w7.terrain() ? w7.terrain()->height(fc.pos.x, fc.pos.y) : fc.pos.z};
    };
    // Joan at the workshop: R opens nothing there (not Alencon).
    warp(*w7.unit(joan7));
    interacts(s);
    w7.interact(*w7.unit(joan7));
    bool opened = false;
    for (const auto& e : interacts(s)) opened |= e.text == "workshop_screen";
    CHECK(!opened);
    tick(28);  // Switch to the Duke of Alencon
    std::printf("player %d (alencon %d)\n", w7.player_id(), al->id);
    CHECK(w7.player_id() == al->id && !al->ai_controlled);
    warp(*al);
    w7.interact(*al);
    for (const auto& e : interacts(s)) {
        std::printf("interact %s a %d b %d\n", e.text.c_str(), e.a, e.b);
        opened |= e.text == "workshop_screen" && e.a == al->id && e.b == iron;
    }
    CHECK(opened);
    return test_result();
}
