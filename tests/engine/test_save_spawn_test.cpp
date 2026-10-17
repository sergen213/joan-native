// Units running in place (alpha.17), headless, mission 3 (S2L1). TestSaves `spawn` hook (Autoplay::apply_item_hook,
// tools/make_test_saves.py): "Dock" (margin 0: the default one steps all the way back to the start) lands beside the
// dock, off the navmesh CharPhysics walks; the hero and her party must be put on it, else none of them can ever move.
// Then the hero, held forward, moves, and without input stands while her troop's APPROACH_ENEMY reaches her.
#include "check.hpp"
#include "engine/session.hpp"
#include "engine/session_autoplay.hpp"
#include "game/ai_system.hpp"
#include "game/mission.hpp"
#include "game/multimap_nav.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace joa;
using namespace joa::engine;

int main() {
    const char* game_dir = std::getenv("JOA_GAME_DIR");
    if (!game_dir || !formats::Vfs(game_dir).exists("scenario/scenario_03.scr")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    Session s{formats::Vfs(game_dir)};
    s.seed = 1;
    std::string err;
    if (!s.new_game(3, 1, &err)) return std::fprintf(stderr, "new_game: %s\n", err.c_str()), 1;
    game::Mission& m = *s.mission();
    m.autosave = false;
    for (int t = 0; t < 30; ++t) s.tick({}), s.take_world_events(), s.take_mission_events();
    m.ui().cs.active = false, m.ui().game_cut_scene = false;
    game::World& w = m.world();
    const game::NavWorld* nav = m.ai().nav();
    CHECK(nav && Autoplay::apply_item_hook(m, {"spawn", "Dock@0"}));
    if (!nav || g_failures) return test_result();

    const game::Unit& joan = *w.unit(w.player_id());
    int near = 0;
    for (const game::Unit& u : w.units())
        if (!u.deleted && std::hypot(u.pos.x - joan.pos.x, u.pos.y - joan.pos.y) < 400.0f) {
            const bool on = nav->find_poly_xy(u.pos.x, u.pos.y, u.pos.z) >= 0;
            std::printf("unit %d at %.0f,%.0f,%.0f on navmesh %d\n", u.id, u.pos.x, u.pos.y, u.pos.z, int(on));
            CHECK(on);
            ++near;
        }
    CHECK(near >= 4);  // Joan, Jean de Metz and troops

    const game::Vec3 from = joan.pos;
    for (int t = 0; t < 30; ++t) {
        game::InputFrame f;
        f.held.set(game::IN_FORWARD);
        s.tick(f), s.take_world_events(), s.take_mission_events();
    }
    const float moved = std::hypot(joan.pos.x - from.x, joan.pos.y - from.y);
    std::printf("hero held forward 30 ticks: moved %.0f\n", moved);
    CHECK(moved > 100.0f);

    // 0x52daa0: the steered hero follows no path. A troop member's APPROACH_ENEMY reaches her as its leader; without
    // input she must stand, not RUN in place (the controller stops her, the path follower re-set RUN every tick).
    game::Unit* member = w.unit(joan.members.empty() ? 0 : joan.members.front());
    const game::Unit* foe = nullptr;
    for (const game::Unit& u : w.units())
        if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && (!foe || std::hypot(u.pos.x - joan.pos.x, u.pos.y - joan.pos.y) <
                                                                             std::hypot(foe->pos.x - joan.pos.x, foe->pos.y - joan.pos.y)))
            foe = &u;
    CHECK(member && foe);
    if (!member || !foe) return test_result();
    m.ai().give_order(*member, game::order::APPROACH_ENEMY, 1, false, foe->id, 60, 60, 0, 30000);
    for (int t = 0; t < 30; ++t) s.tick({}), s.take_world_events(), s.take_mission_events();
    std::printf("hero after her troop's APPROACH_ENEMY, no input: action %s\n", game::action_name(joan.action));
    CHECK(!game::is_locomotion(joan.action));
    return test_result();
}
