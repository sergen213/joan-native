#pragma once
// Dear ImGui developer overlay (CMake -DJOA_DEV_OVERLAY=ON only; docs/PLAYING.md "Developer tools"). Ctrl+F12 toggles
// a window with frame/sim/submit timings, draw and unit counts, the renderer and graphics preset/feature toggles. It
// renders through IRenderer (FrameData::ui_vertices / ui_triangles: clipped textured triangles, GL and Vulkan).
#include "engine/input.hpp"
#include "engine/preset.hpp"
#include "engine/renderer.hpp"

#include <array>
#include <vector>

// JOA_DEV(statements): compiled only into overlay builds (call sites in session_client.cpp).
#ifdef JOA_DEV_OVERLAY
#define JOA_DEV(...) __VA_ARGS__
#else
#define JOA_DEV(...)
#endif

struct SDL_Window;
struct ImGuiContext;
namespace joa::game {
class World;
}

namespace joa::engine {

struct DevStats {
    double sim_ms = 0.0;     // CPU time of this frame's simulation ticks
    int ticks = 0;           // simulation ticks run this frame
    double submit_ms = 0.0;  // CPU time of the previous IRenderer::submit
    const game::World* world = nullptr;  // unit counts; null outside a mission
    float game_minute = -1.0f;           // mission clock (MissionState::minute); < 0 outside a mission
};

class DevOverlay {
public:
    DevOverlay(IRenderer& renderer, const GraphicsSettings& graphics);
    ~DevOverlay();
    DevOverlay(const DevOverlay&) = delete;
    DevOverlay& operator=(const DevOverlay&) = delete;

    // After Platform::pump: Ctrl+F12 toggles (that F12 never reaches the game); while shown, ImGui gets the pointer
    // and the clicks / wheel it captures are removed from `raw`.
    void input(SDL_Window* window, RawInput& raw);
    // Points f.ui_vertices / f.ui_triangles at the overlay (untouched while hidden); reads f.draws / f.ui for the
    // counts. Graphics edits call IRenderer::set_graphics here, i.e. between frames.
    void frame(const DevStats& stats, glm::ivec2 framebuffer, float dt, FrameData& f);

    bool visible = false;
    // Day/night test: while clock_override, the world lighting follows clock_minutes (minutes after midnight),
    // advanced by clock_speed game minutes per real second, instead of the mission clock.
    bool clock_override = false;
    float clock_minutes = 720.0f, clock_speed = 0.0f;

private:
    IRenderer& renderer_;
    GraphicsSettings graphics_;
    ImGuiContext* ctx_ = nullptr;
    TextureHandle font_;
    bool toggle_down_ = false;
    std::array<float, 120> frame_ms_{};
    int frame_count_ = 0;
    std::vector<UiVertex> vertices_;
    std::vector<UiTriangles> batches_;
};

}  // namespace joa::engine
