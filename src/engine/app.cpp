#include "engine/app.hpp"

#include "engine/audio_system.hpp"
#include "engine/camera.hpp"
#include "engine/demo_scene.hpp"
#include "engine/input.hpp"
#include "engine/platform.hpp"
#include "engine/png.hpp"
#include "engine/profile.hpp"
#include "engine/renderdoc.hpp"
#include "engine/renderer_gl.hpp"
#include "engine/renderer_vk.hpp"
#include "engine/session_client.hpp"
#include "engine/sim.hpp"
#include "engine/ui_runner.hpp"
#include "engine/video_player.hpp"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace joa::engine {
namespace {

std::unique_ptr<IRenderer> make_renderer(Platform& platform, const Options& o) {
    auto r = o.renderer == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
    r->set_graphics(o.graphics.resolve());
    return r;
}

std::filesystem::path user_input_config() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::filesystem::path(x) / "joan-native/input.cfg";
    if (const char* h = std::getenv("HOME"); h && *h) return std::filesystem::path(h) / ".config/joan-native/input.cfg";
    return {};
}

bool load_input(const Options& o, InputMap& map) {
    map = InputMap::defaults();
    std::filesystem::path path = o.input_config.empty() ? user_input_config() : std::filesystem::path(o.input_config);
    std::error_code ec;
    if (!path.empty() && (std::filesystem::exists(path, ec) || !o.input_config.empty())) {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string err;
        if (!f || !map.parse(ss.str(), &err)) {
            std::fprintf(stderr, "joan-native: input config %s: %s\n", path.c_str(), f ? err.c_str() : "cannot read");
            return false;
        }
    }
    if (o.deadzone >= 0.0f) map.stick_deadzone = std::clamp(o.deadzone, 0.0f, 0.9f);
    map.mouse_sensitivity *= o.sensitivity;
    map.pad_look_speed *= o.sensitivity;
    return true;
}

// Procedural demo scene (no game data) and the standalone original-interface screens (--ui-screen).
int run_demo(const Options& o) {
    InputMap input;
    if (!load_input(o, input)) return 1;
    const bool capture = !o.screenshot.empty();
    Platform platform(o, capture);
    std::unique_ptr<IRenderer> renderer = make_renderer(platform, o);
    DemoScene scene(*renderer);
    const formats::Vfs ui_vfs(o.game_dir.empty() ? std::filesystem::path(".") : std::filesystem::path(o.game_dir));
    std::unique_ptr<UiRunner> ui;
    UiInputTracker ui_input;
    if (!o.ui_screen.empty()) {
        ui = std::make_unique<UiRunner>(ui_vfs, *renderer, o.ui_screen);
        if (!capture) {  // UI sound effects (Sfx bus = options Sound Volume)
            AudioSettings a;
            a.sfx = float(ui->menu.options.sound_volume) / 100.0f;
            ui->audio = std::make_unique<AudioSystem>(ui_vfs, a);
        }
        SDL_HideCursor();
    }

    World previous, current;
    FixedStep clock;
    FollowCamera camera;
    RawInput raw;
    ActionState actions;
    std::vector<DrawItem> draws;
    std::vector<UiQuad> quads;
    std::uint64_t last_ns = SDL_GetTicksNS();

    for (int frame = 0; o.frames == 0 || frame < o.frames; ++frame) {
        renderdoc_frame(frame);
        if (!platform.pump(raw)) break;
        const std::uint64_t now_ns = SDL_GetTicksNS();
        // Captures use a fixed frame time so the same command line renders the same image.
        const double frame_dt = capture ? kTickSeconds : double(now_ns - last_ns) * 1e-9;
        last_ns = now_ns;

        input.evaluate(raw, actions, float(frame_dt));
        raw.end_frame();
        if (!ui && actions.pressed(Action::Pause)) break;  // without the UI, Escape quits
        if (actions.pressed(Action::ToggleFullscreen)) platform.toggle_fullscreen();
        platform.set_mouse_look(actions.down(Action::MouseLook));

        camera.yaw -= actions[Action::LookX];
        camera.pitch = std::clamp(camera.pitch + actions[Action::LookY], glm::radians(5.0f), glm::radians(80.0f));
        camera.distance = std::clamp(camera.distance - actions[Action::Zoom], 3.0f, 30.0f);

        const PlayerCommand cmd{actions[Action::MoveX], actions[Action::MoveY], camera.yaw, actions.down(Action::Run)};
        for (int n = clock.advance(frame_dt); n > 0; --n) {
            previous = current;
            step(current, cmd, float(clock.tick));
        }
        const World shown = interpolate(previous, current, clock.alpha());

        const glm::ivec2 size = platform.pixel_size();
        const glm::ivec4 vp = fit_viewport(size, o.aspect);
        const float aspect = float(vp.z) / float(std::max(vp.w, 1));
        FrameData f;
        f.view.projection = perspective_hor_plus(glm::radians(kOriginalVerticalFovDeg), aspect, kDefaultNear, kDefaultFar);
        f.view.viewport = vp;
        camera.update(shown.player_position, float(frame_dt));
        draws.clear();
        quads.clear();
        scene.build(shown, ui_layout(vp), draws, quads);
        f.view.view = camera.view();
        f.view.camera_position = camera.eye;
        f.lighting = scene.lighting();
        if (ui) {
            quads.clear();
            if (!ui->show_hud) draws.clear(), f.lighting = Lighting{};  // menus: black behind the 4:3 canvas
            if (!ui->frame(vp, ui_input.update(platform.window(), raw), float(frame_dt), quads)) break;
        }
        f.draws = draws;
        f.ui = quads;
        renderer->begin_frame(size);
        renderer->submit(f);
        if (capture && frame + 1 == o.frames) {
            const Image img = renderer->read_back();
            if (!write_png(o.screenshot, img.width, img.height, img.rgba)) {
                std::fprintf(stderr, "joan-native: cannot write %s\n", o.screenshot.c_str());
                return 1;
            }
            std::fprintf(stderr, "joan-native: wrote %s (%dx%d)\n", o.screenshot.c_str(), img.width, img.height);
        }
        renderer->end_frame();
        JOA_FRAME_MARK();
    }
    renderdoc_report();
    return 0;
}

// --play-movie: one movie, then exit. Audio uses the default device unless capturing a screenshot.
int run_movie(const Options& o) {
    const formats::Vfs vfs(o.game_dir.empty() ? std::filesystem::path(".") : std::filesystem::path(o.game_dir));
    const auto path = std::filesystem::exists(o.movie) ? std::optional(std::filesystem::path(o.movie)) : vfs.resolve(o.movie);
    if (!path) {
        std::fprintf(stderr, "joan-native: movie %s not found\n", o.movie.c_str());
        return 1;
    }
    const bool capture = !o.screenshot.empty();
    Platform platform(o, capture);
    std::unique_ptr<IRenderer> renderer = make_renderer(platform, o);
    std::unique_ptr<AudioSystem> audio = capture ? nullptr : std::make_unique<AudioSystem>(vfs);
    return play_movie(platform, *renderer, audio ? &audio->mixer() : nullptr, *path, {o.movie_time, o.screenshot}) ? 0 : 1;
}

}  // namespace

int run(const Options& o) {
    if (o.help) {
        std::fputs(usage(), stdout);
        return 0;
    }
    if (!o.game_dir.empty() && !std::filesystem::is_directory(o.game_dir)) {
        std::fprintf(stderr, "joan-native: --game-dir %s is not a directory\n", o.game_dir.c_str());
        return 1;
    }
    renderdoc_init(o);  // before any window / GPU object so RenderDoc hooks the API
    try {
        if (!o.movie.empty()) return run_movie(o);
        const formats::Vfs vfs(o.game_dir.empty() ? std::filesystem::path(".") : std::filesystem::path(o.game_dir));
        const bool game = o.ui_screen.empty() && vfs.exists("scenario/scenario_01.scr");
        const bool wants_game = o.headless || !o.scenario.empty() || o.new_game || !o.load.empty() || !o.autoplay.empty();
        if (wants_game && !game && o.ui_screen.empty()) {
            std::fprintf(stderr, "joan-native: no original game data in %s (scenario/scenario_01.scr)\n", vfs.root().c_str());
            return 1;
        }
        return game ? run_game(o) : run_demo(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "joan-native: %s\n", e.what());
        return 1;
    }
}

}  // namespace joa::engine
