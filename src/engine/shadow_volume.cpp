#include "engine/shadow_volume.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace joa::engine {

ShadowCaster::ShadowCaster(std::span<const glm::vec3> bind, std::span<const std::uint32_t> tris) {
    // Equal positions -> lowest index (FUN_005e3040 compares each vertex with all earlier ones; sorted here).
    std::vector<std::uint32_t> order(bind.size()), weld(bind.size());
    std::iota(order.begin(), order.end(), 0u);
    const auto less = [&](std::uint32_t a, std::uint32_t b) {
        const glm::vec3 &p = bind[a], &q = bind[b];
        return p.x != q.x ? p.x < q.x : p.y != q.y ? p.y < q.y : p.z != q.z ? p.z < q.z : a < b;
    };
    std::sort(order.begin(), order.end(), less);
    for (std::size_t i = 0; i < order.size(); ++i)
        weld[order[i]] = i && bind[order[i]] == bind[order[i - 1]] ? weld[order[i - 1]] : order[i];
    for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
        const std::uint32_t a = weld[tris[i]], b = weld[tris[i + 1]], c = weld[tris[i + 2]];
        if (a != b && a != c && b != c) triangles.insert(triangles.end(), {a, b, c});
    }
}

void append_shadow_volume(const ShadowCaster& caster, std::span<const glm::vec3> p, glm::vec3 light, float length, MeshData& out) {
    const glm::vec3 offset = glm::normalize(light) * length;
    std::vector<std::uint64_t> edges;  // directed edges of the lit faces, a << 32 | b
    std::vector<std::uint32_t> slot(p.size(), ~0u);  // caster vertex -> out vertex (its extruded copy follows it)
    const auto vertex = [&](std::uint32_t i) {
        if (slot[i] == ~0u) {
            slot[i] = std::uint32_t(out.vertices.size());
            Vertex v;
            v.position = p[i];
            out.vertices.push_back(v);
            v.position = p[i] + offset;
            out.vertices.push_back(v);
        }
        return slot[i];
    };
    const auto& t = caster.triangles;
    for (std::size_t i = 0; i + 2 < t.size(); i += 3) {
        const std::uint32_t a = t[i], b = t[i + 1], c = t[i + 2];
        // Faces turned towards the light form the near cap (n . L < 0; the original tests < 1e-7).
        if (glm::dot(glm::cross(p[b] - p[a], p[c] - p[a]), light) >= 0.0f) continue;
        out.indices.insert(out.indices.end(), {vertex(a), vertex(b), vertex(c)});
        for (auto [u, v] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) edges.push_back(std::uint64_t(u) << 32 | v);
    }
    std::sort(edges.begin(), edges.end());
    for (const std::uint64_t e : edges) {
        const auto a = std::uint32_t(e >> 32), b = std::uint32_t(e);
        // Silhouette: the neighbouring face across a->b (edge b->a) is not lit. Side quad b, a, a', b'
        // keeps the cap's outward winding (FUN_005e2c80 stores b->a; sides (b, a, b'), (a, a', b')).
        if (std::binary_search(edges.begin(), edges.end(), std::uint64_t(b) << 32 | a)) continue;
        const std::uint32_t sa = slot[a], sb = slot[b];
        out.indices.insert(out.indices.end(), {sb, sa, sb + 1, sa, sa + 1, sb + 1});
    }
}

glm::vec4 shadow_fill_color(glm::vec4 c) {
    return {glm::pow(glm::vec3(c), glm::vec3(2.2f)), 1.0f - std::pow(1.0f - c.a, 2.2f)};
}

}  // namespace joa::engine
