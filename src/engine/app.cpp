#include "engine/app.hpp"

#include "engine/camera.hpp"
#include "engine/demo_scene.hpp"
#include "engine/input.hpp"
#include "engine/platform.hpp"
#include "engine/png.hpp"
#include "engine/profile.hpp"
#include "engine/renderer_gl.hpp"
#include "engine/renderer_vk.hpp"
#include "engine/sim.hpp"
#include "engine/world_view.hpp"

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

// Deterministic scripted input for --headless runs (and their regression hashes).
PlayerCommand scripted_command(std::uint64_t tick) {
    PlayerCommand c;
    c.move_y = 1.0f;
    c.move_x = (tick / 45) % 2 ? 0.5f : -0.5f;
    c.heading = float(tick % 360) * 0.01745329f;
    c.run = (tick / 90) % 2 == 1;
    return c;
}

int run_headless(const Options& o) {
    World w;
    for (int i = 0; i < o.frames; ++i) step(w, scripted_command(w.tick), float(kTickSeconds));
    std::printf("ticks=%llu hash=%016llx pos=%.4f,%.4f,%.4f\n", static_cast<unsigned long long>(w.tick),
                static_cast<unsigned long long>(state_hash(w)), w.player_position.x, w.player_position.y, w.player_position.z);
    return 0;
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

int run_client(const Options& o) {
    InputMap input;
    if (!load_input(o, input)) return 1;
    const bool capture = !o.screenshot.empty();
    Platform platform(o, capture);
    std::unique_ptr<IRenderer> renderer =
        o.renderer == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
    DemoScene scene(*renderer);
    std::unique_ptr<WorldView> world;
    if (!o.scenario.empty()) world = std::make_unique<WorldView>(*renderer, o);
    double frame_ms_sum = 0;
    std::size_t draw_sum = 0;

    World previous, current;
    FixedStep clock;
    FollowCamera camera;
    if (world) world->setup_camera(camera);
    RawInput raw;
    ActionState actions;
    std::vector<DrawItem> draws;
    std::vector<UiQuad> quads;
    std::uint64_t last_ns = SDL_GetTicksNS();

    int frame = 0;
    for (; o.frames == 0 || frame < o.frames; ++frame) {
        if (!platform.pump(raw)) break;
        const std::uint64_t now_ns = SDL_GetTicksNS();
        // Captures use a fixed frame time so the same command line renders the same image.
        const double frame_dt = capture ? kTickSeconds : double(now_ns - last_ns) * 1e-9;
        last_ns = now_ns;

        input.evaluate(raw, actions, float(frame_dt));
        raw.end_frame();
        if (actions.pressed(Action::Pause)) break;  // no menus yet: Escape quits
        if (actions.pressed(Action::ToggleFullscreen)) platform.toggle_fullscreen();
        platform.set_mouse_look(actions.down(Action::MouseLook));

        camera.yaw -= actions[Action::LookX];
        camera.pitch = std::clamp(camera.pitch + actions[Action::LookY], glm::radians(5.0f), glm::radians(80.0f));
        camera.distance = std::clamp(camera.distance - actions[Action::Zoom], 3.0f, 30.0f);

        const PlayerCommand cmd{actions[Action::MoveX], actions[Action::MoveY], camera.yaw, actions.down(Action::Run)};
        for (int n = clock.advance(frame_dt); n > 0; --n) {
            previous = current;
            JOA_ZONE("sim.step");
            step(current, cmd, float(clock.tick));
            if (world) world->tick(current);
        }
        const World shown = interpolate(previous, current, clock.alpha());

        const glm::ivec2 size = platform.pixel_size();
        const glm::ivec4 vp = fit_viewport(size, o.aspect);
        const float aspect = float(vp.z) / float(std::max(vp.w, 1));
        FrameData f;
        f.view.projection = perspective_hor_plus(glm::radians(kOriginalVerticalFovDeg), aspect, kDefaultNear, kDefaultFar);
        f.view.viewport = vp;
        if (world) {
            draws.clear();
            quads.clear();
            world->frame(shown, clock.alpha(), camera, actions, float(frame_dt), f.view.projection, f.view, draws);
            f.lighting = world->lighting();
        } else {
            camera.update(shown.player_position, float(frame_dt));
            scene.build(shown, ui_layout(vp), draws, quads);
            f.view.view = camera.view();
            f.view.camera_position = camera.eye;
            f.lighting = scene.lighting();
        }
        f.draws = draws;
        f.ui = quads;
        renderer->begin_frame(size);
        {
            JOA_ZONE("render.submit");
            renderer->submit(f);
        }
        if (capture && frame + 1 == o.frames) {
            const Image img = renderer->read_back();
            if (!write_png(o.screenshot, img.width, img.height, img.rgba)) {
                std::fprintf(stderr, "joan-native: cannot write %s\n", o.screenshot.c_str());
                return 1;
            }
            std::fprintf(stderr, "joan-native: wrote %s (%dx%d)\n", o.screenshot.c_str(), img.width, img.height);
        }
        renderer->end_frame();
        frame_ms_sum += double(SDL_GetTicksNS() - now_ns) * 1e-6;
        draw_sum += draws.size();
        JOA_FRAME_MARK();
    }
    if (world && frame > 0)
        std::fprintf(stderr, "joan-native: %d frames, mean CPU frame %.2f ms (build+submit+present), mean draws %.0f\n", frame,
                     frame_ms_sum / frame, double(draw_sum) / frame);
    return 0;
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
    if (o.headless) return run_headless(o);
    try {
        return run_client(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "joan-native: %s\n", e.what());
        return 1;
    }
}

}  // namespace joa::engine
