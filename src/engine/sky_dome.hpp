#pragma once
// Daytime sky: gradient dome (fog colour at the horizon -> blue zenith) with the .day cloud layers
// (CloudTexture1/2) drifting over it, centred on the camera, unfogged.
// HYPOTHESIS throughout: the original recolours model/sky01.smo with its Rayleigh/Mie scattering
// (Resource/Scat.pro, SkyBox.pro; not reverse engineered); colours here are matched by eye to
// work/reference/shots/s04_04_mission1_hud.png and s04_06_zoom_out.png.
#include "engine/assets_cache.hpp"

#include <string>
#include <vector>

namespace joa::engine {

class SkyDome {
public:
    // fog_color: linear, the horizon colour (terrain fades to it). clouds: texture names, may be empty.
    SkyDome(AssetCache& assets, glm::vec3 fog_color, const std::vector<std::string>& clouds);
    ~SkyDome();
    SkyDome(const SkyDome&) = delete;
    SkyDome& operator=(const SkyDome&) = delete;

    // time_s drives the cloud drift; tint (linear) scales dome and clouds for the time of day.
    void build(glm::vec3 camera, float time_s, glm::vec3 tint, std::vector<DrawItem>& draws) const;

    // Inside the in-game far plane (kWorldFar 4096), just beyond the fog end (3996): terrain reaching
    // the dome is fully fogged, so the horizon colour (= fog colour) joins it seamlessly.
    static constexpr float kRadius = 4000.0f;

private:
    IRenderer& renderer_;
    MeshHandle dome_;
    struct Layer { MeshHandle mesh; TextureHandle texture; glm::vec2 drift; };
    std::vector<Layer> layers_;
};

}  // namespace joa::engine
