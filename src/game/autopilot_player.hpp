// Test / diagnostic campaign bot (NOT used in normal play): plays the player's hero through the real
// PlayerController with InputFrames only (move, attack, block, jump, interact, F3) plus the
// character-screen point allocation a player makes. It reads what a player sees: the minimap way
// points and target follows the scripts set, the enabled end conditions, the interact indicator,
// nearby units and items. Paths come from the mission navmesh (a player's route knowledge).
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "game/campaign_runner.hpp"

namespace joa::game {

class Autopilot {
public:
    explicit Autopilot(Mission& m) : m_(m) {}
    // Decide this tick's input, run Mission::tick, digest the events.
    void step();

    // Counters for the report.
    int kills = 0, hits = 0, hurt = 0, player_deaths = 0;
    long dealt = 0, taken = 0;     // HP damage dealt by / to the player (Ev::Hit values)
    int last_progress_tick = 0;    // last tick an objective finished or a new way point appeared
    std::string goal;              // current goal ("waypoint 1 (x,y)", "fight #12", ...)
    std::vector<std::string> log;  // objective events (tick: text)
    std::map<int, int> atk_start_;
    std::size_t seen_units_ = 0;
    int trace = 0;                 // diagnostic: print a status line every `trace` ticks (env JOA_BOT_TRACE)

private:
    InputFrame decide();
    void steer(InputFrame& f, const Unit& p, Vec3 goal, bool replan);
    void fight(InputFrame& f, Unit& p, const Unit& foe);
    void shoot(InputFrame& f, Unit& p, const Unit& foe);  // bow: range mode, aim, draw, release
    bool load_ammo(Unit& p, bool explosive);               // character screen: arrows into the ammo slot
    void shop(Unit& p);                                    // buy screen open: armour, arrows
    bool loot(InputFrame& f, const Unit& p, float radius = 1200);  // break boxes / pick up items nearby
    int swing_eta(const Unit& u) const;
    const Unit* protectee() const;  // mission plan: hero to stay with

    Mission& m_;
    int tick_ = 0;
    bool prompt_ = false;          // interact indicator shown
    std::string prompt_kind_;
    int prompt_id_ = 0;              // unit / item the indicator points at
    std::map<int, int> talked_;      // unit -> tick the player last talked to it (or gave up)
    int talk_key_ = 0, talk_tick_ = 0;
    int last_interact_ = -1000, last_heal_ = -1000;
    // path following
    std::vector<Vec3> path_;
    std::size_t wp_ = 0;
    Vec3 path_goal_{};
    int path_tick_ = -1000;
    // stuck handling
    Vec3 check_pos_{};
    int check_tick_ = 0, unstick_until_ = 0, unstick_dir_ = 0, stuck_count_ = 0;
    int fight_target_ = 0;
    bool shot_ = false;              // shoot() ran this tick (range mode is left otherwise)
    std::uint32_t shop_ms_ = ~0u;    // buy screen opening already shopped at
    bool retreat_ = false;           // HP < 30 % and F3 had no effect: guard near an ally until 70 %
    int heal_hp_ = 0;                // HP when F3 was last pressed
    std::vector<Vec3> done_wps_;     // markers stood at for 10 s without the script moving them
    int seen_tick_ = 0;              // last tick the player was away from the marker
    int loot_key_ = 0, loot_tick_ = 0;  // current loot target (-map item recno / world item id), since
    std::set<int> skip_;             // loot targets given up on
};

// One mission played by the bot from its start: Mission::tick until finish != 0 or `max_ticks`.
struct MissionReport {
    int no = 0;
    std::string stem;
    int result = 0;             // finish: 1 won, 2 lost, 0 timed out
    int ticks = 0, attempts = 0, deaths = 0, kills = 0;
    long dealt = 0, taken = 0;  // player HP damage dealt / taken over all attempts
    int objectives_done = 0, objectives_total = 0;  // enabled Win/Side conditions finished WIN / total
    std::uint64_t hash = 0;     // Mission::hash() at the end of the last attempt
    std::string stuck;          // timed out / lost: blocking objective, bot goal, position
    std::vector<std::string> log;  // objective / victory / defeat events of the last attempt
};

// Plays the current mission of `run` with the bot (retrying a loss up to `retries` times, as the
// game-over RETRY does). The caller CONTINUEs after a win.
MissionReport play_mission(CampaignRunner& run, int max_ticks, int retries);

}  // namespace joa::game
