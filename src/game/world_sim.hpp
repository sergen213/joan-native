// Headless gameplay world: units, combat triggers, projectiles, game clock, RNG and the fixed tick
// order of Game_process_tick 0x4e5fc0 for the subsystems that matter to gameplay. Rendering only
// reads snapshot()/events(). Spec and traceability: docs/GAMEPLAY_IMPLEMENTATION.md.
//
// Determinism: one tick = 33 ms of game time (the original's video-capture fixed step, §1.1);
// gameplay draws use one explicitly seeded Rng; cosmetic draws (idle variants) a separate one.
// All persistent state is plain data addressed by recno / table index (serialize()/hash()).
#pragma once

#include <utility>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "game/controller_player.hpp"
#include "game/script_vm.hpp"
#include "game/terrain_query.hpp"
#include "game/unit_state.hpp"
#include "game/unitclass_db.hpp"

namespace joa::game {

enum class Ev : int {
    Action, Hit, Blocked, Knockdown, Death, LevelUp, XpGain, Sound, AnimEvent, Effect, Crit,
    ProjectileSpawn, ProjectileHit, ItemDrop, Custom
};
const char* ev_name(Ev e);

struct GameEvent {
    int time = 0;
    Ev type = Ev::Custom;
    int unit = 0, other = 0;
    int value = 0;
    std::string name;
};

// Attack/event trigger (0x43-byte record, Trigger_create 0x4eef00 / Trigger_process 0x4ef090).
// Types: 1 melee, 2 arrow, 0x10 throw, 0x40 stone/cannon, 0x200 interact, 0x400 down attack,
// 0x2000 explosive arrow, 0x4000 explosive keg. Kept in creation order; delivered once on the first
// tick with now >= start.
struct Trigger {
    int type = 1;
    int target_kind = 1;     // 1 units, 2 map objects
    int target_id = 0;       // <= 0: area
    int src_kind = 1;        // 1 unit, 2 object
    int sender = 0;
    Vec3 pos;
    float radius = 0;
    int start = 0, end = 0;
    int p16 = 0;             // force-override script (horse charge)
    int param = 0;           // projectile type for ranged events; keg item damage (+0x72) for 0x4000 from objects
    template <class V> void visit(V& v) {
        v(type), v(target_kind), v(target_id), v(src_kind), v(sender), v(pos), v(radius), v(start), v(end), v(p16), v(param);
    }
};

// Map item met by the structure look-ahead ray (Physics_sweep_structures 0x693750 narrowphase,
// answered by the mission layer).
struct StructureHit {
    int item = 0;        // map item recno (+0x04), 0 = none
    float t = 1;         // fraction along the ray
    int modify = 0;      // item +0x06: event target id
    Vec3 pos;            // item +0x56
    int struction = 0;   // ItemRes +0x1c StructionType (1/2 -> CANNONHH)
};

// ThrowItem (0x6c bytes) + its rigid body. Physics in metres (46.1 units = 1 m).
struct Projectile {
    int id = 0;
    int type = 0;            // 0 ARW 1 PROJ 2 EXPL 3 STM 4 STC 5 CAN
    int shooter = 0;
    int item = 0;            // +0x08 ItemArray object: the ammo item index (model at level 2, 0x507fc0)
    int launch_ms = 0;
    Vec3 launch;             // +0x18 (z overwritten with 0, CONFIRMED quirk)
    Vec3 pos_m, vel_m;       // body position / velocity (m, m/s)
    Vec3 force;              // force pulse
    int force_start = 0;
    float force_left_ms = 0;
    int tl = 0, dl = 0;
    float m = 1, tr = 0, gl = 0;
    int hit_id = 0;          // +0x26 unit recno or map item recno
    int sweep_next = 0, sweep_hit = 0, sweep_item = 0;   // +0x5a / +0x5e / +0x62 look-ahead (0x693750)
    bool collided = false, ground = false, destroyed = false, exploded = false;
    template <class V> void visit(V& v) {
        v(id), v(type), v(shooter), v(launch_ms), v(launch), v(pos_m), v(vel_m), v(force), v(force_start), v(force_left_ms),
            v(tl), v(dl), v(m), v(tr), v(gl), v(hit_id), v(collided), v(ground), v(destroyed), v(exploded), v(item),
            v(sweep_next), v(sweep_hit), v(sweep_item);
    }
};

// Item lying in the world (drops). Mission objects (gates, boxes) belong to the mission layer.
struct WorldItem {
    int id = 0, item = 0, level = 0, quantity = 1;
    Vec3 pos;
    bool deleted = false;
    float yaw_deg = 0;   // heading of the dropping unit (Unit_dropItem 0x406631 -> 0x499fd0(unit +0x1a))
    template <class V> void visit(V& v) { v(id), v(item), v(level), v(quantity), v(pos), v(deleted), v(yaw_deg); }
};

// Critical-attack key sequence (HUD object 0xa4f820, Part C §4.4).
struct CritSequence {
    bool active = false;
    std::string keys;
    int progress = 0;
    int start = 0;
    template <class V> void visit(V& v) { v(active), v(keys), v(progress), v(start); }
};

struct UnitView {
    int id = 0;
    std::string class_code, model;
    Vec3 pos;
    float yaw_deg = 0;
    int action = 0;
    std::string clip;        // AM2 clip name (UPPERNAME) in anim/<file>.am2
    std::string clip_file;
    float clip_time_ms = 0;
    int hp = 0, max_hp = 0, ep = 0, max_ep = 0;
    int nation = 0, team = 0, level = 1;
    bool dead = false;
};
struct ProjectileView { int id = 0, type = 0; Vec3 pos, vel; int item = 0; };
struct Snapshot {
    int time_ms = 0;
    std::vector<UnitView> units;
    std::vector<ProjectileView> projectiles;
};

// Extension seams for AI (AiTroops). execute_orders: phase 2, think: phase 3 (0x49d810 timing).
class IUnitBrain {
public:
    virtual ~IUnitBrain() = default;
    virtual void execute_orders(World&, Unit&) {}
    virtual void think(World&, Unit&) {}
};
// Character physics replacement (AiTroops). Called once per tick in the physics slot.
class IUnitPhysics {
public:
    virtual ~IUnitPhysics() = default;
    virtual void integrate(World&, float dt_s) = 0;
};

class World {
public:
    static constexpr int kTickMs = 33;
    static constexpr float kUnitsPerMetre = 46.1f;   // 0x9cf780

    World(GameData& gd, const ITerrainQuery* terrain, std::uint32_t seed = 0);

    // --- setup -----------------------------------------------------------------------------
    // Unit_init 0x4f8550 (+ set-level 0x523190 when level > class level). Returns recno, 0 on error.
    int spawn_unit(std::string_view class_code, int nation, Vec3 pos, float yaw_deg, int level = 0);
    void delete_unit(int id);
    int add_item(std::string_view code, int level, int quantity, Vec3 pos);
    void del_item(int id);
    Unit* unit(int id);
    const Unit* unit(int id) const;
    std::vector<Unit>& units() { return units_; }        // index = recno-1; check .deleted
    const std::vector<Unit>& units() const { return units_; }
    WorldItem* item(int id);
    std::vector<WorldItem>& items() { return items_; }
    const std::vector<WorldItem>& items() const { return items_; }
    int player_id() const { return player_; }
    void set_player(int id);                             // switch_controllable_unit
    int difficulty = 1;                                  // config 0xa57fc2: 0 easy .. 3 very difficult
    int game_mode = 3;                                   // 0xa5b9f8 = controller +0x10 (0 kbd, 1 joy, 2 click, 3 kbd+mouse, 4 RTS); default input method 3
    int scenario_no = 0;                                 // 0xa58fa7 scenario no (chapter 1..8, 0 stand-alone); mission layer, not serialized
    void set_hostile(int nation_a, int nation_b, bool hostile);
    bool hostile(int nation_a, int nation_b) const;      // Diplomacy_isHostile 0x4b6860
    bool hostile_to_player(const Unit& u) const;         // 0x5094d0

    Rng& rng() { return *rng_; }
    void use_rng(Rng* shared) { rng_ = shared ? shared : &own_rng_; }
    Rng& cosmetic_rng() { return cosmetic_; }
    int now() const { return now_; }
    GameData& data() { return gd_; }
    const GameData& data() const { return gd_; }
    const ITerrainQuery* terrain() const { return terrain_; }
    void set_terrain(const ITerrainQuery* t) { terrain_ = t; }

    // --- tick ------------------------------------------------------------------------------
    // Player controller (one frame) + stand-in pads, clock += 33, UnitArray_process (all units,
    // all phases, original pass order), projectile manager (#9), Trigger_process (#10).
    void tick_units(const InputFrame& in);
    // Physics integrator 0x6ad660 (projectiles; unit bodies unless an IUnitPhysics is set).
    void tick_physics();
    void tick(const InputFrame& in) { tick_units(in); tick_physics(); }

    PlayerController& controller() { return pc_; }
    const PlayerController& controller() const { return pc_; }
    // Drive a non-player unit with a virtual pad (stand-in AI). The source is not serialized.
    void attach_pad(int unit, std::function<InputFrame(World&, const Unit&)> source);
    void set_brain(IUnitBrain* b) { brain_ = b; }
    void set_physics(IUnitPhysics* p) { physics_ = p; }
    // Receiver for target_kind 2 (map object) triggers: gates, walls, kegs (mission layer).
    std::function<void(const Trigger&)> object_trigger;
    // Owner nation (+0x08) of a map item, for the keg damage 0x49d5f0 (mission layer; 0 when absent).
    std::function<int(int)> object_nation;
    // Ladder climbing (mission layer, map items): consumes the player's input while climbing / at the
    // 0x42d300 interact slot. Returns true when it handled the frame.
    std::function<bool(Unit&, const InputFrame&)> ladder_input;
    // Player interact key (R) handlers 0x43bef0, 0x43bd40, 0x43be40, 0x43c210, 0x43bc50, 0x43bfe0,
    // 0x43c080 in that order (mission layer: interact_player.cpp). True when one consumed the key.
    std::function<bool(Unit&)> interact;
    // Point & click (mode 2) interact key: only the item handler 0x43bc50 (PlayerCtrl_mouse_click 0x438b30).
    std::function<bool(Unit&)> interact_item;
    // Unit_auto_pickup 0x49da30 (unit phase 0): floor items walked over (mission layer: items_pickup.cpp).
    std::function<void(Unit&)> auto_pickup;
    // Physics_sweep_structures 0x693750 mesh test (mission layer: MapObjects::structure_ray). item == 0:
    // nearest map item on from->to (world units); item > 0: that item's event data (.item 0 if gone).
    std::function<StructureHit(int item, Vec3 from, Vec3 to)> structure_ray;

    // --- output ----------------------------------------------------------------------------
    const std::vector<GameEvent>& events() const { return events_; }
    std::vector<GameEvent> take_events() { return std::exchange(events_, {}); }
    void emit(Ev type, int unit, int other = 0, int value = 0, std::string name = {});
    Snapshot snapshot() const;

    void serialize(std::vector<std::uint8_t>& out) const;
    bool deserialize(std::span<const std::uint8_t> in);
    std::uint64_t hash() const;   // FNV-1a over serialize()

    // --- rules (original functions; see the doc for addresses) --------------------------------
    bool set_action(Unit& u, int action, char letter = 0);   // Unit_set_action 0x501190
    void stop(Unit& u);                                      // Unit_stop 0x508b90
    bool can_act(const Unit& u) const;                       // 0x500ba0
    bool dead(const Unit& u) const { return u.hp < 1 && u.lives == 0; }   // 0x42d0e0
    int anim_length(const Unit& u) const;                    // 0x504760 (ms)
    int anim_remaining(const Unit& u) const;                 // 0x5045f0 (ms, 0 when finished)
    const MoveRecord* move(const Unit& u) const;             // current animgrp record (+0xa)
    const MoveRecord* find_move(const Unit& u, int action, std::string_view combo = {}, int posture = -1) const;
    const AnimScript* script_of(const MoveRecord* r) const;
    float rel_angle(const Unit& self, Vec3 target) const;    // 0x505a00, degrees [-180,180], + = clockwise/right
    void set_yaw(Unit& u, float deg);       // Unit_set_yaw_deg 0x50abd0: desired heading (+0x1e); the body turns in phase 1
    void set_yaw_now(Unit& u, float deg);   // 0x50abd0 + Unit_set_yaw_rad 0x4ff870: body and goal at once (mount follows)

    int max_hp(const Unit& u) const;   // 0x523320
    int max_ep(const Unit& u) const;   // 0x523700
    int stat_str(const Unit& u) const; // 0x5237e0
    int stat_dex(const Unit& u) const; // 0x523860
    int stat_def(const Unit& u) const; // 0x5238c0
    int stat_lead(const Unit& u) const;// 0x523920
    // Inv_bonus 0x497e10 over u's inventory; enhancer requirements 0x4a42f0 are tested on `req` (the recno
    // argument: u itself everywhere except Unit_set_morale, which passes the leader).
    int bonus(const Unit& u, int effect, const Unit* req = nullptr) const { return u.inv.bonus(gd_, effect, this, req ? req : &u); }
    bool item_applies(const ItemDef& d, int level, const Unit& r) const;  // 0x4a42f0
    int team_morale(const Unit& u) const;                                // 0x5103c0
    void set_hp(Unit& u, int v);       // 0x4fe510
    void set_ep(Unit& u, int v);       // 0x4fe5d0
    void set_balance(Unit& u, int v);  // 0x4fea80
    void set_morale(Unit& u, int v);   // 0x4fe8c0

    // combat (combat_melee.cpp)
    float melee_attack_power(const Unit& a) const;                      // 0x5104f0
    float attack_skill_mult(const Unit& a) const;                       // 0x510750
    int skill_level(const Unit& u, int type, std::string_view str) const;  // 0x50d3d0
    bool combo_learned(const Unit& u, const MoveRecord& r) const;       // COMBOAVAIL 'Y' or NEW_COMBO learned (0x50d170)
    bool can_use_special(const Unit& u, int action) const;              // 0x500d40
    float apply_melee_damage(Unit& d, Unit& a, int def_ignore);         // 0x509950
    float side_factors(float dmg, int att_nation, int def_nation) const; // F_ATTACK/F_DEFEND tail of 0x509950/0x509e90/0x49d5f0
    float try_melee_hit(Unit& d, Unit& a);                              // 0x5048b0
    void knockdown(Unit& u);                                            // 0x509000
    void unit_die(Unit& victim, int killer);                            // 0x7c6240
    void post_trigger(const Trigger& t) { triggers_.push_back(t); }
    void crit_key(char key, bool reset);                                // 0x462810

    // ranged / siege (projectile_sim.cpp)
    float ranged_base_damage(const Unit& a) const;                      // 0x510bf0
    float apply_ranged_damage(Unit& d, Unit& a, int type, int p);       // 0x509e90
    float try_ranged_hit(Unit& d, Unit& a, int type, bool ignore_down); // 0x5051e0
    int spawn_projectile(Unit& shooter);                                // 0x4ea700 + launch 0x5133e0
    bool ai_shoot(Unit& u, int target);                                 // AI fire start (SHTLOAD)
    void ai_reaim(Unit& u, int type);                                   // draw end re-aim (0x7c7ab0 cases 0x30/0x32)
    std::vector<Projectile>& projectiles() { return projectiles_; }

    // progression (progression_xp.cpp)
    void add_xp(Unit& u, int xp, bool share, bool quiet);  // EXPUP 0x5229a0
    int level_up(Unit& u, bool quiet);                     // LEVELUP 0x522c40, returns levels gained
    void set_level(Unit& u, int level);                    // 0x523190 (+ AI auto-upgrade 0x4a95a0)
    void unlock_upgrades(Unit& u, int level);              // 0x523240
    bool apply_upgrade(Unit& u, int entry);                // 0x4949f0
    void ai_auto_upgrade(Unit& u);                         // 0x4a95a0
    bool allocate_point(Unit& u, int type, bool undo, int entry = -1);  // 0x50d640
    int command_points = 0;                                // DAT_00a5b9f0
    bool stop_unit_process = false;        // DAT_00a596ab (script set_stop_unit_process_flag)
    bool critical_attack_enabled = true;   // DAT_00a4f858 (script set_critical_attack_enabled)
    bool cut_scene = false;                // Game+0x744 (Game_is_cutscene 0x4301c0): special scheduler mode

    // items (items_inventory.cpp)
    bool use_recovery_item(Unit& u, bool energy);          // F3 0x43b490 / F4 0x43b990
    bool recover(Unit& u, int item, bool force);           // potion effect 0x50b4f0 / 0x50ba00 / 0x50bcd0 (team share)
    bool give_item(Unit& u, std::string_view code, int level, int quantity);
    bool equip(Unit& u, int bag_index);                    // move a bag weapon/armour into its slot
    bool use_item(Unit& u, int bag_index);                 // inventory right-click 0x6d8ac0 (potions, armour)
    bool unequip(Unit& u, int slot);                       // 0 melee0, 1 melee1, 2 ranged, 3 ammo -> bag

    // player-side HUD state that gameplay reads
    int combo_hits = 0, combo_hits_ts = 0;                 // DAT_00a5ba62 (XP bonus)
    CritSequence crit;

    struct Impl;
private:
    friend struct PlayerController;
    void unit_process(Unit& u, int phase);
    void update_action(Unit& u);                           // Unit_updateAction 0x7c7ab0
    void unit_move(Unit& u);                               // Unit_move 0x4f0b50
    void root_motion(Unit& u);                             // Unit_process_movement 0x52daa0 root-track branch
    bool turn_step(Unit& u);                               // 0x4f2190: body toward yaw_goal; true = still turning
    void regen(Unit& u);
    void process_triggers();
    void deliver_trigger(const Trigger& t);
    int on_attack_message(Unit& d, const Trigger& t);      // Unit_onMessage 0x75d540 (2 = stop scan)
    void process_projectiles();                            // 0x4913c0
    void integrate_projectiles(float dt_ms);               // 0x6a8060
    void integrate_units(float dt_s);
    void projectile_hit_unit(Projectile& p, Unit& victim);
    int sweep_structures(Projectile& p, Vec3 from, Vec3 vel_m, float dt_ms, float& t);   // 0x693750 look-ahead
    void projectile_hit_object(Projectile& p, int item);  // ThrowItem_on_hit_object 0x4fbc90
    void idle_pick(Unit& u);                               // FUN_00429ec0 (IDLE2_CH / IDLE3_CH)
    void projectile_ground(Projectile& p);
    void schedule_cues(Unit& u, const MoveRecord& r);
    void emit_cues(Unit& u);
    void crit_check(Unit& u);                              // Player_critCheck 0x416680
    void roll_drop(Unit& u);

    GameData& gd_;
    const ITerrainQuery* terrain_;
    Rng own_rng_, cosmetic_;
    Rng* rng_ = &own_rng_;
    int now_ = 0;
    int player_ = 0;
    std::vector<Unit> units_;
    std::vector<WorldItem> items_;
    std::vector<Trigger> triggers_;
    std::vector<Projectile> projectiles_;
    int next_projectile_ = 1;
    int hostile_[8][8] = {};   // [nation][nation], nations 0..7
    int pass_cursor_[13] = {};
    PlayerController pc_;
    std::map<int, PlayerController> pad_ctrl_;
    std::map<int, std::function<InputFrame(World&, const Unit&)>> pads_;
    IUnitBrain* brain_ = nullptr;
    IUnitPhysics* physics_ = nullptr;
    std::vector<GameEvent> events_;
};

}  // namespace joa::game
