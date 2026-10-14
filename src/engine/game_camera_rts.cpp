// GameCamera RTS part: camera (Rts_init 0x43fc90 / Rts_deinit 0x4404c0 tracks, Camera_rts_edge_scroll 0x445990,
// Rts_camera_rotate 0x447740, Rts_camera_zoom 0x447960, Rts_camera_view_valid 0x453930) and mouse selection /
// orders (Rts_process_input 0x440b30). GAMEPLAY_AI_TROOPS.md §2.3-§3.8.
#include "engine/game_camera.hpp"

#include "engine/assets_cache.hpp"
#include "formats/vfs_fs.hpp"
#include "game/mission.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace joa::engine {

namespace {

constexpr float kDeg = 0.017453292f;
constexpr float kTickMs = float(game::World::kTickMs);

float ground(const game::World& w, glm::vec3 p) { return w.terrain() ? w.terrain()->height(p.x, p.y) : 0.0f; }

bool allied(const game::World& w, const game::Unit& u) { return u.id == w.player_id() || !w.hostile_to_player(u); }

float unit_height(const game::World& w, const game::Unit& u) {
    if (u.cls < 0 || std::size_t(u.cls) >= w.data().units.size()) return 80.0f;
    const auto& c = w.data().units[std::size_t(u.cls)];
    return c.height > 0 ? float(c.height) : c.cyl_height;
}

// Original world point -> viewport pixel (top-left origin); false behind the eye.
bool project(const glm::mat4& vp_m, glm::ivec4 vp, glm::vec3 p, glm::vec2& px, float& depth) {
    const glm::vec4 c = vp_m * glm::vec4(to_engine(p.x, p.y, p.z), 1.0f);
    if (c.w <= 1e-3f) return false;
    px = {float(vp.x) + (c.x / c.w * 0.5f + 0.5f) * float(vp.z), float(vp.y) + (0.5f - c.y / c.w * 0.5f) * float(vp.w)};
    depth = c.w;
    return true;
}

// Unit under the cursor (0x447ab0 role): the screen rectangle of its feet..head segment, nearest wins.
int pick_unit(const game::World& w, const glm::mat4& vp_m, glm::ivec4 vp, glm::vec2 mouse) {
    int best = 0;
    float best_depth = 1e30f;
    for (const game::Unit& u : w.units()) {
        if (u.deleted || w.dead(u)) continue;
        glm::vec2 foot, head;
        float d, dh;
        const glm::vec3 p{u.pos.x, u.pos.y, u.pos.z};
        if (!project(vp_m, vp, p, foot, d) || !project(vp_m, vp, p + glm::vec3(0, 0, unit_height(w, u)), head, dh)) continue;
        const float half = std::max(10.0f, 0.3f * (foot.y - head.y));
        if (mouse.x < foot.x - half || mouse.x > foot.x + half || mouse.y < head.y - 4.0f || mouse.y > foot.y + 4.0f) continue;
        if (d < best_depth) best = u.id, best_depth = d;
    }
    return best;
}

// View3D_unproject ray against the terrain (0x452830 role).
bool pick_ground(const game::World& w, const glm::mat4& vp_m, glm::ivec4 vp, glm::vec2 mouse, glm::vec3& out) {
    const glm::vec2 ndc{(mouse.x - float(vp.x)) / float(vp.z) * 2.0f - 1.0f, 1.0f - (mouse.y - float(vp.y)) / float(vp.w) * 2.0f};
    const glm::mat4 inv = glm::inverse(vp_m);
    glm::vec4 a = inv * glm::vec4(ndc, -1.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
    a /= a.w, b /= b.w;
    const glm::vec3 o{a.x, -a.z, a.y}, e{b.x, -b.z, b.y};  // engine -> original (inverse of to_engine)
    const float len = std::min(glm::length(e - o), 8193.0f);
    const glm::vec3 dir = glm::normalize(e - o);
    constexpr float kStep = 16.0f;
    for (float t = 0.0f; t <= len; t += kStep) {
        if ((o + dir * t).z > ground(w, o + dir * t)) continue;
        float lo = std::max(0.0f, t - kStep), hi = t;
        for (int i = 0; i < 10; ++i) {
            const float mid = (lo + hi) * 0.5f;
            ((o + dir * mid).z > ground(w, o + dir * mid) ? lo : hi) = mid;
        }
        out = o + dir * hi;
        out.z = ground(w, out);
        return true;
    }
    return false;
}

std::vector<CamTrack::Key> tpl_keys(const CamTrack* t) { return t ? t->keys : std::vector<CamTrack::Key>{}; }

}  // namespace

// Rts_camera_view_valid 0x453930 (CONFIRMED structure): the rays through the four screen corners must hit the
// terrain inside the map. Original viewport 1024x768 (4:3); grids other than a GRI accept every view.
bool GameCamera::rts_view_valid(const game::World& w, const CamState& s) const {
    const auto* gri = dynamic_cast<const game::GriTerrain*>(w.terrain());
    if (!gri) return true;
    const float mx = float(gri->grid().cells_x) * formats::TerrainGrid::kCellSize;
    const float my = float(gri->grid().cells_y) * formats::TerrainGrid::kCellSize;
    const glm::vec3 f = cam_forward(s), r = glm::normalize(glm::cross(f, glm::vec3(0, 0, 1))), u = glm::cross(r, f);
    const glm::vec3 eye = cam_eye(s);
    const float th = std::tan(22.5f * kDeg), aspect = 4.0f / 3.0f;
    for (const float sx : {-1.0f, 1.0f})
        for (const float sy : {-1.0f, 1.0f}) {
            const glm::vec3 d = glm::normalize(f + r * (sx * th * aspect) + u * (sy * th));
            bool hit = false;
            for (float t = 0.0f; t <= 8193.0f && !hit; t += 32.0f) {
                const glm::vec3 p = eye + d * t;
                if (p.x < 0.0f || p.y < 0.0f || p.x > mx || p.y > my) return false;
                hit = p.z <= gri->height(p.x, p.y);
            }
            if (!hit) return false;
        }
    return true;
}

// Rts_init: selection = player + selectable team; camera "rts_play" = "rts_init" keys around the current camera,
// final distance +0xed lowered in 200 steps until the view is valid (not below the follow distance).
void GameCamera::rts_enter(game::Mission& m) {
    game::World& w = m.world();
    rts_ = true;
    sel_.clear();
    last_commander_ = 0;
    if (const game::Unit* p = w.unit(w.player_id())) {
        sel_ = m.ai().rts_select_with_team(p->id);
        if (sel_.empty()) sel_.push_back(p->id);
    }
    const CamState a = cur_;
    CamState fin = a;
    fin.pitch = kRtsPitch;
    float d = rts_dist_;
    for (; d > follow_dist_; d -= 200.0f) {
        fin.dist = d;
        if (rts_view_valid(w, fin)) break;
    }
    fin.dist = std::max(d, follow_dist_);
    std::vector<CamTrack::Key> k = tpl_keys(track("rts_init"));
    for (CamTrack::Key& key : k) {
        const CamState t = key.s;
        key.s = a;
        key.s.pivot.z += t.pivot.z;
        key.s.pitch = t.pitch;
        key.s.dist = std::min(t.dist, fin.dist);
    }
    rts_cam_ = fin;
    if (k.empty()) return;
    k.front().s.dist = a.dist, k.front().s.pitch = a.pitch;
    k.back().s.dist = fin.dist;
    rts_cam_ = k.back().s;
    play_ = {};
    play_.active = true;
    play_.name = "rts_play";
    play_.start_ms = m.now();
    play_.keys = std::move(k);
}

// Rts_deinit with camera: "rts_deinit" keys from the RTS camera back to the follow pose (0x447930 lerps).
// HYPOTHESIS: pivot / yaw lerp by key index, template pitch / distance between the two end poses.
void GameCamera::rts_exit(game::Mission& m) {
    rts_ = false;
    // Rts_is_exit_done 0x447860: FUN_00436190(1) + FUN_004384e0 loop = camera 0 turned to the hero's facing.
    if (const game::Unit* p = m.world().unit(m.world().player_id())) yaw_ = std::fmod(p->yaw_goal + 360.0f, 360.0f);
    pressing_ = dragging_ = false;
    sel_.clear();
    std::vector<CamTrack::Key> k = tpl_keys(track("rts_deinit"));
    if (k.empty()) return;
    const CamState a = cur_, b = follow_state(m.world());
    const float by = a.yaw + std::remainder(b.yaw - a.yaw, 360.0f);
    for (std::size_t i = 0; i < k.size(); ++i) {
        const CamState t = k[i].s;
        const float f = k.size() > 1 ? float(i) / float(k.size() - 1) : 1.0f;
        k[i].s = a;
        k[i].s.pivot = glm::mix(a.pivot, b.pivot, f) + glm::vec3(0, 0, t.pivot.z);
        k[i].s.yaw = glm::mix(a.yaw, by, f);
        k[i].s.pitch = t.pitch;
        k[i].s.dist = t.dist;
    }
    k.front().s.dist = a.dist, k.front().s.pitch = a.pitch;
    k.back().s.dist = b.dist, k.back().s.pitch = b.pitch;
    play_ = {};
    play_.active = true;
    play_.name = "rts_deinit";
    play_.start_ms = m.now();
    play_.keys = std::move(k);
}

// One tick of RTS input (Rts_process_input 0x440b30 order): commander keys, scroll, rotate, zoom.
void GameCamera::rts_tick(game::Mission& m) {
    game::World& w = m.world();
    const game::InputFrame& f = tick_in_;
    // Commander keys 25..32 -> Rts_select_commander 0x451530 index (Joan 1, Jean 2, La Hire 3, Alencon 4,
    // Orleans 5, Richemont 30 -> 7, Charles 31 -> 6, Flavy 8). Shift+key toggles hold (global handler, not here).
    static constexpr int kIdx[8] = {1, 2, 3, 4, 5, 7, 6, 8};
    if (!f.held[game::IN_SHIFT])
        for (int a = 25; a <= 32; ++a)
            if (f.pressed[std::size_t(a)]) select_commander(m, kIdx[a - 25], false);

    // Camera_rts_edge_scroll: key bits (HYPOTHESIS: DAT_00a51804 = movement actions 4/5/6/7) and screen edges.
    const float y = rts_cam_.yaw * kDeg;
    const glm::vec2 fw{std::cos(y), std::sin(y)}, rt{std::sin(y), -std::cos(y)};
    glm::vec2 mv = fw * (float(f.held[4]) - float(f.held[5])) + rt * (float(f.held[7]) - float(f.held[6]));
    mv += rt * edge_dir_.x + fw * edge_dir_.y;
    edge_dir_ = {};
    CamState c = rts_cam_;
    c.pivot += glm::vec3(mv * (kRtsScrollPerMs * kTickMs), 0.0f);
    const float g = ground(w, c.pivot);  // pivot follows the ground (eases down at frame_ms / 1000)
    c.pivot.z = g > c.pivot.z ? g : c.pivot.z + (g - c.pivot.z) * kTickMs / 1000.0f;
    const glm::vec3 e = cam_eye(c);
    if (const float ge = ground(w, e) + kRtsClearance; e.z < ge) c.pivot.z += ge - e.z;  // 40 units above ground
    if (rts_view_valid(w, c)) rts_cam_ = c;

    // Rts_camera_rotate: action 50 / middle-drag right -> 2 (yaw -), 51 / drag left -> 1 (yaw +), 0.15 deg/ms.
    const int rot = (f.held[50] || mmb_dx_ > 0.0f) ? 2 : (f.held[51] || mmb_dx_ < 0.0f) ? 1 : 0;
    mmb_dx_ = 0;
    if (rot) {
        c = rts_cam_;
        c.yaw += (rot == 1 ? 1.0f : -1.0f) * kRtsRotDegPerMs * kTickMs;
        c.yaw = std::fmod(c.yaw + 360.0f, 360.0f);
        c.pitch = kRtsPitch;
        if (rts_view_valid(w, c)) rts_cam_ = c;
    }
    // Rts_camera_zoom: 48 (wheel up) in / 49 out, 2 units/ms, [follow distance 0xa0bbf0, 2500]; +0xed always stored.
    if (const int z = f.held[48] ? 1 : f.held[49] ? 2 : 0) {
        c = rts_cam_;
        c.dist = std::clamp(c.dist + (z == 1 ? -1.0f : 1.0f) * kRtsZoomPerMs * kTickMs, follow_dist_, kRtsZoomMax);
        rts_dist_ = c.dist;
        if (rts_view_valid(w, c)) rts_cam_ = c;
    }
    std::erase_if(sel_, [&](int id) {
        const game::Unit* u = w.unit(id);
        return !u || u->deleted || w.dead(*u);
    });
}

// Rts_select_commander 0x451530: replace (or append) the selection with the hero and its selectable team; pressing
// the same commander again centres the camera on him (pitch 45, distance stepped -200 until valid).
// Not done: ViewCtrl_switch_character on the second press, portrait clicks, voices.
void GameCamera::select_commander(game::Mission& m, int idx, bool add) {
    static constexpr const char* kNames[9] = {"", "JOAN_OF_ARC", "JEAN_DE_METZ", "LAHIRE", "ALENCON",
                                              "ORLEAN", "charlie7", "arthur_de_richemont", "FLAVY"};
    game::World& w = m.world();
    const game::Unit* hero = nullptr;
    for (const game::Unit& u : w.units())
        if (!u.deleted && !w.dead(u) && u.m.control == 0 && u.cls >= 0 && std::size_t(u.cls) < w.data().units.size() &&
            formats::iequals(w.data().units[std::size_t(u.cls)].code, kNames[idx]))
            hero = &u;
    if (!hero) return;
    if (!add && last_commander_ == idx) {
        CamState c = rts_cam_;
        c.pivot = {hero->pos.x, hero->pos.y, hero->pos.z + kPivotHeight};
        c.pitch = kRtsPitch;
        for (c.dist = rts_dist_; c.dist > follow_dist_ && !rts_view_valid(w, c); c.dist -= 200.0f) {}
        c.dist = std::max(c.dist, follow_dist_);
        rts_cam_ = c;
    }
    if (!add) sel_.clear(), last_commander_ = idx;
    std::vector<int> team = m.ai().rts_select_with_team(hero->id);
    if (team.empty()) team.push_back(hero->id);
    for (int id : team)
        if (std::find(sel_.begin(), sel_.end(), id) == sel_.end()) sel_.push_back(id);
}

void GameCamera::rts_input(game::Mission& m, const RawInput& raw, glm::vec2 mouse, glm::ivec4 vp, const glm::mat4& vp_m) {
    const bool l = raw.mouse_buttons[SDL_BUTTON_LEFT], r = raw.mouse_buttons[SDL_BUTTON_RIGHT];
    const bool l_prev = prev_buttons_[SDL_BUTTON_LEFT], r_prev = prev_buttons_[SDL_BUTTON_RIGHT];
    prev_buttons_ = raw.mouse_buttons;
    if (!rts_ || play_.active || vp.z <= 0 || vp.w <= 0) {
        pressing_ = dragging_ = false;
        return;
    }
    game::World& w = m.world();
    const bool shift = raw.keys[SDL_SCANCODE_LSHIFT] || raw.keys[SDL_SCANCODE_RSHIFT];
    const bool ctrl = raw.keys[SDL_SCANCODE_LCTRL] || raw.keys[SDL_SCANCODE_RCTRL];

    // Screen edges (0x593980 rect tests, 10 px inset): scroll toward the cursor. Speed HYPOTHESIS = key speed.
    const glm::vec2 lo{float(vp.x), float(vp.y)}, hi = lo + glm::vec2(float(vp.z), float(vp.w)), centre = (lo + hi) * 0.5f;
    if (mouse.x >= lo.x && mouse.y >= lo.y && mouse.x < hi.x && mouse.y < hi.y &&
        (mouse.x < lo.x + 10 || mouse.y < lo.y + 10 || mouse.x >= hi.x - 10 || mouse.y >= hi.y - 10))
        edge_dir_ = glm::normalize(glm::vec2(mouse.x - centre.x, centre.y - mouse.y) + glm::vec2(1e-4f, 0.0f));
    if (raw.mouse_buttons[SDL_BUTTON_MIDDLE]) mmb_dx_ += raw.mouse_dx;

    // Left: click select (Rts_click_select 0x449b30) or drag box (Rts_box_select 0x4489c0, cap 100); Shift adds.
    if (l && !l_prev) pressing_ = true, dragging_ = false, drag_from_ = drag_to_ = mouse;
    if (l && pressing_) {
        drag_to_ = mouse;
        if (glm::distance(drag_from_, drag_to_) > 4.0f) dragging_ = true;
    }
    if (!l && l_prev && pressing_) {
        if (!shift) sel_.clear();
        const auto add = [&](int id) {
            if (sel_.size() < 100 && std::find(sel_.begin(), sel_.end(), id) == sel_.end()) sel_.push_back(id);
        };
        if (dragging_) {
            const glm::vec2 a = glm::min(drag_from_, drag_to_), b = glm::max(drag_from_, drag_to_);
            for (game::Unit& u : w.units()) {
                if (u.deleted || w.dead(u) || !allied(w, u) || !m.ai().rts_selectable(u)) continue;
                glm::vec2 px;
                float d;
                if (project(vp_m, vp, {u.pos.x, u.pos.y, u.pos.z + unit_height(w, u) * 0.5f}, px, d) && px.x >= a.x &&
                    px.y >= a.y && px.x <= b.x && px.y <= b.y)
                    add(u.id);
            }
        } else if (const int id = pick_unit(w, vp_m, vp, mouse)) {
            const game::Unit* u = w.unit(id);
            if (u && allied(w, *u)) {
                std::vector<int> team = m.ai().rts_select_with_team(id);
                if (team.empty() && id == w.player_id()) team.push_back(id);
                for (int t : team) add(t);
            }
        }
        pressing_ = dragging_ = false;
    }

    // Right click (Rts_pick_target 0x44ac60 types 0 / 1): hostile unit -> attack, else ground -> move (Ctrl =
    // attack-move). Items, siege crews, team joins and buildings are not handled here.
    if (r && !r_prev && !sel_.empty()) {
        const int id = pick_unit(w, vp_m, vp, mouse);
        const game::Unit* u = id ? w.unit(id) : nullptr;
        glm::vec3 g;
        if (u && w.hostile_to_player(*u)) m.ai().rts_attack_unit(sel_, id);
        else if (pick_ground(w, vp_m, vp, mouse, g)) m.ai().rts_move(sel_, game::Vec3{g.x, g.y, g.z}, ctrl);
    }
}

glm::vec4 GameCamera::drag_box() const {
    if (!dragging_) return glm::vec4(0.0f);
    const glm::vec2 a = glm::min(drag_from_, drag_to_), b = glm::max(drag_from_, drag_to_);
    return {a.x, a.y, b.x, b.y};
}

}  // namespace joa::engine
