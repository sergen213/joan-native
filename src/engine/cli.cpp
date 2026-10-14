#include "engine/cli.hpp"

#include <charconv>
#include <fstream>
#include <sstream>

namespace joa::engine {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

template <class T>
bool number(std::string_view s, T& out) {
    s = trim(s);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool renderer_from(std::string_view v, RendererKind& out) {
    if (v == "gl" || v == "opengl") return out = RendererKind::OpenGL, true;
    if (v == "vulkan" || v == "vk") return out = RendererKind::Vulkan, true;
    return false;
}

}  // namespace

const char* usage() {
    return "usage: joan-native [options]\n"
           "  --renderer gl|vulkan     rendering backend (default gl)\n"
           "  --width N --height N     window size in logical pixels (default 1280x720)\n"
           "  --fullscreen             exclusive fullscreen at the closest mode to width x height\n"
           "  --borderless             borderless fullscreen at desktop resolution\n"
           "  --aspect W:H|auto        force a display aspect (letter/pillarbox), e.g. 4:3\n"
           "  --game-dir DIR           original game installation\n"
           "  --settings FILE          launcher settings.ini (explicit flags override it)\n"
           "  --input-config FILE      input bindings file\n"
           "  --screenshot FILE.png    render --frames N frames (default 5), save PNG, exit\n"
           "  --frames N               stop after N frames (N simulation ticks with --headless)\n"
           "  --headless               simulation only, no window; prints the final state hash\n"
           "  --gpu N                  Vulkan device index (default: prefer discrete; devices are logged)\n"
           "  --present-mode M         Vulkan: fifo (default, vsync) | relaxed | mailbox | immediate\n"
           "  --vk-validation          enable Khronos validation layer (errors are counted and logged)\n"
           "  --gpu-timing             log mean GPU timestamps per pass at exit (Vulkan)\n"
           "  --scenario NAME|PATH     load an original map (e.g. S1L1 or map/scenario1/S1L1) with Joan\n"
           "  --free-camera            fly camera (move/look keys) instead of following Joan\n"
           "  --camera X,Y,Z[,H[,P]]   free camera pose: original units, heading/pitch in degrees\n"
           "  --anim CLIP              force Joan's animation clip (e.g. f_run, f_idle)\n"
           "  --ui-screen NAME         show an original interface screen: main difficulty credits options video audio\n"
           "                           input game remap load save ingame ingame-options hud msglog brief quickload\n"
           "  --play-movie FILE        play a movie (e.g. movie/intro.wmv, relative to the game dir); Esc/click skips\n"
           "  --movie-time SEC         start the movie at SEC (with --screenshot: capture the frame shown at SEC)\n";
}

bool parse_aspect(std::string_view t, float& out) {
    t = trim(t);
    if (t == "auto" || t == "0") return out = 0.0f, true;
    if (const auto c = t.find(':'); c != std::string_view::npos) {
        float w = 0, h = 0;
        if (!number(t.substr(0, c), w) || !number(t.substr(c + 1), h) || w <= 0 || h <= 0) return false;
        return out = w / h, true;
    }
    float r = 0;
    if (!number(t, r) || r <= 0) return false;
    return out = r, true;
}

std::string apply_settings_ini(std::string_view ini, Options& o) {
    std::istringstream in{std::string(ini)};
    std::string line, section;
    while (std::getline(in, line)) {
        std::string_view l = trim(line);
        if (l.empty() || l.front() == ';' || l.front() == '#') continue;
        if (l.front() == '[') {
            section = std::string(trim(l.substr(1, l.find(']') - 1)));
            continue;
        }
        const auto eq = l.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key = section + "." + std::string(trim(l.substr(0, eq)));
        const std::string_view v = trim(l.substr(eq + 1));
        int i = 0;
        float f = 0;
        if (key == "renderer.backend") {
            if (!renderer_from(v, o.renderer)) return "settings: bad renderer.backend";
        } else if (key == "display.width" && number(v, i)) {
            if (i > 0) o.width = i;
        } else if (key == "display.height" && number(v, i)) {
            if (i > 0) o.height = i;
        } else if (key == "display.aspect") {
            if (!parse_aspect(v, o.aspect)) return "settings: bad display.aspect";
        } else if (key == "display.mode") {
            if (v == "windowed") o.mode = WindowMode::Windowed;
            else if (v == "fullscreen") o.mode = WindowMode::Fullscreen;
            else if (v == "borderless") o.mode = WindowMode::Borderless;
            else return "settings: bad display.mode";
        } else if (key == "controller.deadzone" && number(v, f)) {
            o.deadzone = f;
        } else if (key == "controller.sensitivity" && number(v, f)) {
            o.sensitivity = f;
        } else if (key == "paths.game_dir" && !v.empty()) {
            o.game_dir = std::string(v);
        }
        // Other launcher keys (preset, raytracing, update) do not affect this build yet.
    }
    return {};
}

std::string parse_options(int argc, const char* const* argv, Options& o) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) != "--settings") continue;
        o.settings = argv[i + 1];
        std::ifstream f(o.settings);
        if (!f) return "cannot read settings file " + o.settings;
        std::stringstream ss;
        ss << f.rdbuf();
        if (auto err = apply_settings_ini(ss.str(), o); !err.empty()) return err;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&](std::string_view& v) {
            if (i + 1 >= argc) return false;
            v = argv[++i];
            return true;
        };
        std::string_view v;
        if (a == "--help" || a == "-h") o.help = true;
        else if (a == "--fullscreen") o.mode = WindowMode::Fullscreen;
        else if (a == "--borderless") o.mode = WindowMode::Borderless;
        else if (a == "--headless") o.headless = true;
        else if (a == "--vk-validation") o.vk_validation = true;
        else if (a == "--gpu-timing") o.gpu_timing = true;
        else if (a == "--gpu") {
            if (!value(v) || !number(v, o.gpu) || o.gpu < 0) return "--gpu expects a device index";
        } else if (a == "--present-mode") {
            if (!value(v)) return "--present-mode expects fifo|relaxed|mailbox|immediate";
            if (v == "fifo") o.present_mode = PresentMode::Fifo;
            else if (v == "relaxed") o.present_mode = PresentMode::Relaxed;
            else if (v == "mailbox") o.present_mode = PresentMode::Mailbox;
            else if (v == "immediate") o.present_mode = PresentMode::Immediate;
            else return "--present-mode expects fifo|relaxed|mailbox|immediate";
        } else if (a == "--renderer") {
            if (!value(v) || !renderer_from(v, o.renderer)) return "--renderer expects gl|vulkan";
        } else if (a == "--width") {
            if (!value(v) || !number(v, o.width) || o.width <= 0) return "--width expects a positive integer";
        } else if (a == "--height") {
            if (!value(v) || !number(v, o.height) || o.height <= 0) return "--height expects a positive integer";
        } else if (a == "--aspect") {
            if (!value(v) || !parse_aspect(v, o.aspect)) return "--aspect expects W:H, a ratio, or auto";
        } else if (a == "--frames") {
            if (!value(v) || !number(v, o.frames) || o.frames < 0) return "--frames expects a non-negative integer";
        } else if (a == "--game-dir") {
            if (!value(v)) return "--game-dir expects a directory";
            o.game_dir = std::string(v);
        } else if (a == "--settings") {
            if (!value(v)) return "--settings expects a file";
        } else if (a == "--input-config") {
            if (!value(v)) return "--input-config expects a file";
            o.input_config = std::string(v);
        } else if (a == "--screenshot") {
            if (!value(v)) return "--screenshot expects a PNG path";
            o.screenshot = std::string(v);
        } else if (a == "--scenario") {
            if (!value(v) || v.empty()) return "--scenario expects a map name (S1L1) or path";
            o.scenario = std::string(v);
        } else if (a == "--free-camera") {
            o.free_camera = true;
        } else if (a == "--camera") {
            if (!value(v)) return "--camera expects X,Y,Z[,HEADING[,PITCH]]";
            o.camera_pose.clear();
            for (std::size_t p = 0; p <= v.size();) {
                const std::size_t c = std::min(v.find(',', p), v.size());
                float f = 0;
                if (!number(v.substr(p, c - p), f)) return "--camera expects X,Y,Z[,HEADING[,PITCH]]";
                o.camera_pose.push_back(f);
                p = c + 1;
            }
            if (o.camera_pose.size() < 3 || o.camera_pose.size() > 5) return "--camera expects X,Y,Z[,HEADING[,PITCH]]";
        } else if (a == "--anim") {
            if (!value(v)) return "--anim expects a clip name";
            o.anim = std::string(v);
        } else if (a == "--ui-screen") {
            if (!value(v) || v.empty()) return "--ui-screen expects a screen name (see --help)";
            o.ui_screen = std::string(v);
        } else if (a == "--play-movie") {
            if (!value(v) || v.empty()) return "--play-movie expects a file";
            o.movie = std::string(v);
        } else if (a == "--movie-time") {
            if (!value(v) || !number(v, o.movie_time) || o.movie_time < 0) return "--movie-time expects seconds >= 0";
        } else return "unknown option " + std::string(a);
    }
    if (o.frames == 0) o.frames = !o.screenshot.empty() ? 5 : o.headless ? 300 : 0;
    return {};
}

}  // namespace joa::engine
