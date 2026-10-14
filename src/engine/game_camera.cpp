// GameCamera: follow / aim / cut-scene / camera-track / free cameras. RTS camera: game_camera_rts.cpp.
// docs/reverse-engineering/CAMERA_SYSTEM.md "Native GameCamera".
#include "engine/game_camera.hpp"

#include "engine/assets_cache.hpp"
#include "engine/game_input.hpp"
#include "formats/world_text.hpp"
#include "game/mission.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace joa::engine {

namespace {

constexpr float kDeg = 0.017453292f;
constexpr float kTickMs = float(game::World::kTickMs);

float wrap360(float a) {
    a = std::fmod(a, 360.0f);
    return a < 0.0f ? a + 360.0f : a;
}
// b moved by whole turns to within 180 degrees of a.
float near_angle(float a, float b) { return a + std::remainder(b - a, 360.0f); }

float ground(const game::World& w, glm::vec3 p) { return w.terrain() ? w.terrain()->height(p.x, p.y) : 0.0f; }

// Mission::world()/ui() have no const overloads; reset/before_tick only read through them.
game::Mission& mut(const game::Mission& m) { return const_cast<game::Mission&>(m); }

CameraPose to_pose(const CamState& s) {
    const glm::vec3 eye = cam_eye(s);
    const glm::vec3 target = eye + cam_forward(s) * std::max(s.dist, 1.0f);
    CameraPose p;
    p.eye = to_engine(eye.x, eye.y, eye.z);
    p.target = to_engine(target.x, target.y, target.z);
    return p;
}

CamState cut_state(const game::MissionUi::CutScene& cs) {  // FUN_006cb9e0: cam[] = x, y, z, distance, yaw, pitch
    CamState s;
    s.pivot = {cs.cam[0], cs.cam[1], cs.cam[2]};
    s.dist = cs.cam[3];
    s.yaw = cs.cam[4];
    s.pitch = cs.cam[5];
    return s;
}

// "cut_scene_play" built from the "cut_scene_move" template (FUN_006cb9e0 start / FUN_006cc6a0 end, CONFIRMED):
// keys 0..4 rise above `a`, 5..9 travel to `b` in x/y, 10.. turn to b's yaw; first / last keys take a's / b's
// pitch and distance. Yaw is lerped the short way (the original lerps raw degrees).
std::vector<CamTrack::Key> build_move(const std::vector<CamTrack::Key>& tpl, const CamState& a, const CamState& b) {
    std::vector<CamTrack::Key> k(tpl);
    const float by = near_angle(a.yaw, b.yaw);
    for (std::size_t i = 0; i < k.size(); ++i) {
        const CamState& t = tpl[i].s;
        CamState& s = k[i].s;
        s.roll = a.roll;
        if (i < 5) {
            s.pivot = a.pivot + glm::vec3(0, 0, t.pivot.z);
            s.yaw = a.yaw;
            s.pitch = std::max(t.pitch, a.pitch);
            s.dist = std::max(t.dist, a.dist);
        } else if (i < 10) {
            const float f = float(i - 5) / 4.0f;
            s.pivot = {glm::mix(a.pivot.x, b.pivot.x, f), glm::mix(a.pivot.y, b.pivot.y, f), a.pivot.z + t.pivot.z};
            s.yaw = a.yaw;
            s.pitch = t.pitch;
            s.dist = t.dist;
        } else {
            s.pivot = b.pivot + glm::vec3(0, 0, t.pivot.z);
            s.yaw = glm::mix(a.yaw, by, std::min(1.0f, float(i - 10) / 4.0f));
            s.pitch = t.pitch;
            s.dist = std::max(t.dist, b.dist);
        }
    }
    if (!k.empty()) {
        k.front().s.dist = a.dist, k.front().s.pitch = a.pitch;
        k.back().s.dist = b.dist, k.back().s.pitch = b.pitch;
    }
    return k;
}

}  // namespace

glm::vec3 cam_forward(const CamState& s) {
    const float y = s.yaw * kDeg, p = s.pitch * kDeg;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
}
glm::vec3 cam_eye(const CamState& s) { return s.pivot - cam_forward(s) * s.dist; }

std::vector<CamTrack> parse_camera_tracks(std::string_view text) {
    std::vector<CamTrack> out;
    const auto bt = formats::parse_block_text(text);
    if (!bt) return out;
    for (const formats::TextBlock& b : bt->blocks) {
        if (!formats::iequals(b.type, "CreateCameraAnimatedTrack")) continue;
        CamTrack t;
        for (const formats::KvLine& f : b.fields) {
            const float v = std::strtof(f.value.c_str(), nullptr);  // "172.2f" parses as 172.2
            if (formats::iequals(f.key, "track_name")) t.name = f.value;
            else if (formats::iequals(f.key, "frame_time")) t.keys.push_back({int(v), {}});
            else if (t.keys.empty()) continue;
            else if (formats::iequals(f.key, "x")) t.keys.back().s.pivot.x = v;
            else if (formats::iequals(f.key, "y")) t.keys.back().s.pivot.y = v;
            else if (formats::iequals(f.key, "z")) t.keys.back().s.pivot.z = v;
            else if (formats::iequals(f.key, "h_dir")) t.keys.back().s.yaw = v;
            else if (formats::iequals(f.key, "v_dir")) t.keys.back().s.pitch = v;
            else if (formats::iequals(f.key, "rot_a")) t.keys.back().s.roll = v;
            else if (formats::iequals(f.key, "cam_dist")) t.keys.back().s.dist = v;
        }
        out.push_back(std::move(t));
    }
    return out;
}

CamState eval_track(const std::vector<CamTrack::Key>& k, float t) {
    if (k.empty()) return {};
    std::size_t i = 0;
    while (i < k.size() && float(k[i].t_ms) < t) ++i;
    if (i == 0) return k.front().s;
    if (i == k.size()) return k.back().s;
    const CamState &a = k[i - 1].s, &b = k[i].s;
    const float span = float(k[i].t_ms - k[i - 1].t_ms);
    const float s = span > 0.0f ? (t - float(k[i - 1].t_ms)) / span : 1.0f;
    const auto tangent = [&](std::size_t j) {  // FUN_00429a60 with tension 0 (Catmull-Rom), ends clamped
        return (k[std::min(j + 1, k.size() - 1)].s.pivot - k[j == 0 ? 0 : j - 1].s.pivot) * 0.5f;
    };
    const float s2 = s * s, s3 = s2 * s;
    CamState r;
    r.pivot = a.pivot * (2 * s3 - 3 * s2 + 1) + tangent(i - 1) * (s3 - 2 * s2 + s) + b.pivot * (3 * s2 - 2 * s3) +
              tangent(i) * (s3 - s2);
    r.yaw = glm::mix(a.yaw, b.yaw, s);
    r.pitch = glm::mix(a.pitch, b.pitch, s);
    r.roll = glm::mix(a.roll, b.roll, s);
    r.dist = glm::mix(a.dist, b.dist, s);
    return r;
}

const CamTrack* GameCamera::track(std::string_view name) const {
    for (const CamTrack& t : tracks_)
        if (formats::iequals(t.name, name)) return &t;
    return nullptr;
}

void GameCamera::reset(const game::Mission& cm) {
    game::Mission& m = mut(cm);
    tracks_.clear();
    for (const std::string& path : {std::string("Resource\\cutscene.cm"), std::string("Resource\\rts.cm"),
                                    m.map_dir() + "\\" + m.info().stem() + ".cm"}) {
        const auto bytes = m.vfs().read(path);
        if (!bytes) continue;
        auto t = parse_camera_tracks(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        tracks_.insert(tracks_.end(), std::make_move_iterator(t.begin()), std::make_move_iterator(t.end()));
    }
    game::World& w = m.world();
    const game::PlayerController& pc = w.controller();
    const game::Unit* u = w.unit(w.player_id());
    // Behind the hero's heading: a freshly spawned unit still turns from 0 to its .uni heading (yaw_goal).
    yaw_ = wrap360(u ? u->yaw_goal : pc.camera_yaw_out);  // HYPOTHESIS: start / load behind the hero
    aiming_ = pc.range_mode;
    aim_yaw_ = wrap360(pc.range_base_yaw);
    aim_pitch_ = 0;
    w.controller().camera_yaw_out = aiming_ ? aim_yaw_ : yaw_;  // read back after every tick (after_tick)
    player_ = w.player_id();
    play_ = {};
    const auto& cs = m.ui().cs;
    std::copy(cs.cam, cs.cam + 6, seen_cut_);
    cut_cam_ = cs.active && cs.cam[3] != 0.0f;
    cut_back_ = false;
    tick_in_ = {};
    groups_ = {}, active_ = 0, last_commander_ = 0;  // a native load restores them afterwards (load_view, "VIEW")
    hover_type_ = hover_id_ = hover_siege_ = commander_ = 0;
    join_target_ = 0, join_args_.clear();
    rts_family(0);
    pressing_ = dragging_ = false;
    edge_dir_ = {};
    mmb_dx_ = 0;
    rts_ = w.game_mode == 4;
    if (rts_) {  // Rts_resume 0x43f940: no camera track
        rts_cam_ = follow_state(w);
        rts_cam_.pitch = kRtsPitch;
        rts_cam_.dist = rts_dist_;
        if (u) groups_[0] = m.ai().rts_select_with_team(u->id);
    }
    snap_ = true;
    if (cut_cam_) set(cut_state(cs), Mode::Cut);
    else if (rts_) set(rts_cam_, Mode::Rts);
    else if (aiming_) set(aim_state(w), Mode::Aim);
    else set(follow_state(w), Mode::Follow);
}

CamState GameCamera::follow_state(const game::World& w, bool clear) const {
    CamState s;
    const game::Unit* u = w.unit(w.player_id());
    s.pivot = u ? glm::vec3(u->pos.x, u->pos.y, u->pos.z + kPivotHeight) : cur_.pivot;
    s.yaw = yaw_;
    s.pitch = follow_pitch_;
    s.dist = follow_dist_;
    if (clear) clear_terrain(w, s);
    return s;
}

// Camera 1 (FUN_0042cb20): eye 70 above the unit, 10 units to its side, distance 0. The side axis comes from the
// actor matrix; HYPOTHESIS: its x row = the unit's right, subtracted (left shoulder).
CamState GameCamera::aim_state(const game::World& w) const {
    CamState s;
    const game::Unit* u = w.unit(w.player_id());
    const float y = aim_yaw_ * kDeg;
    s.pivot = u ? glm::vec3(u->pos.x, u->pos.y, u->pos.z + 70.0f) - glm::vec3(std::sin(y), -std::cos(y), 0.0f) * 10.0f
                : cur_.pivot;
    s.yaw = aim_yaw_;
    s.pitch = aim_pitch_;
    s.dist = 0;
    return s;
}

// CamMan_collision_adjust 0x424010 casts the pivot->eye ray (+5) against scene geometry and pulls the eye in
// (4.0 margin, never below half the distance). Native: the same against the terrain only, then the eye is lifted
// to ground + kClearance. HYPOTHESIS for the exact semantics; object collision is not reproduced.
void GameCamera::clear_terrain(const game::World& w, CamState& s) const {
    const glm::vec3 f = cam_forward(s);
    constexpr int kSteps = 16;
    for (int i = 1; i <= kSteps; ++i) {
        const glm::vec3 p = s.pivot - f * (s.dist + 5.0f) * (float(i) / kSteps);
        if (p.z < ground(w, p) + kClearance) {
            s.dist = std::max(s.dist * 0.5f, (s.dist + 5.0f) * float(i - 1) / kSteps - kClearance);
            break;
        }
    }
    const glm::vec3 e = cam_eye(s);
    const float g = ground(w, e) + kClearance;
    if (e.z < g) s.pivot.z += g - e.z;
}

void GameCamera::before_tick(const game::Mission& cm, game::InputFrame& f) {
    game::Mission& m = mut(cm);
    const game::World& w = m.world();
    const game::PlayerController& pc = m.world().controller();
    const float t = (std::clamp(f.mouse_sensitivity, -1.0f, 1.0f) + 1.0f) * 0.7f + 0.2f;  // 0x4322d0 curve
    const float inv = f.invert_mouse ? -1.0f : 1.0f;
    if (aiming_ && pc.range_mode) {
        // Camera_mouse_aim 0x42bcd0, scheme 3: yaw free, pitch within AIM_HA (60) of the base pitch.
        aim_yaw_ = wrap360(aim_yaw_ - f.mouse_dx * t * 0.08f);
        const float base = -pc.range_base_pitch;
        aim_pitch_ = std::clamp(aim_pitch_ + f.mouse_dy * t * 0.08f * inv, std::max(-89.0f, base - 60.0f), std::min(89.0f, base + 60.0f));
        f.camera_yaw_deg = aim_yaw_;
        f.camera_pitch_deg = -aim_pitch_;
    } else {
        if (!rts_ && !m.ui().cs.active) {
            // Ctrl_mouse_yaw 0x431790 pitch: += accDY * k (k = t * 0.2) degrees, [-20, 35]; a lowering step that
            // would put the eye under the ground is undone. Accumulated in float (no __ftol truncation).
            if (f.mouse_dy != 0.0f) {
                const float p = std::clamp(follow_pitch_ + f.mouse_dy * t * 0.2f * inv, kPitchMin, kPitchMax);
                CamState c = follow_state(w, false);
                c.pitch = p;
                const glm::vec3 e = cam_eye(c);
                if (p >= follow_pitch_ || e.z >= ground(w, e)) follow_pitch_ = p;
            }
            // Zoom 0x438720: distance -/+ frame_ms * 0.6 for actions 48 / 49 (wheel), [80, 350].
            if (f.held[48]) follow_dist_ -= kTickMs * kZoomPerMs;
            else if (f.held[49]) follow_dist_ += kTickMs * kZoomPerMs;
            follow_dist_ = std::clamp(follow_dist_, kZoomMin, kZoomMax);
        }
        f.camera_yaw_deg = yaw_;
        f.camera_pitch_deg = -follow_pitch_;
    }
    tick_in_ = f;
}

void GameCamera::after_tick(game::Mission& m, const GameInput& input) {  // keys: tick_in_; Shift: input
    game::World& w = m.world();
    game::PlayerController& pc = w.controller();
    game::MissionUi& ui = m.ui();
    yaw_ = wrap360(pc.camera_yaw_out);  // controller turns camera 0 with the hero (0x431790: camera yaw -= d)
    // Action mode keys (PlayerCtrl_process 0x42e1a0): Shift + 25..32 = Unit_toggle_hold of that commander; 24 / 25..28
    // = Ctrl_switch_commander 0 / 1..4 outside range mode (+0xac) when the scenario is > 1 or +0x79 (rts_unlocked),
    // the player neither climbing (Unit_get_busy_state) nor (dis)mounting (H_MNT_L..H_DMNT_R), unless dead. The RTS
    // commander keys are rts_tick's.
    if (w.game_mode != 4 && !ui.cs.active && !ui.game_cut_scene) {
        static constexpr const char* kCmd[8] = {"JOAN_OF_ARC", "JEAN_DE_METZ", "LAHIRE", "ALENCON",
                                                "ORLEAN", "arthur_de_richemont", "charlie7", "FLAVY"};
        bool done = false;
        for (int a = 25; a <= 32 && !done && input.shift_held(); ++a) {
            if (!tick_in_.pressed[std::size_t(a)]) continue;
            done = true;
            for (game::Unit& u : w.units())
                if (!u.deleted && u.cls >= 0 && formats::iequals(w.data().units[std::size_t(u.cls)].code, kCmd[a - 25])) {
                    m.ai().toggle_hold(u, true), hold_voice(m, u);
                    break;
                }
        }
        const game::Unit* p = w.unit(w.player_id());
        if (!done && !pc.range_mode && (rts_unlocked || m.scenario_no() > 1) && p &&
            (w.dead(*p) || (!p->m.climb && (p->action < game::H_MNT_L || p->action > game::H_DMNT_R))))
            for (int a = 24; a <= 28; ++a)
                if (tick_in_.pressed[std::size_t(a)]) {
                    hero_switch(m, a - 24, minimap_scale);
                    break;
                }
    }
    // Hero switch (Ctrl_switch_commander, scheme 3: FUN_00436190(1) + FUN_004384e0): camera 0 turns to the new hero's
    // facing at up to 40 degrees per frame; native: at once.
    if (w.player_id() != player_) {
        if (const game::Unit* u = w.unit(w.player_id()); u && player_) yaw_ = wrap360(u->yaw_goal);
        player_ = w.player_id();
    }
    if (pc.range_mode && !aiming_) aiming_ = true, aim_yaw_ = wrap360(pc.range_base_yaw), aim_pitch_ = 0;  // 0x42b3c0
    else if (!pc.range_mode) aiming_ = false;

    // Cut-scene camera: the script wrote cs.cam (a camera was given) unless it started with no_camera.
    const auto& cs = ui.cs;
    if (!cs.active) cut_cam_ = cut_back_ = false;
    else if (!std::equal(cs.cam, cs.cam + 6, seen_cut_)) cut_cam_ = true, cut_back_ = false;
    std::copy(cs.cam, cs.cam + 6, seen_cut_);

    if (w.game_mode == 4 && !rts_) rts_enter(m);
    else if (w.game_mode != 4 && rts_) rts_exit(m);

    if (ui.camera.playing) {
        if (!play_.active || !play_.ui || play_.name != ui.camera.name || play_.start_ms != ui.camera.start_ms) start_ui_track(m);
    } else if (play_.active && play_.ui) {
        play_ = {};  // stopped by the script (WAIT p301 outside a cut scene)
    }

    CamState s;
    if (play_track(m, s)) set(s, Mode::Track);
    else if (cs.active && cut_cam_ && !cut_back_) set(cut_state(cs), Mode::Cut);
    else if (rts_) rts_tick(m), set(rts_cam_, Mode::Rts);
    else if (aiming_) set(aim_state(w), Mode::Aim);
    else set(follow_state(w), Mode::Follow);
    pc.camera_yaw_out = aiming_ ? aim_yaw_ : yaw_;
}

void GameCamera::start_ui_track(game::Mission& m) {
    game::MissionUi& ui = m.ui();
    play_ = {};
    play_.ui = true;
    play_.name = ui.camera.name;
    play_.start_ms = ui.camera.start_ms;
    if (formats::iequals(play_.name, "cut_scene_play")) {
        const CamTrack* tpl = track("cut_scene_move");
        const bool back = ui.cs.status == 4;  // cut_scene_end(move_back)
        if (!tpl || tpl->keys.empty()) {
            ui.camera_script_done();
            cut_back_ = back;
            return;
        }
        play_.keys = build_move(tpl->keys, cur_, back ? follow_state(m.world()) : cut_state(ui.cs));
        play_.end_move = back;
    } else {
        const CamTrack* t = track(play_.name);
        if (!t || t->keys.empty()) {  // FUN_00428850 refuses unknown tracks
            ui.camera_script_done();
            return;
        }
        play_.keys = t->keys;
        play_.speed = ui.camera.speed > 0.0f ? ui.camera.speed : 1.0f;  // HYPOTHESIS: unset speed plays 1:1
    }
    play_.active = true;
}

// FUN_00427d90 on camera 2: t = (now - start) * speed; the pivot never sinks below the ground and eases toward it
// at frame_ms / 1000 per update.
bool GameCamera::play_track(game::Mission& m, CamState& out) {
    if (!play_.active || play_.keys.empty()) return false;
    const std::uint32_t now = m.now();
    if (std::int32_t(now - play_.start_ms) < 0) return false;
    const float t = float(now - play_.start_ms) * play_.speed;
    const float last = float(play_.keys.back().t_ms);
    out = eval_track(play_.keys, std::min(t, last));
    const float g = ground(m.world(), out.pivot);
    out.pivot.z = g > out.pivot.z ? g : out.pivot.z + (g - out.pivot.z) * kTickMs / 1000.0f;
    if (t > last) {
        if (play_.ui) m.ui().camera_script_done();
        if (play_.end_move) cut_back_ = true;
        play_.active = false;
    }
    return true;
}

void GameCamera::set(const CamState& s, Mode mode) {
    // Camera switches outside tracks are instant in the original (CamMan_start_transition(x, x)).
    if (mode != mode_ && mode != Mode::Track && mode != Mode::Rts && mode_ != Mode::Track) snap_ = true;
    cur_ = s;
    const CameraPose p = to_pose(s);
    prev_pose_ = snap_ ? p : cur_pose_;
    cur_pose_ = p;
    snap_ = false;
    mode_ = mode;
}

CameraPose GameCamera::pose(float alpha) const {
    if (free) {
        const float h = free_heading_ * kDeg, p = free_pitch_ * kDeg;
        const glm::vec3 f{std::cos(h) * std::cos(p), std::sin(h) * std::cos(p), std::sin(p)};
        CameraPose c;
        c.eye = to_engine(free_eye_.x, free_eye_.y, free_eye_.z);
        c.target = c.eye + to_engine(f.x, f.y, f.z);
        return c;
    }
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    CameraPose c = cur_pose_;
    c.eye = glm::mix(prev_pose_.eye, cur_pose_.eye, alpha);
    c.target = glm::mix(prev_pose_.target, cur_pose_.target, alpha);
    return c;
}

void GameCamera::set_free_pose(glm::vec3 eye, float heading_deg, float pitch_deg) {
    free = true;
    free_eye_ = eye;
    free_heading_ = heading_deg;
    free_pitch_ = std::clamp(pitch_deg, -89.0f, 89.0f);
}

// Fly: move 4/5/6/7 (forward/back/strafe), turn 8/9 or 50/51, pitch 48/49, Shift (3) = fast, mouse look.
void GameCamera::free_move(const GameInput& in, float dt, glm::vec2 mouse, const GameOptions& opt) {
    const auto axis = [&](int pos, int neg) { return float(in.held(pos)) - float(in.held(neg)); };
    const float k = ((std::clamp(opt.mouse_sensitivity, -1.0f, 1.0f) + 1.0f) * 0.7f + 0.2f) * 0.2f;  // degrees per count
    free_heading_ = wrap360(free_heading_ + 90.0f * dt * (axis(8, 9) + axis(50, 51)) - mouse.x * k);
    free_pitch_ = std::clamp(free_pitch_ + 45.0f * dt * axis(48, 49) - mouse.y * k * (opt.invert_mouse ? -1.0f : 1.0f), -89.0f, 89.0f);
    const float h = free_heading_ * kDeg, p = free_pitch_ * kDeg;
    const glm::vec3 f{std::cos(h) * std::cos(p), std::sin(h) * std::cos(p), std::sin(p)}, r{std::sin(h), -std::cos(h), 0.0f};
    free_eye_ += (f * axis(4, 5) + r * axis(7, 6)) * ((in.held(3) ? 2400.0f : 600.0f) * dt);
}

}  // namespace joa::engine
