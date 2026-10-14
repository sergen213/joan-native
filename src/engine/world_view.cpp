#include "engine/world_view.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace joa::engine {
namespace {

// Joan's AM1 faces original +X (= unit heading 0); this turns it to face engine -Z at yaw 0.
// Checked visually (work/scene/joan_*.png): she then faces Jean de Metz's scripted position.
constexpr float kModelYaw = glm::half_pi<float>();

glm::mat4 yaw_matrix(float yaw) { return glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0)); }

}  // namespace

WorldView::WorldView(IRenderer& renderer, const Options& o)
    : vfs_(o.game_dir.empty() ? std::filesystem::path(".") : std::filesystem::path(o.game_dir)), forced_clip_(o.anim) {
    assets_ = std::make_unique<AssetCache>(vfs_, renderer);
    scene_ = std::make_unique<WorldScene>(*assets_, o.scenario);
    joan_ = std::make_unique<Character>(*assets_, "unit/Joan_of_Arc.AM1", "anim/joan_of_arc.am2");
    joan_->play(forced_clip_.empty() ? "f_idle" : forced_clip_);
    // Unit heading h (CCW from original +X) faces engine (cos h, 0, -sin h); sim yaw 0 faces -Z.
    frame_yaw_ = scene_->start_yaw() - glm::half_pi<float>();
    joan_pos_ = scene_->start_position();
    // Other scripted start units: unit/<code>.AM1 animated by anim/<code>.am2, idling in place.
    for (const auto& u : scene_->scripted_units()) {
        if (lower(u.code) == "joan_of_arc") continue;
        try {
            Npc n{std::make_unique<Character>(*assets_, "unit/" + u.code + ".AM1", "anim/" + u.code + ".am2"), {}};
            if (!n.character->play("f_idle")) n.character->play("default");
            n.transform = glm::translate(glm::mat4(1.0f), u.position) * yaw_matrix(u.heading - glm::half_pi<float>() + kModelYaw);
            npcs_.push_back(std::move(n));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "joan-native: unit %s: %s\n", u.code.c_str(), e.what());
        }
    }

    const WorldStats& s = scene_->stats();
    std::fprintf(stderr,
                 "joan-native: %s stage=%s objects=%d placed=%d missing=%d terrain_tris=%d groups=%d lightmaps=%d water=%d "
                 "missing_textures=%zu\n",
                 o.scenario.c_str(), s.stage.c_str(), s.objects, s.placed, s.missing_models, s.terrain_triangles, s.terrain_groups,
                 s.lightmaps, s.water, assets_->missing_textures());

    free_ = o.free_camera || !o.camera_pose.empty();
    // Free camera default: behind and above Joan, looking along her heading.
    free_yaw_ = frame_yaw_;
    free_pitch_ = glm::radians(-20.0f);
    free_pos_ = joan_pos_ + glm::vec3(yaw_matrix(frame_yaw_) * glm::vec4(0, 250, 500, 1));
    if (o.camera_pose.size() >= 3) free_pos_ = to_engine(o.camera_pose[0], o.camera_pose[1], o.camera_pose[2]);
    if (o.camera_pose.size() >= 4) free_yaw_ = glm::radians(o.camera_pose[3]) - glm::half_pi<float>();
    if (o.camera_pose.size() >= 5) free_pitch_ = glm::radians(o.camera_pose[4]);
}

WorldView::~WorldView() = default;

glm::vec3 WorldView::to_world(glm::vec3 sim) const {
    return scene_->start_position() + glm::vec3(yaw_matrix(frame_yaw_) * glm::vec4(sim * kUnitsPerMetre, 0.0f));
}

void WorldView::setup_camera(FollowCamera& c) const {
    c.distance = 190.0f / kUnitsPerMetre;
    c.pitch = glm::radians(8.0f);
    c.target_height = 55.0f / kUnitsPerMetre;
}

void WorldView::tick(const World& w) {
    if (forced_clip_.empty()) {
        const float speed = glm::length(glm::vec2(w.player_velocity.x, w.player_velocity.z));
        joan_->play(speed < 0.1f ? "f_idle" : speed > 3.0f ? "f_run" : "f_walk");
    }
    joan_->tick(float(kTickSeconds));
    for (auto& n : npcs_) n.character->tick(float(kTickSeconds));
}

void WorldView::frame(const World& shown, float alpha, FollowCamera& camera, const ActionState& a, float dt,
                      const glm::mat4& projection, FrameView& view, std::vector<DrawItem>& draws) {
    joan_pos_ = to_world(shown.player_position);
    joan_pos_.y = scene_->ground_height(joan_pos_);

    glm::vec3 eye, target;
    if (free_) {
        free_yaw_ -= a[Action::LookX];
        free_pitch_ = std::clamp(free_pitch_ + a[Action::LookY], glm::radians(-89.0f), glm::radians(89.0f));
        const glm::vec3 fwd{-std::sin(free_yaw_) * std::cos(free_pitch_), std::sin(free_pitch_), -std::cos(free_yaw_) * std::cos(free_pitch_)};
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
        const float speed = (a.down(Action::Run) ? 2400.0f : 600.0f) * dt;
        free_pos_ += (fwd * a[Action::MoveY] + right * a[Action::MoveX]) * speed;
        eye = free_pos_;
        target = free_pos_ + fwd;
    } else {
        // Follow camera in sim metres around Joan (terrain-following height), mapped to world units.
        const glm::vec3 subject{shown.player_position.x, (joan_pos_.y - scene_->start_position().y) / kUnitsPerMetre,
                                shown.player_position.z};
        camera.update(subject, dt);
        eye = to_world(camera.eye);
        target = to_world(camera.target);
        // Keep the eye above the ground (the original RTS camera keeps 40 units of clearance).
        eye.y = std::max(eye.y, scene_->ground_height(eye) + 40.0f);
    }
    view.view = glm::lookAtRH(eye, target, glm::vec3(0, 1, 0));
    view.camera_position = eye;

    scene_->build(projection * view.view, eye, draws);
    joan_->pose(alpha);
    const glm::mat4 joan = glm::translate(glm::mat4(1.0f), joan_pos_) * yaw_matrix(frame_yaw_ + shown.player_yaw + kModelYaw);
    joan_->build(joan, draws);
    for (auto& n : npcs_) {
        n.character->pose(alpha);
        n.character->build(n.transform, draws);
    }
}

}  // namespace joa::engine
