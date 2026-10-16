#include "engine/dev_overlay.hpp"

#include "game/world_sim.hpp"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace joa::engine {
namespace {

// ImGui colours are sRGB bytes; UiVertex colours are linear (the UI shaders re-encode before gamma-space blending).
float srgb_to_linear(std::uint32_t byte) {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.0f;
            t[std::size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return lut[byte & 0xff];
}

}  // namespace

DevOverlay::DevOverlay(IRenderer& renderer, const GraphicsSettings& graphics) : renderer_(renderer), graphics_(graphics) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendRendererName = "joan-native IRenderer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    TextureData t;
    t.width = w;
    t.height = h;
    t.format = PixelFormat::Rgba8Unorm;
    t.pixels.assign(pixels, pixels + std::size_t(w) * std::size_t(h) * 4);
    t.mipmaps = false;
    t.repeat = false;
    font_ = renderer_.create_texture(t);
    io.Fonts->SetTexID(ImTextureID(font_.id));
    ImGui::SetCurrentContext(previous);
}

DevOverlay::~DevOverlay() {
    ImGui::DestroyContext(ctx_);
    renderer_.destroy_texture(font_);
}

void DevOverlay::input(SDL_Window* window, RawInput& raw) {
    const bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    const bool down = ctrl && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F12];
    if (down && !toggle_down_) visible = !visible;
    toggle_down_ = down;
    if (ctrl) raw.keys[SDL_SCANCODE_F12] = false;  // Ctrl+F12 is ours, not the Mission Brief key
    if (!visible) return;
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    if (window) {
        float x = 0.0f, y = 0.0f;
        SDL_GetMouseState(&x, &y);
        const float density = SDL_GetWindowPixelDensity(window);
        io.AddMousePosEvent(x * density, y * density);
    }
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, raw.mouse_buttons[SDL_BUTTON_LEFT]);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, raw.mouse_buttons[SDL_BUTTON_RIGHT]);
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, raw.mouse_buttons[SDL_BUTTON_MIDDLE]);
    io.AddMouseWheelEvent(0.0f, raw.wheel);
    if (io.WantCaptureMouse) raw.mouse_buttons.reset(), raw.wheel = 0.0f;  // last frame's answer, as ImGui intends
}

void DevOverlay::frame(const DevStats& stats, glm::ivec2 framebuffer, float dt, FrameData& f) {
    frame_ms_[std::size_t(frame_count_++) % frame_ms_.size()] = dt * 1000.0f;
    if (clock_override) clock_minutes = std::fmod(std::fmod(clock_minutes + clock_speed * dt, 1440.0f) + 1440.0f, 1440.0f);
    if (!visible) return;
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(std::max(framebuffer.x, 1)), float(std::max(framebuffer.y, 1)));
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
    io.MouseDrawCursor = true;  // the game hides the OS pointer
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("joan-native dev overlay (Ctrl+F12)", &visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("renderer %s  %dx%d", renderer_.name(), framebuffer.x, framebuffer.y);
        ImGui::Text("frame %.2f ms (%.0f fps avg)", dt * 1000.0, io.Framerate);
        const int n = int(frame_ms_.size());
        ImGui::PlotLines("##frame_ms", frame_ms_.data(), n, frame_count_ % n, "frame ms (0..50)", 0.0f, 50.0f, ImVec2(260.0f, 40.0f));
        ImGui::Text("sim %.2f ms (%d ticks)  submit %.2f ms", stats.sim_ms, stats.ticks, stats.submit_ms);
        ImGui::Text("draw items %zu  ui quads %zu", f.draws.size(), f.ui.size());
        if (stats.world) {
            int units = 0, alive = 0, hostile = 0;
            for (const game::Unit& u : stats.world->units()) {
                if (u.deleted) continue;
                ++units;
                if (stats.world->dead(u)) continue;
                ++alive;
                if (stats.world->hostile_to_player(u)) ++hostile;
            }
            ImGui::Text("units %d  alive %d  hostile %d", units, alive, hostile);
        } else {
            ImGui::TextDisabled("no mission");
        }
        ImGui::SeparatorText("day / night");
        const auto hhmm = [](float m) { return std::to_string(int(m) / 60 + 100).substr(1) + ":" + std::to_string(int(m) % 60 + 100).substr(1); };
        if (stats.game_minute >= 0.0f) ImGui::Text("game clock %s (24 game min per real min)", hhmm(stats.game_minute).c_str());
        if (ImGui::Checkbox("override game clock", &clock_override) && clock_override && stats.game_minute >= 0.0f)
            clock_minutes = stats.game_minute;
        ImGui::BeginDisabled(!clock_override);
        ImGui::SliderFloat("time of day", &clock_minutes, 0.0f, 1439.0f, hhmm(clock_minutes).c_str());
        ImGui::SliderFloat("speed (game min/s)", &clock_speed, -240.0f, 240.0f, "%.0f");
        if (ImGui::Button("06:00")) clock_minutes = 360.0f;
        ImGui::SameLine();
        if (ImGui::Button("09:00")) clock_minutes = 540.0f;
        ImGui::SameLine();
        if (ImGui::Button("12:00")) clock_minutes = 720.0f;
        ImGui::SameLine();
        if (ImGui::Button("15:00")) clock_minutes = 900.0f;
        ImGui::SameLine();
        if (ImGui::Button("18:00")) clock_minutes = 1080.0f;
        ImGui::SameLine();
        if (ImGui::Button("night")) clock_minutes = 1440.0f - 20.0f;
        ImGui::EndDisabled();
        ImGui::SeparatorText("graphics");
        bool changed = false;
        int preset = int(graphics_.preset);
        if (ImGui::Combo("preset", &preset, "Original\0Enhanced\0Ultra\0Custom\0")) {
            std::vector<std::string> packs = std::move(graphics_.hd_packs);
            graphics_ = preset_settings(GraphicsPreset(preset));
            graphics_.hd_packs = std::move(packs);
            changed = true;
        }
        auto feature = [&](const char* label, bool& on) {
            if (ImGui::Checkbox(label, &on)) changed = true, graphics_.preset = GraphicsPreset::Custom;
        };
        feature("PBR", graphics_.pbr);
        feature("normal maps", graphics_.normal_maps);
        feature("IBL", graphics_.ibl);
        feature("HDR + tone mapping", graphics_.hdr);
        bool shadow_map = graphics_.shadow_map_size > 0;
        feature("shadow map (off = stencil volumes)", shadow_map);
        graphics_.shadow_map_size = shadow_map ? std::max(graphics_.shadow_map_size, preset_settings(GraphicsPreset::Enhanced).shadow_map_size) : 0;
        feature("SSAO", graphics_.ssao);
        bool taa = graphics_.aa == AntiAliasing::Taa;
        feature("TAA", taa);
        graphics_.aa = taa ? AntiAliasing::Taa : AntiAliasing::None;
        feature("volumetric fog", graphics_.volumetric_fog);
        feature("particle lighting", graphics_.particle_lighting);
        feature("RT reflections (Vulkan)", graphics_.rt.reflections);
        feature("RT shadows (Vulkan)", graphics_.rt.shadows);
        feature("RT GI (Vulkan)", graphics_.rt.gi);
        ImGui::TextDisabled("backend: restart with --renderer gl|vulkan");
        if (changed) renderer_.set_graphics(graphics_);
    }
    ImGui::End();
    ImGui::Render();

    // Indexed ImGui draw lists -> one clipped, non-indexed triangle batch per draw command.
    vertices_.clear();
    batches_.clear();
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists) {
        for (const ImDrawCmd& c : list->CmdBuffer) {
            if (c.UserCallback || !c.ElemCount) continue;
            UiTriangles b;
            b.first = std::uint32_t(vertices_.size());
            b.count = c.ElemCount;
            b.clip = {int(std::floor(c.ClipRect.x)), int(std::floor(c.ClipRect.y)), int(std::ceil(c.ClipRect.z)), int(std::ceil(c.ClipRect.w))};
            b.texture = TextureHandle{std::uint32_t(c.GetTexID())};
            for (unsigned i = 0; i < c.ElemCount; ++i) {
                const ImDrawVert& v = list->VtxBuffer[int(c.VtxOffset + list->IdxBuffer[int(c.IdxOffset + i)])];
                vertices_.push_back({{v.pos.x, v.pos.y}, {v.uv.x, v.uv.y},
                                     {srgb_to_linear(v.col), srgb_to_linear(v.col >> 8), srgb_to_linear(v.col >> 16), float(v.col >> 24) / 255.0f}});
            }
            batches_.push_back(b);
        }
    }
    f.ui_vertices = vertices_;
    f.ui_triangles = batches_;
}

}  // namespace joa::engine
