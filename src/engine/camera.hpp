#pragma once
// Camera projection (Hor+), third-person follow camera and UI anchoring. API-neutral.
#include <glm/glm.hpp>

#include <cstdint>

namespace joa::engine {

// HIGH CONFIDENCE: joan.exe builds its projection in a D3DXMatrixPerspectiveFovLH-shaped
// function at VA 0x7674a4 (_11 = cot(fovY/2)/aspect, _22 = cot(fovY/2)); the community FOV fix
// (WarsAndWarriorsJoanOfArcFOVFix) only rescales calls whose cos(fovY/2) == 0.9238795 => fovY = 45 deg,
// and replaces the hard-coded aspect 1.3333334 with the real one. See docs/GRAPHICS_ROADMAP.md.
inline constexpr float kOriginalVerticalFovDeg = 45.0f;
inline constexpr float kOriginalAspect = 4.0f / 3.0f;
// HIGH CONFIDENCE (EngineRE): View3D_init 0x613730 defaults zn = 1.0 (view+0x44), perspective
// zf = 8193.0 (view+0x4c); gameplay View3D_update_matrices 0x6141a0 passes them to 0x7674a4.
// World units are the original's (scale not yet established), so these only fit original assets.
inline constexpr float kDefaultNear = 1.0f;
inline constexpr float kDefaultFar = 8193.0f;

float horizontal_fov(float vertical_fov_rad, float aspect);
float vertical_fov(float horizontal_fov_rad, float aspect);

// Hor+: vertical FOV is kept at every aspect; wider screens see more horizontally.
// OpenGL clip convention (RH, z in [-1,1]).
glm::mat4 perspective_hor_plus(float vertical_fov_rad, float aspect, float z_near, float z_far);

// Largest rectangle of `aspect` centred in the framebuffer (letter/pillarbox); aspect <= 0 = full.
// Returns x, y (top-left origin), w, h.
glm::ivec4 fit_viewport(glm::ivec2 framebuffer, float aspect);

struct FollowCamera {
    float distance = 9.0f;
    float pitch = glm::radians(22.0f);  // above the horizon
    float yaw = 0.0f;                   // orbit angle around the subject; 0 looks toward -Z
    float target_height = 1.5f;
    float smoothing = 10.0f;            // 1/s, exponential follow; <= 0 snaps

    glm::vec3 eye{0.0f};
    glm::vec3 target{0.0f};
    bool initialized = false;

    void update(glm::vec3 subject, float dt);
    glm::mat4 view() const;
};

// A rendered camera: engine space (camera.hpp axes, original units via to_engine), looking eye -> target.
// fov_k = the original CameraState FOV parameter (+0x20, 1.0 = 45 deg vertical); Hor+ keeps it at any aspect.
struct CameraPose {
    glm::vec3 eye{0.0f, 100.0f, 100.0f};
    glm::vec3 target{0.0f};
    float fov_k = 1.0f;
};

// UI placement. The original interface is authored for a 4:3 1024x768 canvas (HIGH CONFIDENCE:
// MENU1024/LD1024* strings, docs/CAMERA_AND_WIDESCREEN.md). Elements anchor either to the centred
// 4:3 safe area (menus, dialogs) or to the real viewport edges (HUD corners on ultrawide).
enum class Anchor : std::uint8_t { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };
enum class UiSpace : std::uint8_t { Safe, Screen };

struct UiLayout {
    glm::vec2 origin{0.0f};       // viewport top-left, pixels
    glm::vec2 size{0.0f};         // viewport size, pixels
    glm::vec2 safe_origin{0.0f};  // 4:3 safe area top-left, pixels
    glm::vec2 safe_size{0.0f};
    float scale = 1.0f;           // pixels per design unit
};

UiLayout ui_layout(glm::ivec4 viewport, glm::vec2 design_canvas = {1024.0f, 768.0f});
// offset/size in design units, offset measured inward from the anchor. Returns x, y, w, h pixels.
glm::vec4 ui_place(const UiLayout& layout, Anchor anchor, UiSpace space, glm::vec2 offset, glm::vec2 size);

}  // namespace joa::engine
