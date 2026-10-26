#include "engine/grass_field.hpp"

#include "engine/world_scene.hpp"

#include "formats/config_dat.hpp"
#include "formats/res_archive.hpp"
#include "formats/world_text.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <climits>
#include <cmath>

namespace joa::engine {
namespace {

constexpr int kPatchCells = 3;  // FUN_00464740: patches step 3 cells (192 units)
constexpr float kOpaqueDistance = 100.0f;

std::uint32_t hash(std::uint32_t x) {  // lowbias32
    x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
    return x;
}

// Plasma_sample_tri: the quad diagonal alternates with (x + y) parity like the terrain mesh.
// Returns the height and the bilinear slope (height per unit) along x and y.
glm::vec3 sample(const formats::TerrainGrid& g, float wx, float wy) {
    const float cs = formats::TerrainGrid::kCellSize;
    const float fx = std::clamp(wx / cs, 0.0f, float(g.cells_x)), fy = std::clamp(wy / cs, 0.0f, float(g.cells_y));
    const int x = std::min(int(fx), g.cells_x - 1), y = std::min(int(fy), g.cells_y - 1);
    const float tx = fx - x, ty = fy - y;
    const float h00 = g.height(x, y), h10 = g.height(x + 1, y), h01 = g.height(x, y + 1), h11 = g.height(x + 1, y + 1);
    float h;
    if (((x + y) & 1) == 0)  // diagonal (0,0)-(1,1)
        h = tx > ty ? h00 + (h10 - h00) * tx + (h11 - h10) * ty : h00 + (h11 - h01) * tx + (h01 - h00) * ty;
    else  // diagonal (1,0)-(0,1)
        h = tx + ty <= 1 ? h00 + (h10 - h00) * tx + (h01 - h00) * ty : h11 + (h01 - h11) * (1 - tx) + (h10 - h11) * (1 - ty);
    return {h, ((h10 - h00) * (1 - ty) + (h11 - h01) * ty) / cs, ((h01 - h00) * (1 - tx) + (h11 - h10) * tx) / cs};
}

}  // namespace

GrassField::GrassField(AssetCache& assets, const formats::TerrainGrid& grid, int scenario)
    : assets_(assets), grid_(grid), scenario_(scenario) {
    if (auto bytes = assets.vfs().read("CONFIG.DAT"))
        if (const auto cfg = formats::parse_config_dat(*bytes)) {
            enabled_ = cfg->dynamic_grass != 0 && cfg->grass_distance > 0;
            radius_ = float(std::max(100, cfg->grass_distance));  // FUN_004641a0 clamps to >= 100
            density_ = cfg->grass_density;
        }
    auto bytes = assets.vfs().read("item/DynGrass.res");
    const auto res = bytes ? formats::ResArchive::parse(std::move(*bytes)) : std::nullopt;
    if (!res) return;
    for (const formats::ResEntry& e : res->entries()) {
        const auto text = res->text(e.name);
        if (!text) continue;
        std::vector<Kind> kinds;
        // "Object\...\grass01_M  : 120 : 100 : -1 : 30" -> key = item path, value = count : percent : yaw : tilt.
        for (const auto& kv : formats::parse_kv_text(*text)) {
            if (kv.key.find('\\') == std::string::npos) continue;
            Kind k;
            float yaw = -1, tilt = 0;
            if (std::sscanf(kv.value.c_str(), "%d : %d : %f : %f", &k.count, &k.percent, &yaw, &tilt) < 2) continue;
            k.yaw = glm::radians(yaw), k.tilt = glm::radians(tilt);
            const std::string path = "item/" + kv.key + ".SMO";
            const formats::Mesh* mesh = assets.vfs().exists(path) ? assets.static_mesh(path) : nullptr;
            if (!mesh) continue;
            k.mesh = build_mesh_data(*mesh);
            k.materials = assets.materials(*mesh);
            std::uint32_t first = 0;
            for (const auto& sub : mesh->submeshes) {
                const auto n = std::uint32_t(formats::strip_to_triangles(sub.strip).size());
                k.submeshes.emplace_back(first, n);
                first += n;
            }
            kinds.push_back(std::move(k));
        }
        if (!kinds.empty()) kinds_[lower(e.name)] = std::move(kinds);
    }
}

GrassField::~GrassField() {
    for (const auto& [key, p] : patches_)
        if (p.mesh) assets_.renderer().destroy_mesh(p.mesh);
}

std::size_t GrassField::tufts() const {
    std::size_t n = 0;
    for (const auto& [key, p] : patches_) n += p.tufts;
    return n;
}

// FUN_00465a20: the ground texture covering at least half of the patch's cells, then its
// DynGrass.res record "<texture>_M<scenario>" or "<texture>".
const std::vector<GrassField::Kind>* GrassField::kinds_at(int px, int py) const {
    std::vector<int> hist(grid_.textures.size(), 0);
    int total = 0;
    for (int x = px; x < std::min(px + kPatchCells, grid_.cells_x); ++x)
        for (int y = py; y < std::min(py + kPatchCells, grid_.cells_y); ++y) {
            ++total;
            // Level_load_terrain cell texture: invalid cells keep 0; tex2 when the corner blend mean < 0.5.
            int t = 0;
            if (grid_.valid(x, y)) {
                const int blend = grid_.blend(x, y) + grid_.blend(x + 1, y) + grid_.blend(x, y + 1) + grid_.blend(x + 1, y + 1);
                t = (grid_.tex2(x, y) != 0 && blend / 1020.0f < 0.5f ? grid_.tex2(x, y) : grid_.tex1(x, y)) - 1;
            }
            if (t >= 0 && t < int(hist.size())) ++hist[std::size_t(t)];
        }
    const auto best = std::max_element(hist.begin(), hist.end());  // first maximum, as the original
    if (best == hist.end() || *best == 0 || *best < total / 2) return nullptr;
    const std::string name = lower(grid_.textures[std::size_t(best - hist.begin())].name);
    auto it = kinds_.find(name + "_m" + std::to_string(scenario_));
    if (it == kinds_.end()) it = kinds_.find(name);
    return it == kinds_.end() ? nullptr : &it->second;
}

GrassField::Patch GrassField::generate(int px, int py) const {
    Patch p;
    const float cs = formats::TerrainGrid::kCellSize, size = kPatchCells * cs;
    const float x0 = float(px) * cs, y0 = float(py) * cs;
    p.centre = to_engine(x0 + size / 2, y0 + size / 2, sample(grid_, x0 + size / 2, y0 + size / 2).x);
    const std::vector<Kind>* kinds = kinds_at(px, py);
    if (!kinds) return p;
    // ponytail: hashed per patch instead of the original's global Misc_random, so a patch regrows identically.
    std::uint32_t h = hash(std::uint32_t(px) * 73856093U ^ std::uint32_t(py) * 19349663U);
    const auto rnd = [&h](int n) { return int((h = hash(h + 0x9e3779b9U)) % std::uint32_t(n)); };
    struct Instance { const Kind* kind; glm::mat4 m; };
    std::vector<Instance> placed;
    std::size_t verts = 0, indices = 0;
    for (const Kind& k : *kinds) {
        if (rnd(100) >= k.percent) continue;
        const int n = std::max(1, k.count * density_ / 100);
        for (int i = 0; i < n; ++i) {
            float yaw = 0, tilt = 0;
            if (k.yaw >= 0) {
                yaw = k.yaw + glm::radians(float(rnd(10) - 20));
                if (k.tilt != 0) tilt = k.tilt + glm::radians(float(rnd(5) - 10));
            } else if (k.tilt == 0) {
                yaw = float(rnd(32)) * glm::two_pi<float>() / 32.0f;
            } else {
                tilt = k.tilt + glm::radians(float(rnd(5) - 10));
                yaw = glm::radians(float(rnd(360)));
            }
            const float x = x0 + float(rnd(int(size))), y = y0 + float(rnd(int(size)));
            if (x / cs > float(grid_.cells_x) || y / cs > float(grid_.cells_y)) continue;
            glm::vec3 s = sample(grid_, x, y);
            if (std::abs(s.y) >= 0.4f || std::abs(s.z) >= 0.4f) continue;  // too steep
            if (std::abs(s.y) >= 0.3f) s.x -= 10.0f;  // the original tests only dh/dx here
            if (verts > 3275 || indices > 13106) continue;  // FUN_00466560 patch mesh limits
            glm::mat4 m = glm::translate(glm::mat4(1.0f), to_engine(x, y, s.x) - p.centre);
            m = glm::rotate(m, yaw, glm::vec3(0, 1, 0));
            m = glm::rotate(m, tilt, glm::vec3(0, 0, 1));  // the tuft's own original Y axis
            placed.push_back({&k, m});
            verts += k.mesh.vertices.size(), indices += k.mesh.indices.size();
        }
    }
    if (placed.empty()) return p;
    // One mesh per patch: instances baked relative to the centre, one draw per kind submesh.
    MeshData all;
    std::vector<std::uint32_t> base;
    for (const Instance& in : placed) {
        base.push_back(std::uint32_t(all.vertices.size()));
        const glm::mat3 nm(in.m);
        for (Vertex v : in.kind->mesh.vertices) {
            v.position = glm::vec3(in.m * glm::vec4(v.position, 1.0f));
            v.normal = glm::normalize(nm * v.normal);
            p.radius = std::max(p.radius, glm::length(v.position));
            all.vertices.push_back(v);
        }
    }
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), p.centre);
    for (const Kind& k : *kinds)
        for (std::size_t s = 0; s < k.submeshes.size() && s < k.materials.size(); ++s) {
            const auto first = std::uint32_t(all.indices.size());
            for (std::size_t i = 0; i < placed.size(); ++i)
                if (placed[i].kind == &k)
                    for (std::uint32_t j = 0; j < k.submeshes[s].second; ++j)
                        all.indices.push_back(base[i] + k.mesh.indices[k.submeshes[s].first + j]);
            if (all.indices.size() > first) p.draws.push_back({{}, k.materials[s], model, first, std::uint32_t(all.indices.size()) - first});
        }
    p.mesh = assets_.renderer().create_mesh(all);
    for (DrawItem& d : p.draws) d.mesh = p.mesh;
    p.tufts = placed.size();
    return p;
}

void GrassField::build(glm::vec3 camera, const std::array<glm::vec4, 6>& planes, glm::vec3 to_ground, std::vector<DrawItem>& draws) {
    visible_ = 0;
    if (!enabled_ || kinds_.empty() || grid_.cells_x <= 0 || grid_.cells_y <= 0) return;
    // Original x/y of the camera eye (engine (x, z) = original (x, -y)).
    const float cx = camera.x, cy = -camera.z, size = kPatchCells * formats::TerrainGrid::kCellSize;
    const int x0 = std::max(0, int(std::floor((cx - radius_) / size)) * kPatchCells);
    const int x1 = std::min(grid_.cells_x - 1, int(std::floor((cx + radius_) / size)) * kPatchCells);
    const int y0 = std::max(0, int(std::floor((cy - radius_) / size)) * kPatchCells);
    const int y1 = std::min(grid_.cells_y - 1, int(std::floor((cy + radius_) / size)) * kPatchCells);
    for (auto it = patches_.begin(); it != patches_.end();) {
        const auto [px, py] = it->first;
        if (px >= x0 && px <= x1 && py >= y0 && py <= y1) {
            ++it;
            continue;
        }
        if (it->second.mesh) assets_.renderer().destroy_mesh(it->second.mesh);
        it = patches_.erase(it);
    }
    int budget = patches_.size() <= 1 ? INT_MAX : 1;  // FUN_00464740: one new patch per frame once filled
    for (int px = x0; px <= x1 && budget > 0; px += kPatchCells)
        for (int py = y0; py <= y1 && budget > 0; py += kPatchCells)
            if (!patches_.contains({px, py})) {
                patches_.emplace(std::pair{px, py}, generate(px, py));
                --budget;
            }
    // "change grass alpha": opaque within 100 units, fading to alpha 127 at the radius, hidden beyond.
    for (const auto& [key, p] : patches_) {
        if (!p.mesh) continue;
        const float d = glm::distance(glm::vec2(p.centre.x, -p.centre.z), glm::vec2(cx, cy));
        if (d > radius_) continue;
        // Frustum cull (as WorldView's units): no tuft rises more than the sphere's diameter above the ground it
        // shades, so a shadow lands at most 2r / sin(elevation) away along the sun (low sun capped like the units').
        const float reach = 2.0f * p.radius / std::max(-to_ground.y, 0.25f);
        if (!sphere_visible(planes, p.centre + to_ground * (reach * 0.5f), p.radius + reach * 0.5f)) continue;
        ++visible_;
        if (d <= kOpaqueDistance) {
            draws.insert(draws.end(), p.draws.begin(), p.draws.end());
            continue;
        }
        const float alpha = float(int(255.0f - 128.0f * (d - kOpaqueDistance) / (radius_ - kOpaqueDistance))) / 255.0f;
        for (DrawItem dr : p.draws) {
            dr.material.blend = BlendMode::AlphaBlend;
            dr.material.tint.a *= alpha;
            draws.push_back(dr);
        }
    }
}

}  // namespace joa::engine
