// Unit object state (joan.exe Unit, size >= 0x432, Unit_init 0x4f8550). Field comments give the
// original byte offset; evidence and confidence per field: GAMEPLAY_CHARACTER_COMBAT.md Part A §1,
// Part B §2.1, Part C §8. Plain copyable data: references to other objects are recnos / indices.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "game/items_inventory.hpp"

namespace joa::game {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    template <class V> void visit(V& v) { v(x), v(y), v(z); }
};

// Action / MOVEMENT enum (+0x215, parser AnimName_to_id 0x415820, CONFIRMED; ids 39/48/57/58/74/81 HIGH).
enum Act : int {
    DEFAULT = 0, IDLE, IDLE2, IDLE3, IDLE_BAT, IDLEUP, IDLEDN, IDLE_T, JATTBI, TIPTOE, WALK, RUN,
    T_LEFT, T_RIGHT, MOVE_L, MOVE_R, MOVE_B, MOVE_FL, MOVE_FR, MOVE_BL, MOVE_BR, JUMP_B, JUMP_R,
    JUMP_L, DOWN, JATTBD, UP, CHAIN, ATTACK, JATTACKB, ENGATK, ENGATK2, ENGATK3, ENGATK4, DOWNATK,
    ATTACKED, ATTKED2, ATTKEDR, BATTKED, LATTKED, RATTKED, DNATKED, DIE, GUARD1, GUARD2, GUARDR,
    BACK, CLIMB, SHTLOAD, SHTLOADC, SHTLOAD2, SHTOUT, SHTOUT2, SHTOUTC, SHTIDLE, SHTIDLEC, SHTOUTH,
    SHTIDLEH, THROW, THROWN, STANDUP, DIZZY, STOP, STOP_B, KNOCKDN, KNOCKDN2, KNOCKDN3, H_IDLE,
    H_WALK, H_BACK, H_RUN, H_STOP_B, H_STOP, H_ATTACK, H_HITED, H_BHITED, H_LHITED, H_RHITED,
    H_DROP, H_MNT_L, H_MNT_R, H_DMNT_L, H_DMNT_R, RUSH, PUSH, PUSH2, PUSH_L, PUSH_R, BATTER_C,
    ACT_COUNT_KNOWN
};
const char* action_name(int a);             // "IDLE", ...; "#n" when unnamed
int action_from_name(std::string_view s);   // case-insensitive; -1 if unknown (0x415820)

// Groups used by code (Part C §1, CONFIRMED).
bool is_attacking(int a);       // IsAttacking 0x510f80
bool is_knocked(int a);         // 0x508fb0: KNOCKDN..KNOCKDN3
bool is_knocked_ext(int a);     // knocked or H_DROP or DNATKED (most hit checks)
bool can_act_state(int a);      // state half of CanAct 0x500ba0
bool is_flinch(int a);          // ATTACKED..RATTKED (0x23..0x28)
bool is_locomotion(int a);     // TIPTOE, WALK, RUN, MOVE_L..MOVE_BR (Unit_move speed table)

// Learned upgrade entry (+0x1aa list; 0x5313d0).
struct UpgradeEntry {
    int upitem = 0;     // index into GameData::upitems
    int unlocked = 0, applied = 0, combo_level = 1;
    template <class V> void visit(V& v) { v(upitem), v(unlocked), v(applied), v(combo_level); }
};

// Sound / AM2 event cue scheduled by SetAction, emitted when due (owner: World phase 1).
struct PendingCue {
    int at_ms = 0;
    int action = 0;      // only emitted while the unit is still in this action
    int kind = 0;        // 0 move-record SOUND, 1 AM2 clip event
    std::string name;
    template <class V> void visit(V& v) { v(at_ms), v(action), v(kind), v(name); }
};

// Script / mission-layer fields (owned by MissionRuntime; World only stores and serializes them).
struct UnitMission {
    int cutscene_process = 0;   // +0x2e
    int control = 0;            // +0x2f
    int hide_cmd = 0;           // +0x30
    int interact_unit = 0, interact_item = 0, interact_stage = 0, can_interact = 0;  // +0x2e3..
    int bribe = 0;              // +0x2eb (2 = bribed)
    int active = 1;             // +0x2ed instantiated in active scene (melee needs attacker active)
    int vision = 2000;          // +0x3c1 (VISION_R)
    int training = 0, saved_lives = 0;  // +0x10c / +0x110
    int task_id = 0, task_state = 0;    // +0x2a5 / +0x2ba
    int task_params[4] = {};            // +0x2aa..+0x2b2
    int order_id = 0;
    int track = 0;                      // +0x3c9
    std::vector<int> crew, reserved;    // +0x378 / +0x37c per class attach point (unit recnos)
    int vehicle = 0, slot = 0;          // +0x384 / +0x388
    int can_attach = 0;                 // +0x38c (1 when the class has attach points)
    int bound = 0;                      // +0x2e2
    int prisoner_can_be_attacked = 0;   // +0x358
    int getting_weapon = 0;             // +0x374
    int physics_disabled = 0;           // +0x3b8
    int siege_tower_state = 0;          // +0x3b4
    int can_rided = 1;                  // +0x3c0
    int cart = 0;                       // +0x1f5
    int cart_driver = 0;                // +0x1f9 (on the cart)
    int bounding = 1;                   // actor+0xb8 collision volume on
    int hide_hp_bar = 0;                // HP-bar object +0x52
    int model_alt = 0;                  // switch_unit_model
    int held_item[2] = {};              // +0x1bd / +0x1bf map-object recno held in hand
    int held_bone[2] = {};              // bone index 1..10 (0x9f748c table), 0 none
    int rider = 0;                      // +0x1e6 on a horse: rider recno
    template <class V> void visit(V& v) {
        v(cutscene_process), v(control), v(hide_cmd), v(interact_unit), v(interact_item), v(interact_stage),
            v(can_interact), v(bribe), v(active), v(vision), v(training), v(saved_lives), v(task_id),
            v(task_state);
        for (int& p : task_params) v(p);
        v(order_id), v(track);
        v(crew), v(reserved), v(vehicle), v(slot);
        v(can_attach), v(bound), v(prisoner_can_be_attacked), v(getting_weapon), v(physics_disabled),
            v(siege_tower_state), v(can_rided), v(cart), v(cart_driver), v(bounding), v(hide_hp_bar), v(model_alt);
        for (int i = 0; i < 2; ++i) v(held_item[i]), v(held_bone[i]);
        v(rider);
    }
};

struct Unit {
    int id = 0;                 // +0x000 recno (1-based)
    int nation = 0;             // +0x002 side (1-based NATION row; 0 none)
    int move_index = -1;        // +0x00a current move (animgrp) record, index into the class move table
    Vec3 pos;                   // +0x00e world position (z up)
    float yaw_deg = 0;          // +0x01e facing in degrees (mirror of radians +0x01a)
    int lod_tier = 1;           // +0x022
    int last_phase1 = 0;        // +0x02a
    int team = 0;               // +0x031 troop id
    std::vector<int> members;   // +0x033 team object (leader only): member recnos
    int leader = 0;             // +0x037 leader recno (0 = none / self)
    int cls = -1;               // +0x03b index into GameData::units
    std::string name, short_name, title;  // +0x03d / +0x07f / +0x0c1
    bool ai_controlled = true;  // +0x102 (0 for the player-controlled hero; speed x AI_MOVE)
    int rank = 0;               // +0x104 (Ranking: 0 INFANTRY..5 KING)
    int kingdom = 0, race = 0;  // +0x108 / +0x10a
    int retreat = 0;            // +0x114 (-1 no HP regen, 1 survive death with HP 1)
    int ts_hp_regen = 0;        // +0x118
    int max_lives = 0;          // +0x11c
    int lives = 0;              // +0x120
    int level = 1;              // +0x124
    int xp = 0;                 // +0x126
    int skill_points = 0;       // +0x12a
    int attr_points = 0;        // +0x12c
    bool level_arrow = false;   // +0x130
    int ts_level = 0;           // +0x131
    int hp = 0;                 // +0x135
    int hp_trail = 0;           // +0x139 (crit trigger snapshot)
    int base_max_hp = 0;        // +0x13d
    int ep = 0;                 // +0x145
    int base_max_ep = 0;        // +0x149
    int ts_ep_regen = 0;        // +0x151 (also: time of last energy attack)
    int str = 0, dex = 0, def = 0, lead = 0;  // +0x155..+0x15b base stats
    int armor_level = 0;        // +0x15d (class EArmorId)
    int armor_item = 0;         // +0x15f
    int loyalty = 0, morale = 0, reputation = 0;  // +0x161 / +0x163 / +0x165
    int mode = 0;               // +0x167 1 ATTACK 2 ACTIVE 3 IDLE
    int attack_behaviour = 1;   // +0x16b 1 NEAREST..5
    // +0x16f..+0x197 skill columns of unit.res (copied at Unit_init).
    int archery = 0, horse_riding = 0, swimming = 0, elephant_riding = 0, wide_animal = 0, preaching = 0,
        power_jump = 0, disguise = 0, persuade = 0, lock_picking = 0, mountain = 0;
    int ai_class = 0;           // AI behaviour class (AiTroops)
    std::vector<UpgradeEntry> upgrades;  // +0x1aa
    int medals = 0, kills = 0, officer_kills = 0;  // +0x1ae / +0x1b0 / +0x1b2
    bool crit_flag = false;     // +0x1b4
    int crit_count = 0, crit_kills = 0;  // +0x1b5 / +0x1b7
    int troop_lost = 0, troop_kills = 0; // +0x1b9 / +0x1bb (leader)
    Inventory inv;              // +0x1c1
    int drop_item = 0, drop_level = 0;   // +0x1c5 / +0x1c7
    int balance = 20;           // +0x1cb fall gauge 0..100
    int ts_balance = 0;         // +0x1cd
    int knock_ms = 0;           // +0x1d1
    int invuln_ms = 0;          // +0x1dd
    bool down_attacked = false; // +0x1e1
    int mount = 0;              // +0x1e2 ridden horse recno
    int ts_state = 0;           // +0x20f time current state began (CanAct)
    int posture = 0;            // +0x213 0 STAND, 1 JUMP
    int action = IDLE;          // +0x215
    int prev_action = IDLE;     // +0x219
    int item_using = 1;         // +0x21b weapon anim set (1SWORD)
    int special = 0;            // +0x21d anim SPECIAL key
    std::string combo;          // +0x227 current combo string (<= 6 chars)
    int combo_idx = 0;          // +0x233 (-1 = finished, lockout)
    std::string idle_variant;   // +0x22d 'A' x random_range(0,3) (HYPOTHESIS: idle-row selector)
    bool follow_queued = false; // +0x235
    int win_open = 0, win_close = 0;  // +0x237 / +0x23b
    int combo_lock = 0;         // +0x23f
    int gravity_scale = 3;      // +0x243
    int think_ts = -1;          // +0x29d (-1 = not yet staggered)
    int target = 0;             // +0x2d7
    int threat = 0;             // +0x2df guard/threat accumulator (short)
    bool no_react = false;      // +0x2e1 (scripted / prisoner)
    int takeoff_ts = 0;         // +0x351
    Vec3 aim_dir;               // +0x365
    int upgrade_ts = 0;         // +0x3bc
    // Actor / physics body (actor+0x4c anim start, +0x5c body).
    int anim_start = 0;
    Vec3 vel;                   // world units / s
    bool airborne = false;
    bool shot_spawned = false;  // fire state: projectile already released this shot
    int range_target = 0;       // +0x2db locked ranged target
    std::vector<PendingCue> cues;
    UnitMission m;
    bool deleted = false;

    float yaw_rad() const { return yaw_deg * 0.017453292f; }

    template <class V> void visit(V& v) {
        v(id), v(nation), v(move_index), v(pos), v(yaw_deg), v(lod_tier), v(last_phase1), v(team), v(members),
            v(leader), v(cls), v(name), v(short_name), v(title), v(ai_controlled), v(rank), v(kingdom), v(race),
            v(retreat), v(ts_hp_regen), v(max_lives), v(lives), v(level), v(xp), v(skill_points), v(attr_points),
            v(level_arrow), v(ts_level), v(hp), v(hp_trail), v(base_max_hp), v(ep), v(base_max_ep),
            v(ts_ep_regen), v(str), v(dex), v(def), v(lead), v(armor_level), v(armor_item), v(loyalty),
            v(morale), v(reputation), v(mode), v(attack_behaviour), v(ai_class), v(archery), v(horse_riding), v(swimming),
            v(elephant_riding), v(wide_animal), v(preaching), v(power_jump), v(disguise), v(persuade), v(lock_picking),
            v(mountain), v(upgrades), v(medals),
            v(kills), v(officer_kills), v(crit_flag), v(crit_count), v(crit_kills), v(troop_lost),
            v(troop_kills), v(inv), v(drop_item), v(drop_level), v(balance), v(ts_balance), v(knock_ms),
            v(invuln_ms), v(down_attacked), v(mount), v(ts_state), v(posture), v(action), v(prev_action),
            v(item_using), v(special), v(combo), v(combo_idx), v(idle_variant), v(follow_queued), v(win_open),
            v(win_close), v(combo_lock), v(gravity_scale), v(think_ts), v(target), v(threat), v(no_react),
            v(takeoff_ts), v(aim_dir), v(upgrade_ts), v(anim_start), v(vel), v(airborne), v(shot_spawned),
            v(range_target), v(cues), v(m), v(deleted);
    }
};

}  // namespace joa::game
