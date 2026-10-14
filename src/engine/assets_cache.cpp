#include "engine/assets_cache.hpp"

#include "formats/anim_clip.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace joa::engine {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

namespace {

// Base name without directory and extension, lowercase ("Joan.tga" -> "joan").
std::string texture_key(std::string_view name) {
    if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) name.remove_prefix(slash + 1);
    if (const auto dot = name.rfind('.'); dot != std::string_view::npos) name = name.substr(0, dot);
    return lower(name);
}

}  // namespace

MeshData build_mesh_data(const formats::Mesh& mesh, std::span<const std::vector<formats::MeshVertex>> posed) {
    MeshData out;
    for (std::size_t s = 0; s < mesh.submeshes.size(); ++s) {
        const formats::Submesh& sub = mesh.submeshes[s];
        const auto& verts = s < posed.size() && !posed[s].empty() ? posed[s] : sub.vertices;
        const auto base = std::uint32_t(out.vertices.size());
        for (const formats::MeshVertex& v : verts) {
            Vertex o;
            o.position = to_engine(v.pos);
            // Tag 3: the engine replaces every normal by one constant (DAT_00a18a60, value UNKNOWN);
            // HYPOTHESIS: straight up, so vegetation lights like the ground.
            o.normal = mesh.constantNormals ? glm::vec3(0, 1, 0) : to_engine(v.normal);
            o.uv = {v.uv[0], v.uv[1]};
            out.vertices.push_back(o);
        }
        for (std::uint32_t i : formats::strip_to_triangles(sub.strip)) out.indices.push_back(base + i);
    }
    return out;
}

AssetCache::AssetCache(const formats::Vfs& vfs, IRenderer& renderer) : vfs_(vfs), renderer_(renderer) {}

AssetCache::~AssetCache() {
    for (auto& [k, t] : textures_)
        if (t) renderer_.destroy_texture(t->handle);
    for (auto& [k, m] : models_)
        if (m) renderer_.destroy_mesh(m->mesh);
}

void AssetCache::index_textures() const {
    if (indexed_) return;
    indexed_ = true;
    static const char* const dirs[] = {"int8x6",       "int8x6/item",  "save",           "texture",      "texture/item",
                                       "texture/unit", "texture/terrain", "texture/particle", "texture/misc"};
    for (const char* d : dirs) {
        const auto dir = vfs_.resolve(d);
        if (!dir) continue;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(*dir, ec)) {
            if (!e.is_regular_file() || lower(e.path().extension().string()) != ".dds") continue;
            texture_index_.try_emplace(lower(e.path().stem().string()), e.path());
        }
    }
}

std::filesystem::path AssetCache::texture_path(std::string_view name) const {
    index_textures();
    const auto it = texture_index_.find(texture_key(name));
    return it == texture_index_.end() ? std::filesystem::path{} : it->second;
}

formats::DdsImage AssetCache::texture_pixels(std::string_view name) const {
    const auto path = texture_path(name);
    if (path.empty()) return {};
    const auto bytes = formats::read_file(path);
    formats::DdsTexture dds;
    std::string err;
    if (!bytes || !formats::parse_dds(*bytes, dds, err, 1) || dds.images.empty()) return {};
    return std::move(dds.images[0]);
}

const TextureInfo* AssetCache::texture(std::string_view name, bool repeat) {
    const std::string key = texture_key(name) + (repeat ? "" : "#clamp");
    if (auto it = textures_.find(key); it != textures_.end()) return it->second.get();
    auto& slot = textures_[key];
    formats::DdsImage img = texture_pixels(name);
    if (img.rgba.empty()) {
        missing_textures_.push_back(std::string(name));
        return nullptr;
    }
    auto info = std::make_unique<TextureInfo>();
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] != 255) {
            info->has_alpha = true;
            break;
        }
    TextureData t;
    t.width = int(img.width);
    t.height = int(img.height);
    t.pixels = std::move(img.rgba);
    t.repeat = repeat;
    // ponytail: GPU-generated mips instead of the file's chain (same box-filtered content in shipped DDS).
    info->handle = renderer_.create_texture(t);
    slot = std::move(info);
    return slot.get();
}

std::vector<Material> AssetCache::materials(const formats::Mesh& mesh) {
    std::vector<Material> out;
    for (const formats::Submesh& sub : mesh.submeshes) {
        Material m;
        const TextureInfo* tex = nullptr;
        if (sub.texture >= 0 && std::size_t(sub.texture) < mesh.textures.size()) tex = texture(mesh.textures[sub.texture].name);
        if (tex) m.albedo = tex->handle;
        // Alpha-carrying textures (foliage, hair, fences) are alpha tested; trailer tag 1 blend modes
        // are UNKNOWN per value, so they are not mapped to blending (no sorting artefacts).
        if (tex && tex->has_alpha) m.blend = BlendMode::AlphaTest;
        // Trailer tag 2 flag -> material flags low nibble 2: HYPOTHESIS two-sided.
        m.two_sided = sub.tag2Flag != 0 || m.blend == BlendMode::AlphaTest;
        out.push_back(m);
    }
    return out;
}

const Model* AssetCache::model(std::string_view game_path, std::string_view anim_am2) {
    const std::string key = lower(game_path);
    if (auto it = models_.find(key); it != models_.end()) return it->second.get();
    auto& slot = models_[key];
    const auto bytes = vfs_.read(game_path);
    if (!bytes) return nullptr;
    const bool skinned = key.size() > 4 && key.ends_with(".am1");
    formats::Mesh mesh;
    std::string err;
    if (!formats::parse_mesh(*bytes, skinned, mesh, err)) {
        std::fprintf(stderr, "joan-native: %.*s: %s\n", int(game_path.size()), game_path.data(), err.c_str());
        return nullptr;
    }
    std::vector<std::vector<formats::MeshVertex>> posed;
    if (skinned) {
        // AM1 positions live in bone space: without an animation there is no pose to show.
        const auto anim_bytes = anim_am2.empty() ? std::nullopt : vfs_.read(anim_am2);
        formats::Animation anim;
        if (!anim_bytes || !formats::parse_animation(*anim_bytes, anim, err)) return nullptr;
        const auto pose = formats::sample_pose(anim, anim.clips.empty() ? anim.start : anim.clips.front().start);
        for (const auto& sub : mesh.submeshes) posed.push_back(formats::skin_vertices(sub, pose));
    }
    const MeshData data = build_mesh_data(mesh, posed);
    if (data.indices.empty()) return nullptr;

    auto m = std::make_unique<Model>();
    m->mesh = renderer_.create_mesh(data);
    const auto mats = materials(mesh);
    std::uint32_t first = 0;
    for (std::size_t s = 0; s < mesh.submeshes.size(); ++s) {
        const auto n = std::uint32_t(formats::strip_to_triangles(mesh.submeshes[s].strip).size());
        if (n) m->parts.push_back({first, n, mats[s]});
        first += n;
    }
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const Vertex& v : data.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    m->center = (lo + hi) * 0.5f;
    for (const Vertex& v : data.vertices) m->radius = std::max(m->radius, glm::length(v.position - m->center));
    slot = std::move(m);
    return slot.get();
}

}  // namespace joa::engine
