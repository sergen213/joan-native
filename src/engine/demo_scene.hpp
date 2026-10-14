#pragma once
// Procedural placeholder scene (ground, cubes, UI anchors) used until original assets load.
// Real content enters through the same seam: build a MeshData/TextureData and hand it to
// IRenderer::create_mesh / create_texture.
#include "engine/camera.hpp"
#include "engine/renderer.hpp"
#include "engine/sim.hpp"

#include <vector>

namespace joa::engine {

MeshData make_box_mesh(glm::vec3 half_extents);
MeshData make_ground_mesh(float half_size, float uv_repeat);
TextureData make_checker_texture(int size, int cells, glm::u8vec4 a, glm::u8vec4 b);

class DemoScene {
public:
    explicit DemoScene(IRenderer& renderer);
    ~DemoScene();
    DemoScene(const DemoScene&) = delete;
    DemoScene& operator=(const DemoScene&) = delete;

    Lighting lighting() const;
    void build(const World& world, const UiLayout& ui, std::vector<DrawItem>& draws, std::vector<UiQuad>& quads) const;

private:
    IRenderer& renderer_;
    MeshHandle ground_, box_;
    TextureHandle checker_, crate_;
};

}  // namespace joa::engine
