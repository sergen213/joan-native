// Campaign driver: mission order, win -> next mission with carried progression, retry, saves.
// Headless counterpart of the game-end menu (FUN_00720ca0) and screen modes 0xd/0xc
// (GAMEPLAY_AI_TROOPS.md Part D §4.4).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "game/campaign.hpp"
#include "game/mission.hpp"

namespace joa::game {

class CampaignRunner {
public:
    CampaignRunner(GameData& gd, const formats::Vfs& vfs) : gd_(gd), vfs_(vfs) {}

    bool init(std::string* err = nullptr);  // loads scenario/scenario_01..08.scr
    std::uint32_t seed = 1;                 // every mission of the run starts its World/VM with it
    CampaignProfile profile;
    std::vector<CampaignMission> missions;

    // Battle::start_scenario(no, 1): a fresh mission with the current profile.
    bool start(int no, std::string* err = nullptr);
    Mission* mission() { return mission_.get(); }

    // Game-end menu CONTINUE after a win (finish == 1): store heroes/lists/gold/time into the
    // profile (DAT_00a58f95 = 1), then start scenario no + 1 (5 -> 6 keeps scenario 5's world:
    // Mission::continue_in_world). After scenario 8 the ending plays instead
    // (MissionEvent::Ending) and false is returned with *err = "campaign complete".
    bool continue_after_win(std::string* err = nullptr);
    // RETRY (game-over box / end menu): start_scenario(no, step, 0); the profile is unchanged.
    bool retry(std::string* err = nullptr);

    SaveFile save() const { return mission_->save(); }
    // Starts the save's scenario with its seed, then restores every section (profile included).
    bool load(const SaveFile& f, std::string* err = nullptr);

private:
    GameData& gd_;
    const formats::Vfs& vfs_;
    std::unique_ptr<Mission> mission_;
};

}  // namespace joa::game
