#pragma once
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
    std::string scenario;            // original map to load (e.g. S1L1); empty = procedural demo scene
    bool free_camera = false;        // fly camera instead of following Joan
    std::vector<float> camera_pose;  // free camera X,Y,Z[,heading[,pitch]] in original units/degrees
    std::string anim;                // force Joan's animation clip (e.g. f_run)
    bool help = false;
};

const char* usage();
// "16:9", "21:9", "1.7778" -> ratio; "auto"/"0" -> 0. Returns false if malformed.
bool parse_aspect(std::string_view text, float& out);
// Applies [renderer]/[display]/[controller]/[paths] keys from a launcher settings INI.
std::string apply_settings_ini(std::string_view ini, Options& o);
// Parses argv (loading --settings first so explicit flags win). Returns an error message or "".
std::string parse_options(int argc, const char* const* argv, Options& o);

}  // namespace joa::engine
