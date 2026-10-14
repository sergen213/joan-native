#pragma once
#include "engine/preset.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace joa::engine {

enum class RendererKind { OpenGL, Vulkan };
enum class WindowMode { Windowed, Fullscreen, Borderless };
enum class PresentMode { Fifo, Relaxed, Mailbox, Immediate };  // Vulkan swapchain; GL uses adaptive vsync

struct Options {
    RendererKind renderer = RendererKind::OpenGL;
    int width = 1280, height = 720;  // logical window size (fullscreen: requested mode)
    WindowMode mode = WindowMode::Windowed;
    float aspect = 0.0f;             // > 0 forces this aspect (letter/pillarbox); 0 = window aspect
    std::string game_dir;
    std::string settings;            // launcher INI (docs/LAUNCHER.md)
    std::string input_config;        // InputMap text file; empty = defaults (+ user config if present)
    float deadzone = -1.0f;          // < 0 = keep InputMap default
    float sensitivity = 1.0f;        // look sensitivity multiplier
    std::string screenshot;          // write PNG after `frames` frames, then exit
    int frames = 0;                  // 0 = run until quit (screenshot default 5, headless default 300)
    bool headless = false;           // simulation only, no window/renderer
    int gpu = -1;                    // Vulkan physical device index; -1 = prefer discrete
    bool vk_validation = false;      // enable VK_LAYER_KHRONOS_validation + debug messenger
    bool gpu_timing = false;         // log GPU timestamp means at exit (Vulkan)
    PresentMode present_mode = PresentMode::Fifo;
    std::string scenario;            // original map to play (e.g. S1L1: its campaign mission, menus skipped); empty = demo
    bool free_camera = false;        // fly camera instead of following Joan
    std::vector<float> camera_pose;  // free camera X,Y,Z[,heading[,pitch]] in original units/degrees
    // Game flow shortcuts (docs/PLAYING.md): start a mission / load a save directly, skipping movies and menus.
    bool new_game = false;           // --new-game
    int mission = 1;                 // --mission N (campaign number 1..8)
    int difficulty = -1;             // --difficulty 0..3 (-1 = options.ini / CONFIG.DAT)
    std::string load;                // --load FILE (.jsav native or original .SAV)
    std::string autoplay;            // --autoplay-script FILE (session_autoplay.hpp)
    unsigned seed = 0;               // --seed N: World/VM RNG seed of a new game (0 = random)
    bool no_movies = false;          // --no-movies: skip enlight.wmv / intro.wmv at startup
    bool rts_unlocked = false;       // --rts-unlocked: allow F2 before scenario 5 (test hook)
    bool camera_fix = false;         // --camera-fix / [controller] camera_fix: Camera Fix Patch turn rate (5x)
    // settings.ini [audio] master/music/sound/environment volumes 0..100 (-1 = options.ini values).
    int master_volume = -1, music_volume = -1, sound_volume = -1, environment_volume = -1;
    std::string ui_screen;           // --ui-screen: original 2D interface screen (ui_screens.hpp names)
    std::string movie;               // --play-movie: original movie (e.g. movie/intro.wmv) instead of the game
    double movie_time = 0;           // --movie-time: start offset in seconds (with --screenshot: frame shown then)
    bool help = false;
    GraphicsConfig graphics;         // [renderer] preset, [graphics] keys, --preset/--graphics/--hd-packs (preset.hpp)
};

const char* usage();
// "16:9", "21:9", "1.7778" -> ratio; "auto"/"0" -> 0. Returns false if malformed.
bool parse_aspect(std::string_view text, float& out);
// Applies [renderer]/[display]/[controller]/[paths] keys from a launcher settings INI.
std::string apply_settings_ini(std::string_view ini, Options& o);
// Parses argv (loading --settings first so explicit flags win). Returns an error message or "".
std::string parse_options(int argc, const char* const* argv, Options& o);

}  // namespace joa::engine
