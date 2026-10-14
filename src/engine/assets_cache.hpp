#pragma once
// Bridge from original assets (joa_formats parsers) to renderer resources, cached by lowercase name.
// Texture lookup and mesh layout: docs/formats/MODELS_AND_ANIMATION.md.
#include "engine/hd_pack.hpp"
#include "engine/renderer.hpp"
#include "formats/anim_clip.hpp"
#include "formats/dds.hpp"
#include "formats/model_mesh.hpp"
#include "formats/vfs_fs.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace joa::engine {

// Original data is Z-up (Joan stands on z = 0). Engine space is right-handed Y-up:
// (x, y, z) -> (x, z, -y), a proper rotation (no mirror). HYPOTHESIS for handedness: the data is
// 3ds Max (right-handed) and the biped's "R" bones must stay on the character's right.
inline glm::vec3 to_engine(float x, float y, float z) { return {x, z, -y}; }
inline glm::vec3 to_engine(const float* p) { return to_engine(p[0], p[1], p[2]); }

struct TextureInfo {
    TextureHandle handle;
    bool has_alpha = false;  // any texel with alpha < 255 (DXT1 1-bit, DXT3/5, A8R8G8B8, ...)
};

struct ModelPart {
    std::uint32_t first_index = 0, index_count = 0;
    Material material;
};

struct Model {
    MeshHandle mesh;
    std::vector<ModelPart> parts;
    glm::vec3 center{0.0f};  // bounding sphere, engine space
    float radius = 0.0f;
};

// Builds renderer geometry for one parsed mesh. vertices[s] overrides submesh s positions/normals
// (posed AM1); empty = use the file's vertices (SMO).
MeshData build_mesh_data(const formats::Mesh& mesh, std::span<const std::vector<formats::MeshVertex>> posed = {});

class AssetCache {
public:
    // hd_packs: resolved HD pack directories (resolve_hd_packs), first wins; docs/HD_ASSETS.md.
    AssetCache(const formats::Vfs& vfs, IRenderer& renderer, std::vector<std::filesystem::path> hd_packs = {});
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    const formats::Vfs& vfs() const { return vfs_; }
    IRenderer& renderer() { return renderer_; }

    // Engine texture rule: drop the extension, match the base name case-insensitively in
    // INT8X6\, INT8X6\ITEM\, SAVE\, TEXTURE\ and TEXTURE\{ITEM,UNIT,TERRAIN,PARTICLE,MISC}\ (first wins).
    // ponytail: .DDS only (every model reference that resolves at all has a .dds); TGA/BMP-only names count as missing.
    // An HD pack file at the same relative path (.dds/.png) replaces the pixels; companions and the
    // material class go to IRenderer::set_material_maps for every loaded texture.
    const TextureInfo* texture(std::string_view name, bool repeat = true);
    // Decoded base level (RGBA8) of a texture found by the same rule (HD override first); empty if missing.
    formats::DdsImage texture_pixels(std::string_view name) const;
    std::filesystem::path texture_path(std::string_view name) const;  // original file
    std::size_t hd_overrides() const { return hd_overrides_; }  // textures loaded from an HD pack

    // Static model (.SMO), or .AM1 posed with `anim_am2` at time 0 (static props such as boxes).
    // Returns nullptr when the file is missing or malformed. Cached by lowercase path.
    const Model* model(std::string_view game_path, std::string_view anim_am2 = {});
    // Parsed .AM1 / .AM2 / .AM3, shared by every character using them; nullptr if missing or malformed.
    const formats::Mesh* skinned_mesh(std::string_view game_path);
    const formats::Mesh* static_mesh(std::string_view game_path);  // .SMO, CPU-side (shadow casters)
    const formats::Animation* animation(std::string_view game_path);
    const formats::Skeleton* skeleton(std::string_view game_path);
    // Materials for a parsed mesh (textures resolved through the cache).
    std::vector<Material> materials(const formats::Mesh& mesh);

    std::size_t missing_textures() const { return missing_textures_.size(); }
    const std::vector<std::string>& missing_texture_names() const { return missing_textures_; }

private:
    void index_textures() const;
    // Decodes the original file's HD override (falls back to the original if unreadable), else the original.
    bool load_texture(const std::filesystem::path& original, formats::DdsTexture& dds, std::uint32_t max_mips, HdOverride* hd) const;
    TextureHandle companion(const std::filesystem::path& file, bool repeat);

    const formats::Vfs& vfs_;
    IRenderer& renderer_;
    HdPacks hd_;
    std::size_t hd_overrides_ = 0;
    std::vector<TextureHandle> companions_;
    mutable std::unordered_map<std::string, std::filesystem::path> texture_index_;  // lowercase stem -> file
    mutable bool indexed_ = false;
    std::unordered_map<std::string, std::unique_ptr<TextureInfo>> textures_;
    std::unordered_map<std::string, std::unique_ptr<Model>> models_;  // value null = failed load
    std::vector<std::string> missing_textures_;
    std::unordered_map<std::string, std::unique_ptr<formats::Mesh>> meshes_, static_meshes_;
    std::unordered_map<std::string, std::unique_ptr<formats::Animation>> anims_;
    std::unordered_map<std::string, std::unique_ptr<formats::Skeleton>> skeletons_;
};

std::string lower(std::string_view s);

}  // namespace joa::engine
