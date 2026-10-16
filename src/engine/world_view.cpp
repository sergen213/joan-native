#include "engine/world_view.hpp"

#include "formats/res_archive.hpp"
#include "formats/world_text.hpp"

#include <glm/gtc/color_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace joa::engine {
namespace {

// Unit heading h (degrees CCW from original +X) as a model matrix about +Y: the AM1 meshes face original
// +X, so this faces engine (cos h, 0, -sin h) = to_engine(cos h, sin h). Checked visually
// (work/scene/joan_*.png): Joan then faces Jean de Metz's scripted position.
glm::mat4 unit_frame(const game::Vec3& p, float yaw_deg) {
    return glm::rotate(glm::translate(glm::mat4(1.0f), to_engine(p.x, p.y, p.z)), glm::radians(yaw_deg), glm::vec3(0, 1, 0));
}

float wrap180(float d) { return d - 360.0f * std::floor((d + 180.0f) / 360.0f); }

std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
}

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

}  // namespace

WorldView::WorldView(IRenderer& renderer, const formats::Vfs& vfs, const game::Mission& mission,
                     std::vector<std::filesystem::path> hd_packs)
    : vfs_(vfs), db_(vfs_), objects_(&mission.objects()) {
    assets_ = std::make_unique<AssetCache>(vfs_, renderer, std::move(hd_packs));
    // MAP\scenarioN + the nodes of <WorldFileName>.wor at their origins (the game's own node list).
    const std::string dir = slashes(mission.map_dir());
    std::vector<MapNode> maps;
    for (const game::WorldNode& n : game::world_nodes(vfs_, mission.map_dir(), mission.info().step))
        maps.push_back({n.name, {n.ox, n.oy}});
    const std::string& wf = mission.info().step.world_file;  // "scenario3\S3L1w"
    const std::string world = wf.substr(wf.find_last_of("\\/") + 1);
    // Dev: JOA_WORLD_CLOCK=1440 (minutes after midnight) picks another .day lighting stage, e.g. Normal_Night.
    const char* clock = std::getenv("JOA_WORLD_CLOCK");
    scene_ = std::make_unique<WorldScene>(*assets_, dir, maps, world, clock ? float(std::atof(clock)) : 720.0f);
    lighting_ = scene_->lighting();
    sky_ = std::make_unique<SkyDome>(*assets_, glm::convertSRGBToLinear(lighting_.fog_color), scene_->cloud_textures());
    // map/scenarioN -> DynGrass.res "_M<N>" variants.
    const int scenario_no = std::atoi(dir.c_str() + std::min(dir.size(), dir.find_last_not_of("0123456789") + 1));
    for (const WorldScene::Node& sn : scene_->nodes()) {
        Node& n = nodes_.emplace_back();
        n.offset = sn.offset();
        n.grass = std::make_unique<GrassField>(*assets_, sn.grid, scenario_no);
        n.particles = std::make_unique<ParticleSystem>(*assets_, sn.map.name, n.offset);
        n.water = std::make_unique<WaterField>(*assets_, sn.map.name, sn.map.origin);
    }
    effects_ = std::make_unique<ParticleSystem>(*assets_, "");
    // Tie each .stg object the game turned into a map item to that item (same res code, same position); one
    // without a live item was deleted before this view (a loaded save) and is hidden.
    std::vector<bool> linked(std::size_t(objects_->size()) + 1);
    for (std::size_t i = 0; i < scene_->objects().size(); ++i) {
        const PlacedObject& o = scene_->objects()[i];
        const int res = objects_->res_by_code(o.code);
        if (!objects_->is_map_item(res)) continue;
        int recno = 0;
        for (int r = 1; r <= objects_->size() && !recno; ++r)
            if (const game::MapItem* it = objects_->item(r); it && !linked[std::size_t(r)] && it->res == res &&
                                                             glm::distance(glm::vec3(it->pos.x, it->pos.y, it->pos.z), o.at) < 0.5f)
                recno = r;
        if (!recno) {
            scene_->hide(i);
            continue;
        }
        linked[std::size_t(recno)] = true;
        map_actors_.push_back({recno, res, objects_->item(recno)->stage, i});
    }

    // Attack-script emitter slots (Resource/animscr.res, AnimScript_parse 0x419320): EmitterFlag 1, then
    // TerrainEmitterFlag and 6 x (EmitterFileName, Start/EndTime, [MinSeparateDist], X, Y, Z, LockType,
    // WeaponType). CONFIRMED layout.
    if (auto bytes = vfs_.read("Resource/animscr.res"))
        if (const auto res = formats::ResArchive::parse(std::move(*bytes)))
            for (const formats::ResEntry& e : res->entries()) {
                const auto text = res->text(e.name);
                if (!text) continue;
                std::string code = e.name;
                ScriptEmitters s;
                bool flag = false;
                for (const auto& kv : formats::parse_kv_text(*text)) {
                    const std::string k = lower(kv.key);
                    const float v = std::strtof(kv.value.c_str(), nullptr);
                    if (k == "code") code = trim(kv.value);
                    else if (k == "emitterflag") flag = v != 0.0f;
                    else if (k == "terrainemitterflag") s.terrain = v != 0.0f;
                    else if (k == "emitterfilename") s.slots.push_back({trim(kv.value)});
                    else if (s.slots.empty()) continue;
                    else if (k == "emitterstarttime") s.slots.back().start = v;
                    else if (k == "emitterendtime") s.slots.back().end = v;
                    else if (k == "emitterx") s.slots.back().offset.x = v;
                    else if (k == "emittery") s.slots.back().offset.y = v;
                    else if (k == "emitterz") s.slots.back().offset.z = v;
                    else if (k == "emitterlocktype") s.slots.back().lock = int(v);
                }
                if (flag) scripts_[lower(code)] = std::move(s);
            }

    const WorldStats& s = scene_->stats();
    std::fprintf(stderr, "joan-native: %s nodes=%zu stage=%s objects=%d placed=%d missing=%d terrain_tris=%d groups=%d lightmaps=%d "
                 "missing_textures=%zu attack_emitter_scripts=%zu\n",
                 world.c_str(), nodes_.size(), s.stage.c_str(), s.objects, s.placed, s.missing_models, s.terrain_triangles,
                 s.terrain_groups, s.lightmaps, assets_->missing_textures(), scripts_.size());
}

WorldView::~WorldView() = default;

const Character* WorldView::actor(int unit_id) const {
    const auto it = actors_.find(unit_id);
    return it == actors_.end() ? nullptr : it->second.character.get();
}

std::string WorldView::actor_model(int unit_id) const {
    const auto it = actors_.find(unit_id);
    return it == actors_.end() || !it->second.character ? std::string() : it->second.model;
}

void WorldView::equip(Actor& a, const game::World& world, const game::Unit& u) {
    // Hand weapons, else the ranged weapon (archers hold their bow; weapon mode 2 = ranged only).
    // HYPOTHESIS, matches the original captures of archers. Armour does not hang on a bone.
    const game::Inventory& inv = u.inv;
    const bool melee = inv.melee[0].item || inv.melee[1].item;
    const std::array<int, 3> want = inv.weapon_mode != 2 && melee ? std::array<int, 3>{inv.melee[0].item, inv.melee[1].item, 0}
                                                                  : std::array<int, 3>{0, 0, inv.ranged.item};
    if (want == a.items) return;
    a.items = want;
    Character& c = *a.character;
    c.clear_attachments();
    const auto& items = world.data().items;
    for (const int id : want) {
        if (id <= 0 || id >= int(items.size())) continue;
        const auto item = db_.item(items[std::size_t(id)].code);
        if (!item || item->bone.empty()) continue;  // not a carried mesh
        const std::string anim = item_anim(item->model);
        const std::string shadow = item->model.substr(0, item->model.rfind('.')) + "#s.SMO";
        const formats::Mesh* caster = anim.empty() && vfs_.exists(shadow) ? assets_->static_mesh(shadow) : nullptr;
        if (c.attach(assets_->model(item->model, anim), item->bone, caster)) ++unit_stats_.items;
        else ++unit_stats_.items_missing, std::fprintf(stderr, "joan-native: item %s on %s: not attached\n", item->model.c_str(), item->bone.c_str());
    }
}

void WorldView::sync(const game::World& world, const game::Snapshot& prev, const game::Snapshot& cur) {
    world_ = &world;
    player_ = world.player_id();
    scene_->tick_lights();
    std::unordered_map<int, const game::UnitView*> before, now;
    for (const game::UnitView& v : prev.units) before[v.id] = &v;
    for (const game::UnitView& v : cur.units) now[v.id] = &v;
    std::erase_if(actors_, [&](const auto& kv) { return !now.contains(kv.first); });
    const float tick_ms = float(cur.time_ms - prev.time_ms);
    for (const game::UnitView& v : cur.units) {
        auto [it, fresh] = actors_.try_emplace(v.id);
        Actor& a = it->second;
        const game::Unit* u = world.unit(v.id);
        const int level = u ? u->model_level : 0;
        if (fresh || level != a.level) {
            // Unit_set_model_level 0x511060 (armour use 0x6d8ac0, broken siege weapon in Unit_die): a new actor
            // with the level's mesh (class +0x84 + n*4) and animation group (+0x90 + n*4) replaces the old one.
            a.level = level;
            try {
                auto look = db_.look(v.class_code, level);
                if (!look) throw std::runtime_error("no unit.res record / idle animation");
                auto c = std::make_unique<Character>(*assets_, look->model, look->anim);
                c->play(look->anim, look->idle);
                if (!look->shadow.empty()) c->set_shadow_model(assets_->model(look->shadow));
                a.anim_file = lower(look->anim), a.clip = lower(look->idle);
                a.t0 = a.t1 = 0;
                // A level with its own animation group (BROKEN_CANNON...) holds that group's idle: the game layer
                // keeps playing the class table, whose clips do not fit the wreck. HYPOTHESIS (static wrecks).
                const auto base = level ? db_.look(v.class_code) : look;
                a.own_anim = base && !formats::iequals(base->anim_code, look->anim_code);
                a.character = std::move(c);
                a.model = look->model;
                a.items = {-1, -1, -1};
            } catch (const std::exception& e) {
                if (fresh) ++unit_stats_.missing;
                std::fprintf(stderr, "joan-native: unit %d %s level %d: %s\n", v.id, v.class_code.c_str(), level, e.what());
            }
        }
        const auto pi = before.find(v.id);
        const game::UnitView& pv = pi == before.end() ? v : *pi->second;
        a.p0 = {pv.pos.x, pv.pos.y, pv.pos.z}, a.p1 = {v.pos.x, v.pos.y, v.pos.z};
        a.speed = glm::length(glm::vec2(a.p1 - a.p0)) * 1000.0f / std::max(tick_ms, 1.0f);
        a.yaw0 = pv.yaw_deg, a.yaw1 = v.yaw_deg;
        if (!a.character) continue;
        Character& c = *a.character;
        // Snapshot clip = anim/<UPPERFILE>.am2 : UPPERNAME at the game's clip time (looping actions wrap,
        // one-shots clamp: a dead unit holds the last frame of its death clip until it is deleted).
        const std::string file = v.clip_file.empty() || a.own_anim ? a.anim_file : lower("anim/" + v.clip_file + ".am2");
        const std::string clip = v.clip.empty() || a.own_anim ? a.clip : lower(v.clip);
        a.wrap = false;
        if (a.own_anim) {
            a.t0 = a.t1 = 0;
        } else if (file != a.anim_file || clip != a.clip) {
            if (!c.play(file, clip)) std::fprintf(stderr, "joan-native: unit %d: clip %s:%s missing\n", v.id, file.c_str(), clip.c_str());
            a.anim_file = file, a.clip = clip;
            a.t0 = a.t1 = v.clip_time_ms;
        } else {
            a.t0 = pv.clip_time_ms, a.t1 = v.clip_time_ms;
            if (a.t1 < a.t0) {
                // Looped (prev + tick wraps onto cur) or restarted (same clip again): snap.
                const float len = c.clip_length();
                if (std::abs(std::fmod(a.t0 + tick_ms, len) - a.t1) < 2.0f) a.t1 += len, a.wrap = true;
                else a.t0 = a.t1;
            }
        }
        // FUN_00417f20 overwrites the Bip01 x/y keys of root-motion clips with the start key: the game layer moves
        // the body by that travel (World::root_motion), so the drawn pose must not repeat it.
        a.root = nullptr;
        if (u && !a.own_anim && game::root_motion_action(u->action))
            if (const game::MoveRecord* r = world.move(*u))
                if (const game::ClipInfo* ci = world.data().clip(*r); ci && !ci->root.empty()) a.root = ci;
        if (u) equip(a, world, *u);
    }
    unit_stats_.actors = int(std::count_if(actors_.begin(), actors_.end(), [](const auto& kv) { return kv.second.character != nullptr; }));
    sync_items(world, prev, cur);
    sync_map_items(world);
}

void WorldView::spawn_slot(const std::string& script, std::size_t slot, const game::UnitView& at) {
    const auto it = scripts_.find(lower(script));
    // ponytail: TerrainEmitterFlag scripts (ground-type dust/ripple names + MinSeparateDist, player only,
    // FUN_0041b640 terrain branch) are skipped.
    if (it == scripts_.end() || it->second.terrain || slot >= it->second.slots.size()) return;
    const SlotEmitter& s = it->second.slots[slot];
    if (s.name.empty() || lower(s.name) == "none") return;
    // FUN_0041a2a0: position = unit position + (X, Y) turned by the unit yaw + Z; start/end relative
    // to the trigger. LockType != 0 re-places the emitter on the unit every update (Actor_set_pos):
    // HIGH for "follows the unit", the per-type differences (1..3, bone/weapon) are not reproduced.
    const glm::mat4 frame = unit_frame(at.pos, at.yaw_deg);
    const glm::vec3 local = to_engine(s.offset.x, s.offset.y, s.offset.z);
    effects_->spawn(s.name, glm::vec3(frame * glm::vec4(local, 1.0f)), glm::radians(at.yaw_deg), s.start / 1000.0f, s.end / 1000.0f,
                    s.lock ? at.id : 0, local);
}

void WorldView::on_events(const std::vector<game::GameEvent>& events, const game::Snapshot& cur) {
    if (!world_) return;
    std::unordered_map<int, const game::UnitView*> at;
    for (const game::UnitView& v : cur.units) at[v.id] = &v;
    const auto script_of = [&](int unit) -> const game::AnimScript* {
        const game::Unit* u = world_->unit(unit);
        const game::MoveRecord* r = u ? world_->move(*u) : nullptr;
        return r ? world_->script_of(r) : nullptr;
    };
    for (const game::GameEvent& e : events) {
        const auto v = at.find(e.unit);
        if (v == at.end()) continue;
        switch (e.type) {
        case game::Ev::Action: {
            // Unit_set_action 0x501190: slots 0..3 of the new move's attack script on the acting unit. CONFIRMED.
            const game::Unit* u = world_->unit(e.unit);
            if (!u || u->action != e.value) break;  // superseded within the tick
            if (const game::AnimScript* s = script_of(e.unit))
                for (std::size_t slot = 0; slot < 4; ++slot) spawn_slot(s->code, slot, *v->second);
            break;
        }
        case game::Ev::Hit:
            // Melee hit landing (0x5048b0): slot 4 of the attacker's script on the victim. CONFIRMED; the
            // ranged / keg / down-attack damage paths spawn none.
            if (e.other && e.name != "ARROW" && e.name != "STONE" && e.name != "EXPLOSIVE" && e.name != "KEG" && e.name != "DOWNATK")
                if (const game::AnimScript* s = script_of(e.other)) spawn_slot(s->code, 4, *v->second);
            break;
        case game::Ev::Blocked:
            // Guarded hit (0x5048b0 / arrow guard 0x5051e0): slot 5 of script "DEFEND" on the defender. CONFIRMED.
            spawn_slot("DEFEND", 5, *v->second);
            break;
        case game::Ev::Effect: {
            // flash.txt (level up, FUN_00522c40 non-quiet, every unit) / spiritual.txt (recovery items):
            // pscript on the unit's actor, locked, 5000 ms. CONFIRMED (0x41baf0 call, uVar = 5000).
            if (e.name == "PICKUP") {
                // Unit_auto_pickup 0x49da30: the item's emitter (+0x17a = EmitterName slot 2, "*_eaten") at the
                // item for 5000 ms (0x41baf0). Slot choice HIGH (every "_eaten" pscript sits in slot 2).
                if (e.other >= 1 && e.other <= int(world_->items().size()))
                    spawn_item_emitter(e.value, 2, world_->items()[std::size_t(e.other - 1)].pos);
                else if (const auto g = gone_.find(e.other); g != gone_.end())  // map item (-recno), deleted
                    spawn_item_emitter(e.value, 2, g->second);
                break;
            }
            const std::string name = e.name == "LEVELUP" ? std::string("flash.txt") : e.name;
            if (!lower(name).ends_with(".txt")) break;
            effects_->spawn(name.substr(0, name.size() - 4), glm::vec3(unit_frame(v->second->pos, v->second->yaw_deg)[3]),
                            glm::radians(v->second->yaw_deg), 0.0f, 5.0f, e.unit);
            break;
        }
        case game::Ev::Death:
        case game::Ev::Knockdown: {  // interactive water: a body falling in it
            const game::Vec3& p = v->second->pos;
            splash(to_engine(p.x, p.y, p.z), e.type == game::Ev::Death ? 1.0f : 0.8f, 150.0f);
            break;
        }
        default: break;
        }
    }
}

void WorldView::frame(float alpha, const CameraPose& pose, float dt, FrameView& view, std::vector<DrawItem>& draws) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const glm::vec3 eye = pose.eye;
    view.view = glm::lookAtRH(eye, pose.target, glm::vec3(0, 1, 0));
    view.camera_position = eye;
    // In-game depth range 4..4096 (world_scene.hpp kWorldNear/kWorldFar), same field of view.
    view.projection[2][2] = -(kWorldFar + kWorldNear) / (kWorldFar - kWorldNear);
    view.projection[3][2] = -2.0f * kWorldFar * kWorldNear / (kWorldFar - kWorldNear);
    const glm::mat4 view_proj = view.projection * view.view;
    lighting_ = scene_->lighting();
    lighting_.fog_color = scene_->fog_color(pose.target - eye);
    lighting_.clear_color = glm::vec4(glm::convertSRGBToLinear(lighting_.fog_color), 1.0f);

    time_s_ += dt;
    // Interactive water: the wave window follows the look-at point; kicks of this frame (wade below) enter the next step.
    waves_.follow({pose.target.x, pose.target.z});
    waves_.step(dt);
    sky_->build(eye, time_s_, draws);
    scene_->build(view_proj, eye, draws);
    for (Node& n : nodes_) {
        n.water->build(time_s_, eye, draws);
        // Grass patches live in node-local space.
        const std::size_t first = draws.size();
        n.grass->build(eye - n.offset, draws);
        for (std::size_t i = first; i < draws.size(); ++i) draws[i].model = glm::translate(glm::mat4(1.0f), n.offset) * draws[i].model;
        n.particles->build(dt, view.view, view_proj, draws);
    }

    const glm::vec3 sun = lighting_.sun_direction;
    shadow_stats_ = {};
    frames_.clear();
    // Range mode (0x42a6c0 enter / 0x42b6e0 exit): the hero's actor is hidden (FUN_0042cf20(0/1): actor +0x10a)
    // behind the first-person aim camera. HIGH (the call's ECX = unit +0x2f5 actor is inferred from the guard).
    const bool aiming = world_ && world_->controller().range_mode;
    for (auto& [id, a] : actors_) {
        if (!a.character) continue;
        const glm::vec3 p = glm::mix(a.p0, a.p1, alpha);
        const glm::mat4 t = unit_frame({p.x, p.y, p.z}, a.yaw0 + wrap180(a.yaw1 - a.yaw0) * alpha);
        frames_[id] = t;
        const glm::vec3 at(t[3]);
        if (glm::distance(eye, at) > kWorldFar + 200.0f) continue;  // beyond the far plane: no skinning
        if (aiming && id == player_) continue;
        Character& c = *a.character;
        float ms = a.t0 + (a.t1 - a.t0) * alpha;
        if (a.wrap) ms = std::fmod(ms, c.clip_length());
        float rx = 0, ry = 0;
        if (a.root) game::root_offset(*a.root, ms, rx, ry);
        c.pose(ms, rx, ry);
        if (waves_.covers({at.x, at.z})) wade(id, a, t, dt);
        // Hardware-skinned units get one point light (Shader_build_vertex_shader clamps the skinned light
        // counts to 1 point + 1 directional): the first light of their sector.
        const std::size_t first = draws.size();
        c.build(t, draws);
        for (std::size_t i = first; i < draws.size(); ++i) scene_->sector_lights(at, 1, draws[i]), draws[i].object = std::uint32_t(id) + 1;
        // Scene::draw_shadow actor loop 0x5f1cc0: a volume within 1000 units of the eye (mode 3: every
        // unit, mode 2: the player's hero), else the ShadowModel within 2000 (CONFIRMED thresholds).
        if (shadow_mode <= 0) continue;
        const float d = glm::distance(eye, at + glm::vec3(0.0f, c.height() * 0.5f, 0.0f));
        if (d <= 1000.0f && (shadow_mode >= 3 || (shadow_mode == 2 && id == player_)))
            c.build_shadow(t, sun, Character::ShadowKind::Volume, draws), ++shadow_stats_.volumes;
        else if (d <= 2000.0f)
            c.build_shadow(t, sun, Character::ShadowKind::Model, draws), ++shadow_stats_.models;
    }
    build_items(alpha, draws);
    build_map_items(alpha, draws);
    build_rings(draws);
    effects_->follow(frames_);
    effects_->build(dt, view.view, view_proj, draws);
}

}  // namespace joa::engine
