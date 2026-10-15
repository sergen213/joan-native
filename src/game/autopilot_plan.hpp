// Per-mission strategy for the campaign bot (test / diagnostic only, see autopilot_player.hpp). The generic bot
// (Autopilot::decide) calls these hooks; a plan returns true from a decision hook when it set this tick's input (and
// ap.goal), which skips the generic logic after that point. Plans use the Autopilot's public helpers (travel, fight,
// shoot, region, ...) and read the mission through ap.mission().
//
// Adding a plan: src/game/autopilot_plan_<stem>.cpp (stem = Mission::info().stem(), e.g. S5L1) defining
//   std::unique_ptr<MissionPlan> make_plan_<stem>();
// CMake globs the file into the registry (make_plan); no other file changes.
#pragma once

#include <memory>
#include <string_view>

#include "game/campaign_runner.hpp"

namespace joa::game {

class Autopilot;

struct MissionPlan {
    virtual ~MissionPlan() = default;
    // Each tick before the generic decisions, after the always-on preamble (buy screen, character screen, other heroes'
    // points, food, ladder rungs). p is the player's hero.
    virtual bool first(Autopilot&, InputFrame&, Unit&) { return false; }
    // Before the enemy selection (after the interact prompt, way point and gate-closing logic).
    virtual bool before_targets(Autopilot&, InputFrame&, Unit&) { return false; }
    // Nothing left to fight, talk to or walk to a marker for: before the objective-territory walk and the hunt.
    virtual bool before_hunt(Autopilot&, InputFrame&, Unit&) { return false; }
    // Enemy u as a target: factor on its score (distance, lower is preferred); 0 = never a target. The default skips
    // siege weapons (unit class 8) and horses, as the generic bot did.
    virtual float foe_weight(Autopilot& ap, const Unit& p, const Unit& u);
    // Minimap way point `slot` (1-based) at `at` as the bot's marker; false skips it as if not shown.
    virtual bool use_waypoint(Autopilot&, int /*slot*/, Vec3 /*at*/) { return true; }
    // Friend u as a talk errand without a marker (talk icon); false skips him.
    virtual bool use_talker(Autopilot&, const Unit&) { return true; }
    // Buy screen open (also during the shop's cut scene), after the generic purchases.
    virtual void shop(Autopilot&, Unit&) {}
    // After Mission::tick and the event digest.
    virtual void after_tick(Autopilot&) {}
};

// The plan for scenario `stem`, or nullptr (generic bot only).
std::unique_ptr<MissionPlan> make_plan(std::string_view stem);

}  // namespace joa::game
