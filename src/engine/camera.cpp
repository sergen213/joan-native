#include "engine/camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace joa::engine {

float horizontal_fov(float vertical_fov_rad, float aspect) {
    return 2.0f * std::atan(std::tan(vertical_fov_rad * 0.5f) * aspect);
}

float vertical_fov(float horizontal_fov_rad, float aspect) {
    return 2.0f * std::atan(std::tan(horizontal_fov_rad * 0.5f) / aspect);
}

glm::mat4 perspective_hor_plus(float vertical_fov_rad, float aspect, float z_near, float z_far) {
    return glm::perspectiveRH_NO(vertical_fov_rad, aspect, z_near, z_far);
}

glm::ivec4 fit_viewport(glm::ivec2 fb, float aspect) {
    if (aspect <= 0.0f || fb.x <= 0 || fb.y <= 0) return {0, 0, fb.x, fb.y};
    int w = fb.x, h = fb.y;
    if (float(fb.x) / float(fb.y) > aspect)
        w = int(std::lround(fb.y * aspect));
    else
        h = int(std::lround(fb.x / aspect));
    return {(fb.x - w) / 2, (fb.y - h) / 2, w, h};
}

void FollowCamera::update(glm::vec3 subject, float dt) {
    const glm::vec3 look = subject + glm::vec3(0.0f, target_height, 0.0f);
    const glm::vec3 back{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const glm::vec3 desired = look + back * distance;
    if (!initialized || smoothing <= 0.0f) {
        eye = desired;
        target = look;
        initialized = true;
        return;
    }
    const float k = 1.0f - std::exp(-smoothing * dt);
    eye = glm::mix(eye, desired, k);
    target = glm::mix(target, look, k);
}

glm::mat4 FollowCamera::view() const { return glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f)); }

UiLayout ui_layout(glm::ivec4 vp, glm::vec2 canvas) {
    UiLayout l;
    l.origin = {float(vp.x), float(vp.y)};
    l.size = {float(vp.z), float(vp.w)};
    const glm::ivec4 safe = fit_viewport({vp.z, vp.w}, canvas.x / canvas.y);
    l.safe_origin = l.origin + glm::vec2(safe.x, safe.y);
    l.safe_size = {float(safe.z), float(safe.w)};
    l.scale = l.safe_size.y / canvas.y;
    return l;
}

glm::vec4 ui_place(const UiLayout& l, Anchor anchor, UiSpace space, glm::vec2 offset, glm::vec2 size) {
    const int a = int(anchor);
    const glm::vec2 align{float(a % 3) * 0.5f, float(a / 3) * 0.5f};
    const glm::vec2 box_origin = space == UiSpace::Safe ? l.safe_origin : l.origin;
    const glm::vec2 box_size = space == UiSpace::Safe ? l.safe_size : l.size;
    const glm::vec2 px = size * l.scale;
    const glm::vec2 inward{align.x == 1.0f ? -1.0f : 1.0f, align.y == 1.0f ? -1.0f : 1.0f};
    const glm::vec2 pos = box_origin + align * (box_size - px) + inward * offset * l.scale;
    return {pos.x, pos.y, px.x, px.y};
}

}  // namespace joa::engine
