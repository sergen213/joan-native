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
    // (directed edge, triangle) sorted by edge: each edge's reverse is one equal_range away.
    const std::size_t n = triangles.size() / 3;
    const auto key = [&](std::size_t e) {
        return std::uint64_t(triangles[e]) << 32 | triangles[e / 3 * 3 + (e % 3 + 1) % 3];
    };
    std::vector<std::pair<std::uint64_t, std::uint32_t>> by_edge(n * 3);
    for (std::size_t e = 0; e < n * 3; ++e) by_edge[e] = {key(e), std::uint32_t(e / 3)};
    std::sort(by_edge.begin(), by_edge.end());
    twin_first.reserve(n * 3 + 1);
    for (std::size_t e = 0; e < n * 3; ++e) {
        twin_first.push_back(std::uint32_t(twins.size()));
        const std::uint64_t k = key(e), rev = k << 32 | k >> 32;
        for (auto it = std::lower_bound(by_edge.begin(), by_edge.end(), std::pair{rev, 0u}); it != by_edge.end() && it->first == rev; ++it)
            twins.push_back(it->second);
    }
    twin_first.push_back(std::uint32_t(twins.size()));
}

void append_shadow_volume(const ShadowCaster& caster, std::span<const glm::vec3> p, glm::vec3 light, float length, MeshData& out) {
    const glm::vec3 offset = glm::normalize(light) * length;
    // Per-call scratch kept across calls (every visible unit, every frame).
    thread_local std::vector<std::uint32_t> slot;  // caster vertex -> out vertex (its extruded copy follows it)
    thread_local std::vector<std::uint8_t> lit;
    slot.assign(p.size(), ~0u);
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
    const std::size_t n = t.size() / 3;
    // Faces turned towards the light form the near cap (n . L < 0; the original tests < 1e-7).
    lit.resize(n);
    for (std::size_t f = 0; f < n; ++f) {
        const glm::vec3 &a = p[t[3 * f]], &b = p[t[3 * f + 1]], &c = p[t[3 * f + 2]];
        lit[f] = glm::dot(glm::cross(b - a, c - a), light) < 0.0f;
    }
    for (std::size_t f = 0; f < n; ++f) {
        if (!lit[f]) continue;
        out.indices.insert(out.indices.end(), {vertex(t[3 * f]), vertex(t[3 * f + 1]), vertex(t[3 * f + 2])});
    }
    for (std::size_t f = 0; f < n; ++f) {
        if (!lit[f]) continue;
        for (std::size_t k = 0; k < 3; ++k) {
            // Silhouette: no lit face holds the reverse edge b->a. Side quad b, a, a', b' keeps the cap's outward
            // winding (FUN_005e2c80 stores b->a; sides (b, a, b'), (a, a', b')).
            const std::size_t e = 3 * f + k;
            bool covered = false;
            for (std::uint32_t i = caster.twin_first[e]; i < caster.twin_first[e + 1] && !covered; ++i) covered = lit[caster.twins[i]];
            if (covered) continue;
            const std::uint32_t sa = slot[t[e]], sb = slot[t[3 * f + (k + 1) % 3]];
            out.indices.insert(out.indices.end(), {sb, sa, sb + 1, sa, sa + 1, sb + 1});
        }
    }
}

glm::vec4 shadow_fill_color(glm::vec4 c) {
    return {glm::pow(glm::vec3(c), glm::vec3(2.2f)), 1.0f - std::pow(1.0f - c.a, 2.2f)};
}

}  // namespace joa::engine
