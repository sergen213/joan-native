#include "engine/character_actor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace joa::engine {

Character::Character(AssetCache& assets, std::string_view am1, std::string_view am2) : assets_(assets) {
    std::string err;
    const auto mesh_bytes = assets.vfs().read(am1);
    if (!mesh_bytes || !formats::parse_mesh(*mesh_bytes, true, mesh_, err))
        throw std::runtime_error(std::string(am1) + ": " + (mesh_bytes ? err : "missing"));
    const auto anim_bytes = assets.vfs().read(am2);
    if (!anim_bytes || !formats::parse_animation(*anim_bytes, anim_, err))
        throw std::runtime_error(std::string(am2) + ": " + (anim_bytes ? err : "missing"));
    if (anim_.clips.empty()) anim_.clips.push_back({"all", 0, anim_.start, anim_.end});
    materials_ = assets.materials(mesh_);
    for (const auto& sub : mesh_.submeshes) {
        part_first_.push_back(part_first_.empty() ? 0 : part_first_.back() + part_count_.back());
        part_count_.push_back(std::uint32_t(formats::strip_to_triangles(sub.strip).size()));
    }
    pose(0.0f);
}

Character::~Character() { assets_.renderer().destroy_mesh(gpu_); }

bool Character::play(std::string_view name) {
    for (std::size_t i = 0; i < anim_.clips.size(); ++i)
        if (lower(anim_.clips[i].name) == lower(name)) {
            if (i != clip_) clip_ = i, prev_ms_ = cur_ms_ = 0;
            return true;
        }
    return false;
}

void Character::tick(float dt) {
    const float len = std::max(clip().end - clip().start, 1.0f);
    prev_ms_ = cur_ms_;
    cur_ms_ += dt * 1000.0f;
    if (cur_ms_ >= len) {
        cur_ms_ = std::fmod(cur_ms_, len);
        prev_ms_ -= len;  // keep prev < cur so interpolation runs forward across the loop
    }
}

float Character::time_ms(float alpha) const {
    const float t = prev_ms_ + (cur_ms_ - prev_ms_) * std::clamp(alpha, 0.0f, 1.0f);
    const float len = std::max(clip().end - clip().start, 1.0f);
    return t < 0 ? t + len : t;
}

void Character::pose(float alpha) {
    const auto bones = formats::sample_pose(anim_, clip().start + time_ms(alpha));
    std::vector<std::vector<formats::MeshVertex>> posed;
    posed.reserve(mesh_.submeshes.size());
    float top = 0;
    for (const auto& sub : mesh_.submeshes) {
        posed.push_back(formats::skin_vertices(sub, bones));
        for (const auto& v : posed.back()) top = std::max(top, v.pos[2]);
    }
    height_ = top;
    const MeshData data = build_mesh_data(mesh_, posed);
    IRenderer& r = assets_.renderer();
    if (gpu_ && r.update_mesh(gpu_, data)) return;
    r.destroy_mesh(gpu_);
    gpu_ = r.create_mesh(data);
}

void Character::build(const glm::mat4& transform, std::vector<DrawItem>& draws) const {
    for (std::size_t s = 0; s < materials_.size(); ++s)
        if (part_count_[s]) draws.push_back({gpu_, materials_[s], transform, part_first_[s], part_count_[s]});
}

}  // namespace joa::engine
