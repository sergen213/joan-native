// GameCamera headless on mission 1 (Session + original data): follow pose behind Joan, controller yaw, zoom
// limits, terrain clearance, cut-scene camera + cut_scene_play tracks, RTS camera / box select / move order.
#include "check.hpp"
#include "engine/assets_cache.hpp"
#include "engine/game_camera.hpp"
#include "engine/game_input.hpp"
#include "engine/session.hpp"
#include "game/mission.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace joa;
using namespace joa::engine;

namespace {

struct Rig {
    Session& s;
    GameCamera cam;
    GameInput in;  // after_tick reads keys from the InputFrame given to before_tick
    void tick(game::InputFrame f = {}) {
        cam.before_tick(*s.mission(), f);
        s.tick(f);
        cam.after_tick(*s.mission(), in);
    }
};

float ground(game::World& w, glm::vec3 p) { return w.terrain()->height(p.x, p.y); }
float angle_diff(float a, float b) { return std::fabs(std::remainder(a - b, 360.0f)); }

game::Unit* find_unit(game::World& w, const char* code) {
    for (game::Unit& u : w.units())
        if (!u.deleted && u.cls >= 0 && formats::iequals(w.data().units[std::size_t(u.cls)].code, code)) return &u;
    return nullptr;
}

glm::mat4 view_proj(const CameraPose& p) {
    return perspective_hor_plus(glm::radians(45.0f), 4.0f / 3.0f, 1.0f, 8193.0f) * glm::lookAtRH(p.eye, p.target, glm::vec3(0, 1, 0));
}

glm::vec2 to_px(const glm::mat4& vp, glm::vec3 o) {
    const glm::vec4 c = vp * glm::vec4(to_engine(o.x, o.y, o.z), 1.0f);
    return {(c.x / c.w * 0.5f + 0.5f) * 1024.0f, (0.5f - c.y / c.w * 0.5f) * 768.0f};
}

// One rendered frame of RTS mouse input.
void mouse(Rig& r, glm::vec2 at, bool left, bool right, const glm::mat4& vp) {
    RawInput raw;
    raw.mouse_buttons[SDL_BUTTON_LEFT] = left;
    raw.mouse_buttons[SDL_BUTTON_RIGHT] = right;
    r.cam.rts_input(*r.s.mission(), raw, at, {0, 0, 1024, 768}, vp);
}

}  // namespace

int main() {
    const char* game_dir = std::getenv("JOA_GAME_DIR");
    if (!game_dir || !formats::Vfs(game_dir).exists("Resource/cutscene.cm")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    Session s{formats::Vfs(game_dir)};
    s.seed = 1;
    std::string err;
    if (!s.new_game(1, 1, &err)) {
        std::fprintf(stderr, "new_game: %s\n", err.c_str());
        return 1;
    }
    game::Mission& m = *s.mission();
    m.autosave = false;
    game::World& w = m.world();
    Rig r{s};
    r.cam.reset(m);

    // 1. Follow camera: pivot 50 above Joan, eye 187.5 behind her facing (pitch 0 at start), pose in engine space.
    {
        const game::Unit& joan = *w.unit(w.player_id());
        const CamState& c = r.cam.state();
        const glm::vec3 eye = cam_eye(c), pj{joan.pos.x, joan.pos.y, joan.pos.z};
        CHECK(angle_diff(c.yaw, joan.yaw_goal) < 0.01f);  // spawn heading (the body still turns toward it)
        CHECK_NEAR(c.dist, GameCamera::kFollowDistance, 0.01);
        CHECK_NEAR(c.pivot.z, pj.z + GameCamera::kPivotHeight, 0.01);
        const glm::vec2 back = glm::vec2(eye - pj), face{std::cos(glm::radians(joan.yaw_goal)), std::sin(glm::radians(joan.yaw_goal))};
        CHECK_NEAR(glm::length(back), 187.5, 0.5);
        CHECK(glm::dot(glm::normalize(back), face) < -0.999f);
        CHECK(eye.z >= ground(w, eye) + GameCamera::kClearance - 0.01f);
        const CameraPose p = r.cam.pose(1.0f);
        CHECK(glm::distance(p.eye, to_engine(eye.x, eye.y, eye.z)) < 0.01f);
        CHECK(glm::distance(p.target, to_engine(c.pivot.x, c.pivot.y, c.pivot.z)) < 0.01f);
    }

    // Let the mission's opening cut scene (ticks 2..244 on mission 1) run out so the controller takes input. While it
    // runs, forward + mouse look + attack leave Joan alone (0x42e1a0 cut-scene guard).
    for (int i = 0; i < 5; ++i) r.tick();
    {
        CHECK(w.cut_scene);
        const game::Unit& joan = *w.unit(w.player_id());
        const glm::vec2 p0{joan.pos.x, joan.pos.y};
        const float y0 = joan.yaw_goal;
        game::InputFrame f;
        f.held.set(game::IN_FORWARD), f.pressed.set(game::IN_ATTACK_A), f.mouse_dx = 50;
        for (int i = 0; i < 60; ++i) r.tick(f);
        CHECK(w.cut_scene && glm::distance(p0, glm::vec2(joan.pos.x, joan.pos.y)) < 0.01f && joan.yaw_goal == y0 && joan.action == game::IDLE);
    }
    for (int i = 0; i < 235; ++i) r.tick();
    for (int i = 0; i < 3000 && (m.ui().cs.active || m.ui().game_cut_scene); ++i) r.tick();
    CHECK(!m.ui().cs.active);
    {  // the opening cut scene ends with the follow camera behind Joan (reference s04_04_mission1_hud)
        const game::Unit& joan = *w.unit(w.player_id());
        const glm::vec2 back = glm::vec2(cam_eye(r.cam.state())) - glm::vec2(joan.pos.x, joan.pos.y);
        const glm::vec2 face{std::cos(glm::radians(joan.yaw_deg)), std::sin(glm::radians(joan.yaw_deg))};
        std::printf("after opening: joan yaw %.1f camera yaw %.1f\n", joan.yaw_deg, r.cam.state().yaw);
        CHECK(glm::dot(glm::normalize(back), face) < -0.99f);
    }

    // 2. Mouse turns the hero and camera 0 together (0x431790): d = int(dx * 0.9 * 0.2) = 9 per tick.
    {
        const float y0 = r.cam.state().yaw, u0 = w.unit(w.player_id())->yaw_deg;
        game::InputFrame f;
        f.mouse_dx = 50;
        r.tick(f);
        CHECK(angle_diff(r.cam.state().yaw, y0 - 9.0f) < 0.01f);
        CHECK(angle_diff(w.unit(w.player_id())->yaw_deg, u0 - 9.0f) < 1.01f);  // the unit turns in whole degrees
        f.mouse_dx = -50;
        r.tick(f);
        CHECK(angle_diff(r.cam.state().yaw, y0) < 0.01f);
    }

    // 3. Zoom actions 48 / 49 clamp to [80, 350]; mouse pitch clamps to [-20, 35].
    {
        game::InputFrame out;
        out.press(49);
        for (int i = 0; i < 40; ++i) r.tick(out);
        CHECK_NEAR(r.cam.follow_distance(), GameCamera::kZoomMax, 0.01);
        game::InputFrame in;
        in.press(48);
        r.tick(in);
        CHECK_NEAR(r.cam.follow_distance(), GameCamera::kZoomMax - 33 * 0.6, 0.01);
        for (int i = 0; i < 40; ++i) r.tick(in);
        CHECK_NEAR(r.cam.follow_distance(), GameCamera::kZoomMin, 0.01);
        for (int i = 0; i < 6; ++i) r.tick(out);
        CHECK(r.cam.follow_distance() > GameCamera::kZoomMin + 100);
        game::InputFrame up;
        up.mouse_dy = 1000;
        for (int i = 0; i < 3; ++i) r.tick(up);
        CHECK_NEAR(r.cam.follow_pitch(), GameCamera::kPitchMax, 0.01);
    }

    // 4. Clearance: looking up as far as allowed while circling Joan keeps the eye above the ground.
    {
        game::InputFrame f;
        f.mouse_dy = -20;  // -3.6 degrees per tick; steps that would sink the eye are undone (0x431790)
        f.mouse_dx = 120;  // 21 degrees per tick
        float lowest = 1e9f;
        for (int i = 0; i < 40; ++i) {
            r.tick(f);
            const glm::vec3 e = cam_eye(r.cam.state());
            lowest = std::min(lowest, e.z - ground(w, e));
        }
        CHECK(r.cam.follow_pitch() < 0.0f);
        CHECK(lowest >= GameCamera::kClearance - 0.01f);
        std::printf("follow pitch %.1f, lowest eye clearance %.1f\n", r.cam.follow_pitch(), lowest);
        game::InputFrame level;
        level.mouse_dy = -r.cam.follow_pitch() / (0.9f * 0.2f);
        r.tick(level);
        CHECK_NEAR(r.cam.follow_pitch(), 0.0, 0.01);
    }

    // 5. Cut scene: instant camera on the target, then cut_scene_end(move_back) plays cut_scene_play back to Joan.
    {
        m.ui().auto_advance = false;  // headless mode would end camera scripts after auto_ms (2 s)
        game::Unit& joan = *w.unit(w.player_id());
        const glm::vec3 pj{joan.pos.x, joan.pos.y, joan.pos.z};
        CHECK(m.ui().cut_scene_start(m, false, false, joan.id, nullptr, 300, 90, 20, 0) == 1);
        r.tick();
        const CamState& c = r.cam.state();
        CHECK(glm::distance(c.pivot, pj + glm::vec3(0, 0, 50)) < 1.0f);
        CHECK_NEAR(c.dist, 300, 0.01);
        CHECK_NEAR(c.yaw, 90, 0.01);
        CHECK_NEAR(c.pitch, 20, 0.01);
        CHECK(glm::distance(r.cam.pose(1.0f).eye, r.cam.pose(0.0f).eye) < 0.01f);  // cut, not a blend
        CHECK(m.ui().cut_scene_end(m, true, 0) == 1);
        CHECK(m.ui().camera.playing);
        int n = 0;
        for (; n < 400 && m.ui().cs.active; ++n) r.tick();
        CHECK(!m.ui().camera.playing);  // camera_script_done at the end of the 5.2 s track
        CHECK(!m.ui().cs.active);
        std::printf("cut scene move back: %d ticks\n", n);
        CHECK(n > 150);
        r.tick();
        CHECK(glm::distance(r.cam.state().pivot, glm::vec3(joan.pos.x, joan.pos.y, joan.pos.z + 50)) < 1.0f);
        CHECK_NEAR(r.cam.state().dist, r.cam.follow_distance(), 0.5);
    }
    // ... and a moving start: cut_scene_play flies to a far target and ends exactly on it.
    {
        const float pos[3] = {w.unit(w.player_id())->pos.x + 800, w.unit(w.player_id())->pos.y, w.unit(w.player_id())->pos.z};
        CHECK(m.ui().cut_scene_start(m, false, true, 0, pos, 250, 180, 15, 0) == 1);
        float max_dist = 0;
        for (int n = 0; n < 400 && m.ui().camera.playing; ++n) r.tick(), max_dist = std::max(max_dist, r.cam.state().dist);
        CHECK(!m.ui().camera.playing);
        CHECK(max_dist >= 799.0f);  // template rises to 800
        r.tick();
        const CamState& c = r.cam.state();
        CHECK(std::fabs(c.pivot.x - pos[0]) < 1.0f && std::fabs(c.pivot.y - pos[1]) < 1.0f);
        CHECK_NEAR(c.dist, 250, 0.01);
        CHECK_NEAR(c.pitch, 15, 0.01);
        m.ui().cut_scene_end(m, false, 0);
        for (int n = 0; n < 200 && m.ui().cs.active; ++n) r.tick();
        CHECK(!m.ui().cs.active);
        r.tick();
        CHECK(r.cam.state().dist <= r.cam.follow_distance() + 0.01f);
    }

    // 6. RTS: rts_init track to pitch 45 above Joan, box select, right-click move order.
    {
        game::Unit& joan = *w.unit(w.player_id());
        const game::Unit* jean = find_unit(w, "JEAN_DE_METZ");
        CHECK(jean != nullptr);
        s.rts_unlocked = true;
        CHECK(s.toggle_rts());
        for (int i = 0; i < 60; ++i) r.tick();
        const CamState c = r.cam.state();
        std::printf("rts camera pivot (%.0f %.0f %.0f) yaw %.1f pitch %.1f dist %.0f\n", c.pivot.x, c.pivot.y, c.pivot.z, c.yaw, c.pitch, c.dist);
        CHECK_NEAR(c.pitch, 45, 0.01);
        CHECK(c.dist >= GameCamera::kFollowDistance && c.dist <= 700.0f);
        CHECK(glm::distance(glm::vec2(c.pivot), glm::vec2(joan.pos.x, joan.pos.y)) < 1.0f);
        CHECK(cam_eye(c).z > joan.pos.z + 100.0f);

        const glm::mat4 vp = view_proj(r.cam.pose(1.0f));
        mouse(r, {12, 12}, true, false, vp);
        mouse(r, {500, 400}, true, false, vp);
        CHECK(r.cam.drag_box().z > 0.0f);
        mouse(r, {1012, 756}, true, false, vp);
        mouse(r, {1012, 756}, false, false, vp);
        const auto& sel = r.cam.rts_selection();
        std::printf("box selection: %zu units\n", sel.size());
        CHECK(std::find(sel.begin(), sel.end(), joan.id) != sel.end());
        CHECK(jean && std::find(sel.begin(), sel.end(), jean->id) != sel.end());
        CHECK(r.cam.drag_box() == glm::vec4(0.0f));

        // Right click on the ground 200 units ahead of the camera pivot: RTS_MOVE_TO (task 12) for the selection.
        const float y = glm::radians(c.yaw);
        glm::vec3 dest = c.pivot + glm::vec3(std::cos(y), std::sin(y), 0.0f) * 200.0f;
        dest.z = ground(w, dest);
        const glm::vec2 px = to_px(vp, dest);
        const int task0 = m.ai().st(joan).task.id;
        mouse(r, px, false, true, vp);
        mouse(r, px, false, false, vp);
        std::printf("joan task %d -> %d\n", task0, m.ai().st(joan).task.id);
        CHECK(m.ai().st(joan).task.id == 12);
        CHECK(m.ai().st(joan).task.id != task0);

        // Zoom out is clamped to 2500 and needs a valid view; leaving RTS flies back to the follow camera.
        game::InputFrame z;
        z.press(49);
        for (int i = 0; i < 5; ++i) r.tick(z);
        CHECK(r.cam.state().dist >= c.dist);
        CHECK(s.toggle_rts());
        for (int i = 0; i < 60; ++i) r.tick();
        CHECK_NEAR(r.cam.state().pitch, r.cam.follow_pitch(), 0.01);
        CHECK(r.cam.rts_selection().empty());
    }

    // 7. Camera tracks: the only map track (S2L1 joan_cut_scene_1) parses with 11 keys; eval hits the keys.
    {
        const auto bytes = formats::Vfs(game_dir).read("map/scenario2/S2L1.cm");
        CHECK(bytes.has_value());
        if (bytes) {
            const auto t = parse_camera_tracks(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            CHECK(t.size() == 1 && t[0].name == "joan_cut_scene_1" && t[0].keys.size() == 11);
            if (!t.empty() && t[0].keys.size() > 2) {
                const CamState k1 = eval_track(t[0].keys, 10000.0f);
                CHECK_NEAR(k1.pivot.x, 8294.31, 0.01);
                CHECK_NEAR(k1.yaw, 143.2, 0.01);
                CHECK_NEAR(k1.pitch, 11.0, 0.01);
            }
        }
    }

    // 8. Free camera: original units / degrees, heading 90 looks along +Y (engine -Z).
    {
        r.cam.set_free_pose({1000, 2000, 300}, 90, 0);
        const CameraPose p = r.cam.pose(0.5f);
        CHECK(glm::distance(p.eye, glm::vec3(1000, 300, -2000)) < 0.01f);
        CHECK(glm::distance(glm::normalize(p.target - p.eye), glm::vec3(0, 0, -1)) < 1e-4f);
    }
    return test_result();
}
