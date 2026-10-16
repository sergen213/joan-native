// Interactive water (port extension, no original counterpart): ripple rings on the remaster-shaded CreateWater
// surfaces (FrameData::ripples, shade_water in shaders/common/pbr_shade.glsl) where units wade, weapons dip in and
// projectiles / items / bodies fall in. Every renderer receives them; `water=original` ignores them.
#include "engine/world_view.hpp"

#include <algorithm>

namespace joa::engine {

std::optional<float> WorldView::water_height(glm::vec3 engine_pos) const {
    std::optional<float> h;
    for (const Node& n : nodes_)
        if (const auto s = n.water->surface_height(engine_pos)) h = std::max(h.value_or(*s), *s);
    return h;
}

void WorldView::ripple(glm::vec3 p, float strength) {
    if (ripples_.size() >= std::size_t(kMaxWaterRipples)) ripples_.erase(ripples_.begin());  // newest kept
    ripples_.push_back({p.x, p.z, time_s_, strength});
}

void WorldView::splash(glm::vec3 p, float strength, float depth) {
    // The CreateWater rectangles run under the banks: only points at or below the surface (2 units of slack for
    // ground lying at the water line), and not deeper than `depth`, are in the water.
    const std::optional<float> h = water_height(p);
    if (!h || p.y > *h + 2.0f || p.y < *h - depth) return;
    ripple(p, strength);
    // script/pscript/jumpintowatersplash_s.txt: a shipped water splash no data or code path references.
    if (strength >= 1.0f) effects_->spawn("jumpintowatersplash_s", {p.x, *h, p.z}, 0.0f, 0.0f, 0.3f);
}

void WorldView::splash_segment(const glm::vec3& a, const glm::vec3& b, float strength) {
    const glm::vec3 ea = to_engine(a.x, a.y, a.z), eb = to_engine(b.x, b.y, b.z);
    const std::optional<float> h = water_height(eb);
    if (h && ea.y > *h && eb.y <= *h) splash(glm::mix(ea, eb, (ea.y - *h) / (ea.y - eb.y)), strength, 0.0f);
}

void WorldView::wade(int id, Actor& a, const glm::mat4& t) {
    const Character& c = *a.character;
    const glm::vec3 feet(t[3]);
    const std::optional<float> h = water_height(feet);
    if (!h || feet.y > *h + c.height()) {  // dry, or above the water out of a weapon's reach (bridges, walls)
        a.tips.clear();
        return;
    }
    // Body: feet in the water, the unit not under it. Moving: a ring every 0.3 s or 35 units, stronger when running;
    // standing: a faint one every 1.2..1.5 s, phased by unit id so a wading crowd does not pulse together.
    if (feet.y <= *h + 2.0f && feet.y > *h - c.height()) {
        const float since = time_s_ - a.ripple_t;
        const bool moving = a.speed > 30.0f;
        if (moving ? since >= 0.3f || glm::distance(glm::vec2(feet.x, feet.z), glm::vec2(a.ripple_p.x, a.ripple_p.z)) >= 35.0f
                   : since >= 1.2f + 0.3f * glm::fract(float(id) * 0.618f)) {
            ripple(feet, moving ? std::clamp(0.2f + a.speed * 0.0008f, 0.25f, 0.5f) : 0.15f);
            a.ripple_t = time_s_, a.ripple_p = feet;
        }
    }
    // Weapons: a ring where a blade / bow tip goes through the surface, and every 40 units it sweeps under it
    // (at most one per tip per 0.15 s: a tip resting at the water line).
    const std::size_t n = c.attachments();
    const bool fresh = a.tips.size() != n;
    a.tips.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        Actor::Tip& tip = a.tips[i];
        const glm::vec3 p = c.attachment_tip(i, t);
        if (fresh) {
            tip = {p, p};
            continue;
        }
        const bool under = p.y < *h, crossed = under != (tip.p.y < *h);
        const bool swept = under && glm::distance(glm::vec2(p.x, p.z), glm::vec2(tip.mark.x, tip.mark.z)) > 40.0f;
        if ((crossed || swept) && time_s_ - tip.at > 0.15f) {
            ripple(crossed ? glm::mix(tip.p, p, (tip.p.y - *h) / (tip.p.y - p.y)) : p, 0.6f);
            tip.mark = p, tip.at = time_s_;
        }
        tip.p = p;
    }
}

}  // namespace joa::engine
