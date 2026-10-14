#pragma once
// In-game camera (docs/reverse-engineering/CAMERA_SYSTEM.md "Native GameCamera"): third-person follow camera 0
// (Ctrl_mouse_yaw 0x431790 / zoom 0x438720 / FUN_00563a20), range-mode aim camera 1 (Ctrl_range_enter 0x42b3c0,
// Camera_mouse_aim 0x42bcd0), cut-scene camera 5 (FUN_006cb9e0 / FUN_006cc6a0), timed camera tracks played on
// camera 2 (CreateCameraAnimatedTrack in Resource/*.cm and map/<dir>/<stem>.cm, FUN_00427d90), the RTS camera
// (Rts_init 0x43fc90, Camera_rts_edge_scroll 0x445990, Rts_camera_rotate/zoom 0x447740/0x447960) and a free
// fly camera for render tests. Simulation-side state is evaluated once per 33 ms tick; pose(alpha) interpolates.
#include "engine/camera.hpp"
#include "engine/input.hpp"
#include "engine/ui_model.hpp"  // GameOptions (free camera mouse look)
#include "game/controller_player.hpp"

#include <glm/glm.hpp>

#include <array>
#include <bitset>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace joa::game {
class Mission;
class World;
}  // namespace joa::game

namespace joa::engine {

class AssetCache;
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

// Unit_toggle_hold 0x5121c0 voice after AiSystem::toggle_hold: "RTS" + prefix + 006 (held) / 007 (resumed) for a
// rank > 3 unit when in RTS or not the player (MissionEvent Sound, played 2D by GameAudio).
void hold_voice(game::Mission& m, const game::Unit& u);

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
    // Action mode keys (PlayerCtrl_process 0x42e1a0): Shift + 25..32 toggles that commander's hold; 24 Switch
    // character / 25..28 Joan, Jean, La Hire, Alencon = Mission::switch_hero, outside range mode, when the scenario is > 1 or
    // rts_unlocked (+0x79, the same test hook as Session::rts_unlocked), the player not climbing / (dis)mounting.
    bool rts_unlocked = false;
    glm::vec2 minimap_scale{0.0f};  // switch_hero view test (set from the drawn HUD; 0 = no minimap)
    // Rts_deinit 0x4404c0 with camera, before the gameplay part (Session::on_rts_exit): the commander last chosen by
    // key / portrait (+0x1237) becomes the player if he is a playable hero.
    void rts_deinit(game::Mission& m);
    CameraPose pose(float alpha) const;

    // Free fly camera (--free-camera / --camera X,Y,Z,H,P): eye in original units, heading as unit yaw,
    // pitch positive up (WorldView's former free camera conventions).
    bool free = false;
    void set_free_pose(glm::vec3 eye, float heading_deg, float pitch_deg);
    // Mouse look (relative mode while the fly camera runs): `mouse` = raw counts this frame; right turns right, down
    // looks down (opt.invert_mouse flips pitch) at the game's mouse-yaw rate ((sensitivity + 1) * 0.7 + 0.2) * 0.2
    // degrees per count (GameInput::take, 0x431790).
    void free_move(const GameInput& input, float dt, glm::vec2 mouse = glm::vec2(0.0f), const GameOptions& opt = {});

    // RTS mouse (per rendered frame while Session::in_rts(), Rts_process_input 0x440b30): Ctrl/Alt+digit groups,
    // double-click (siege crew dismiss / gate / box), click / box select, hover pick (Rts_pick_target 0x44ac60) and
    // the RMB command of the hovered type, edge scrolling and middle-drag rotation (applied on the next tick).
    // view_proj = the frame's engine-space projection * view (GL clip convention); viewport x, y, w, h in pixels
    // (top-left origin). Presses inside `rts_blocked` (HUD minimap / portraits) are left to the HUD.
    void rts_input(game::Mission& m, const RawInput& raw, glm::vec2 mouse_px, glm::ivec4 viewport, const glm::mat4& view_proj);
    // Point & click (mode 2) pick for the tick's popped click (PlayerCtrl_mouse_click 0x438b30, game_camera_rts.cpp):
    // drops the click outside the 3-D view (Mouse_click_in_rect 0x5945d0), else fills f.pick_* with the nearest unit
    // (hero excluded) / map item / floor drop on the ray (0x439110) or the terrain point (0x43a3b0).
    void click_pick(game::Mission& m, game::InputFrame& f, glm::vec2 mouse_px, glm::ivec4 viewport, const glm::mat4& view_proj) const;
    // Working selection = group[active] (ViewCtrl +0xf9/+0x105/+0x1236): group 0 or an Alt-recalled group 1..9.
    const std::vector<int>& rts_selection() const { return groups_[std::size_t(active_)]; }
    const std::vector<int>& rts_group(int n) const { return groups_[std::size_t(n)]; }
    glm::vec4 drag_box() const;  // x0, y0, x1, y1 pixels of the selection rectangle; all zero when not dragging
    // Rts_pick_target result of the last rts_input: 0 terrain, 1 enemy unit, 2 attackable item, 4 floor item,
    // 6 siege weapon to crew, 7 join team, 8 follow commander, 9 cannot, 11 interact (GAMEPLAY_AI_TROOPS.md §5 3.3).
    int rts_hover_type() const { return hover_type_; }
    int rts_hover_target() const { return hover_id_; }
    int rts_hover_siege() const { return hover_siege_; }  // DAT_00a5704d: crewable siege weapon under the cursor
    int rts_commander() const { return commander_; }  // ViewCtrl +0x1238 focused commander recno (0 none)
    // HUD minimap click (FUN_00717240): left = camera jump (distance stepped -200 until the view is valid), right =
    // Rts_cmd_move with the frame's Ctrl (attack-move).
    void rts_minimap(game::Mission& m, int button, glm::vec2 world);
    // Party portrait click (Rts_process_input step 4 -> Rts_select_commander by recno; 2nd click within 500 ms = dbl).
    void rts_portrait(game::Mission& m, int recno);
    // "Team over capacity" (Rts_cmd_assign 0x44f750 -> G_POPUP t_interf CHARMOVE|1, game paused, +0x1269): target
    // leader of the pending join (0 none) and the popup arguments (name, max, joining, removed, is Joan).
    int rts_join_target() const { return join_target_; }
    const std::vector<std::string>& rts_join_args() const { return join_args_; }
    void rts_join_answer(game::Mission& m, bool ok);  // OK -> Rts_cmd_assign(target, 1); CANCEL -> cannot (CASSTO)
    const std::string& rts_voice() const { return voice_; }  // last acknowledgement voice (SOUND code) emitted
    // View-control block (the original saves ViewCtrl raw, 0x4857c0: 0x126e bytes + camera): groups 0..9, active
    // group, +0x1237 / +0x1238 commanders, RTS camera. Native save section "VIEW" (not hashed).
    std::vector<std::uint8_t> save_view() const;
    void load_view(std::span<const std::uint8_t> data);  // after reset(); ignores malformed data
    std::vector<glm::vec4> rts_blocked;  // pixel rects (x, y, w, h) the RTS mouse ignores; set from the drawn HUD
    // RTSICON cursor sprite (Rts_process 0x442ef0 animation, Rts_draw 0x4450f0 idle name by hover type); advances
    // the command animation by dt (50 ms per frame).
    std::string rts_cursor(float dt);
    AssetCache* assets = nullptr;  // map item models: Rts_pick_target picks their bounds (actor +0x90)

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
    // Rts_select_commander 0x451530: commander idx 1..8 (or the unit `recno`), add = Shift, dbl = double-clicked portrait.
    void select_commander(game::Mission& m, int idx, bool add, int recno = 0, bool dbl = false);
    void rts_pick(game::Mission& m, glm::vec2 mouse, glm::ivec4 vp, const glm::mat4& vp_m);
    void rts_command(game::Mission& m);   // RMB on the hovered target (Rts_process_input step 9)
    void rts_double_click(game::Mission& m, glm::vec2 mouse, glm::ivec4 vp, const glm::mat4& vp_m);
    void rts_flatten();                   // additive select: live members of group[active] -> group 0, active 0
    void rts_family(int f) { cursor_family_ = f, cursor_frame_ = 0, cursor_ms_ = 0; }
    void say(game::Mission& m, std::string code);  // acknowledgement voice (FUN_00492c40, 2D SOUND entry)
    int rnd(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }  // Misc_random_range
    void join_done(game::Mission& m, int target, bool ok, bool follow);  // Rts_cmd_assign voices + cursor
    std::string cannot_code(const game::World& w);  // "cannot" voice of the selection
    std::string select_code(const game::World& w);  // selection voice
    void move_order(game::Mission& m, glm::vec3 to, bool minimap);  // Rts_cmd_move + voice + cursor
    int pick_building(game::Mission& m, glm::vec3 at, bool rmb);  // Rts_pick_building + type-10 tests / RMB

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
    std::array<std::vector<int>, 10> groups_;  // group 0 working selection, 1..9 Ctrl-stored (max 100 each)
    int active_ = 0;
    std::vector<int> drag_base_;    // group 0 entries that pre-existed the Shift drag box (+0x104)
    int hover_type_ = 0, hover_id_ = 0, hover_att_ = 3;  // hover_att_: attack cursor family of the selection
    int hover_siege_ = 0, commander_ = 0;
    glm::vec3 hover_pos_{0.0f};     // terrain / floor item point of the hover pick
    int cursor_family_ = 0, cursor_frame_ = 0;
    float cursor_ms_ = 0;
    bool ctrl_ = false, alt_ = false, shift_ = false;  // modifiers of the last rts_input frame
    std::string cursor_name_ = "POINT00";              // ViewCtrl +0x1258
    std::bitset<10> digits_;        // top-row 1..9 held last frame (group key edges)
    int click_ms_ = -100000, portrait_ms_ = -100000, portrait_ = 0;
    glm::vec2 click_px_{0.0f};
    int last_commander_ = 0;        // +0x1237 commander index of the last key / portrait selection (0 none)
    int join_target_ = 0;
    std::vector<std::string> join_args_;
    std::string voice_;
    std::minstd_rand rng_{1};       // cosmetic (voice variants): never the sim RNG
    glm::vec2 edge_dir_{0.0f};      // screen-space edge scroll direction of the last frames (x right, y up)
    float mmb_dx_ = 0;              // middle-drag mouse x since the last tick
    std::bitset<8> prev_buttons_;
    bool pressing_ = false, dragging_ = false;
    glm::vec2 drag_from_{0.0f}, drag_to_{0.0f};
};

}  // namespace joa::engine
