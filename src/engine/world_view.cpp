#include "engine/world_view.hpp"

#include <glm/gtc/color_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

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
    lighting_ = scene_->lighting();
    sky_ = std::make_unique<SkyDome>(*assets_, glm::convertSRGBToLinear(lighting_.fog_color), scene_->cloud_textures());
    // "S1L1" -> map/scenario1 (DynGrass.res "_M1" variants).
    grass_ = std::make_unique<GrassField>(*assets_, scene_->grid(), o.scenario.size() > 1 ? o.scenario[1] - '0' : 0);
    particles_ = std::make_unique<ParticleSystem>(*assets_, o.scenario);
    water_ = std::make_unique<WaterField>(*assets_, o.scenario);
    const UnitDb db(vfs_);
    // Joan: unit.res JOAN_OF_ARC. `Model` (joan_armorC) is the mesh worn with the start armour
    // JOAN_ARMOR_LV1; the ModelName list (Joan_of_Arc, joan_armorB) are the other armour levels
    // (checked: only joan_armorC has the gold-trimmed skirt of s04_04_mission1_hud.png).
    const auto joan_look = db.look("JOAN_OF_ARC");
    if (!joan_look) throw std::runtime_error("unit.res JOAN_OF_ARC: missing");
    joan_ = spawn(*joan_look);
    joan_->play(forced_clip_.empty() ? joan_look->idle : forced_clip_);
    // Unit heading h (CCW from original +X) faces engine (cos h, 0, -sin h); sim yaw 0 faces -Z.
    frame_yaw_ = scene_->start_yaw() - glm::half_pi<float>();
    joan_pos_ = scene_->start_position();
    // Every other start unit (script placements, then the .uni armies) idles in place.
    for (const auto& u : scene_->units()) {
        if (lower(u.code) == "joan_of_arc") continue;
        try {
            const auto look = db.look(u.code);
            if (!look) throw std::runtime_error("no unit.res record / idle animation");
            Npc n{spawn(*look), glm::translate(glm::mat4(1.0f), u.position) * yaw_matrix(u.heading - glm::half_pi<float>() + kModelYaw)};
            n.character->play(look->idle);
            npcs_.push_back(std::move(n));
            ++unit_stats_.placed;
        } catch (const std::exception& e) {
            ++unit_stats_.missing;
            std::fprintf(stderr, "joan-native: unit %s: %s\n", u.code.c_str(), e.what());
        }
    }

    const WorldStats& s = scene_->stats();
    std::fprintf(stderr,
                 "joan-native: %s stage=%s objects=%d placed=%d missing=%d terrain_tris=%d groups=%d lightmaps=%d water=%d "
                 "units=%d missing_units=%d items=%d missing_items=%d missing_textures=%zu\n",
                 o.scenario.c_str(), s.stage.c_str(), s.objects, s.placed, s.missing_models, s.terrain_triangles, s.terrain_groups,
                 s.lightmaps, water_->count(), unit_stats_.placed, unit_stats_.missing, unit_stats_.items, unit_stats_.items_missing,
                 assets_->missing_textures());

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

std::unique_ptr<Character> WorldView::spawn(const UnitLook& look) {
    auto c = std::make_unique<Character>(*assets_, look.model, look.anim);
    for (const UnitLook::Item& item : look.items) {
        // Animated items (bows, e_bmlb.am1) are posed at their first key: item/anim/<name>.am2.
        std::string anim;
        if (lower(item.model).ends_with(".am1")) {
            const std::string stem = item.model.substr(item.model.rfind('/') + 1, item.model.size() - item.model.rfind('/') - 5);
            anim = "item/anim/" + stem + ".am2";
        }
        const std::string shadow = item.model.substr(0, item.model.rfind('.')) + "#s.SMO";
        const formats::Mesh* caster = anim.empty() && vfs_.exists(shadow) ? assets_->static_mesh(shadow) : nullptr;
        if (c->attach(assets_->model(item.model, anim), item.bone, caster)) ++unit_stats_.items;
        else ++unit_stats_.items_missing, std::fprintf(stderr, "joan-native: item %s on %s: not attached\n", item.model.c_str(), item.bone.c_str());
    }
    if (!look.shadow.empty()) c->set_shadow_model(assets_->model(look.shadow));
    return c;
}

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
    scene_->tick_lights();
}

void WorldView::frame(const World& shown, float alpha, FollowCamera& camera, const ActionState& a, float dt,
                      const glm::mat4& /*projection: replaced by the in-game depth range*/, FrameView& view,
                      std::vector<DrawItem>& draws) {
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
    // In-game depth range 4..4096 (world_scene.hpp kWorldNear/kWorldFar), same field of view.
    view.projection[2][2] = -(kWorldFar + kWorldNear) / (kWorldFar - kWorldNear);
    view.projection[3][2] = -2.0f * kWorldFar * kWorldNear / (kWorldFar - kWorldNear);
    lighting_ = scene_->lighting();
    lighting_.fog_color = scene_->fog_color(target - eye);
    lighting_.clear_color = glm::vec4(glm::convertSRGBToLinear(lighting_.fog_color), 1.0f);

    time_s_ += dt;
    sky_->build(eye, time_s_, draws);
    scene_->build(view.projection * view.view, eye, draws);
    water_->build(time_s_, eye, draws);
    grass_->build(eye, draws);
    particles_->build(dt, view.view, view.projection * view.view, draws);
    joan_->pose(alpha);
    const glm::mat4 joan = glm::translate(glm::mat4(1.0f), joan_pos_) * yaw_matrix(frame_yaw_ + shown.player_yaw + kModelYaw);
    const glm::vec3 sun = lighting_.sun_direction;
    shadow_stats_ = {};
    // Hardware-skinned units get one point light (Shader_build_vertex_shader clamps the skinned light
    // counts to 1 point + 1 directional): the first light of their sector.
    const auto unit = [&](Character& c, const glm::mat4& t) {
        const std::size_t first = draws.size();
        c.build(t, draws);
        for (std::size_t i = first; i < draws.size(); ++i) scene_->sector_lights(glm::vec3(t[3]), 1, draws[i]);
        // Scene::draw_shadow actor loop 0x5f1cc0: a volume within 1000 units of the eye (mode 3: every
        // unit, mode 2: the player's hero), else the ShadowModel within 2000 (CONFIRMED thresholds).
        if (shadow_mode <= 0) return;
        const float d = glm::distance(eye, glm::vec3(t[3]) + glm::vec3(0.0f, c.height() * 0.5f, 0.0f));
        if (d <= 1000.0f && (shadow_mode >= 3 || (shadow_mode == 2 && &c == joan_.get())))
            c.build_shadow(t, sun, Character::ShadowKind::Volume, draws), ++shadow_stats_.volumes;
        else if (d <= 2000.0f)
            c.build_shadow(t, sun, Character::ShadowKind::Model, draws), ++shadow_stats_.models;
    };
    unit(*joan_, joan);
    for (auto& n : npcs_) {
        n.character->pose(alpha);
        unit(*n.character, n.transform);
    }
}

}  // namespace joa::engine
