#pragma once
// Skinned, animated character (AM1 mesh + AM2 clips), CPU-skinned once per rendered frame.
// AM1 influences carry bone-space positions and AM2 keys are model-space matrices, so skinning is
// world = sum(weight * key[bone] * pos) (docs/formats/MODELS_AND_ANIMATION.md); there is no bind pose.
#include "engine/assets_cache.hpp"
#include "formats/anim_clip.hpp"

#include <string_view>
#include <vector>

namespace joa::engine {

class Character {
public:
    // Throws std::runtime_error if either file is missing or malformed.
    Character(AssetCache& assets, std::string_view am1, std::string_view am2);
    ~Character();
    Character(const Character&) = delete;
    Character& operator=(const Character&) = delete;

    // Switches to a looping clip by name (case-insensitive); unknown names keep the current clip.
    bool play(std::string_view clip);
    const formats::AnimClip& clip() const { return anim_.clips[clip_]; }
    // One fixed simulation tick: advances the clip time (ms, looping).
    void tick(float dt_seconds);
    // Clip-relative time between the last two ticks (alpha = FixedStep::alpha()).
    float time_ms(float alpha) const;
    // Skins at time_ms(alpha) and uploads the vertices. Call before begin_frame.
    void pose(float alpha);

    // Model-space bounds of the current pose (engine axes); valid after pose().
    float height() const { return height_; }
    void build(const glm::mat4& transform, std::vector<DrawItem>& draws) const;

private:
    AssetCache& assets_;
    formats::Mesh mesh_;
    formats::Animation anim_;
    std::vector<Material> materials_;
    std::vector<std::uint32_t> part_first_, part_count_;
    MeshHandle gpu_;
    std::size_t clip_ = 0;
    float prev_ms_ = 0, cur_ms_ = 0;
    float height_ = 0;
};

}  // namespace joa::engine
