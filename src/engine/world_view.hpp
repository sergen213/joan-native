#pragma once
// Client-side glue for an original scenario: world, Joan, follow/free camera. The fixed-step
// simulation keeps running in metres (sim.hpp); this maps it into original world units.
#include "engine/attach_items.hpp"
#include "engine/camera.hpp"
#include "engine/character_actor.hpp"
#include "engine/cli.hpp"
#include "engine/input.hpp"
#include "engine/grass_field.hpp"
#include "engine/particles.hpp"
#include "engine/sim.hpp"
#include "engine/sky_dome.hpp"
#include "engine/world_scene.hpp"
#include "engine/water.hpp"

#include <memory>
#include <optional>

namespace joa::engine {

// HYPOTHESIS: Joan's mesh is 78.7 units tall, taken as 1.75 m.
inline constexpr float kUnitsPerMetre = 45.0f;

class WorldView {
public:
    // o.game_dir (default "."), o.scenario, o.free_camera, o.camera_pose, o.anim. Throws on load failure.
    WorldView(IRenderer& renderer, const Options& o);
    ~WorldView();

    // Third-person framing of the original start capture (s04_04_mission1_hud.png: Joan about half
    // the screen height, eye slightly above her shoulders, nearly level). HYPOTHESIS values.
    void setup_camera(FollowCamera& camera) const;
    // After each simulation step: picks Joan's clip from the movement and advances all clips one tick.
    void tick(const World& current);
    // Updates the camera, poses Joan and appends the frame's draws. Fills view.view/camera_position.
    void frame(const World& shown, float alpha, FollowCamera& camera, const ActionState& actions, float dt,
               const glm::mat4& projection, FrameView& view, std::vector<DrawItem>& draws);

    // Stage lighting with the fog/clear colour of the current view direction (refreshed by frame()).
    Lighting lighting() const { return lighting_; }
    const WorldScene& scene() const { return *scene_; }
    const GrassField& grass() const { return *grass_; }
    const ParticleSystem& particles() const { return *particles_; }
    const WaterField& water() const { return *water_; }
    glm::vec3 joan_position() const { return joan_pos_; }
    struct UnitStats {
        int placed = 0, missing = 0;        // start units other than Joan
        int items = 0, items_missing = 0;   // equipment meshes attached / not (all units incl. Joan)
    };
    const UnitStats& unit_stats() const { return unit_stats_; }
    // CONFIG.DAT 0x9C shadow mode (GameOptions::shadow): 0 none, 1 ShadowModel blobs only, 2 a stencil
    // volume for the player's hero only, 3 volumes for every unit (Scene::draw_shadow actor loop 0x5f1cc0).
    int shadow_mode = 3;
    struct ShadowStats {
        int volumes = 0, models = 0;  // units drawn with a volume / with their ShadowModel, last frame()
    };
    const ShadowStats& shadow_stats() const { return shadow_stats_; }

private:
    glm::vec3 to_world(glm::vec3 sim) const;
    std::unique_ptr<Character> spawn(const UnitLook& look);

    formats::Vfs vfs_;
    std::unique_ptr<AssetCache> assets_;
    std::unique_ptr<WorldScene> scene_;
    std::unique_ptr<SkyDome> sky_;
    std::unique_ptr<GrassField> grass_;
    std::unique_ptr<ParticleSystem> particles_;
    std::unique_ptr<WaterField> water_;
    float time_s_ = 0;  // rendered time, drives sky drift
    Lighting lighting_;
    ShadowStats shadow_stats_;
    std::unique_ptr<Character> joan_;
    struct Npc {
        std::unique_ptr<Character> character;
        glm::mat4 transform{1.0f};
    };
    std::vector<Npc> npcs_;  // other start units (S1L1: Jean de Metz + 71 .uni soldiers), idling
    UnitStats unit_stats_;
    std::string forced_clip_;
    float frame_yaw_ = 0;  // sim -> world rotation about +Y
    glm::vec3 joan_pos_{0.0f};
    bool free_ = false;
    glm::vec3 free_pos_{0.0f};
    float free_yaw_ = 0, free_pitch_ = 0;  // engine: yaw about +Y (0 looks -Z), pitch up+
};

}  // namespace joa::engine
