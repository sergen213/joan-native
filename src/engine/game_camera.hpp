#pragma once
// In-game camera (docs/reverse-engineering/CAMERA_SYSTEM.md "Native GameCamera"): third-person follow camera 0
// (Ctrl_mouse_yaw 0x431790 / zoom 0x438720 / FUN_00563a20), range-mode aim camera 1 (Ctrl_range_enter 0x42b3c0,
// Camera_mouse_aim 0x42bcd0), cut-scene camera 5 (FUN_006cb9e0 / FUN_006cc6a0), timed camera tracks played on
// camera 2 (CreateCameraAnimatedTrack in Resource/*.cm and map/<dir>/<stem>.cm, FUN_00427d90), the RTS camera
// (Rts_init 0x43fc90, Camera_rts_edge_scroll 0x445990, Rts_camera_rotate/zoom 0x447740/0x447960) and a free
// fly camera for render tests. Simulation-side state is evaluated once per 33 ms tick; pose(alpha) interpolates.
#include "engine/camera.hpp"
#include "engine/input.hpp"
#include "game/controller_player.hpp"

#include <glm/glm.hpp>

#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace joa::game {
class Mission;
class World;
}  // namespace joa::game

namespace joa::engine {

class GameInput;

// One original CameraState (CamMan +0x04..+0x1c) in original world units, z up: the camera looks from
// eye = pivot - forward * dist toward the pivot. yaw = heading the camera looks along (degrees, CCW from +X,
// the unit-yaw convention), pitch = degrees below the horizon (positive looks down), roll unused by the view.
struct CamState {
    glm::vec3 pivot{0.0f};
    float yaw = 0, pitch = 0, roll = 0, dist = 0;
};
glm::vec3 cam_forward(const CamState& s);
glm::vec3 cam_eye(const CamState& s);

// CreateCameraAnimatedTrack: keys frame_time (ms), x/y/z, h_dir, v_dir, rot_a (deg), cam_dist.
struct CamTrack {
    struct Key {
        int t_ms = 0;
        CamState s;
    };
    std::string name;
    std::vector<Key> keys;
};
std::vector<CamTrack> parse_camera_tracks(std::string_view text);
// FUN_00427d90 without the ground clamp: Catmull-Rom position (FUN_00429a60 tangents, Hermite 0x4279b0),
// linear angles / distance (0x427bb0). t_ms clamps to the key range.
CamState eval_track(const std::vector<CamTrack::Key>& keys, float t_ms);

class GameCamera {
public:
    // Follow-camera constants (CONFIRMED unless noted; see CAMERA_SYSTEM.md).
    static constexpr float kPivotHeight = 50.0f;      // FUN_00563a20: unit position + 50 z
    static constexpr float kFollowDistance = 187.5f;  // .data 0xa0bbf0 (foot) / 0xa0bbf4 (mounted)
    static constexpr float kZoomMin = 80.0f, kZoomMax = 350.0f, kZoomPerMs = 0.6f;  // 0x438720
    static constexpr float kPitchMin = -20.0f, kPitchMax = 35.0f;                    // 0x431790
    static constexpr float kClearance = 4.0f;         // HYPOTHESIS: CamMan_collision_adjust near margin 4.0
    static constexpr float kRtsPitch = 45.0f;         // 0x3f490fdb
    static constexpr float kRtsZoomMax = 2500.0f, kRtsZoomPerMs = 2.0f, kRtsRotDegPerMs = 0.15f;
    static constexpr float kRtsScrollPerMs = 0.5f;    // keys: normalized axis * 5.0 * frame_ms / 10
    static constexpr float kRtsClearance = 40.0f;

    void reset(const game::Mission& m);  // start / load: behind the player, camera tracks loaded
    // Per tick, before Session::tick: mouse pitch / zoom / range aim from the tick's input; fills
    // f.camera_yaw_deg and f.camera_pitch_deg (elevation of the view, positive up, as the controller expects).
    void before_tick(const game::Mission& m, game::InputFrame& f);
    // Per tick, after Session::tick: controller yaw read-back, cut scenes / camera scripts, RTS camera and
    // commander keys. Keys come from the tick's InputFrame seen in before_tick (the GameInput is not read).
    void after_tick(game::Mission& m, const GameInput& input);
    CameraPose pose(float alpha) const;

    // Free fly camera (--free-camera / --camera X,Y,Z,H,P): eye in original units, heading as unit yaw,
    // pitch positive up (WorldView's former free camera conventions).
    bool free = false;
    void set_free_pose(glm::vec3 eye, float heading_deg, float pitch_deg);
    void free_move(const GameInput& input, float dt);

    // RTS mouse (per rendered frame while Session::in_rts()): click / box select, RMB move / attack, edge
    // scrolling and middle-drag rotation (applied on the next tick). view_proj = the frame's engine-space
    // projection * view (GL clip convention); viewport x, y, w, h in pixels (top-left origin).
    void rts_input(game::Mission& m, const RawInput& raw, glm::vec2 mouse_px, glm::ivec4 viewport, const glm::mat4& view_proj);
    const std::vector<int>& rts_selection() const { return sel_; }
    glm::vec4 drag_box() const;  // x0, y0, x1, y1 pixels of the selection rectangle; all zero when not dragging

    const CamState& state() const { return cur_; }  // camera of the last tick
    float follow_distance() const { return follow_dist_; }
    float follow_pitch() const { return follow_pitch_; }

private:
    enum class Mode : std::uint8_t { Follow, Aim, Cut, Track, Rts };
    struct Playing {
        bool active = false;
        bool ui = false;        // a MissionUi camera script (camera_script_done when it ends)
        bool end_move = false;  // cut_scene_play built by cut_scene_end(move_back): returns to the follow camera
        bool rts_out = false;
        std::string name;
        std::uint32_t start_ms = 0;
        float speed = 1.0f;
        std::vector<CamTrack::Key> keys;
    };

    const CamTrack* track(std::string_view name) const;
    CamState follow_state(const game::World& w, bool clear = true) const;
    CamState aim_state(const game::World& w) const;
    void clear_terrain(const game::World& w, CamState& s) const;
    void start_ui_track(game::Mission& m);
    bool play_track(game::Mission& m, CamState& out);  // true while the track drives the camera
    void set(const CamState& s, Mode mode);
    // game_camera_rts.cpp
    void rts_enter(game::Mission& m);
    void rts_exit(game::Mission& m);
    void rts_tick(game::Mission& m);
    bool rts_view_valid(const game::World& w, const CamState& s) const;
    void select_commander(game::Mission& m, int idx, bool add);

    std::vector<CamTrack> tracks_;  // Resource/cutscene.cm, Resource/rts.cm, map/<dir>/<stem>.cm
    float yaw_ = 0;                 // camera 0 yaw (degrees, [0, 360))
    float follow_pitch_ = 0, follow_dist_ = kFollowDistance;
    float aim_yaw_ = 0, aim_pitch_ = 0;
    bool aiming_ = false;
    float seen_cut_[6] = {};
    bool cut_cam_ = false, cut_back_ = false;
    Playing play_;
    game::InputFrame tick_in_;
    CamState cur_;
    CameraPose prev_pose_, cur_pose_;
    Mode mode_ = Mode::Follow;
    bool snap_ = true;

    // Free camera.
    glm::vec3 free_eye_{0.0f};
    float free_heading_ = 0, free_pitch_ = 0;

    // RTS camera / selection.
    bool rts_ = false;
    int player_ = 0;                // hero of the last tick (switch -> camera turns to the new hero)
    CamState rts_cam_;
    float rts_dist_ = 700.0f;       // PlayerCtrl +0xed (700.0 at init, HYPOTHESIS: RTS distance)
    std::vector<int> sel_;
    int last_commander_ = 0;
    glm::vec2 edge_dir_{0.0f};      // screen-space edge scroll direction of the last frames (x right, y up)
    float mmb_dx_ = 0;              // middle-drag mouse x since the last tick
    std::bitset<8> prev_buttons_;
    bool pressing_ = false, dragging_ = false;
    glm::vec2 drag_from_{0.0f}, drag_to_{0.0f};
};

}  // namespace joa::engine
