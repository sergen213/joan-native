// Campaign-bot mission plan registry (see autopilot_plan.hpp).
#include "game/autopilot_plan.hpp"

#include "game/autopilot_player.hpp"

namespace joa::game {

float MissionPlan::foe_weight(Autopilot& ap, const Unit&, const Unit& u) {
    const auto& c = ap.mission().data().units[std::size_t(u.cls)];
    return c.unit_class == 8 || c.special == 2 ? 0.0f : 1.0f;  // siege weapons, horses
}

#define JOA_PLAN(s) std::unique_ptr<MissionPlan> make_plan_##s();
#include "autopilot_plans.inc"
#undef JOA_PLAN

std::unique_ptr<MissionPlan> make_plan(std::string_view stem) {
#define JOA_PLAN(s) \
    if (stem == #s) return make_plan_##s();
#include "autopilot_plans.inc"
#undef JOA_PLAN
    (void)stem;
    return nullptr;
}

}  // namespace joa::game
