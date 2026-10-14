// Hor+ projection, viewport fitting, UI anchoring and follow camera math.
#include "check.hpp"
#include "engine/camera.hpp"

#include <glm/gtc/constants.hpp>

using namespace joa::engine;

static glm::vec3 ndc(const glm::mat4& p, glm::vec3 view_point) {
    const glm::vec4 c = p * glm::vec4(view_point, 1.0f);
    return glm::vec3(c) / c.w;
}

int main() {
    const float vfov = glm::radians(kOriginalVerticalFovDeg);
    CHECK_NEAR(kOriginalVerticalFovDeg, 45.0f, 0.0f);

    struct Case { const char* name; float aspect; float hfov_deg; };
    // Reference horizontal FOVs computed independently: 2*atan(tan(22.5 deg)*aspect).
    const Case cases[] = {
        {"4:3", 4.0f / 3.0f, 57.8224f},  {"5:4", 5.0f / 4.0f, 54.7473f},   {"16:9", 16.0f / 9.0f, 72.7344f},
        {"16:10", 1.6f, 67.0682f},       {"21:9", 21.0f / 9.0f, 88.0480f}, {"32:9", 32.0f / 9.0f, 111.6472f},
        {"custom 1.9", 1.9f, 76.4059f},
    };
    for (const Case& c : cases) {
        const float h = horizontal_fov(vfov, c.aspect);
        std::fprintf(stderr, "%-10s aspect %.4f hfov %.4f deg\n", c.name, c.aspect, glm::degrees(h));
        CHECK_NEAR(glm::degrees(h), c.hfov_deg, 1e-3);
        CHECK_NEAR(vertical_fov(h, c.aspect), vfov, 1e-5);

        const glm::mat4 p = perspective_hor_plus(vfov, c.aspect, kDefaultNear, kDefaultFar);
        // Same _22 at every aspect (vertical FOV kept); _11 = _22 / aspect as in joan.exe 0x7674a4.
        CHECK_NEAR(p[1][1], 2.4142135f, 1e-5);
        CHECK_NEAR(p[0][0], p[1][1] / c.aspect, 1e-5);
        // Frustum edges land on the NDC edges.
        const float z = -10.0f;
        CHECK_NEAR(ndc(p, {10.0f * std::tan(h * 0.5f), 0.0f, z}).x, 1.0f, 1e-4);
        CHECK_NEAR(ndc(p, {0.0f, 10.0f * std::tan(vfov * 0.5f), z}).y, 1.0f, 1e-4);
        // Depth range is OpenGL's [-1, 1].
        CHECK_NEAR(ndc(p, {0, 0, -kDefaultNear}).z, -1.0f, 1e-4);
        CHECK_NEAR(ndc(p, {0, 0, -kDefaultFar}).z, 1.0f, 1e-3);
        if (c.aspect > 4.0f / 3.0f) CHECK(h > horizontal_fov(vfov, 4.0f / 3.0f));  // Hor+: wider sees more
    }

    // Viewport fitting (aspect override / pillarbox / letterbox).
    CHECK(fit_viewport({1920, 1080}, 4.0f / 3.0f) == glm::ivec4(240, 0, 1440, 1080));
    CHECK(fit_viewport({1280, 1024}, 16.0f / 9.0f) == glm::ivec4(0, 152, 1280, 720));
    CHECK(fit_viewport({2560, 1080}, 0.0f) == glm::ivec4(0, 0, 2560, 1080));
    CHECK(fit_viewport({800, 600}, 4.0f / 3.0f) == glm::ivec4(0, 0, 800, 600));

    // UI: ultrawide 3440x1440 -> centred 1920x1440 safe area, 1440/768 = 1.875 px per design unit.
    {
        const UiLayout l = ui_layout({0, 0, 3440, 1440});
        const float k = 1.875f;
        CHECK_NEAR(l.safe_origin.x, 760.0f, 0.0f);
        CHECK_NEAR(l.safe_size.x, 1920.0f, 0.0f);
        CHECK_NEAR(l.scale, k, 1e-6);
        const glm::vec4 tr = ui_place(l, Anchor::TopRight, UiSpace::Screen, {8, 8}, {16, 16});
        CHECK_NEAR(tr.x, 3440.0f - 16 * k - 8 * k, 1e-3);
        CHECK_NEAR(tr.y, 8 * k, 1e-3);
        const glm::vec4 tl = ui_place(l, Anchor::TopLeft, UiSpace::Safe, {8, 8}, {16, 16});
        CHECK_NEAR(tl.x, 760.0f + 8 * k, 1e-3);
        const glm::vec4 c = ui_place(l, Anchor::Center, UiSpace::Safe, {0, 0}, {100, 50});
        CHECK_NEAR(c.x + c.z * 0.5f, 1720.0f, 1e-3);
        CHECK_NEAR(c.y + c.w * 0.5f, 720.0f, 1e-3);
        const glm::vec4 br = ui_place(l, Anchor::BottomRight, UiSpace::Safe, {0, 0}, {1024, 768});
        CHECK_NEAR(br.x, 760.0f, 1e-3);  // full canvas fills the safe area exactly
        CHECK_NEAR(br.w, 1440.0f, 1e-3);
    }
    // 5:4 -> letterboxed 4:3 safe area; viewport offset is carried through.
    {
        const UiLayout l = ui_layout({100, 50, 1280, 1024});
        CHECK_NEAR(l.safe_origin.x, 100.0f, 0.0f);
        CHECK_NEAR(l.safe_origin.y, 50.0f + 32.0f, 0.0f);
        CHECK_NEAR(l.scale, 960.0f / 768.0f, 1e-6);
    }

    // Follow camera: subject is centred on screen at the configured distance.
    {
        FollowCamera cam;
        cam.yaw = 0.7f;
        cam.update({5, 0, -3}, 0.016f);
        const glm::vec3 target = glm::vec3(5, cam.target_height, -3);
        CHECK_NEAR(glm::distance(cam.eye, target), cam.distance, 1e-4);
        const glm::vec3 v = glm::vec3(cam.view() * glm::vec4(target, 1.0f));
        CHECK_NEAR(v.x, 0.0f, 1e-4);
        CHECK_NEAR(v.y, 0.0f, 1e-4);
        CHECK_NEAR(v.z, -cam.distance, 1e-4);
        CHECK(cam.eye.y > target.y);  // looking down at positive pitch
        // Smoothed follow converges toward a moved subject without overshooting.
        const glm::vec3 before = cam.eye;
        cam.update({6, 0, -3}, 0.016f);
        CHECK(cam.eye.x > before.x && cam.eye.x < before.x + 1.0f);
    }
    return test_result();
}
