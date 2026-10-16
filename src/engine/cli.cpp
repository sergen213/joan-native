#include "engine/cli.hpp"

#include <algorithm>
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
           "  --headless               no window: run the game session for --frames ticks (default 300), print the hash\n"
           "  --gpu N                  Vulkan device index (default: prefer discrete; devices are logged)\n"
           "  --present-mode M         Vulkan: fifo (default, vsync) | relaxed | mailbox | immediate\n"
           "  --vk-validation          enable Khronos validation layer (errors are counted and logged)\n"
           "  --gpu-timing             log mean GPU timestamps per pass at exit (Vulkan)\n"
           "  --renderdoc              load the RenderDoc in-app API (RENDERDOC_LIB or system librenderdoc.so)\n"
           "  --renderdoc-capture N    with RenderDoc: capture frame N (0-based; implies --renderdoc)\n"
           "  --scenario NAME          play the campaign mission of an original map (e.g. S1L1), menus skipped\n"
           "  --free-camera            fly camera (move/look keys) instead of following Joan\n"
           "  --camera X,Y,Z[,H[,P]]   free camera pose: original units, heading/pitch in degrees\n"
           "  --new-game               start a new campaign directly (no movies/menus); see --mission/--difficulty\n"
           "  --mission N              campaign mission 1..8 for --new-game (default 1)\n"
           "  --difficulty D           0 easy, 1 normal, 2 hard, 3 very hard (default: game options)\n"
           "  --load FILE              load a native save (.jsav) or import an original Save/*.SAV directly\n"
           "  --autoplay-script FILE   timed synthetic input (docs/PLAYING.md); exits after its last event\n"
           "  --seed N                 World/script RNG seed of a new game (default random)\n"
           "  --no-movies              skip the startup movies\n"
           "  --rts-unlocked           allow the RTS toggle (F2) before mission 5 (test hook)\n"
           "  --camera-fix             Camera Fix Patch mouse turn rate (5x), docs/CAMERA_AND_WIDESCREEN.md\n"
           "  --ui-screen NAME         show an original interface screen: main difficulty credits options video audio\n"
           "                           input game remap load save ingame ingame-options hud msglog brief quickload\n"
           "                           objective attention brief-background brief-objectives brief-townships\n"
           "                           brief-townships-2 brief-commanders brief-commanders-2 brief-battleground\n"
           "                           brief-battleground-2d brief-strategy brief-siege\n"
           "  --play-movie FILE        play a movie (e.g. movie/intro.wmv, relative to the game dir); Esc/click skips\n"
           "  --preset NAME            graphics preset: original (faithful, default) | enhanced | ultra | custom\n"
           "  --graphics K=V[,K=V]     graphics keys (docs/GRAPHICS_ROADMAP.md). Image-quality keys (aa=none|fxaa|smaa|taa,\n"
           "                           msaa=1|2|4|8, anisotropy=1..16, render_scale=0.5..2, upscale=none|fsr1|fsr3,\n"
           "                           fsr_quality=native|quality|balanced|performance|ultra_performance, frame_generation=0|1\n"
           "                           (FSR 3 + frame generation: Vulkan), sharpness) work with every preset; other keys\n"
           "                           (e.g. ssao=false) imply --preset custom\n"
           "  --hd-packs A[,B]         HD texture packs (names under $XDG_DATA_HOME/joan-native/hd/ or dirs), first wins\n"
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
        } else if (key == "controller.camera_fix") {
            o.camera_fix = v == "true" || v == "1";
        } else if (key == "controller.rumble") {
            o.rumble = v == "true" || v == "1";
        } else if (key.starts_with("audio.") && number(v, i)) {
            const std::string_view k = std::string_view(key).substr(6);
            i = std::clamp(i, 0, 100);
            if (k == "master") o.master_volume = i;
            else if (k == "music") o.music_volume = i;
            else if (k == "sound") o.sound_volume = i;
            else if (k == "environment") o.environment_volume = i;
        } else if (key == "paths.game_dir" && !v.empty()) {
            o.game_dir = std::string(v);
        } else if (auto err = o.graphics.apply(key, v); !err.empty()) {
            return err;  // renderer.preset, graphics.*, raytracing.* (preset.hpp)
        }
        // Other launcher keys (update) do not affect this build.
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
        else if (a == "--renderdoc") o.renderdoc = true;
        else if (a == "--renderdoc-capture") {
            if (!value(v) || !number(v, o.renderdoc_capture) || o.renderdoc_capture < 0) return "--renderdoc-capture expects a frame number";
            o.renderdoc = true;
        } else if (a == "--gpu") {
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
        } else if (a == "--new-game") {
            o.new_game = true;
        } else if (a == "--mission") {
            if (!value(v) || !number(v, o.mission) || o.mission < 1 || o.mission > 8) return "--mission expects 1..8";
        } else if (a == "--difficulty") {
            if (!value(v) || !number(v, o.difficulty) || o.difficulty < 0 || o.difficulty > 3) return "--difficulty expects 0..3";
        } else if (a == "--load") {
            if (!value(v) || v.empty()) return "--load expects a save file";
            o.load = std::string(v);
        } else if (a == "--autoplay-script") {
            if (!value(v) || v.empty()) return "--autoplay-script expects a file";
            o.autoplay = std::string(v);
        } else if (a == "--seed") {
            if (!value(v) || !number(v, o.seed)) return "--seed expects an unsigned integer";
        } else if (a == "--no-movies") {
            o.no_movies = true;
        } else if (a == "--rts-unlocked") {
            o.rts_unlocked = true;
        } else if (a == "--camera-fix") {
            o.camera_fix = true;
        } else if (a == "--ui-screen") {
            if (!value(v) || v.empty()) return "--ui-screen expects a screen name (see --help)";
            o.ui_screen = std::string(v);
        } else if (a == "--play-movie") {
            if (!value(v) || v.empty()) return "--play-movie expects a file";
            o.movie = std::string(v);
        } else if (a == "--movie-time") {
            if (!value(v) || !number(v, o.movie_time) || o.movie_time < 0) return "--movie-time expects seconds >= 0";
        } else if (a == "--preset") {
            if (!value(v) || !parse_preset(v, o.graphics.preset)) return "--preset expects original|enhanced|ultra|custom";
        } else if (a == "--graphics") {
            if (!value(v)) return "--graphics expects KEY=VALUE[,KEY=VALUE]";
            for (std::size_t p = 0; p <= v.size();) {
                const std::size_t c = std::min(v.find(',', p), v.size());
                const std::string_view kv = v.substr(p, c - p);
                const auto eq = kv.find('=');
                if (eq == std::string_view::npos) return "--graphics expects KEY=VALUE[,KEY=VALUE]";
                const std::string key(trim(kv.substr(0, eq)));
                if (!quality_key(key)) o.graphics.preset = GraphicsPreset::Custom;  // look keys imply custom
                if (auto err = o.graphics.apply("graphics." + key, trim(kv.substr(eq + 1))); !err.empty()) return err;
                p = c + 1;
            }
        } else if (a == "--hd-packs") {
            if (!value(v)) return "--hd-packs expects NAME|DIR[,NAME|DIR]";
            o.graphics.apply("graphics.hd_packs", v);
        } else return "unknown option " + std::string(a);
    }
    if (std::string err; o.graphics.resolve(&err), !err.empty()) return err;
    if (o.frames == 0) o.frames = !o.screenshot.empty() ? 5 : o.headless && o.autoplay.empty() ? 300 : 0;
    return {};
}

}  // namespace joa::engine
