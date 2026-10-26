#pragma once
// Renderer that draws nothing: headless sessions (--headless) and tests run the real UI/world code
// paths (texture/mesh creation, draw-list building) without a GPU.
#include "engine/renderer.hpp"

namespace joa::engine {

class NullRenderer final : public IRenderer {
public:
    const char* name() const override { return "null"; }
    MeshHandle create_mesh(const MeshData&) override { return {++next_}; }
    void destroy_mesh(MeshHandle) override {}
    bool update_mesh(MeshHandle, const MeshData&) override { return true; }
    TextureHandle create_texture(const TextureData&) override { return {++next_}; }
    void destroy_texture(TextureHandle) override {}
    bool update_texture(TextureHandle, const TextureData&) override { return true; }
    void begin_frame(glm::ivec2 size) override { size_ = size; }
    void submit(const FrameData&) override {}
    void end_frame() override {}
    Image read_back() override { return {size_.x, size_.y, std::vector<std::uint8_t>(std::size_t(size_.x) * size_.y * 4)}; }

private:
    std::uint32_t next_ = 0;
    glm::ivec2 size_{0};
};

}  // namespace joa::engine
