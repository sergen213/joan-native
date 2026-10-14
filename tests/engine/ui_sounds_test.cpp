// UI sound effects (Ui::sounds, SOUND table codes) and the options close slide, without a GPU: the main-menu opening
// SE timings (FUN_0070acb0), the button click (0x6218c0, once per frame), the options root/value clicks (0x73d830)
// and the close animation (0x73cc00 case 6, switch at 0.2 s + slide 1024 px = 0.514 s).
#include "check.hpp"
#include "engine/audio_system.hpp"
#include "engine/ui_runner.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace joa::engine;

namespace {
struct NullRenderer final : IRenderer {
    const char* name() const override { return "null"; }
    MeshHandle create_mesh(const MeshData&) override { return {1}; }
    void destroy_mesh(MeshHandle) override {}
    TextureHandle create_texture(const TextureData&) override { return {1}; }
    void destroy_texture(TextureHandle) override {}
    void begin_frame(glm::ivec2) override {}
    void submit(const FrameData&) override {}
    void end_frame() override {}
    Image read_back() override { return {}; }
};

constexpr float kDt = 1.0f / 30.0f;
SoundBank g_bank;

// One frame; `click` = left release at that design point. Returns the frame's sounds (all must be SOUND codes).
std::vector<std::string> step(UiRunner& u, glm::vec2 click = {-1, -1}) {
    std::vector<UiQuad> q;
    UiInput in;
    in.mouse = click;
    in.mouse_released = click.x >= 0;
    u.frame({0, 0, 1024, 768}, in, kDt, q);
    for (const std::string& s : u.ui.sounds) {
        if (g_bank.find(s) < 0) std::fprintf(stderr, "not in SOUND: %s\n", s.c_str());
        CHECK(g_bank.find(s) >= 0);
    }
    return u.ui.sounds;
}

int count(const std::vector<std::string>& v, const char* code) { return int(std::count(v.begin(), v.end(), code)); }
// Fired in the first frame whose clock reached `at` (float accumulation of 1/30 s steps).
bool at_frame(float t, float at) { return t >= at - 1e-4f && t < at + kDt + 1e-4f; }
}  // namespace

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    joa::formats::Vfs vfs(game);
    CHECK(g_bank.load(vfs));
    NullRenderer r;

    {  // Opening: MENU_SLIDE_IN at the main fade (1.0 s), BUTTON_SLIDE_MANY when the buttons start (3.573 s), once each.
        UiRunner u(vfs, r, "main");
        u.menu.time = 0;
        float in_at = -1, slide_at = -1;
        int in_n = 0, slide_n = 0;
        for (int i = 0; i < 180; ++i) {
            const float t = u.menu.time;
            const auto s = step(u);
            if (count(s, "MENU_SLIDE_IN")) in_at = t, ++in_n;
            if (count(s, "BUTTON_SLIDE_MANY")) slide_at = t, ++slide_n;
        }
        std::fprintf(stderr, "MENU_SLIDE_IN at %.3f, BUTTON_SLIDE_MANY at %.3f\n", in_at, slide_at);
        CHECK(in_n == 1 && slide_n == 1);
        CHECK(at_frame(in_at, 1.0f));
        CHECK(at_frame(slide_at, 3.5733f));
    }
    {  // New Campaign click: one IF_BUTTON_MOUSE_DOWN + the slide-out; the difficulty buttons slide in after 0.657 s.
        UiRunner u(vfs, r, "main");
        const auto s = step(u, {258, 248});
        CHECK(count(s, "IF_BUTTON_MOUSE_DOWN") == 1 && count(s, "BUTTON_SLIDE_MANY") == 1 && s.size() == 2);
        int diff = 0;
        for (int i = 0; i < 30; ++i) diff += count(step(u), "BUTTON_SLIDE_MANY");
        CHECK(u.menu.screen == MenuScreen::Difficulty && diff == 1);
    }
    {  // Options root VIDEO: click + IF_SETTING_BUTTON_CLICK + root leave; the page's slide sound 0.5 s later;
       // a combo arrow (value change) plays the button click only.
        UiRunner u(vfs, r, "options");
        auto s = step(u, {512, 340});
        CHECK(u.menu.options_tab == OptionsTab::Video);
        CHECK(count(s, "IF_BUTTON_MOUSE_DOWN") == 1 && count(s, "IF_SETTING_BUTTON_CLICK") == 1 && count(s, "BUTTON_SLIDE_MANY") == 1);
        float enter_at = -1;
        for (int i = 0; i < 45; ++i) {
            const float t = u.menu.time;
            if (count(step(u), "BUTTON_SLIDE_MANY")) enter_at = t;
        }
        CHECK(at_frame(enter_at, 0.5f));
        const bool grass = u.menu.options.dynamic_grass;
        s = step(u, {490, 462});  // Dynamic Grass combo, left arrow
        CHECK(u.menu.options.dynamic_grass != grass);
        CHECK(count(s, "IF_BUTTON_MOUSE_DOWN") == 1 && count(s, "IF_SETTING_BUTTON_CLICK") == 0);
    }
    {  // Options close (BACK): BUTTON_SLIDE_MANY x2, input ignored while the bars slide out, Main at 0.514 s.
        UiRunner u(vfs, r, "options");
        const auto s = step(u, {512, 727});
        CHECK(u.menu.options_closing && count(s, "IF_BUTTON_MOUSE_DOWN") == 1 && count(s, "BUTTON_SLIDE_MANY") == 2);
        float done = -1;
        for (int i = 0; i < 30 && done < 0; ++i) {
            const float t = u.menu.time;
            const auto c = step(u, {512, 340});  // VIDEO clicks during the close
            CHECK(c.empty());
            if (u.menu.screen != MenuScreen::Options) done = t;
        }
        std::fprintf(stderr, "options closed at %.3f\n", done);
        CHECK(u.menu.screen == MenuScreen::Main && !u.menu.options_closing && u.menu.options_tab == OptionsTab::Root);
        CHECK(at_frame(done, 0.5137f));
    }
    {  // Escape on the in-game root closes too (key shortcut plays the click).
        UiRunner u(vfs, r, "ingame-options");
        std::vector<UiQuad> q;
        UiInput in;
        in.key_pressed = SDL_SCANCODE_ESCAPE;
        u.frame({0, 0, 1024, 768}, in, kDt, q);
        CHECK(u.menu.options_closing && count(u.ui.sounds, "IF_BUTTON_MOUSE_DOWN") == 1);
        for (int i = 0; i < 20; ++i) step(u);
        CHECK(u.menu.screen == MenuScreen::InGame);
    }
    return test_result();
}
