#pragma once
// .stg CreateWater surfaces as the original draws them with the default video settings
// (Reflection = Modulate, Bump Map Detail = Yes). RE: CreateWater parser FUN_004f5210, texteff.res
// material stage set-up FUN_005fa360, layer frame animation in the mesh draw FUN_005d0570.
// docs/reverse-engineering/RENDERING_PIPELINE.md section 5.
#include "engine/assets_cache.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace joa::engine {

class WaterField {
public:
    // Reads the scenario's .stg CreateWater blocks (see find_scenario), placed at the map node `origin`
    // (original X/Y). Throws if the .stg is missing.
    WaterField(AssetCache& assets, std::string_view scenario, glm::vec2 origin = glm::vec2(0.0f));
    ~WaterField();
    WaterField(const WaterField&) = delete;
    WaterField& operator=(const WaterField&) = delete;

    int count() const { return int(surfaces_.size()); }  // CreateWater blocks
    int bump_count() const;                              // surfaces drawn with the EMBM texteff material
    int bump_frames() const { return int(bump_frames_.size()); }  // loaded height-map frames (Lake1..65)

    // Animates the bump frame (time_s) and the per-vertex reflection coordinates (eye, engine space).
    void build(float time_s, glm::vec3 eye, std::vector<DrawItem>& draws);
    // Engine-space height (Y) of the remaster-shaded surfaces (not ice) over engine position p's x/z, the highest
    // where several overlap; nullopt off the water (interactive ripples, world_view_water.cpp).
    std::optional<float> surface_height(glm::vec3 p) const;

private:
    struct Surface {
        MeshData mesh;  // grid relative to the surface centre
        MeshHandle handle;
        DrawItem draw;
        glm::vec3 centre{0.0f};  // original coordinates (Z up)
        glm::vec2 half{0.0f};    // WIDTH / 2, LENGTH / 2
        bool embm = false;
    };
    AssetCache& assets_;
    std::vector<Surface> surfaces_;
    std::vector<TextureHandle> bump_frames_;
    float frame_ms_ = 30.0f;
    TextureHandle pool_;
    TextureHandle normal_;  // remaster water normal map + height (water_normal_map of Lake1.bmp)
};

}  // namespace joa::engine
