#include "engine/character_actor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace joa::engine {
namespace {

// Original Z-up -> engine Y-up as a matrix (to_engine in assets_cache.hpp).
const glm::mat4 kToEngine{glm::vec4(1, 0, 0, 0), glm::vec4(0, 0, -1, 0), glm::vec4(0, 1, 0, 0), glm::vec4(0, 0, 0, 1)};

glm::mat4 to_mat4(const formats::Mat3x4& m) {
    glm::mat4 r(1.0f);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) r[col][row] = m[std::size_t(row * 4 + col)];
    return r;
}

// Caster positions in engine axes transformed by `m`, all submeshes concatenated; `bones` empty = static mesh.
std::vector<glm::vec3> caster_positions(const formats::Mesh& mesh, std::span<const formats::Mat3x4> bones, const glm::mat4& m) {
    std::vector<glm::vec3> out;
    for (const auto& sub : mesh.submeshes) {
        const auto verts = bones.empty() ? sub.vertices : formats::skin_vertices(sub, bones);
        for (const formats::MeshVertex& v : verts) out.push_back(glm::vec3(m * glm::vec4(to_engine(v.pos), 1.0f)));
    }
    return out;
}

ShadowCaster make_caster(const formats::Mesh& mesh, std::span<const glm::vec3> positions) {
    std::vector<std::uint32_t> tris;
    std::uint32_t base = 0;
    for (const auto& sub : mesh.submeshes) {
        for (std::uint32_t i : formats::strip_to_triangles(sub.strip)) tris.push_back(base + i);
        base += std::uint32_t(sub.vertices.size());
    }
    return ShadowCaster(positions, tris);
}

}  // namespace

Character::Character(AssetCache& assets, std::string_view am1, std::string_view am2) : assets_(assets) {
    mesh_ = assets.skinned_mesh(am1);
    anim_ = assets.animation(am2);
    if (!mesh_ || !anim_) throw std::runtime_error(std::string(mesh_ ? am2 : am1) + ": missing or malformed");
    const std::string a1(am1);
    const std::string stem = a1.substr(0, a1.rfind('.'));
    skeleton_ = assets.skeleton(stem + ".AM3");
    // Mesh set loader 0x5c2370 also loads base#S (docs/formats/MODELS_AND_ANIMATION.md): the shadow caster.
    shadow_mesh_ = assets.vfs().exists(stem + "#S.AM1") ? assets.skinned_mesh(stem + "#S.AM1") : nullptr;
    if (!shadow_mesh_) shadow_mesh_ = mesh_;
    materials_ = assets.materials(*mesh_);
    // Units are not shadow receivers (stencil reference 0): no self-shadowing, as in s04_04_mission1_hud.png.
    for (Material& m : materials_) m.receive_shadow = false;
    for (const auto& sub : mesh_->submeshes) {
        part_first_.push_back(part_first_.empty() ? 0 : part_first_.back() + part_count_.back());
        part_count_.push_back(std::uint32_t(formats::strip_to_triangles(sub.strip).size()));
    }
    pose(0.0f);
    // Welded in the first pose, as the original welds the posed buffer once per shadow model.
    shadow_caster_ = make_caster(*shadow_mesh_, caster_positions(*shadow_mesh_, bones_, glm::mat4(1.0f)));
}

Character::~Character() {
    assets_.renderer().destroy_mesh(gpu_);
    assets_.renderer().destroy_mesh(shadow_gpu_);
}

bool Character::play(std::string_view am2, std::string_view name) {
    const formats::Animation* anim = assets_.animation(am2);
    if (!anim) return false;
    for (std::size_t i = 0; i < anim->clips.size(); ++i)
        if (lower(anim->clips[i].name) == lower(name)) {
            anim_ = anim, clip_ = i;
            return true;
        }
    return false;
}

void Character::pose(float clip_ms) {
    bones_ = formats::sample_pose(*anim_, clip().start + std::clamp(clip_ms, 0.0f, clip_length()));
    std::vector<std::vector<formats::MeshVertex>> posed;
    posed.reserve(mesh_->submeshes.size());
    float top = 0;
    for (const auto& sub : mesh_->submeshes) {
        posed.push_back(formats::skin_vertices(sub, bones_));
        for (const auto& v : posed.back()) top = std::max(top, v.pos[2]);
    }
    height_ = top;
    const MeshData data = build_mesh_data(*mesh_, posed);
    IRenderer& r = assets_.renderer();
    if (gpu_ && r.update_mesh(gpu_, data)) return;
    r.destroy_mesh(gpu_);
    gpu_ = r.create_mesh(data);
}

bool Character::attach(const Model* model, std::string_view bone, const formats::Mesh* shadow) {
    if (!model || !skeleton_) return false;
    for (std::size_t i = 0; i < skeleton_->nodes.size(); ++i)
        if (skeleton_->nodes[i].present && lower(skeleton_->nodes[i].name) == lower(bone)) {
            attached_.push_back({model, i, shadow, shadow ? make_caster(*shadow, caster_positions(*shadow, {}, glm::mat4(1.0f))) : ShadowCaster{}});
            return true;
        }
    return false;
}

void Character::build(const glm::mat4& transform, std::vector<DrawItem>& draws) const {
    for (std::size_t s = 0; s < materials_.size(); ++s)
        if (part_count_[s]) draws.push_back({gpu_, materials_[s], transform, part_first_[s], part_count_[s]});
    for (const Attached& a : attached_) {
        if (a.bone >= bones_.size()) continue;
        // Item vertices were converted to engine axes at load: undo, apply the bone, redo.
        const glm::mat4 m = transform * kToEngine * to_mat4(bones_[a.bone]) * glm::transpose(kToEngine);
        for (const ModelPart& p : a.model->parts) {
            draws.push_back({a.model->mesh, p.material, m, p.first_index, p.index_count});
            draws.back().material.receive_shadow = false;
        }
    }
}

void Character::build_shadow(const glm::mat4& transform, glm::vec3 sun, ShadowKind kind, std::vector<DrawItem>& draws) {
    if (kind == ShadowKind::Model) {
        if (!shadow_model_) return;
        for (const ModelPart& p : shadow_model_->parts) {
            draws.push_back({shadow_model_->mesh, p.material, transform, p.first_index, p.index_count});
            draws.back().material.blend = BlendMode::ShadowVolume;  // texture alpha cuts the blob (HYPOTHESIS)
            draws.back().material.two_sided = true;
        }
        return;
    }
    // HYPOTHESIS: the original extrudes by shadow-model size (+0x3c + +0x40) + 300; the pose height stands in.
    const float length = height_ + 300.0f;
    MeshData data;
    append_shadow_volume(shadow_caster_, caster_positions(*shadow_mesh_, bones_, transform), sun, length, data);
    for (const Attached& a : attached_)
        if (a.shadow && a.bone < bones_.size())
            append_shadow_volume(a.caster, caster_positions(*a.shadow, {}, transform * kToEngine * to_mat4(bones_[a.bone]) * glm::transpose(kToEngine)),
                                 sun, length, data);
    if (data.indices.empty()) return;
    IRenderer& r = assets_.renderer();
    if (!shadow_gpu_ || !r.update_mesh(shadow_gpu_, data)) {
        r.destroy_mesh(shadow_gpu_);
        shadow_gpu_ = r.create_mesh(data);
    }
    Material m;
    m.blend = BlendMode::ShadowVolume;
    m.unlit = true;
    m.two_sided = true;
    draws.push_back({shadow_gpu_, m, glm::mat4(1.0f)});
}

}  // namespace joa::engine
