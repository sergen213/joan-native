#pragma once
// SDL3 window, event pump and device state (Wayland/X11 chosen by SDL; SDL_VIDEO_DRIVER overrides).
#include "engine/cli.hpp"
#include "engine/input.hpp"

#include <glm/glm.hpp>

#include <vector>

struct SDL_Window;
struct SDL_Gamepad;

namespace joa::engine {

class Platform {
public:
    // Creates an SDL_WINDOW_OPENGL or _VULKAN window (per options.renderer, high pixel density) in the
    // requested mode. Throws on failure.
    Platform(const Options& options, bool hidden);
    ~Platform();
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    SDL_Window* window() const { return window_; }
    glm::ivec2 pixel_size() const;

    // Drains pending events into `raw`; returns false when the user asked to quit.
    bool pump(RawInput& raw);
    void set_mouse_look(bool on);  // relative mouse mode while steering the camera
    void toggle_fullscreen();      // windowed <-> borderless

private:
    SDL_Window* window_ = nullptr;
    std::vector<SDL_Gamepad*> pads_;
    bool mouse_look_ = false;
};

}  // namespace joa::engine
