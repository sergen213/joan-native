// Test / diagnostic campaign bot (NOT used in normal play): plays the player's hero through the real
// PlayerController with InputFrames only (move, attack, block, jump, interact, F3) plus the
// character-screen point allocation a player makes. It reads what a player sees: the minimap way
// points and target follows the scripts set, the enabled end conditions, the interact indicator,
// nearby units and items. Paths come from the mission navmesh (a player's route knowledge).
#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "game/autopilot_plan.hpp"
#include "game/campaign_runner.hpp"

namespace joa::game {

class Autopilot {
public:
    explicit Autopilot(Mission& m);
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
    // Buy screen (shop()): ammo of the launcher in hand is bought up to these counts carried, and gold is never spent below
    // shop_gold_keep. Mission plans set them (default: explosive up to 20, plain with all the gold left).
    // shop_plain_first plain ones are bought before a heavier armour.
    int shop_explosive_max = 20, shop_plain_max = std::numeric_limits<int>::max(), shop_gold_keep = 0, shop_plain_first = 0;
    // Explosive arrows the generic volleys leave alone (a plan keeps them for a structure: S4L1's Tourelles gate).
    int explosive_reserve = 0;
    // NPC a plan walks up to this tick: talked to at the interact prompt again 300 ticks after the last word (as the
    // errand NPC; the plan sets it each tick it leads there).
    int plan_talk = 0;
    int talk_prompt() const { return prompt_ && prompt_kind_ == "talk" ? prompt_id_ : 0; }   // unit the talk indicator shows, 0 none
    // Ticks a way point on an open gate is shown before the bot goes to close it (default 30 s: mission 2's generic play
    // closed it after John's 10 s mark, closing then sends him and team 11 off, long before the 180 s reinforcements).
    int gate_delay = 30 * 30;
    // Below rest_pct % HP enemies that have not noticed the player are left alone and she rests, eating (F3) while she
    // carries more than rest_food (mission 4 lowers both: regen of 1 HP / 150 ticks cost it 20000+ ticks a rest).
    int rest_pct = 70, rest_food = 10;
    // travel(): also open a gate near the player when the goal's region is not linked to hers (S9L1's Orleans east gate).
    bool gate_unlinked = false;
    // Character screen: where every attribute point goes (World::allocate_point type: 6 HIT_POINT, 1 STRENGTH).
    int attr_kind = 6;

    // --- helpers (also for the mission plans, autopilot_plan.hpp) --------------------------------------
    Mission& mission() { return m_; }
    int tick() const { return tick_; }
    struct Seam { int a, b; Vec3 at, in; };  // (level, component) region a meets region b: `at` just inside b, `in` just inside a
    void steer(InputFrame& f, const Unit& p, Vec3 goal, bool replan);
    void travel(InputFrame& f, const Unit& p, Vec3 goal);  // steer, opening a closed gate that shuts the way
    const Seam* detour(const Unit& p, Vec3 g);             // first level-seam crossing of a route to g over cut-off regions
    int region(Vec3 q) const;                              // (nav level, path-mesh component) under q, -1 none
    void build_seams();                                    // seams_ for the current gate states
    void give_up(int id);                                  // leave a foe the player could not get at alone for a while
    bool unreached(const Unit& u) const;                   // given up on and still left alone
    bool open_territory(Vec3& g, int* index = nullptr);    // centre of the first open WIN objective's territory left to walk to
    bool linked(int a, int b);                             // region b walkable from region a
    int unit_region(const Unit& u);                        // region() of u, cached 30 ticks
    const MapItem* ladder_to(int from, int to, Vec3& end, bool& up);  // ladder between two regions, the end to walk to
    bool use_ladder(InputFrame& f, const Unit& p, int to);  // walk to / mount a ladder towards region `to`
    const Unit* bearer() const;
    bool following(const Unit& u) const;                   // told to follow the player
    int hero_key(const Unit& u) const;                     // switch_hero key 1..4, 0 not a hero                            // friend in the player's team dropping a MISSION item
    Vec3 home() const;                                     // centroid of the units not hostile to the player
    Vec3 gate_inside(const MapItem& g) const;              // a point 100 inside a gate (towards home())
    void fight(InputFrame& f, Unit& p, const Unit& foe);
    void shoot(InputFrame& f, Unit& p, const Unit& foe);  // bow: range mode, aim, draw, release
    void shoot_at(InputFrame& f, Unit& p, Vec3 at, bool release);  // range mode, camera at `at`, draw, release when told
    bool clear_shot(const Unit& p, const Unit& t) const;  // an arrow aimed at t now hits t
    const Unit* densest(const Unit& p, float range, bool steady, int& n) const;  // centre of the densest enemy group
    bool load_ammo(Unit& p, bool explosive);               // character screen: arrows into the ammo slot
    int arrows(const Unit& p, bool explosive) const;       // arrows of that kind carried (0 without a bow)
    void character_screen(Unit& p);                        // F1: attribute / skill points, a better sword
    void shop(Unit& p);                                    // buy screen open: armour, arrows
    bool loot(InputFrame& f, const Unit& p, float radius = 1200);  // break boxes / pick up items nearby
    bool discard(Unit& p, int bag_index);                  // drop a bag item for room; loot() leaves it lying
    void make_room(Unit& p);                               // bag room for purchases (drops, troop grid, repack)
    int swing_eta(const Unit& u) const;
    bool energy_ok(const Unit& p) const;  // EP / HP for the energy attack

private:
    InputFrame decide();
    std::unique_ptr<MissionPlan> plan_;
    Mission& m_;
    int tick_ = 0;
    bool prompt_ = false;          // interact indicator shown
    std::string prompt_kind_;
    int prompt_id_ = 0;              // unit / item the indicator points at
    int prompt_tick_ = 0;            // tick the indicator last changed
    int gate_ = 0, gate_seen_ = 0;   // gate item the plan closes (R at its indicator), 0 none; tick its marker appeared
    int open_gate_ = 0;              // closed gate the bot opens to get through (R at its indicator)
    Vec3 open_goal_{};               // the destination it is opened for
    int travel_tick_ = -1;           // last tick travel() ran
    int opened_gate_ = 0;            // last gate the bot opened itself (closed again once nobody is left outside)
    int shut_tick_ = -1;             // last tick travel() walked to close opened_gate_ behind the player
    bool go_home_ = false;           // a message asked the player back behind the gate she opened
    std::vector<Seam> seams_;
    unsigned seam_gates_ = 0;        // gate states seams_ was built with
    bool seams_built_ = false;
    std::map<std::pair<int, int>, bool> linked_;  // linked() cache for seams_
    std::map<int, std::pair<int, int>> unit_region_;  // unit -> (tick, region)
    Vec3 via_{}, via_in_{}, via_goal_{};  // detour seam crossing (next level / this level) on the way to via_goal_
    bool via_cross_ = false;         // reached via_in_: straight across to via_
    std::vector<Vec3> bad_vias_;     // detour crossings not reached in time (skipped by detour())
    std::map<int, int> chase_;       // foe -> ticks spent closing in on it since last within 150
    int via_tick_ = -1, detour_tick_ = -1000;
    int no_wait_until_ = 0;          // no wait for a lagging follower before this tick
    std::map<int, int> territory_seen_;  // WIN objective -> tick the player last stood at its territory
    struct Way { float best = 0; int headway = 0, last = -1000; };
    std::map<int, Way> territory_way_;   // WIN objective -> trip there: nearest yet, tick of the last headway, last tick
    std::map<int, int> territory_skip_;  // WIN objective -> tick its territory is tried again (no headway)
    Way escort_way_;                     // waiting for an escort: nearest yet, tick of its last headway, last tick
    Way volley_way_;                     // walking up to an explosive-volley group: nearest yet, last headway, last tick
    int escort_skip_ = 0;                // no escort wait before this tick
    struct Ladder { int item; Vec3 foot, top; int foot_region, top_region; };
    std::vector<Ladder> ladders_;    // built with seams_
    std::set<std::pair<int, int>> no_ladder_;  // (from, to) regions ladder_to() found no ladder chain between
    bool climb_up_ = true;           // direction of the ladder use_ladder() mounted
    int hunt_target_ = 0, hunt_tick_ = -1000;  // enemy hunted, since
    unsigned gates_ = 0;             // gate states the current path was planned with
    std::map<int, int> talked_;      // unit -> tick the player last talked to it (or gave up)
    std::map<int, int> talk_news_;   // unit -> news_ when last talked to (or its talk's own outcome)
    int news_ = 0;                   // count of news for the talk errands (step())
    std::uint64_t news_sig_ = 0;     // talk icons / followers / MISSION items at the last check
    int last_talk_ = 0, last_talk_tick_ = -1000, last_cut_tick_ = -1000, cut_start_tick_ = -1000;
    std::set<int> shopkeepers_;      // talks that opened the buy screen
    std::set<int> mute_, new_talkers_;  // friends seen without the talk icon / given it since
    std::map<int, int> giver_;          // new talker -> NPC whose talk brought its talk icon
    std::map<int, std::set<std::string>> errand_territories_;  // WIN objective -> every territory it has named
    int talk_key_ = 0, talk_tick_ = 0, talk_seen_ = -1000, talk_near_ = 0;  // NPC walked up to, since, last tick of that approach, last tick beyond 400
    bool talk_errand_ = false;       // the NPC walked up to is an errand's (no marker)
    int last_interact_ = -1000, last_heal_ = -1000;
    int wait_tick_ = -1;
    int home_hero_ = 0, switch_retry_ = 0;  // hero switched away from to spend another's points; next try             // waiting for a lagging ally / bearer since
    // path following
    std::vector<Vec3> path_;
    std::size_t wp_ = 0;
    Vec3 path_goal_{};
    int path_tick_ = -1000;
    // stuck handling
    Vec3 check_pos_{};
    int check_tick_ = 0, unstick_until_ = 0, unstick_dir_ = 0, stuck_count_ = 0;
    int fight_target_ = 0;
    struct Unreach { int until = 0, times = 0; Vec3 at{}; };
    std::map<int, Unreach> unreach_;  // foe -> left alone until tick, times given up on, where it stood then
    bool shot_ = false;              // shoot() ran this tick (range mode is left otherwise)
    std::uint32_t shop_ms_ = ~0u;    // buy screen opening already shopped at
    bool retreat_ = false;           // HP < 30 % and F3 had no effect: guard near an ally until 70 %
    int heal_hp_ = 0;                // HP when F3 was last pressed
    std::vector<Vec3> done_wps_;     // markers stood at for 10 s without the script moving them
    std::vector<bool> wp_shown_;     // per way point slot: shown last tick
    int seen_tick_ = 0;              // last tick the player was away from the marker
    std::map<int, int> loot_spent_;  // loot target (-map item recno / world item id) -> ticks spent going for it
    std::set<int> skip_;
    std::set<int> held_;             // ally heroes the bot put on hold beside a ward             // loot targets given up on
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
