#include "game/campaign_runner.hpp"

namespace joa::game {

bool CampaignRunner::init(std::string* err) {
    missions = load_campaign(vfs_, err);
    return !missions.empty();
}

bool CampaignRunner::start(int no, std::string* err) {
    if (no < 1 || no > int(missions.size())) {
        if (err) *err = "no campaign scenario " + std::to_string(no);
        return false;
    }
    mission_.reset();
    mission_ = std::make_unique<Mission>(gd_, vfs_, profile);
    return mission_->start(missions[no - 1], seed, err);
}

bool CampaignRunner::continue_after_win(std::string* err) {
    if (!mission_ || mission_->finish() != 1) {
        if (err) *err = "mission not won";
        return false;
    }
    const int no = mission_->scenario_no();
    if (no >= int(missions.size())) {  // scenario 8: ending FUN_006d19e0(0), no profile store
        mission_->emit(MissionEvent::Kind::Ending, 0);
        if (err) *err = "campaign complete";
        return false;
    }
    mission_->store_profile();
    mission_->scenario().game_finish = 3;  // "continue to next scenario"
    auto events = mission_->take_events();
    // ModeNextScenario_process 0x755d80: Battle_start_scenario(no + 1, 1, keep = 1). The keep flag
    // only matters leaving scenario 5: scenario 6 (S9L1) continues in scenario 5's world.
    if (no == 5 ? !mission_->continue_in_world(missions[no], err) : !start(no + 1, err)) return false;
    mission_->state().events = std::move(events);
    mission_->emit(MissionEvent::Kind::Transition, no, no + 1, missions[no].stem());
    return true;
}

bool CampaignRunner::retry(std::string* err) {
    if (!mission_) return false;
    return start(mission_->scenario_no(), err);
}

bool CampaignRunner::load(const SaveFile& f, std::string* err) {
    const SaveSection* m = f.find("MISN");
    if (!m) {
        if (err) *err = "not a mission save";
        return false;
    }
    formats::ByteReader r(m->data);
    const int no = r.i32();
    seed = r.u32();
    if (!r.ok() || !start(no, err)) return false;
    return mission_->load(f, err);
}

}  // namespace joa::game
