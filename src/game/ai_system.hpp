// Native unit AI, tasks, orders, troops, formations, path following and RTS commands
// (joan.exe Part A §A3-A5 and Part B of docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md).
// AiSystem is the World's IUnitBrain: phase 2 = auto-join + formation + order executor + path follow,
// phase 3 = per-class AI think (UnitAI_dispatch 0x5467b0). State lives here keyed by recno; World
// serializes only its own Unit fields, AiSystem::serialize()/hash() cover the rest.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>  // world_sim.hpp uses std::exchange
#include <vector>

#include "game/orders_queue.hpp"
#include "game/world_sim.hpp"

namespace joa::game {

class NavWorld;

// Mission "place" (array 0xa484d0): AABB + optional numbered slots. Filled by the mission layer.
struct AiPlace {
    Vec3 centre, min, max;
    std::vector<Vec3> slots;            // 1-based in the original (slot i -> slots[i-1])
    std::vector<float> slot_facing;     // radians
    std::vector<int> occupant;          // unit recno per slot (0 free)
    int view_angle = 0;                 // get/set_place_unit_view_angle (FUNC 183/184)
    bool contains(Vec3 p) const {       // AABB_contains 0x60d5f0, inclusive
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
};

// Map item as the AI sees it (gates, walls, boxes, barrels). Supplied by the mission layer.
struct AiItem {
    Vec3 pos;
    std::string code;                   // item res code (ENGLISH_EX_ARCHER searches by code)
    int nation = 0;
    int hp = 1;
    bool gate = false, wall = false, box = false;
    int gate_open = 0;                  // gates: 0 closed, 1 open
    int size = 0;                       // ItemRes +0x24 footprint size (approach stand-off uses size/2)
};

// Per-unit AI / movement state (Unit fields +0x2a1..+0x490 of the original).
struct AiUnit {
    Task task, task2;                   // +0x2a5 current, +0x2be parked (hold / RTS)
    OrderQueue orders;                  // +0x2a1
    // path (+0x432 path, +0x3e6 waypoint index, +0x40e length, +0x3f2..+0x406 wait counters)
    std::vector<Vec3> path;
    int wp = 0;
    float path_len = 0;
    Vec3 path_goal;
    int wait_len = 5, wait_cnt = 5;     // INIT_W_C
    int sidestepped = 0, side = 0;
    int stuck_since = -1;
    Vec3 last_pos;
    Vec3 approach_goal;                 // +0x3d9 APPROACH_ENEMY cached goal
    // formation (+0x412 world offset, +0x41e width, +0x422/+0x426 local slot)
    Vec3 slot;
    float width = 0, local_fwd = 0, local_side = 0;
    int place = 0, place_slot = 0;      // +4 / +6 claimed place slot
    int orphan = 0;                     // +0x39
    int spawned = 0;                    // +0x102 runtime-spawned flag (barracks / recruit)
    int push_slot = -1;                 // +0x380 LARGE_WEAPON_PUSH slot
    bool initialised = false;
    template <class V> void visit(V& v) {
        v(task), v(task2), v(orders), v(path), v(wp), v(path_len), v(path_goal), v(wait_len), v(wait_cnt),
            v(sidestepped), v(side), v(stuck_since), v(last_pos), v(approach_goal), v(slot), v(width),
            v(local_fwd), v(local_side), v(place), v(place_slot), v(orphan), v(spawned), v(push_slot), v(initialised);
    }
};

// Team (0x58 bytes, Unit+0x33 on the leader). Membership itself is Unit::members / Unit::leader.
struct AiTeam {
    int ftype = 1;                      // +0x37 formation type (1 = grid)
    Vec3 anchor;                        // +0x38
    float angle = 0;                    // +0x44 radians
    Vec3 prev_anchor;                   // +0x48
    float prev_angle = 0;               // +0x54
    template <class V> void visit(V& v) { v(ftype), v(anchor), v(angle), v(prev_anchor), v(prev_angle); }
};

class AiSystem final : public IUnitBrain {
public:
    static constexpr int kMaxTeam = 20;           // hard cap (0x14)
    static constexpr int kAbsorbRadius = 300;     // 0x4655a0
    static constexpr int kLeaderLeash = 1000;
    static constexpr int kFormationSpacing = 100; // 0x4fa3c0
    static constexpr int kRtsTaskMs = 180000;     // RTS task expiry

    // Registers itself as w's brain. nav may be null (straight-line movement, no LOS quads).
    AiSystem(World& w, const NavWorld* nav);
    ~AiSystem() override;

    // --- IUnitBrain ---------------------------------------------------------------------
    void execute_orders(World& w, Unit& u) override;   // phase 2 (0x52d000)
    void think(World& w, Unit& u) override;            // phase 3 (0x49d810 body after the timer)

    // --- mission data hooks ---------------------------------------------------------------
    std::map<int, AiPlace> places;                     // place id -> place
    std::map<int, AiPlace> zones;                      // facility/zone id -> AABB (APPROACH_PLACE p2)
    std::map<int, std::vector<Vec3>> tracks;           // .trk id -> nodes
    std::function<std::optional<AiItem>(int)> item;    // item id -> info; default: World items (pos only)
    std::function<std::vector<int>()> item_ids;        // all item ids for search_for_item; default World items
    std::function<void(int unit, int item, int desired)> on_gate_request;  // GATE_OPEN event 0x20
    // Nation recruit purse (Nation_spend_recruit 0x4b5bf0): returns true and pays if affordable.
    std::function<bool(int nation)> spend_recruit;

    // --- state access -----------------------------------------------------------------------
    AiUnit& st(const Unit& u);                         // created on demand
    const AiUnit* find(int recno) const;
    AiTeam* team_of_leader(int recno);                 // non-null iff recno leads a team
    const NavWorld* nav() const { return nav_; }

    // --- tasks (Part B §1) ------------------------------------------------------------------
    bool set_task(Unit& u, const Task& t);             // 0x527fb0 (strict prio, lazy expiry)
    void clear_task(Unit& u);                          // 0x528300 stop_curr_task
    void expire_task(Unit& u);                         // 0x528440
    int curr_task(const Unit& u);                      // 0x6520d0 (inherits leader's task unless RTS_MOVE_TO)
    int curr_task_para(const Unit& u, int i);          // 0x652190
    int curr_task_state(const Unit& u);                // 0x652280
    void set_curr_task_state(Unit& u, int s);          // 0x6522f0
    // Script give_task 0x6619d0 / give_team_task 0x661b40 (duration 0 = never expires).
    bool give_task(Unit& u, int id, int prio, int p1, int p2, int p3, int duration_ms);
    bool give_team_task(Unit& u, int id, int prio, int p1, int p2, int p3, int duration_ms);
    void stop_all_team_task(int team_id);              // 0x6523c0
    void set_idle(Unit& u);                            // lose/win freeze: clear orders + IDLE prio 0x7f p1=1

    // --- orders (Part B §2) -----------------------------------------------------------------
    // Unit_give_order 0x661340: team=true (and always for APPROACH_ENEMY) copies to leader + members.
    int give_order(Unit& u, int action, int prio, bool team, int p1, int p2, int p3, int p4, int duration_ms);
    void stop_all_order(Unit& u);                      // 0x6618f0
    void finish_current(Unit& u);                      // 0x4bb660 (cleanup + cur.action = 0)
    // Small script-FUNC helpers the routines call (FUNC handler addresses in the comments).
    static int combo_para(std::string_view s, int i);  // string_to_order_para 0x6511f0 (4 chars big-endian per int)
    bool inside_place(int place, int recno) const;     // is_unit_inside_place (AABB)
    int distance_to(const Unit& u, Vec3 p) const;      // get_distance 0x65b800 int 3-D
    int gate_state(int item) const;                    // get_gate_state 0x6524a0: 0 closed, 1 open, -1 not a gate
    void set_unit_dead(Unit& u);                       // script set_unit_dead: "escaped" fade-out, no kill credit
    void set_target(Unit& u, int recno) { u.target = recno; }   // 0x6558d0
    void set_mode(Unit& u, int mode) { u.mode = mode; }         // 0x6515d0: 1 ATTACK 2 ACTIVE 3 IDLE
    static bool attack_mode(const Unit& u) { return u.mode == 1; }   // 0x651590
    static bool idle_mode(const Unit& u) { return u.mode > 1; }      // 0x651550 (ACTIVE or IDLE)
    bool can_attack_down(const Unit& u) const;         // FUNC 58: can do DOWN_ATTACK now (has anim, not mounted, can act)
    bool facing_knock_down_enemy(const Unit& u) const; // FUNC 56: a knocked-down enemy within reach in front
    bool can_energy_attack(const Unit& u) const;       // FUNC 60 / 0x652c10
    bool can_rush_attack(const Unit& u) const;         // FUNC 59
    int chapter = 0;                                   // DAT_00a58fa7 scenario counter (mission layer sets it)
    void set_place_unit_view_angle(int place, int deg) { places[place].view_angle = deg; }  // FUNC 184
    int place_unit_view_angle(int place) { return places[place].view_angle; }               // FUNC 183
    int curr_order(const Unit& u);                     // 0x651260
    bool has_order(const Unit& u, int action);

    // --- perception (Part A §A5.2-A5.4) -------------------------------------------------------
    // Unit_search_for_unit 0x54b480. type: "T","E","EP","P","S","F","L","O". radius 0 -> 1600.
    int search_for_unit(Unit& u, std::string_view type, int radius = 0, bool centre_on_leader = false,
                        int place = 0, int max_yaw = 0, int max_pitch = 0);
    int search_for_item(Unit& u, std::string_view type, int radius, bool centre_on_leader = false);  // 0x503680
    int unit_range(const Unit& u, int kind) const;     // 0x65b280
    int base_attack_chance(const Unit& u) const;       // 0x511e70
    int attack_chance(const Unit& u) const;            // 0x511f80
    int block_chance(const Unit& u) const;             // 0x512070
    bool is_attacked(int recno) const;                 // 0x6512d0
    bool will_be_attacked(const Unit& victim, int attacker) const;   // 0x651430
    bool busy_attacking(const Unit& u) const;          // 0x510f80
    bool unit_dead(int recno) const;                   // 0x65c8d0 (invalid counts as dead)
    int unit_dist(const Unit& a, int b) const;         // 0x653150 int 3-D
    int item_dist(const Unit& a, int item) const;      // 0x653060
    int rotation_to(const Unit& u, int target) const;  // 0x652ee0 heading (deg) toward target, TURN p1
    int item_rotation(const Unit& u, int item) const;  // 0x653000 heading (deg) toward item
    bool line_of_fire_blocked(const Unit& u, int target) const;   // 0x5130a0 (terrain ray)
    int nearest_incoming_missile(const Unit& u, bool enemy_only, int radius) const;  // 0x467c30
    int all_attached(const Unit& u, char side) const;  // 0x656bb0 is_all_attached(u,"M"/"F")
    int class_of(const Unit& u) const;                 // unit type class (type+2)
    int class_of(int recno) const;
    int priority_of(const Unit& u) const;              // 0x527790 type priority (NEAREST_HIGH_PRIORITY)
    int rand(int n);                                   // Misc_random 0x590540

    // --- teams / formations (Part B §4) ---------------------------------------------------------
    void create_team(Unit& u);                         // 0x447600
    void join_team(Unit& m, int leader);               // 0x438f80
    void leave_team(Unit& m);                          // 0x42fcb0
    void disband_team(Unit& l);                        // 0x42dd70
    int leader_of(const Unit& u) const;                // 0x4216d0 (self if leader)
    int leader_recno(const Unit& u) const;             // 0x6569f0 (0 if self/none)
    bool is_leader(const Unit& u) const;
    int max_troop_size(Unit& l);                       // 0x448080
    void update_formation(Unit& l, bool force, bool use_delta);   // 0x4fa0f0
    Vec3 formation_pos(Unit& u);                       // 0x448870
    int follow_unit_distance(int recno);               // 0x6592c0
    void auto_join(Unit& l);                           // 0x4655a0 leader absorbs free units within 300
    bool set_unit_team(Unit& u, int team_id);          // 0x657fd0
    int recruit_team_member(Unit& u, std::string_view class_code, int n);   // 0x6584e0 -> spawned count
    int share_team_member(Unit& dst, Unit& src, int n);                     // 0x65ef40
    void set_formation_pos(Unit& u, Vec3 anchor, float deg);                // 0x658810
    void set_team_member_pos(Unit& u);                                      // 0x658920
    void on_unit_removed(int recno);                   // corpse deletion: leave team / disband

    // --- player / RTS (Part B §5) -------------------------------------------------------------
    void toggle_hold(Unit& u, bool recurse);           // Unit_toggle_hold 0x5121c0
    bool is_holding(const Unit& u);                    // current task == {0, p1 != 0}
    int find_recruitable(Unit& player);                // 0x48dfb0(100): nearest recruitable recno, 0 none
    int recruit_nearby(Unit& player);                  // 0x43c210; returns recruit recno
    bool rts_selectable(const Unit& u);                // 0x453620
    std::vector<int> rts_select_with_team(int recno);  // click-select: unit (+ team if leader)
    bool rts_move(std::span<const int> sel, Vec3 dest, bool attack_move);   // 0x44cfb0
    void rts_attack_unit(std::span<const int> sel, int target);             // 0x44eb80
    void rts_attack_item(std::span<const int> sel, int item);               // 0x44f1f0
    // 0x44f750 branch B: join a leader's team / follow a commander. Returns false when nothing happened.
    // confirmed=false and over capacity -> returns false and sets *needs_confirm.
    bool rts_assign(std::span<const int> sel, int target, bool confirmed, bool* needs_confirm = nullptr);
    void rts_dismiss_crew(int weapon);                 // Rts_dismiss_siege_crew 0x44a690 (double-click weapon)
    void rts_enter(Unit& player);                      // Rts_init: player handed to AI
    void rts_exit(Unit& player);                       // Rts_deinit: park tasks, members inherit

    // --- movement (Part C §3) ----------------------------------------------------------------
    // Unit_move_to 0x505dd0: returns 0 nothing / 1 path built / 2 empty / 3 used leader path.
    int move_to(Unit& u, Vec3 goal, int budget, bool from_leader, int issuer);
    void clear_path(Unit& u);                          // 0x422f10
    void update_movement(Unit& u);                     // Unit_update_movement 0x52daa0 (per tick)
    void stop(Unit& u);                                // STOP cleanup: clear path, stand

    // --- persistence --------------------------------------------------------------------------
    void serialize(std::vector<std::uint8_t>& out) const;
    bool deserialize(std::span<const std::uint8_t> in);
    std::uint64_t hash() const;

    // Per-class AI routines (dispatcher 0x5467b0 + routines in ai_routines_*.cpp). Return value as in the
    // original: false = "not handled" (melee roll during an attack anim) -> script fallback (none native).
    bool dispatch(Unit& u);

    World& world() { return w_; }

private:
    friend struct AiRoutines;
    void execute_order(Unit& u);                       // OrderQueue_process 0x4baf80
    bool can_interrupt(Unit& u);                       // 0x4bb3e0
    void cleanup(Unit& u, int action);                 // per-action cleanup functions
    void ensure_init(Unit& u);
    int reach(const Unit& u) const;                    // REACH_DT rule
    int ai_class_for(const Unit& u) const;             // UnitAI_class_from_name 0x546520

    World& w_;
    const NavWorld* nav_;
    std::map<int, AiUnit> units_;
    std::map<int, AiTeam> teams_;
};

}  // namespace joa::game
