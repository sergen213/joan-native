#include "engine/platform.hpp"

#include "engine/renderer_gl.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace joa::engine {

Platform::Platform(const Options& o, bool hidden) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    const bool vulkan = o.renderer == RendererKind::Vulkan;
    if (!vulkan) gl_set_window_attributes();
    SDL_WindowFlags flags = (vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (hidden && !std::getenv("JOA_SHOW_WINDOWS")) flags |= SDL_WINDOW_HIDDEN;  // tests on the real display
    window_ = SDL_CreateWindow("Wars and Warriors: Joan of Arc (native)", o.width, o.height, flags);
    if (!window_) {
        SDL_Quit();
        throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    }
    if (o.mode == WindowMode::Fullscreen) {
        // Launcher width/height 0 = desktop resolution (docs/LAUNCHER.md): the closest mode to the 1280x720 default was a
        // 720p swapchain stretched over the display (blurry on a 4K panel).
        SDL_DisplayMode mode;
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
        const SDL_DisplayMode* desktop = o.desktop_size ? SDL_GetDesktopDisplayMode(display) : nullptr;
        const int w = desktop ? desktop->w : o.width, h = desktop ? desktop->h : o.height;
        if (SDL_GetClosestFullscreenDisplayMode(display, w, h, desktop ? desktop->refresh_rate : 0.0f, true, &mode))
            SDL_SetWindowFullscreenMode(window_, &mode);
        SDL_SetWindowFullscreen(window_, true);
    } else if (o.mode == WindowMode::Borderless) {
        SDL_SetWindowFullscreenMode(window_, nullptr);  // desktop-resolution fullscreen
        SDL_SetWindowFullscreen(window_, true);
    }
    SDL_SyncWindow(window_);
}

Platform::~Platform() {
    for (SDL_Gamepad* p : pads_) SDL_CloseGamepad(p);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

glm::ivec2 Platform::pixel_size() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return {w, h};
}

bool Platform::pump(RawInput& raw) {
    bool running = true;
    SDL_Event e;
    auto axis = [](Sint16 v) { return std::clamp(float(v) / 32767.0f, -1.0f, 1.0f); };
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (e.key.scancode < SDL_SCANCODE_COUNT) raw.keys[e.key.scancode] = e.key.down;
            if (e.key.down && e.key.repeat && text_input_) raw.key_repeat = e.key.scancode;
            break;
        case SDL_EVENT_TEXT_INPUT: raw.text += e.text.text; break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button < raw.mouse_buttons.size()) raw.mouse_buttons[e.button.button] = e.button.down;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            raw.mouse_dx += e.motion.xrel;
            raw.mouse_dy += e.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_WHEEL: raw.wheel += e.wheel.y; break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_Gamepad* p = SDL_OpenGamepad(e.gdevice.which)) pads_.push_back(p);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            std::erase_if(pads_, [&](SDL_Gamepad* p) {
                if (SDL_GetGamepadID(p) != e.gdevice.which) return false;
                SDL_CloseGamepad(p);
                return true;
            });
            raw.pad_buttons.reset();
            raw.pad_axes.fill(0.0f);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (e.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) raw.pad_buttons[e.gbutton.button] = e.gbutton.down;
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (e.gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) raw.pad_axes[e.gaxis.axis] = axis(e.gaxis.value);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:  // no stuck keys after alt-tab
            raw.keys.reset();
            raw.mouse_buttons.reset();
            break;
        default: break;
        }
    }
    raw.mouse_look = mouse_look_;
    raw.pad_connected = !pads_.empty();
    return running;
}

void Platform::set_mouse_look(bool on) {
    if (on == mouse_look_) return;
    mouse_look_ = on;
    SDL_SetWindowRelativeMouseMode(window_, on);
}

void Platform::set_text_input(bool on) {
    if (on == text_input_) return;
    text_input_ = on;
    if (on) SDL_StartTextInput(window_);
    else SDL_StopTextInput(window_);
}

void Platform::toggle_fullscreen() {
    const bool fs = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!fs) SDL_SetWindowFullscreenMode(window_, nullptr);
    SDL_SetWindowFullscreen(window_, !fs);
}

void Platform::rumble(float low, float high, int ms) {
    const auto motor = [](float v) { return Uint16(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
    for (SDL_Gamepad* p : pads_) SDL_RumbleGamepad(p, motor(low), motor(high), Uint32(std::max(ms, 0)));
}

}  // namespace joa::engine
