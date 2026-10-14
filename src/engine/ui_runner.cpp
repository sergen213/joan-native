#include "engine/ui_runner.hpp"

#include "engine/menu_options_store.hpp"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace joa::engine {

UiRunner::UiRunner(const formats::Vfs& vfs, IRenderer& renderer, std::string_view screen) : ui(vfs, renderer) {
    if (!ui_sample_state(screen, menu, hud, show_hud)) throw std::runtime_error("unknown --ui-screen " + std::string(screen));
    load_options(vfs, options_ini_path(), menu.options, menu.bindings);
}

bool UiRunner::frame(glm::ivec4 viewport, const UiInput& input, float dt, std::vector<UiQuad>& out) {
    ui.begin(viewport, out, input, dt);
    const std::size_t first = commands.size();
    if (show_hud) {
        ui_hud(ui, hud, commands);
        if (menu.screen == MenuScreen::None && menu.in_game && ui.key(SDL_SCANCODE_ESCAPE)) menu.screen = MenuScreen::InGame;
    }
    ui_menu(ui, menu, commands);
    ui.frame(Anchor::Center);
    ui.cursor();
    if (audio) {
        for (const std::string& s : ui.sounds) audio->play_sound(s);  // UI SE, 2D
        audio->update();
    }
    bool running = true;
    // No game layer behind the standalone UI: honour navigation-only commands, log the rest.
    for (std::size_t i = first; i < commands.size(); ++i) {
        const UiCommand& c = commands[i];
        switch (c.type) {
            case UiCommand::Type::Quit: running = false; break;
            case UiCommand::Type::Resume: menu.screen = MenuScreen::None; break;
            case UiCommand::Type::QuitToMain:
                menu = MenuModel{};
                show_hud = false;
                load_options(ui.vfs(), options_ini_path(), menu.options, menu.bindings);
                break;
            case UiCommand::Type::OptionsChanged:
                if (!save_options(options_ini_path(), menu.options, menu.bindings))
                    std::fprintf(stderr, "ui: cannot write %s\n", options_ini_path().c_str());
                break;
            default: std::fprintf(stderr, "ui: command %d value=%d text=%s\n", int(c.type), c.value, c.text.c_str()); break;
        }
    }
    return running;
}

UiInput UiInputTracker::update(SDL_Window* window, const RawInput& raw) {
    UiInput in;
    float x = -1.0f, y = -1.0f;
    if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_FOCUS)) {
        SDL_GetMouseState(&x, &y);
        const float density = SDL_GetWindowPixelDensity(window);
        in.mouse = {x * density, y * density};
    }
    const bool left = raw.mouse_buttons[SDL_BUTTON_LEFT], right = raw.mouse_buttons[SDL_BUTTON_RIGHT];
    in.mouse_down = left;
    in.mouse_pressed = left && !left_;
    in.mouse_released = !left && left_;
    in.right_pressed = right && !right_;
    in.wheel = raw.wheel;
    for (std::size_t k = 1; k < raw.keys.size(); ++k)
        if (raw.keys[k] && !keys_[k]) {
            in.key_pressed = int(k);
            break;
        }
    if (!in.key_pressed) in.key_pressed = raw.key_repeat;  // typematic repeat (text fields)
    in.text = raw.text;
    keys_ = raw.keys;
    left_ = left;
    right_ = right;
    return in;
}

}  // namespace joa::engine
