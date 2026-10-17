#pragma once
// Runs the menus/HUD inside the client loop (and the screenshot tests): device state -> UiInput,
// screens -> UiQuads, and the few UiCommands the client can honour without a game layer.
#include "engine/audio_system.hpp"
#include "engine/input.hpp"
#include "engine/ui_core.hpp"
#include "engine/ui_screens.hpp"

#include <bitset>
#include <memory>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace joa::engine {

class UiRunner {
public:
    // `screen`: a ui_screen_names() entry; throws std::runtime_error for an unknown name. hd_packs: as Ui.
    UiRunner(const formats::Vfs& vfs, IRenderer& renderer, std::string_view screen, std::vector<std::filesystem::path> hd_packs = {});

    // Draws the HUD (if shown) then the active menu, then the cursor. Returns false once Quit was chosen.
    bool frame(glm::ivec4 viewport, const UiInput& input, float dt, std::vector<UiQuad>& out);

    Ui ui;
    MenuModel menu;
    HudModel hud;
    bool show_hud = false;
    std::vector<UiCommand> commands;  // everything emitted so far (tests inspect it)
    std::unique_ptr<AudioSystem> audio;  // optional: plays ui.sounds after each frame (tests read ui.sounds instead)
};

// Window mouse/keyboard -> UiInput. Edge detection against the previous call's state.
class UiInputTracker {
public:
    UiInput update(SDL_Window* window, const RawInput& raw);

private:
    std::bitset<SDL_SCANCODE_COUNT> keys_;
    bool left_ = false, right_ = false;
};

}  // namespace joa::engine
