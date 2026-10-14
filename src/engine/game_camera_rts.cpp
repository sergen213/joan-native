// GameCamera RTS part: camera (Rts_init 0x43fc90 / Rts_deinit 0x4404c0 tracks, Camera_rts_edge_scroll 0x445990,
// Rts_camera_rotate 0x447740, Rts_camera_zoom 0x447960, Rts_camera_view_valid 0x453930, minimap jump 0x717240) and
// the mouse / key part of Rts_process_input 0x440b30: groups (0x418310 / 0x44aad0), double-click (0x44a690 /
// 0x44a430), click / box select (0x449b30 / 0x4489c0), hover pick (0x44ac60) and its commands, the RTSICON cursor
// (Rts_process 0x442ef0, Rts_draw 0x4450f0). GAMEPLAY_AI_TROOPS.md Part B §5 2.3-3.8.
#include "engine/game_camera.hpp"

#include "engine/assets_cache.hpp"
#include "engine/attach_items.hpp"
#include "formats/vfs_bytes.hpp"
#include "formats/vfs_fs.hpp"
#include "game/mission.hpp"
#include "game/savegame_container.hpp"
#include "game/siege_crew.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

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

// View3D_unproject: the pick ray through `mouse` in original units (origin on the near plane, unit direction).
void pick_ray(const glm::mat4& vp_m, glm::ivec4 vp, glm::vec2 mouse, glm::vec3& o, glm::vec3& dir, float& len) {
    const glm::vec2 ndc{(mouse.x - float(vp.x)) / float(vp.z) * 2.0f - 1.0f, 1.0f - (mouse.y - float(vp.y)) / float(vp.w) * 2.0f};
    const glm::mat4 inv = glm::inverse(vp_m);
    glm::vec4 a = inv * glm::vec4(ndc, -1.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
    a /= a.w, b /= b.w;
    o = {a.x, -a.z, a.y};  // engine -> original (inverse of to_engine)
    const glm::vec3 e{b.x, -b.z, b.y};
    len = std::min(glm::length(e - o), 8193.0f);
    dir = glm::normalize(e - o);
}

// Unit under the cursor (0x447ab0 / Rts_pick_target unit loop): the pick ray against the unit's collision cylinder
// (actor +0x90 kind 3: class CylinderRadius / height; ponytail: radius at least 20 so thin units stay clickable),
// nearest hit along the ray wins. `t_out` = distance along the ray; `skip` = a unit never picked (point & click hero).
int pick_unit(const game::World& w, const glm::mat4& vp_m, glm::ivec4 vp, glm::vec2 mouse, float* t_out = nullptr, int skip = 0) {
    glm::vec3 o, dir;
    float len;
    pick_ray(vp_m, vp, mouse, o, dir, len);
    int best = 0;
    float best_t = 1e30f;
    const glm::vec2 d2{dir.x, dir.y};
    const float a = glm::dot(d2, d2);
    for (const game::Unit& u : w.units()) {
        if (u.deleted || u.id == skip || w.dead(u) || a < 1e-8f) continue;
        const float r = std::max(20.0f, u.cls >= 0 ? w.data().units[std::size_t(u.cls)].cyl_radius : 15.0f), h = unit_height(w, u);
        const glm::vec2 oc = glm::vec2(o) - glm::vec2(u.pos.x, u.pos.y);
        const float b = 2.0f * glm::dot(oc, d2), c = glm::dot(oc, oc) - r * r, disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) continue;
        float lo = (-b - std::sqrt(disc)) / (2.0f * a), hi = (-b + std::sqrt(disc)) / (2.0f * a);
        if (std::fabs(dir.z) > 1e-6f) {
            const float ta = (u.pos.z + h - o.z) / dir.z, tb = (u.pos.z - o.z) / dir.z;
            lo = std::max(lo, std::min(ta, tb)), hi = std::min(hi, std::max(ta, tb));
        } else if (o.z < u.pos.z || o.z > u.pos.z + h) {
            continue;
        }
        if (lo > hi || hi < 0.0f) continue;
        if (const float t = std::max(lo, 0.0f); t < best_t) best = u.id, best_t = t;
    }
    if (t_out) *t_out = best_t;
    return best;
}

// View3D_unproject ray against the terrain (0x452830 role).
bool pick_ground(const game::World& w, const glm::mat4& vp_m, glm::ivec4 vp, glm::vec2 mouse, glm::vec3& out) {
    glm::vec3 o, dir;
    float len;
    pick_ray(vp_m, vp, mouse, o, dir, len);
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

// Ray against a sphere: distance along the ray, or a negative value when missed.
float ray_sphere(glm::vec3 o, glm::vec3 dir, glm::vec3 c, float r) {
    const glm::vec3 oc = c - o;
    const float t = glm::dot(oc, dir), d2 = glm::dot(oc, oc) - t * t;
    return t > 0.0f && d2 <= r * r ? t - std::sqrt(r * r - d2) : -1.0f;
}

// Map item under the cursor (Rts_pick_target item loop / 0x448340 role). The original intersects the item actor's
// bounds (actor +0x90, from its model); here the bounding sphere of the item's model (ITEM\<dir>\<Model>, turned by
// the item heading and scaled), or without a model a sphere over the ItemRes footprint (Width/Height, at least 30
// units) standing on the item position. ponytail: sphere only (no box / cylinder kinds), pitch / roll ignored.
// `rts` = Rts_pick_target's filter (classes 1 gate, 4 wall, 10 box, 0x27, or a pickable floor marker; not the
// player's nation; gate / wall with negative hp skipped).
int pick_item(game::Mission& m, AssetCache* assets, glm::vec3 o, glm::vec3 dir, bool rts, int player_nation, float& best) {
    game::MapObjects& objs = m.objects();
    int found = 0;
    for (int r = 1; r <= objs.size(); ++r) {
        const game::MapItem* it = objs.item(r);
        if (!it) continue;
        const game::ObjRes* res = objs.res(it->res);
        if (rts) {
            const bool cls = it->cls == 1 || it->cls == 4 || it->cls == 10 || it->cls == 0x27;
            if (!cls && !(it->on_floor && res && res->pickable)) continue;
            if (it->nation == player_nation) continue;
            if ((it->cls == 1 || it->cls == 4) && it->hp < 0) continue;
        }
        float rad = std::max(30.0f, res ? float(std::max(res->width, res->height)) * 0.5f : 0.0f);
        glm::vec3 c{it->pos.x, it->pos.y, it->pos.z + rad * 0.5f};
        const std::string path = res && assets && !res->model.empty() ? "item\\" + res->dir + res->model : std::string();
        if (const Model* md = path.empty() ? nullptr : assets->model(path, item_anim(path)); md && md->radius > 0.0f) {
            const glm::vec3 lc{md->center.x, -md->center.z, md->center.y};  // engine -> original axes
            const float cs = std::cos(it->dir), sn = std::sin(it->dir), k = it->scale > 0.0f ? it->scale : 1.0f;
            c = glm::vec3(it->pos.x, it->pos.y, it->pos.z) + k * glm::vec3(cs * lc.x - sn * lc.y, sn * lc.x + cs * lc.y, lc.z);
            rad = md->radius * k;
        }
        const float t = ray_sphere(o, dir, c, rad);
        if (t >= 0.0f && t < best) best = t, found = r;
    }
    return found;
}

// Command cursor family of an attack (Rts_cmd_attack_unit 0x44eb80): by the class of a single selected unit.
int attack_family(const game::World& w, const std::vector<int>& sel) {
    const game::Unit* u = sel.size() == 1 ? w.unit(sel[0]) : nullptr;
    const int c = u && u->cls >= 0 ? w.data().units[std::size_t(u->cls)].unit_class : 1;
    return c >= 2 && c <= 4 ? 2 : c == 5 || c == 6 ? 4 : 3;
}

bool light_infantry(const game::World& w, const game::Unit& u) {  // E/F/MER_LIGHT_INFANTRY (0x5270b0 type ids)
    const std::string& c = w.data().units[std::size_t(u.cls)].code;
    return c == "ENGLISH_LIGHT_INFANTRY" || c == "FRENCH_LIGHT_INFANTRY" || c == "MER_LIGHT_INFANTRY";
}

// Trigger_create 0x4eef00 for the RTS player commands (event, target kind 2 item / 1 unit, sender unit).
void post_trigger(game::World& w, int type, int kind, int target, const game::Unit& sender, float radius) {
    game::Trigger t;
    t.type = type, t.target_kind = kind, t.target_id = target, t.src_kind = 1, t.sender = sender.id;
    t.pos = sender.pos, t.radius = radius, t.start = t.end = w.now();
    w.post_trigger(t);
}

std::vector<CamTrack::Key> tpl_keys(const CamTrack* t) { return t ? t->keys : std::vector<CamTrack::Key>{}; }

const std::string& code_of(const game::World& w, const game::Unit& u) { return w.data().units[std::size_t(u.cls)].code; }
int class_of(const game::World& w, const game::Unit& u) { return w.data().units[std::size_t(u.cls)].unit_class; }
bool alive(const game::World& w, const game::Unit* u) { return u && !u->deleted && !w.dead(*u); }

// UnitArray_find_by_type: the unit of a class code (heroes are unique).
game::Unit* find_code(game::World& w, std::string_view code) {
    for (game::Unit& u : w.units())
        if (!u.deleted && u.cls >= 0 && std::size_t(u.cls) < w.data().units.size() && formats::iequals(code_of(w, u), code)) return &u;
    return nullptr;
}

// FUN_00453da0: voice prefix by class (+2) for rank 0..3, by hero for rank 4 / 5; "" otherwise (no SOUND entry).
std::string voice_prefix(const game::World& w, const game::Unit& u) {
    if (u.rank >= 0 && u.rank <= 3) {
        static constexpr const char* kCls[9] = {"", "_LTINF_", "_HVINF_", "_POLE_", "_MACE_", "_ARCHER_", "_CROSS_", "", "_SIEGE_"};
        const int c = class_of(w, u);
        return c >= 0 && c <= 8 ? kCls[c] : "";
    }
    static constexpr std::pair<const char*, const char*> kHero[7] = {
        {"JOAN_OF_ARC", "_JOAN_"}, {"JEAN_DE_METZ", "_METZ_"}, {"LAHIRE", "_LAHIRE_"}, {"CHARLIE7", "_CHARLES_"},
        {"ALENCON", "_ALENCON_"}, {"ORLEAN", "_ORLEANS_"}, {"arthur_de_richemont", "_RICHEMONT_"}};
    if (u.rank <= 5)
        for (const auto& [code, p] : kHero)
            if (formats::iequals(code_of(w, u), code)) return p;
    return "";
}

std::string num3(int n) {
    char b[8];
    std::snprintf(b, sizeof b, "%03d", n);
    return b;
}

// Shared head of the select / move / cannot voices (Rts_click_select 0x449b30, Troop_cmd_formation_move 0x44cfb0,
// PlayerCtrl_rts 0x440b30): "RTS" + prefix + `cmd` of the first live commander (rank > 3) when exactly one is
// selected, else `generic` + Misc_random_range(lo, hi). `one` = the unit when exactly one is selected (callers
// replace the code for it).
std::string ack(const game::World& w, const std::vector<int>& sel, const char* cmd, const char* generic, int variant,
                const game::Unit** one) {
    std::string s = "RTS";
    int n = 0;
    for (int id : sel)
        if (const game::Unit* u = w.unit(id); alive(w, u) && u->rank > 3 && n++ == 0) s += voice_prefix(w, *u) + cmd;
    if (n != 1) s = generic + std::to_string(variant);
    *one = sel.size() == 1 ? w.unit(sel[0]) : nullptr;
    return s;
}

}  // namespace

void hold_voice(game::Mission& m, const game::Unit& u) {
    const game::World& w = m.world();
    if (u.m.control == 1 || u.rank <= 3 || (w.game_mode != 4 && u.id == w.player_id())) return;
    m.emit(game::MissionEvent::Kind::Sound, 0, 0, "RTS" + voice_prefix(w, u) + (m.ai().is_holding(u) ? "006" : "007"));
}

void GameCamera::say(game::Mission& m, std::string code) {
    voice_ = code;
    m.emit(game::MissionEvent::Kind::Sound, 0, 0, std::move(code));
}

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
    groups_[0].clear(), active_ = 0;  // groups 1..9 survive (Rts_init resets group 0 and the active index only)
    last_commander_ = 0;
    rts_family(0);
    if (const game::Unit* p = w.unit(w.player_id())) {
        groups_[0] = m.ai().rts_select_with_team(p->id);
        if (groups_[0].empty()) groups_[0].push_back(p->id);
        commander_ = p->id;  // Rts_init step 6: +0x1238 = player
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
    groups_[0].clear(), active_ = 0, commander_ = last_commander_ = 0;  // Rts_deinit: highlights cleared; 1..9 stay
    hover_type_ = hover_id_ = hover_siege_ = 0;
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

void GameCamera::rts_deinit(game::Mission& m) {
    if (last_commander_ >= 1 && last_commander_ <= 4) m.switch_hero(last_commander_, minimap_scale.x, minimap_scale.y);
}

std::vector<std::uint8_t> GameCamera::save_view() const {
    std::vector<std::uint8_t> out;
    game::ByteWriter b(out);
    b.i32(active_), b.i32(last_commander_), b.i32(commander_), b.f32(rts_dist_);
    for (const float v : {rts_cam_.pivot.x, rts_cam_.pivot.y, rts_cam_.pivot.z, rts_cam_.yaw, rts_cam_.pitch, rts_cam_.dist}) b.f32(v);
    for (const std::vector<int>& g : groups_) {
        b.u32(std::uint32_t(g.size()));
        for (int id : g) b.i32(id);
    }
    return out;
}

void GameCamera::load_view(std::span<const std::uint8_t> data) {
    formats::ByteReader r(data);
    const int active = r.i32(), last = r.i32(), commander = r.i32();
    const float dist = r.get<float>();
    CamState c;
    c.pivot.x = r.get<float>(), c.pivot.y = r.get<float>(), c.pivot.z = r.get<float>();
    c.yaw = r.get<float>(), c.pitch = r.get<float>(), c.dist = r.get<float>();
    std::array<std::vector<int>, 10> groups;
    for (std::vector<int>& g : groups) {
        const std::uint32_t n = r.u32();
        for (std::uint32_t i = 0; i < n && i <= 100 && r.ok(); ++i) g.push_back(r.i32());
    }
    if (!r.ok() || !r.at_end() || active < 0 || active > 9) return;
    groups_ = std::move(groups), active_ = active, last_commander_ = last, commander_ = commander, rts_dist_ = dist;
    if (rts_) rts_cam_ = c, snap_ = true, set(c, Mode::Rts);  // Rts_resume: the saved camera (0x485930), no track
}

// One tick of RTS input (Rts_process_input 0x440b30 order): recovery items, commander keys, scroll, rotate, zoom.
void GameCamera::rts_tick(game::Mission& m) {
    game::World& w = m.world();
    const game::InputFrame& f = tick_in_;
    // F3 / F4 (actions 19 / 20; Alt+F4 is not 20) -> 0x453790: Ctrl_use_hp_item / Ctrl_use_ep_item on every live
    // selected unit of rank > 3.
    for (const bool ep : {false, true})
        if (f.pressed[ep ? 20u : 19u] && !(ep && (f.alt_held || alt_)))
            for (int id : rts_selection())
                if (game::Unit* u = w.unit(id); alive(w, u) && u->rank > 3) w.use_recovery_item(*u, ep);
    // Commander keys 25..32 -> Rts_select_commander 0x451530 index (Joan 1, Jean 2, La Hire 3, Alencon 4,
    // Orleans 5, Richemont 30 -> 7, Charles 31 -> 6, Flavy 8). Shift + key also toggles the hero's hold (global key
    // handler ViewCtrl_process_key 0x42e1a0 step 3, Unit_toggle_hold 0x5121c0) and appends to the selection; a
    // Ctrl / Alt digit is a group key (rts_input) and selects nothing.
    static constexpr int kIdx[8] = {1, 2, 3, 4, 5, 7, 6, 8};
    for (int a = 25; a <= 32; ++a) {
        if (!f.pressed[std::size_t(a)] || ctrl_ || alt_ || f.alt_held) continue;
        if (shift_) select_commander(m, kIdx[a - 25], true, -1);  // recno -1: hold toggle only
        select_commander(m, kIdx[a - 25], shift_);
    }

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
    for (std::vector<int>& g : groups_)
        std::erase_if(g, [&](int id) {
            const game::Unit* u = w.unit(id);
            return !u || u->deleted || w.dead(*u);
        });
}

// Rts_select_commander 0x451530: replace (or append) the selection with the hero and its selectable team; pressing
// the same commander again (or double-clicking his portrait) centres the camera on him (pitch 45, distance stepped
// -200 until valid) and, for JOAN / JEAN / LAHIRE / ALENCON (Unit_is_playable_hero), switches the player to him
// (Ctrl_switch_commander, mode 4: player swap). Voice "RTS" + prefix + 001 (that switch) / 002.
void GameCamera::select_commander(game::Mission& m, int idx, bool add, int recno, bool dbl) {
    static constexpr const char* kNames[9] = {"", "JOAN_OF_ARC", "JEAN_DE_METZ", "LAHIRE", "ALENCON",
                                              "ORLEAN", "charlie7", "arthur_de_richemont", "FLAVY"};
    game::World& w = m.world();
    game::Unit* hero = nullptr;
    if (recno > 0) {
        hero = w.unit(recno);
        idx = 0;
        for (int i = 1; hero && i <= 8; ++i)
            if (formats::iequals(code_of(w, *hero), kNames[i])) idx = i;
    } else if (idx >= 1 && idx <= 8) {
        hero = find_code(w, kNames[idx]);
    }
    if (!hero || hero->deleted || w.dead(*hero) || hero->m.control != 0) return;
    if (recno < 0) return m.ai().toggle_hold(*hero, true), hold_voice(m, *hero);
    std::string voice = "RTS" + voice_prefix(w, *hero) + "002";
    if (!add && (last_commander_ == idx || dbl)) {
        if (idx >= 1 && idx <= 4) voice.replace(voice.size() - 3, 3, "001"), m.switch_hero(idx, minimap_scale.x, minimap_scale.y);
        CamState c = rts_cam_;
        c.pivot = {hero->pos.x, hero->pos.y, hero->pos.z + kPivotHeight};
        c.pitch = kRtsPitch;
        for (c.dist = rts_dist_; c.dist > follow_dist_ && !rts_view_valid(w, c); c.dist -= 200.0f) {}
        c.dist = std::max(c.dist, follow_dist_);
        rts_cam_ = c;
    }
    if (add) rts_flatten(), commander_ = 0;
    else groups_[0].clear(), active_ = 0, last_commander_ = idx, commander_ = hero->id;
    std::vector<int> team = m.ai().rts_select_with_team(hero->id);
    if (team.empty()) team.push_back(hero->id);
    std::vector<int>& g = groups_[0];
    for (int id : team)
        if (g.size() < 100 && std::find(g.begin(), g.end(), id) == g.end()) g.push_back(id);
    say(m, voice);
}

void GameCamera::rts_flatten() {
    if (active_ != 0) groups_[0] = groups_[std::size_t(active_)], active_ = 0;
}

void GameCamera::rts_portrait(game::Mission& m, int recno) {
    const bool dbl = recno == portrait_ && m.now() - portrait_ms_ <= 500;
    portrait_ = recno, portrait_ms_ = dbl ? -100000 : m.now();
    select_commander(m, 0, shift_, recno, dbl);
}

void GameCamera::rts_minimap(game::Mission& m, int button, glm::vec2 world) {
    if (!rts_ || play_.active) return;
    game::World& w = m.world();
    if (button == 1) {  // 0x717700 -> Rts_cmd_move(pos, ctrl)
        if (!rts_selection().empty()) move_order(m, {world.x, world.y, ground(w, {world.x, world.y, 0.0f})}, true);
        return;
    }
    // FUN_00717240: pivot x/y = clicked point, distance -200 until the view is valid. The original keeps z and lets
    // Camera_rts_edge_scroll pull the pivot to the ground on the next frames; with the stale z the eye could land
    // inside a hill (still a "valid" view: the corner rays hit terrain at once), so the pivot takes the ground
    // height and the 40-unit eye clearance at once (RtsFinish: mid-map jumps showed sky in real play).
    CamState c = rts_cam_;
    c.pivot = {world.x, world.y, ground(w, {world.x, world.y, 0.0f})};
    for (; c.dist > follow_dist_; c.dist -= 200.0f) {
        CamState t = c;
        if (const glm::vec3 e = cam_eye(t); e.z < ground(w, e) + kRtsClearance) t.pivot.z += ground(w, e) + kRtsClearance - e.z;
        if (rts_view_valid(w, t)) return void(rts_cam_ = t);
    }
}

// Rts_pick_target 0x44ac60: nearest unit / map item under the cursor, else the terrain; the type decides the cursor
// and the right-click command. -1 = nothing picked (no terrain under the cursor).
void GameCamera::rts_pick(game::Mission& m, glm::vec2 mouse, glm::ivec4 vp, const glm::mat4& vp_m) {
    game::World& w = m.world();
    game::AiSystem& ai = m.ai();
    hover_type_ = -1, hover_id_ = 0, hover_siege_ = 0;
    const game::Unit* p = w.unit(w.player_id());
    if (!p) return;
    std::vector<const game::Unit*> sel;
    for (int id : rts_selection())
        if (const game::Unit* u = w.unit(id); u && !u->deleted && !w.dead(*u)) sel.push_back(u);
    glm::vec3 o, dir;
    float len;
    pick_ray(vp_m, vp, mouse, o, dir, len);
    float best = 1e30f;
    const int uid = pick_unit(w, vp_m, vp, mouse, &best);
    const game::Unit* u = uid ? w.unit(uid) : nullptr;
    const int iid = pick_item(m, assets, o, dir, true, p->nation, best);
    int drop = 0;  // unit drops (World::items) = pickable floor items
    for (const game::WorldItem& it : w.items())
        if (!it.deleted) {
            const float t = ray_sphere(o, dir, glm::vec3(it.pos.x, it.pos.y, it.pos.z + 15.0f), 30.0f);
            if (t >= 0.0f && t < best) best = t, drop = it.id;
        }
    if (drop) {
        if (!sel.empty()) {
            const game::WorldItem& it = *w.item(drop);
            hover_type_ = 4, hover_pos_ = {it.pos.x, it.pos.y, it.pos.z};
            return;
        }
    } else if (iid) {
        const game::MapItem& it = *m.objects().item(iid);
        const game::ObjRes* res = m.objects().res(it.res);
        if (!sel.empty()) {
            const bool enemy = it.nation != 0 && w.hostile(p->nation, it.nation);  // 0x509590
            if ((it.cls == 1 || it.cls == 4 || it.cls == 0x12) && enemy) {
                const bool gate = it.cls == 1 && it.hp > 0 && it.final_state == 1 && it.state != it.final_state;
                if (gate || (it.cls == 4 && it.hp > 0) || it.cls == 0x12) return void((hover_type_ = 2, hover_id_ = iid));
            } else if (it.cls == 10) {  // box: attack unless it opens with Interact; no terrain pick either way
                hover_type_ = res && res->open_with_interact ? 0 : 2;
                hover_id_ = iid;
                return;
            }
            if (it.on_floor && res && res->pickable) return void((hover_type_ = 4, hover_pos_ = {it.pos.x, it.pos.y, it.pos.z}));
        }
    } else if (u) {
        // Rts_pick_target 0x44b939: DAT_00a5704d = picked siege weapon (class 8 with push slots), any selection.
        if (w.data().units[std::size_t(u->cls)].unit_class == 8 && u->m.can_attach) hover_siege_ = u->id;
        if (const game::Unit* rd = w.unit(u->m.rider); rd && !rd->deleted) u = rd;  // horse -> its rider
        if (!sel.empty()) {
            int leaders = 0;
            const game::Unit* leader = nullptr;
            for (const game::Unit* s : sel)
                if (ai.is_leader(*s) && !leaders++) leader = s;
            const auto set = [&](int type, int id) { hover_type_ = type, hover_id_ = id; };
            if (u->m.can_interact && leaders == 1 && leader == p && glm::distance(glm::vec3(p->pos.x, p->pos.y, p->pos.z),
                                                                                   glm::vec3(u->pos.x, u->pos.y, u->pos.z)) <= 100.0f)
                return set(11, u->id);
            if (w.hostile_to_player(*u)) return set(1, u->id);
            if (u->m.can_attach && (!game::siege_slots_full(w, *u, 1) || !game::siege_slots_full(w, *u, 2)))
                for (const game::Unit* s : sel)
                    if (!ai.is_leader(*s) && light_infantry(w, *s)) return set(6, u->id);
            if (u->nation == p->nation && u->rank > 3)
                for (const game::Unit* s : sel)
                    if (s->rank > 2 && s != u) return set(8, u->id);
            if (u->nation != p->nation || !ai.is_leader(*u)) return set(9, 0);
            for (const game::Unit* s : sel) {
                if (s->rank > 2) break;
                const int k = w.data().units[std::size_t(s->cls)].unit_class;
                if (!ai.is_leader(*s) && s != u && k != 8 && k != 9 && s->team != u->team) return set(7, u->id);
            }
        }
    }
    glm::vec3 g;
    if (!pick_ground(w, vp_m, vp, mouse, g)) return;
    hover_type_ = 0, hover_pos_ = g;
    if (const int f = pick_building(m, g, false)) hover_type_ = 10, hover_id_ = f;
}

// Point & click pick (PlayerCtrl_mouse_click 0x438b30): the RTS pick helpers on every map item (no RTS filter), the
// hero skipped like 0x439110's `unit != player`; nearest of unit / map item / floor drop, else the terrain.
void GameCamera::click_pick(game::Mission& m, game::InputFrame& f, glm::vec2 mouse, glm::ivec4 vp, const glm::mat4& vp_m) const {
    f.pick_unit = f.pick_item = f.pick_drop = 0, f.pick_ground = false;
    if (mouse.x < float(vp.x) || mouse.y < float(vp.y) || mouse.x >= float(vp.x + vp.z) || mouse.y >= float(vp.y + vp.w)) {
        f.click_left = f.click_right = false;  // Mouse_click_in_rect: outside the 3-D view
        return;
    }
    game::World& w = m.world();
    glm::vec3 o, dir;
    float len, best = 1e30f;
    pick_ray(vp_m, vp, mouse, o, dir, len);
    const int uid = pick_unit(w, vp_m, vp, mouse, &best, w.controller().unit);
    const int iid = pick_item(m, assets, o, dir, false, 0, best);
    int drop = 0;
    for (const game::WorldItem& it : w.items())
        if (!it.deleted) {
            const float t = ray_sphere(o, dir, glm::vec3(it.pos.x, it.pos.y, it.pos.z + 15.0f), 30.0f);
            if (t >= 0.0f && t < best) best = t, drop = it.id;
        }
    glm::vec3 g;
    if (drop) f.pick_drop = drop;
    else if (iid) f.pick_item = iid;
    else if (uid) f.pick_unit = uid;
    else if (pick_ground(w, vp_m, vp, mouse, g)) f.pick_ground = true, f.ground_x = g.x, f.ground_y = g.y, f.ground_z = g.z;
}

// Rts_pick_building 0x452610 (from the top, a recruit (3) / workshop (0x10) facility whose box holds the terrain point)
// + the type-10 tests of Rts_pick_target 0x44ac60 (at most one selected leader L; the owner place's nation = L's,
// else the player's for a barracks without L; barracks: spawn barrack valid, recruit list set (+0x79), +0x97 can
// recruit, L within 512 of the place; workshop: L within 512, IRON / WOOD only when L is ALENCON within 1024). The
// hover starts the BARR / WEAPON / IRON / WOOD cursor loop. `rmb` = PlayerCtrl_rts 0x440b30 case 10: also L of rank
// > 3, then the screen opens through MissionEvent Interact "recruit_screen" / "workshop_screen" (a = L or 0 for a
// barracks without a selected leader = DAT_00a48959, b = facility), as for the player's R key.
int GameCamera::pick_building(game::Mission& m, glm::vec3 at, bool rmb) {
    game::World& w = m.world();
    game::Places& pl = m.places();
    int f = 0;
    for (int i = int(pl.facilities().size()); i >= 1 && !f; --i) {
        const game::Facility& c = pl.facilities()[std::size_t(i - 1)];
        if ((c.kind == game::Facility::Recruit || c.kind == game::Facility::Workshop) && pl.facility_valid(i) && c.box_min.x <= at.x &&
            at.x <= c.box_max.x && c.box_min.y <= at.y && at.y <= c.box_max.y && c.box_min.z <= at.z && at.z <= c.box_max.z)
            f = i;
    }
    const game::Facility* c = f ? pl.facility(f) : nullptr;
    const game::Place* place = c ? pl.place(c->place) : nullptr;
    if (!place) return 0;
    const game::Unit* lead = nullptr;  // Rts_first_selected_leader / Rts_count_selected_leaders < 2
    int leaders = 0;
    for (int id : groups_[0])  // +0xf9 / +0x105: group 0 even while a recalled group is active
        if (const game::Unit* u = w.unit(id); u && m.ai().is_leader(*u) && !leaders++) lead = u;
    if (leaders > 1) return 0;
    const glm::vec3 centre{place->pos.x, place->pos.y, place->pos.z};
    const auto within = [&](const game::Unit& u, float r) { return glm::distance(glm::vec3(u.pos.x, u.pos.y, u.pos.z), centre) <= r; };
    int fam = 0xf;
    if (c->kind == game::Facility::Recruit) {
        const game::Unit* who = lead ? lead : w.unit(w.player_id());
        if (!pl.facility_valid(c->spawn_barrack) || c->list1 == 0 || c->can_recruit != 1 || !who || who->nation != place->nation ||
            (lead && !within(*lead, 512.0f)))
            return 0;
        if (rmb) {
            if (lead && lead->rank <= 3) return 0;
            m.emit(game::MissionEvent::Kind::Interact, lead ? lead->id : 0, f, "recruit_screen");
        }
    } else {
        if (!lead || lead->nation != place->nation || !within(*lead, 512.0f)) return 0;
        if (c->workshop_type != 0) {
            const game::Unit* al = find_code(w, "ALENCON");
            if (al != lead || glm::distance(glm::vec3(al->pos.x, al->pos.y, al->pos.z), centre) >= 1024.0f) return 0;
        }
        fam = c->workshop_type == 1 ? 0x11 : c->workshop_type == 2 ? 0x12 : 0x10;
        if (rmb) {
            if (lead->rank <= 3) return 0;
            m.emit(game::MissionEvent::Kind::Interact, lead->id, f, "workshop_screen");
        }
    }
    if (!rmb && cursor_family_ != fam) rts_family(fam), cursor_frame_ = fam == 0x10 ? 0 : 1;  // BARR01 / WEAPON00 / ..
    return f;
}

// "RTS" + prefix + 015 / RTS_LTINF_011..013 (cannot move / assign, PlayerCtrl_rts 0x440b30, 0x44cfb0).
std::string GameCamera::cannot_code(const game::World& w) {
    const game::Unit* one;
    std::string s = ack(w, rts_selection(), "015", "RTS_LTINF_0", rnd(11, 13), &one);
    if (one) s = "RTS" + voice_prefix(w, *one) + (one->rank > 3 ? std::string("015") : "0" + std::to_string(rnd(11, 13)));
    return s;
}

// Selection voice (Rts_click_select 0x449b30 / box select release in PlayerCtrl_rts): one commander -> his 008, else
// RTS_LTINF_002..004; a single unit: commander 008, siege RTS_LTINF_001, troops prefix + 001.
std::string GameCamera::select_code(const game::World& w) {
    const game::Unit* one;
    std::string s = ack(w, rts_selection(), "008", "RTS_LTINF_00", rnd(2, 4), &one);
    if (one) s = one->rank > 3 ? "RTS" + voice_prefix(w, *one) + "008" : class_of(w, *one) == 8 ? "RTS_LTINF_001" : "RTS" + voice_prefix(w, *one) + "001";
    return s;
}

// Troop_cmd_formation_move 0x44cfb0: MOVTO (Ctrl: attack-move; POINT from the minimap) or CMOVTO for an invalid
// destination. Voices: one commander 003, else RTS_LTINF_005..007 (a single troop too); siege: M slots full
// RTS_SIEGE_003, else RTS_SIEGE_005 + CMOVTO; invalid: cannot_code, siege RTS_SIEGE_004.
void GameCamera::move_order(game::Mission& m, glm::vec3 to, bool minimap) {
    game::World& w = m.world();
    const std::vector<int>& sel = rts_selection();
    const bool ok = m.ai().rts_move(sel, {to.x, to.y, to.z}, ctrl_);
    rts_family(!ok ? 7 : minimap ? 1 : ctrl_ ? 0xe : 6);
    const game::Unit* one = sel.size() == 1 ? w.unit(sel[0]) : nullptr;
    std::string s;
    if (!ok) {
        s = one && class_of(w, *one) == 8 ? "RTS_SIEGE_004" : cannot_code(w);
    } else {
        s = ack(w, sel, "003", "RTS_LTINF_00", rnd(5, 7), &one);
        if (one) s = one->rank > 3 ? "RTS" + voice_prefix(w, *one) + "003" : "RTS_LTINF_00" + std::to_string(rnd(5, 7));
        if (one && class_of(w, *one) == 8) {
            if (game::siege_slots_full(w, *one, 1)) s = "RTS_SIEGE_003";
            else s = "RTS_SIEGE_005", rts_family(7);
        }
    }
    say(m, s);
}

// Rts_cmd_assign B result (0x44f750): join -> "RTS" + target prefix + 005 (troop leader) / 012 (commander), ASSIGN;
// follow -> per follower (rank > 2, not the target) its prefix + 013 (014 following CHARLIE7), troops
// RTS_LTINF_005..007, FOLLOW; nothing happened -> CASSTO + RTS_LTINF_011..013.
void GameCamera::join_done(game::Mission& m, int target, bool ok, bool follow) {
    game::World& w = m.world();
    const game::Unit* t = w.unit(target);
    if (!ok || !t) return rts_family(0xb), say(m, "RTS_LTINF_0" + std::to_string(rnd(11, 13)));
    rts_family(follow ? 10 : 9);
    if (!follow) return say(m, "RTS" + voice_prefix(w, *t) + (t->rank > 3 ? "012" : "005"));
    for (int id : std::vector<int>(rts_selection()))
        if (const game::Unit* u = w.unit(id); alive(w, u) && u->rank > 2 && u != t)
            say(m, u->rank > 3 ? "RTS" + voice_prefix(w, *u) + (formats::iequals(code_of(w, *t), "CHARLIE7") ? "014" : "013")
                               : "RTS_LTINF_00" + std::to_string(rnd(5, 7)));
}

void GameCamera::rts_join_answer(game::Mission& m, bool ok) {
    const int t = join_target_;
    join_target_ = 0, join_args_.clear();
    if (!t) return;
    if (ok) return join_done(m, t, m.ai().rts_assign(rts_selection(), t, true), false);
    rts_family(0xb), say(m, cannot_code(m.world()));
}

// Right click in the view (Rts_process_input step 9): the command of the hovered type (§3.4).
void GameCamera::rts_command(game::Mission& m) {
    std::vector<int>& sel = groups_[std::size_t(active_)];
    if (hover_type_ < 0 || (sel.empty() && hover_type_ != 10)) return;  // a barracks also opens for the player alone
    game::World& w = m.world();
    game::AiSystem& ai = m.ai();
    const game::Unit* one = sel.size() == 1 ? w.unit(sel[0]) : nullptr;
    switch (hover_type_) {
        case 0:
        case 4: move_order(m, hover_pos_, false); break;
        case 1: {  // Rts_cmd_attack_unit 0x44eb80: single unit prefix + 003 (commander) / 008..010, siege
            ai.rts_attack_unit(sel, hover_id_), rts_family(attack_family(w, sel));  // RTS_SIEGE_005 unless F full
            std::string s = "RTS_LTINF_" + num3(rnd(8, 10));
            if (one && class_of(w, *one) == 8) s = game::siege_slots_full(w, *one, 2) ? s : "RTS_SIEGE_005";
            else if (one) s = "RTS" + voice_prefix(w, *one) + (one->rank > 3 ? "003" : num3(rnd(8, 10)));
            say(m, s);
            break;
        }
        case 2: {  // Rts_cmd_attack_item 0x44f1f0: several RTS_LTINF_008/009; commander 005; siege RTS_SIEGE_006 unless F
            ai.rts_attack_item(sel, hover_id_), rts_family(attack_family(w, sel));  // full (then RTS_LTINF_005..007)
            std::string s = "RTS_LTINF_00" + std::to_string(rnd(8, 9));
            if (one) s = one->rank > 3 ? "RTS" + voice_prefix(w, *one) + "005"
                         : class_of(w, *one) != 8 ? std::string()
                         : game::siege_slots_full(w, *one, 2) ? "RTS_LTINF_00" + std::to_string(rnd(5, 7)) : "RTS_SIEGE_006";
            if (!s.empty()) say(m, s);
            break;
        }
        case 6: {  // Rts_cmd_assign A: the new crew leaves the selection; RTS_SIEGE_001 / 002
            const bool ok = ai.rts_assign(sel, hover_id_, false);
            std::erase_if(sel, [&](int id) {
                const game::Unit* u = w.unit(id);
                return u && ai.st(*u).task.id == game::task::LARGE_WEAPON_PUSH;
            });
            rts_family(ok ? 8 : 0xb);
            say(m, ok ? (rnd(0, 1) ? "RTS_SIEGE_001" : "RTS_SIEGE_002") : "RTS_LTINF_0" + std::to_string(rnd(11, 13)));
            break;
        }
        case 7:
        case 8: {  // over capacity (a join that would push members out): CHARMOVE|1 popup, the game pauses (+0x1269)
            bool confirm = false;
            const bool ok = ai.rts_assign(sel, hover_id_, false, &confirm);
            if (game::Unit* t = w.unit(hover_id_); confirm && t) {
                int joining = 0;
                for (int id : sel)
                    if (const game::Unit* u = w.unit(id); alive(w, u) && !ai.is_leader(*u) && u->rank < 3 && u->team != t->team && u != t) ++joining;
                const int cap = ai.max_troop_size(*t), size = int(t->members.size());
                join_target_ = t->id;
                join_args_ = {t->name, std::to_string(cap), std::to_string(joining), std::to_string(size - std::max(0, cap - joining)),
                              formats::iequals(code_of(w, *t), "JOAN_OF_ARC") ? "1" : "0"};
                break;
            }
            join_done(m, hover_id_, ok, hover_type_ == 8);
            break;
        }
        case 9: rts_family(0xb), say(m, cannot_code(w)); break;
        case 10: pick_building(m, hover_pos_, true); break;
        case 11:  // Rts_cmd_ride 0x452720: TRIGGER_INTERACT from the selected leader (the player) to the unit
            if (const game::Unit* p = w.unit(w.player_id())) post_trigger(w, 0x200, 1, hover_id_, *p, 100.0f);
            break;
    }
}

// Left double-click in the view: Rts_dismiss_siege_crew 0x44a690 on a siege weapon, else Rts_operate_item 0x44a430
// (own / neutral gate without key -> event 0x20 open/close; box opened by Interact without key -> 0x200).
void GameCamera::rts_double_click(game::Mission& m, glm::vec2 mouse, glm::ivec4 vp, const glm::mat4& vp_m) {
    game::World& w = m.world();
    const game::Unit* p = w.unit(w.player_id());
    if (!p) return;
    if (const game::Unit* u = w.unit(pick_unit(w, vp_m, vp, mouse)); u && u->m.can_attach)
        return m.ai().rts_dismiss_crew(u->id), say(m, "RTS_SIEGE_007");
    glm::vec3 o, dir;
    float len, best = 1e30f;
    pick_ray(vp_m, vp, mouse, o, dir, len);
    const game::MapItem* it = m.objects().item(pick_item(m, assets, o, dir, false, 0, best));
    if (!it) return;
    const game::ObjRes* res = m.objects().res(it->res);
    if (it->cls == 1 && (it->nation == 0 || it->nation == p->nation) && it->need_key == 0)
        post_trigger(w, 0x20, 2, it->modify, *p, 1.0f);
    else if (it->cls == 10 && res && res->open_with_interact && res->need_key == 0)
        post_trigger(w, 0x200, 2, it->modify, *p, 1.0f);
}

void GameCamera::rts_input(game::Mission& m, const RawInput& raw, glm::vec2 mouse, glm::ivec4 vp, const glm::mat4& vp_m) {
    const bool l = raw.mouse_buttons[SDL_BUTTON_LEFT], r = raw.mouse_buttons[SDL_BUTTON_RIGHT];
    const bool l_prev = prev_buttons_[SDL_BUTTON_LEFT], r_prev = prev_buttons_[SDL_BUTTON_RIGHT];
    prev_buttons_ = raw.mouse_buttons;
    std::bitset<10> digits;
    for (int n = 1; n <= 9; ++n) digits[std::size_t(n)] = raw.keys[std::size_t(SDL_SCANCODE_1 + n - 1)];
    const std::bitset<10> digit_down = digits & ~digits_;
    digits_ = digits;
    shift_ = raw.keys[SDL_SCANCODE_LSHIFT] || raw.keys[SDL_SCANCODE_RSHIFT];
    ctrl_ = raw.keys[SDL_SCANCODE_LCTRL] || raw.keys[SDL_SCANCODE_RCTRL];
    alt_ = raw.keys[SDL_SCANCODE_LALT] || raw.keys[SDL_SCANCODE_RALT];
    if (!rts_ || play_.active || vp.z <= 0 || vp.w <= 0) {
        pressing_ = dragging_ = false;
        return;
    }
    game::World& w = m.world();

    // Steps 2 / 3: Ctrl+'1'..'9' stores group 0 into n (Rts_store_group 0x418310, group 0 even while another group is
    // active; RTS_NARRATOR_001), Alt+'1'..'9' makes group n the working selection (Rts_recall_group 0x44aad0,
    // RTS_LTINF_002..004). Either ends the frame.
    for (int n = 1; n <= 9; ++n) {
        if (!digit_down[std::size_t(n)] || !(ctrl_ || alt_)) continue;
        if (ctrl_ && !groups_[0].empty()) groups_[std::size_t(n)] = groups_[0], say(m, "RTS_NARRATOR_001");
        else if (alt_ && !ctrl_ && !groups_[std::size_t(n)].empty())  // Rts_clear_highlight
            active_ = n, commander_ = last_commander_ = 0, say(m, "RTS_LTINF_00" + std::to_string(rnd(2, 4)));
        return;
    }

    // Screen edges (0x593980 rect tests, 10 px inset): scroll toward the cursor. Speed HYPOTHESIS = key speed.
    const glm::vec2 lo{float(vp.x), float(vp.y)}, hi = lo + glm::vec2(float(vp.z), float(vp.w)), centre = (lo + hi) * 0.5f;
    if (mouse.x >= lo.x && mouse.y >= lo.y && mouse.x < hi.x && mouse.y < hi.y &&
        (mouse.x < lo.x + 10 || mouse.y < lo.y + 10 || mouse.x >= hi.x - 10 || mouse.y >= hi.y - 10))
        edge_dir_ = glm::normalize(glm::vec2(mouse.x - centre.x, centre.y - mouse.y) + glm::vec2(1e-4f, 0.0f));
    if (raw.mouse_buttons[SDL_BUTTON_MIDDLE]) mmb_dx_ += raw.mouse_dx;
    // Minimap (Minimap_is_mouse_over 0x717650) and the commander portraits (step 4) take their own clicks.
    const bool over_hud = std::any_of(rts_blocked.begin(), rts_blocked.end(), [&](glm::vec4 b) {
        return mouse.x >= b.x && mouse.y >= b.y && mouse.x < b.x + b.z && mouse.y < b.y + b.w;
    });

    // Left press: double-click (step 5; ponytail: the Windows double-click time 500 ms of game time, 4 px), else a
    // click / box select starts (steps 6 / 7). Release without a drag = Rts_click_select 0x449b30 (Shift adds).
    if (l && !l_prev && !over_hud) {
        if (m.now() - click_ms_ <= 500 && glm::distance(mouse, click_px_) <= 4.0f) {
            click_ms_ = -100000;
            pressing_ = dragging_ = false;
            return rts_double_click(m, mouse, vp, vp_m);
        }
        click_ms_ = m.now(), click_px_ = mouse;
        pressing_ = true, dragging_ = false, drag_from_ = drag_to_ = mouse;
        if (shift_) rts_flatten();
        drag_base_ = shift_ ? groups_[0] : std::vector<int>{};
    }
    std::vector<int>& g0 = groups_[0];
    const auto add = [&](int id) {
        if (g0.size() < 100 && std::find(g0.begin(), g0.end(), id) == g0.end()) g0.push_back(id);
    };
    const auto add_team = [&](int id) {  // selectable unit + its selectable team when it leads one
        std::vector<int> team = m.ai().rts_select_with_team(id);
        if (team.empty() && id == w.player_id()) team.push_back(id);
        for (int t : team) add(t);
    };
    if (l && pressing_) {
        drag_to_ = mouse;
        if (glm::distance(drag_from_, drag_to_) > 4.0f) dragging_ = true;
    }
    // Rts_box_select 0x4489c0 every frame while dragging: pre-existing (Shift) entries + allied selectable units whose
    // centre is inside the rectangle (+ their teams), cap 100; units leaving the box drop out again.
    if (pressing_ && dragging_) {
        g0 = drag_base_, active_ = 0, commander_ = last_commander_ = 0;
        const glm::vec2 a = glm::min(drag_from_, drag_to_), b = glm::max(drag_from_, drag_to_);
        for (game::Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || !allied(w, u) || !m.ai().rts_selectable(u)) continue;
            glm::vec2 px;
            float d;
            if (project(vp_m, vp, {u.pos.x, u.pos.y, u.pos.z + unit_height(w, u) * 0.5f}, px, d) && px.x >= a.x && px.y >= a.y &&
                px.x <= b.x && px.y <= b.y)
                add_team(u.id);
        }
    }
    if (!l && l_prev && pressing_) {
        if (!dragging_) {
            if (!shift_) g0.clear(), active_ = 0;
            commander_ = last_commander_ = 0;
            if (const game::Unit* u = w.unit(pick_unit(w, vp_m, vp, mouse))) {
                if (const game::Unit* rd = w.unit(u->m.rider); rd && !rd->deleted) u = rd;
                if (allied(w, *u)) add_team(u->id), commander_ = !shift_ && u->rank >= 4 ? u->id : 0;  // 0x449b30 +0x1238
            }
            rts_family(1);  // POINT animation
        }
        if (!g0.empty()) say(m, select_code(w));
        pressing_ = dragging_ = false;
    }

    // Step 9: hover pick every frame (cursor), the right click executes the hovered command.
    if (over_hud) hover_type_ = 0, hover_id_ = 0, hover_siege_ = 0;
    else if (!dragging_) rts_pick(m, mouse, vp, vp_m);
    hover_att_ = attack_family(w, rts_selection());
    if (r && !r_prev && !over_hud && !dragging_) rts_command(m);
}

// Rts_process 0x442ef0 / Rts_draw 0x4450f0: a command family plays its frames (prefix + 2 digits) every 50 ms, then
// the idle name follows the hover type (attack cursor by the class of a single selected unit).
std::string GameCamera::rts_cursor(float dt) {
    struct Family { const char* prefix; int last; };
    static constexpr Family kFam[0xc] = {{"POINT", 0},  {"POINT", 5},  {"HVYATT", 8}, {"INFATT", 8},
                                         {"ARCATT", 8}, {"", 0},       {"MOVTO", 10}, {"CMOVTO", 10},
                                         {"ASSTO_", 10}, {"ASSIGN", 15}, {"FOLLOW", 17}, {"CASSTO", 10}};
    // Building hover (Rts_process families 0xf..0x12): BARR01..18 / WEAPON00, 01..09 / IRON01..09 / WOOD01..17 loop
    // every 50 ms while the cursor stays on a type-10 target.
    if (cursor_family_ >= 0xf && cursor_family_ <= 0x12) {
        static constexpr Family kBld[4] = {{"BARR", 18}, {"WEAPON", 9}, {"IRON", 9}, {"WOOD", 17}};
        if (hover_type_ == 10) {
            const Family& b = kBld[cursor_family_ - 0xf];
            cursor_ms_ += dt * 1000.0f;
            if (cursor_ms_ > 50.0f) cursor_ms_ = 0, cursor_frame_ = cursor_frame_ >= b.last ? 1 : cursor_frame_ + 1;
            char name[16];
            std::snprintf(name, sizeof name, "%s%02d", b.prefix, cursor_frame_);
            return cursor_name_ = name;
        }
        cursor_family_ = 0;
    }
    const int fam = cursor_family_ == 0xe ? 6 : cursor_family_;
    if (fam > 0 && fam < 0xc && kFam[fam].prefix[0]) {
        cursor_ms_ += dt * 1000.0f;
        if (cursor_ms_ > 50.0f) {
            cursor_ms_ = 0;
            if (cursor_frame_ < kFam[fam].last) ++cursor_frame_;
            else cursor_family_ = 0;
        }
        if (cursor_family_) {
            char name[16];
            std::snprintf(name, sizeof name, "%s%02d", kFam[fam].prefix, cursor_frame_);
            return cursor_name_ = name;
        }
    } else {
        cursor_family_ = 0;
    }
    switch (hover_type_) {
        case 0: cursor_name_ = "POINT00"; break;
        case 1:
        case 2: {
            static constexpr const char* kAtt[5] = {"", "", "HVYATT00", "INFATT00", "ARCATT00"};
            cursor_name_ = kAtt[hover_att_];
            break;
        }
        case 6: cursor_name_ = "ASSTO_00"; break;
        case 7: cursor_name_ = "ASSIGN01"; break;
        case 8: cursor_name_ = "FOLLOW01"; break;
        default: break;  // 4 / 9 / 11 keep the previous name (Rts_draw writes none)
    }
    return cursor_name_;
}

glm::vec4 GameCamera::drag_box() const {
    if (!dragging_) return glm::vec4(0.0f);
    const glm::vec2 a = glm::min(drag_from_, drag_to_), b = glm::max(drag_from_, drag_to_);
    return {a.x, a.y, b.x, b.y};
}

}  // namespace joa::engine
