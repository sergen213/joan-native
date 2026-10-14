// Headless game flow (no window): main menu NEW CAMPAIGN (Normal) -> loading -> mission 1 runs 3000 ticks
// under synthetic keyboard/mouse input -> F5 quick save -> more play -> F6 + OK quick load (state hash equal)
// -> Esc menu pauses -> forced win (objectives finished, normal win delay) -> CONTINUE -> mission 2 (S3L1).
#include "check.hpp"
#include "engine/game_input.hpp"
#include "engine/menu_options_store.hpp"
#include "engine/session_flow.hpp"

#include <SDL3/SDL_mouse.h>

#include <cstdlib>
#include <filesystem>

using namespace joa;
using namespace joa::engine;

namespace {

// One tick of input: the device state of a frame fed through the real binding map.
game::InputFrame tick_input(GameInput& in, int t) {
    RawInput raw;
    raw.keys[SDL_SCANCODE_W] = (t / 200) % 3 != 2;      // run, pause, run ...
    raw.keys[SDL_SCANCODE_A] = (t / 90) % 4 == 1;       // strafe now and then
    raw.mouse_buttons[SDL_BUTTON_LEFT] = (t / 7) % 5 == 0;   // light attacks (AAAB combos)
    raw.mouse_buttons[SDL_BUTTON_RIGHT] = (t / 7) % 9 == 4;  // heavy
    raw.mouse_dx = (t / 150) % 2 ? 3.0f : -2.0f;        // turn
    in.feed(raw, float(kGameTickSeconds));
    return in.take();
}

}  // namespace

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !formats::Vfs(game).exists("scenario/scenario_01.scr")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    const char* out = std::getenv("JOA_TEST_OUT");
    const std::filesystem::path xdg = std::filesystem::path(out ? out : ".") / "flow-xdg";
    std::filesystem::remove_all(xdg);
    setenv("XDG_DATA_HOME", xdg.c_str(), 1);
    setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);

    const formats::Vfs vfs(game);
    Session s(vfs);
    s.seed = 1;
    MenuModel menu;
    HudModel hud;
    GameFlow flow(s, menu, hud);
    flow.persist_options = false;
    load_options(vfs, options_ini_path(), menu.options, menu.bindings);
    GameInput input;
    input.configure(menu.bindings, menu.options, {}, false);

    CHECK(flow.state() == FlowState::Menu && menu.screen == MenuScreen::Main);
    flow.command({UiCommand::Type::NewGame, 1});  // MAINMENU New Campaign -> Difficulty Normal
    CHECK(flow.state() == FlowState::Loading && flow.loading_mission() == 1);
    CHECK(flow.run_pending());
    CHECK(flow.state() == FlowState::Playing && s.mission() && s.mission()->scenario_no() == 1);
    if (g_failures) return test_result();
    s.mission()->autosave = false;
    CHECK(!flow.paused() && menu.screen == MenuScreen::None);

    // Mission 1 under input: Joan moves away from the start and fights.
    const game::Vec3 start = s.mission()->world().unit(s.mission()->world().player_id())->pos;
    int hits = 0;
    for (int t = 0; t < 3000; ++t) {
        s.tick(tick_input(input, t));
        for (const auto& e : s.take_world_events()) hits += e.type == game::Ev::Action;
        s.take_mission_events();
        flow.update();
    }
    const game::Unit* joan = s.mission()->world().unit(s.mission()->world().player_id());
    CHECK(joan != nullptr);
    if (joan) std::fprintf(stderr, "after 3000 ticks: Joan at %.0f,%.0f (start %.0f,%.0f) hp %d, %d actions, state %s\n", joan->pos.x,
                           joan->pos.y, start.x, start.y, joan->hp, hits, flow_state_name(flow.state()));
    CHECK(joan && (std::abs(joan->pos.x - start.x) + std::abs(joan->pos.y - start.y)) > 200.0f);
    CHECK(hits > 10);
    CHECK(s.ticks() >= 3000);

    // F5 quick save, keep playing, F6 -> prompt -> OK.
    flow.action(53);
    CHECK(std::filesystem::exists(Session::quick_save_path()));
    const std::uint64_t saved_hash = s.mission()->hash(), saved_ticks = s.ticks();
    for (int t = 0; t < 120; ++t) s.tick(tick_input(input, 3000 + t));
    CHECK(s.mission()->hash() != saved_hash);
    flow.action(54);
    CHECK(hud.show_quickload_prompt && flow.paused());
    const int gen = s.generation();
    flow.command({UiCommand::Type::QuickLoad});
    CHECK(flow.state() == FlowState::Loading && flow.run_pending());
    CHECK(flow.state() == FlowState::Playing && s.generation() == gen + 1);
    CHECK(s.ticks() == saved_ticks);
    CHECK(s.mission()->hash() == saved_hash);
    std::fprintf(stderr, "quick load: tick %llu hash %016llx (saved %016llx)\n", static_cast<unsigned long long>(s.ticks()),
                 static_cast<unsigned long long>(s.mission()->hash()), static_cast<unsigned long long>(saved_hash));
    s.mission()->autosave = false;

    // Esc opens the in-game menu (sim paused), Resume closes it.
    flow.action(34);
    if (s.mission()->ui().game_cut_scene) flow.action(34);  // first Esc may skip a cut scene
    CHECK(menu.screen == MenuScreen::InGame && flow.paused());
    flow.command({UiCommand::Type::Resume});
    CHECK(!flow.paused());

    // Forced win through the normal win-delay path, then CONTINUE.
    s.mission()->finish_objectives();
    for (int t = 0; t < 3000 && flow.state() == FlowState::Playing; ++t) s.tick({}), flow.update();
    CHECK(flow.state() == FlowState::Won && flow.paused());
    flow.command({UiCommand::Type::ContinueCampaign});
    CHECK(flow.state() == FlowState::Loading && flow.loading_mission() == 2);
    CHECK(flow.run_pending());
    CHECK(flow.state() == FlowState::Playing && s.mission()->scenario_no() == 2 && s.mission()->info().stem() == "S3L1");
    s.mission()->autosave = false;
    for (int t = 0; t < 60; ++t) s.tick(tick_input(input, t));
    CHECK(s.mission()->world().unit(s.mission()->world().player_id()) != nullptr);
    flow.command({UiCommand::Type::QuitToMain});
    CHECK(flow.state() == FlowState::Menu && menu.screen == MenuScreen::Main && !menu.in_game);
    return test_result();
}
