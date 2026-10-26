// WorldView: projectiles in flight and items lying on the floor (GAMEPLAY_CHARACTER_COMBAT.md "Item models").
#include "engine/world_view.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace joa::engine {
namespace {

// Original-space direction -> model matrix at `p`: the meshes lie along original +X (arrows, bolts);
// yaw about engine +Y (heading CCW from +X), then pitch about the model's side axis (nose up = +).
glm::mat4 along(const glm::vec3& p, const glm::vec3& dir) {
    const float yaw = std::atan2(dir.y, dir.x), pitch = std::atan2(dir.z, std::hypot(dir.x, dir.y));
    glm::mat4 m = glm::translate(glm::mat4(1.0f), to_engine(p.x, p.y, p.z));
    m = glm::rotate(m, yaw, glm::vec3(0, 1, 0));
    return glm::rotate(m, pitch, glm::vec3(0, 0, 1));
}

}  // namespace

const Model* WorldView::item_model(const std::string& path) {
    const auto [it, fresh] = item_models_.try_emplace(lower(path), nullptr);
    if (fresh) {
        it->second = assets_->model(path, item_anim(path));
        if (!it->second) ++item_stats_.missing, std::fprintf(stderr, "joan-native: item model %s missing\n", path.c_str());
    }
    return it->second;
}

void WorldView::spawn_item_emitter(int item, int slot, const game::Vec3& at) {
    if (!world_ || item <= 0 || item >= int(world_->data().items.size())) return;
    const std::string name = db_.item_emitter(world_->data().items[std::size_t(item)].code, slot);
    if (!name.empty()) effects_->spawn(name, to_engine(at.x, at.y, at.z), 0.0f, 0.0f, 5.0f, 0);
}

void WorldView::sync_items(const game::World& world, const game::Snapshot& prev, const game::Snapshot& cur) {
    const auto& defs = world.data().items;
    const auto code = [&](int item) { return item > 0 && item < int(defs.size()) ? defs[std::size_t(item)].code : std::string(); };

    // Projectiles. ThrowItem_create 0x507fc0 sets the ammo item to model level 2 (vtable +0x18 (2) for every
    // type; ItemRes model 0x4a3f20: the 2nd ModelName, e.g. arrow01_hold.smo, crossbow_arrow_shootout.smo).
    // Launch 0x5133e0 turns the model to the shot direction; ThrowItem_update 0x4fcc00 re-orients PROJ/EXPL
    // along the velocity each tick (ARW flies straight; stones/balls keep the launch heading). CONFIRMED.
    std::unordered_map<int, const game::ProjectileView*> before;
    for (const game::ProjectileView& p : prev.projectiles) before[p.id] = &p;
    const float tick_s = float(cur.time_ms - prev.time_ms) / 1000.0f;
    std::unordered_map<int, Shot> next;
    for (const game::ProjectileView& p : cur.projectiles) {
        const glm::vec3 pos(p.pos.x, p.pos.y, p.pos.z), vel(p.vel.x, p.vel.y, p.vel.z);
        auto old = shots_.find(p.id);
        Shot s = old != shots_.end() ? std::move(old->second) : Shot{};
        if (old == shots_.end()) {
            s.item = p.item;
            if (const auto look = db_.item(code(p.item), 2)) s.path = look->model, s.model = item_model(look->model);
            if (glm::length(vel) > 1e-3f) s.dir = glm::normalize(vel);
        } else if (p.type <= 2 && glm::length(vel) > 1e-3f) {
            s.dir = glm::normalize(vel);
        }
        const auto b = before.find(p.id);
        s.p0 = b != before.end() ? glm::vec3(b->second->pos.x, b->second->pos.y, b->second->pos.z) : pos - vel * tick_s;
        s.p1 = pos;
        splash_segment(s.p0, s.p1, 1.0f);  // interactive water: through the surface this tick
        next.emplace(p.id, std::move(s));
    }
    // A projectile gone from the world leaves its item's impact emitter (EmitterName slot 3: explosion_arrow,
    // CannonImpact) where it was last seen. HYPOTHESIS (slot naming; the spawn site is not decoded).
    // Interactive water: one gone above the water within a tick's travel of it hit the surface (the game deletes it
    // on the bed under it within the tick that crossed the surface).
    for (const auto& [id, s] : shots_) {
        if (next.contains(id)) continue;
        spawn_item_emitter(s.item, 3, {s.p1.x, s.p1.y, s.p1.z});
        const glm::vec3 e = to_engine(s.p1.x, s.p1.y, s.p1.z);
        if (const auto h = water_height(e); h && e.y > *h && e.y - *h < glm::distance(s.p0, s.p1) + 30.0f) splash({e.x, *h, e.z}, 1.0f, 0.0f);
    }
    shots_ = std::move(next);

    // Floor items (Unit_dropItem 0x406631: ItemArray_create = model level 0, the item.res Model; placed at
    // the unit). Removed once picked up (Unit_auto_pickup 0x49da30 deletes the item). Map items with a floor
    // marker (box drops FUN_00463ed0, script add_item 0x651ad0) are the same actors, keyed -recno. Every
    // Pickable record is an item.res one (no ITEM\**\*.pro sets Pickable: 1) and .stg CreateObject only names
    // .pro files, so the static WorldScene holds no pickable item: they exist (and vanish) only here.
    const auto& items = world.items();
    gone_.clear();
    std::erase_if(drops_, [&](const auto& kv) {
        if (kv.first > 0) return kv.first > int(items.size()) || items[std::size_t(kv.first - 1)].deleted;
        const game::MapItem* mi = objects_->item(-kv.first);
        if (mi && mi->on_floor && mi->res == kv.second.item) return false;
        gone_[kv.first] = {kv.second.pos.x, kv.second.pos.y, kv.second.pos.z};
        return true;
    });
    const auto place = [&](int key, int item, const game::Vec3& pos, float yaw) {
        auto [d, fresh] = drops_.try_emplace(key);
        d->second.pos = {pos.x, pos.y, pos.z}, d->second.yaw = yaw, d->second.item = item;
        if (!fresh) return;
        if (time_s_ > 0.0f) splash(to_engine(pos.x, pos.y, pos.z), 0.8f, 150.0f);  // dropped in the water (not the map's own)
        const auto look = db_.item(code(item), 0);
        if (!look) return;
        d->second.path = look->model;
        // AM1 pickup models (potions, gems, itm_* weapons) loop their item/anim clip (spin / bob).
        if (const std::string anim = item_anim(look->model); !anim.empty() && vfs_.exists(anim)) {
            try {
                d->second.anim = std::make_unique<Character>(*assets_, look->model, anim);
                return;
            } catch (const std::exception&) {
            }
        }
        d->second.model = item_model(look->model);
    };
    for (const game::WorldItem& it : items)
        if (!it.deleted) place(it.id, it.item, it.pos, glm::radians(it.yaw_deg));
    for (int r = 1; r <= objects_->size(); ++r)
        if (const game::MapItem* mi = objects_->item(r); mi && mi->on_floor && mi->res > 0 && mi->res < int(defs.size()) &&
                                                         defs[std::size_t(mi->res)].pickable)
            place(-r, mi->res, mi->pos, mi->dir);
}

void WorldView::build_items(float alpha, std::vector<DrawItem>& draws) {
    item_stats_.shots = item_stats_.drops = 0;
    const auto push = [&](const Model& m, const glm::mat4& t) {
        const int sector = scene_->sector_at(glm::vec3(t[3]));
        for (const ModelPart& p : m.parts) {
            draws.push_back({m.mesh, p.material, t, p.first_index, p.index_count});
            scene_->sector_lights(sector, 1, draws.back());
        }
    };
    for (const auto& [id, s] : shots_) {
        if (!s.model) continue;
        push(*s.model, along(glm::mix(s.p0, s.p1, alpha), s.dir));
        ++item_stats_.shots;
    }
    for (auto& [id, d] : drops_) {
        const glm::mat4 t = along(d.pos, {std::cos(d.yaw), std::sin(d.yaw), 0.0f});
        if (d.anim) {
            d.anim->pose(std::fmod(time_s_ * 1000.0f, d.anim->clip_length()));
            const std::size_t first = draws.size();
            d.anim->build(t, draws);
            const int sector = scene_->sector_at(glm::vec3(t[3]));
            for (std::size_t i = first; i < draws.size(); ++i) scene_->sector_lights(sector, 1, draws[i]);
        } else if (d.model) {
            push(*d.model, t);
        } else {
            continue;
        }
        ++item_stats_.drops;
    }
}

void WorldView::sync_map_items(const game::World& world) {
    const int now = world.now();
    std::erase_if(map_actors_, [&](MapActor& a) {
        const game::MapItem* it = objects_->item(a.recno);
        if (!it || it->res != a.res) return scene_->hide(a.object), true;
        const game::ObjRes& r = *objects_->res(a.res);
        const PlacedObject& o = scene_->objects()[a.object];
        // stage_down: stage s -> s-1 plays stages[s-1].anim on the model shown at s-1 (FUN_004a3f20).
        if (it->stage != a.stage && it->stage < int(r.stages.size()) && !o.anim.empty()) {
            const std::string& clip = r.stages[std::size_t(it->stage)].anim;
            const std::string& shown = it->stage >= 1 && !r.stages[std::size_t(it->stage - 1)].model.empty()
                                           ? r.stages[std::size_t(it->stage - 1)].model : r.model;
            const std::string file = lower(o.path.substr(o.path.find_last_of("/\\") + 1));
            if (!clip.empty() && file == lower(shown)) {
                try {
                    if (!a.anim) a.anim = std::make_unique<Character>(*assets_, o.path, o.anim);
                    if (a.anim->play(o.anim, clip)) a.clip = clip, a.start = now, a.t1 = 0, scene_->hide(a.object);
                } catch (const std::exception&) {
                }
            }
        }
        if (it->stage < a.stage) splash(to_engine(o.at.x, o.at.y, o.at.z), 1.0f, 150.0f);  // broken in the water
        a.stage = it->stage;
        a.t0 = a.t1, a.t1 = float(now - a.start);
        a.fade = it->fade_until ? std::clamp(float(it->fade_until - now) / 2000.0f, 0.0f, 1.0f) : 1.0f;
        return false;
    });
}

void WorldView::build_map_items(float alpha, std::vector<DrawItem>& draws) {
    for (MapActor& a : map_actors_) {
        if (a.clip.empty()) continue;
        const PlacedObject& o = scene_->objects()[a.object];
        a.anim->pose(glm::mix(a.t0, a.t1, alpha));
        const std::size_t first = draws.size();
        a.anim->build(o.transform, draws);
        for (std::size_t i = first; i < draws.size(); ++i) {
            DrawItem& d = draws[i];
            d.light_count = o.light_count, d.lights = o.lights, d.object = 0x40000000u + std::uint32_t(a.recno);
            d.material.volume_caster = false;  // a scene prop, not a unit
            if (a.fade < 1.0f) d.material.blend = BlendMode::AlphaBlend, d.material.tint.a *= a.fade;
        }
    }
}

std::string WorldView::map_item_clip(int recno) const {
    for (const MapActor& a : map_actors_)
        if (a.recno == recno) return a.clip;
    return {};
}

std::string WorldView::shot_model(int projectile_id) const {
    const auto it = shots_.find(projectile_id);
    return it == shots_.end() ? std::string() : it->second.path;
}

std::string WorldView::drop_model(int item_id) const {
    const auto it = drops_.find(item_id);
    return it == drops_.end() ? std::string() : it->second.path;
}

glm::mat4 WorldView::shot_frame(int projectile_id) const {
    const auto it = shots_.find(projectile_id);
    return it == shots_.end() ? glm::mat4(1.0f) : along(it->second.p1, it->second.dir);
}

}  // namespace joa::engine
