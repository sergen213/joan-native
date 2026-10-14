#pragma once
// Skinned, animated character (AM1 mesh + AM2 clips), CPU-skinned once per rendered frame.
// AM1 influences carry bone-space positions and AM2 keys are model-space matrices, so skinning is
// world = sum(weight * key[bone] * pos) (docs/formats/MODELS_AND_ANIMATION.md); there is no bind pose.
// Equipment meshes (item.res AttachBoneName) ride on a bone: their vertices are in that bone's space.
#include "engine/assets_cache.hpp"
#include "engine/shadow_volume.hpp"
#include "formats/anim_clip.hpp"

#include <string_view>
#include <vector>

namespace joa::engine {

class Character {
public:
    // Throws std::runtime_error if either file is missing or malformed. Bone names come from the
    // AM3 next to the AM1 (unit/<name>.AM3).
    Character(AssetCache& assets, std::string_view am1, std::string_view am2);
    ~Character();
    Character(const Character&) = delete;
    Character& operator=(const Character&) = delete;

    // Switches to a looping clip by name (case-insensitive); unknown names keep the current clip.
    bool play(std::string_view clip);
    const formats::AnimClip& clip() const { return anim_->clips[clip_]; }
    // One fixed simulation tick: advances the clip time (ms, looping).
    void tick(float dt_seconds);
    // Clip-relative time between the last two ticks (alpha = FixedStep::alpha()).
    float time_ms(float alpha) const;
    // Skins at time_ms(alpha) and uploads the vertices. Call before begin_frame.
    void pose(float alpha);

    // Hangs `model` on the skeleton node named `bone` (case-insensitive). False if the bone is unknown.
    // `shadow` (the item's #s.SMO, optional) casts its own volume next to the body's.
    bool attach(const Model* model, std::string_view bone, const formats::Mesh* shadow = nullptr);
    std::size_t attachments() const { return attached_.size(); }
    // Model-space bounds of the current pose (engine axes); valid after pose().
    float height() const { return height_; }
    void build(const glm::mat4& transform, std::vector<DrawItem>& draws) const;
    // unit.res ShadowModel (model/shadow_bitmap.smo: a 40-unit textured quad at the feet), optional.
    void set_shadow_model(const Model* model) { shadow_model_ = model; }
    // Volume: stencil shadow volume of the current pose, the low-poly <am1>#S.AM1 copy (else the full
    // mesh) plus the item #s.SMO casters extruded along `sun` (light travel direction) by height() + 300
    // units; uploads a per-frame mesh, call after pose() and before begin_frame. Model: the ShadowModel
    // quad drawn into the stencil with the unit's transform (0x5f1cc0 path for units without a volume).
    // Both are BlendMode::ShadowVolume draws.
    enum class ShadowKind { Volume, Model };
    void build_shadow(const glm::mat4& transform, glm::vec3 sun, ShadowKind kind, std::vector<DrawItem>& draws);

private:
    AssetCache& assets_;
    const formats::Mesh* mesh_ = nullptr;
    const formats::Animation* anim_ = nullptr;
    const formats::Skeleton* skeleton_ = nullptr;
    const formats::Mesh* shadow_mesh_ = nullptr;
    ShadowCaster shadow_caster_;
    const Model* shadow_model_ = nullptr;
    std::vector<Material> materials_;
    std::vector<std::uint32_t> part_first_, part_count_;
    std::vector<formats::Mat3x4> bones_;  // last pose
    struct Attached {
        const Model* model;
        std::size_t bone;
        const formats::Mesh* shadow;
        ShadowCaster caster;  // welded `shadow` (empty without one)
    };
    std::vector<Attached> attached_;
    MeshHandle gpu_, shadow_gpu_;
    std::size_t clip_ = 0;
    float prev_ms_ = 0, cur_ms_ = 0;
    float height_ = 0;
};

}  // namespace joa::engine
