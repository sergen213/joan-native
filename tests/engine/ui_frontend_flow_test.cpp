// Main menu / difficulty exit animations (menu_main.cpp) without a GPU: screen switches and commands wait for the
// original's exit timings (notes_MenuFrontEnd.md), clicks during an exit are ignored, commands fire once.
#include "check.hpp"
#include "engine/ui_runner.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

// Clicks `first`, then keeps clicking `spam` every frame until `done`; returns the seconds that took.
template <class Done>
float click_until(UiRunner& u, glm::vec2 first, glm::vec2 spam, Done done) {
    std::vector<UiQuad> q;
    UiInput in;
    in.mouse = first;
    in.mouse_released = true;
    u.frame({0, 0, 1024, 768}, in, kDt, q);
    in.mouse = spam;
    float t = 0.0f;
    for (int i = 0; i < 300 && !done(); ++i, t += kDt) {
        q.clear();
        u.frame({0, 0, 1024, 768}, in, kDt, q);
    }
    return t;
}

int count(const UiRunner& u, UiCommand::Type type) {
    int n = 0;
    for (const UiCommand& c : u.commands) n += c.type == type;
    return n;
}
}  // namespace

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    joa::formats::Vfs vfs(game);
    NullRenderer r;

    {  // Credits: buttons out (0.657 s) + Joan/stripe 0.25 s + reverse sequence 1.54 s; Quit clicks ignored.
        UiRunner u(vfs, r, "main");
        const float t = click_until(u, {258, 483}, {258, 559}, [&] { return u.menu.screen != MenuScreen::Main; });
        std::fprintf(stderr, "credits after %.3f s\n", t);
        CHECK(u.menu.screen == MenuScreen::Credits && std::abs(t - 2.447f) < 0.07f && count(u, UiCommand::Type::Quit) == 0);
    }
    {  // Load: buttons out + Joan/stripe fade.
        UiRunner u(vfs, r, "main");
        const float t = click_until(u, {258, 332}, {-1, -1}, [&] { return u.menu.screen != MenuScreen::Main; });
        CHECK(u.menu.screen == MenuScreen::Load && std::abs(t - 0.907f) < 0.07f);
    }
    {  // Quit: buttons out, then the command, once.
        UiRunner u(vfs, r, "main");
        const float t = click_until(u, {258, 559}, {-1, -1}, [&] { return count(u, UiCommand::Type::Quit) > 0; });
        CHECK(std::abs(t - 0.657f) < 0.07f && count(u, UiCommand::Type::Quit) == 1);
    }
    {  // EASY: difficulty buttons out (4th at 0.566 s) + Joan 0.7 s + reverse sequence 1.54 s, NewGame{0} once.
        UiRunner u(vfs, r, "difficulty");
        const float t = click_until(u, {258, 248}, {258, 456}, [&] { return count(u, UiCommand::Type::NewGame) > 0; });
        std::fprintf(stderr, "new game after %.3f s\n", t);
        CHECK(std::abs(t - 2.806f) < 0.07f && u.commands.back().value == 0 && u.menu.screen == MenuScreen::Difficulty);
        click_until(u, {-1, -1}, {-1, -1}, [] { return false; });
        CHECK(count(u, UiCommand::Type::NewGame) == 1);
    }
    return test_result();
}
