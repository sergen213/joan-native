// Gameplay databases loaded from the original data:
//   Resource/GAMEPLAY.RES  {UNIT}{CRITICAL}{DANGER}{MINIMAP}{EXP}{R_ATT}{C_BOUNS}{KILL}{CHARMOVE}{GAME}{THROWI}
//                          with the original atoi/atof split (GAMEPLAY_CHARACTER_COMBAT.md §3)
//   unit/unit.res          unit classes (UnitRes 0x525080, Part A §2)
//   item/item.res          items (itemres.cpp loader; Damage +0x4e, IncDamage +0x52, MinLevel +0x36,
//                          Range +0x32, DamageType +0x4a, AdditionalEffect* list +0x24d/+0x251)
//   item/dropitem.res      drop lists ({ITEM : weight})
//   Resource/animscr.res   attack scripts (AnimScript_parse 0x419320, Part C §1.2)
//   Resource/STD.SET       per-unit move tables (ANIMATE -> e.g. JOAN; 0x99-byte animgrp records,
//                          Part A §1d / Part C §1.1), LEVEL, UPGRADE, UPITEM, NATION
//   anim/<UPPERFILE>.am2   clip lengths + clip events (move timing), loaded lazily per file.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "formats/anim_clip.hpp"
#include "formats/res_archive.hpp"
#include "formats/vfs_fs.hpp"

namespace joa::game {

// GAMEPLAY.RES values the simulation reads. Integer keys go through atoi (fraction truncated:
// STR_DAM 3.0->3, DEX_DAM 1.5->1, DEF_RAT 0.3->0), float keys through atof, as the exe does.
struct Consts {
    int body_r = 32, reach_dt = 15, fell_dur = 2500, invc_dur = 6000, str_hp = 0, def_hp = 0;
    float def_fall = 0.2f, def_hp_r = 0.1f;
    int recov_hp = 10000, egatk_mp = 40, egatk_hp = 20, recov_mp = 1, recov_t = 10000, clb_idle = 20;
    int max_com = 2000, cri_diff = 10;
    float critexp = 3.0f;
    int levelarr = 5000;
    float ai_move = 1.2f;
    int mindexs = 30, maxdexs = 200, mor_nor = 50;
    float mor_ch_u = 1.0f, mor_ch_t = 0.3f;
    int wp_damg = 0, amr_def = 1;
    float amrexdef = 0.5f, sie_udam = 1.5f;
    int f_attack = 130, f_defend = 70;
    int arr_accu = 50, blockper = 50, blockang = 70, arr_fall = 25, t_fallf = 6000, v_fallf = 3000, f_fallr = 250;
    int drop_pct[6] = {10, 12, 10, 8, 8, 8};      // LINF HINF POLE MACE ARC CRO (rank < 3)
    int drop_cap_pct[6] = {100, 100, 100, 100, 100, 100};
    int hp_per = 4, ep_per = 3, str_per = 1, def_per = 1, dex_per = 1, lds_per = 1;
    int str_dam = 3, dex_dam = 1, def_rat = 0;
    int upg_time = 600000;
    int bri_comm = 5, bri_capt = 10;   // UNIT BRI_COMM 0xa3cd10 / BRI_CAPT 0xa3cc54 (Unit_bribe 0x4e8500)
    float indanger = 0.2f;
    int vision_r = 2000;
    float mem_exp = 0.4f, ld_exp = 0.4f;
    int fall_s = 800, fall_e = 2400;
    float dist_p = 0.5f;
    int c_bonusl = 50, e_bonusl = 100, m_bonusl = 3;
    int ep_nor = 6, ep_cap = 15, ep_gen = 30;
    float aim_r = 25, aim_mxd = 2500, aim_ha = 60, turn_ang = 6;
    float stop_cc = 0.3f;   // UNIT STOP_CC 0xa3cce4 (rider stop 0x4533e0)
    float idle2_ch = 0.2f, idle3_ch = 0.4f;   // UNIT IDLE2_CH 0xa3cc94 / IDLE3_CH 0xa3cc30 (FUN_00429ec0)
    float idle_cc = 0.01f;                    // UNIT IDLE_CC 0xa3ccf0 (Unit_updateAction 0x7c7ab0 case IDLE)
    int jumpmaxd = 100, chainpro = 200, cri_time = 10000;
    int crit_fade_in1 = 200, crit_fade_in2 = 200, crit_glow_in[3] = {200, 200, 200};   // {CRITICAL} FADE_IN1/2, GLOW_IN1..3 (Crit_drawHUD 0x45fae0)
    float diff_d[4] = {0.7f, 1.0f, 1.5f, 2.0f};   // EASY_D..VDIFF_D (damage of player-hostile units)
    float diff_a[4] = {0.5f, 1.0f, 1.5f, 2.0f};   // EASY_A..VDIFF_A (BLOCKPER / guard chance)
};

// THROWI row (loader 0x5525b0, Part D §1). Types: 0 ARW 1 PROJ 2 EXPL 3 STM 4 STC 5 CAN.
struct ThrowConsts {
    int tl = 0, dl = 0;
    float m = 1, mxd = 0, mnd = 0, fc = 0, tr = 0, av = 0, gl = 0, ze = 0;
    float v0 = 0, ft = 0;   // derived (V0 / FT)
};

struct ItemDef {
    std::string code, name, item_class;  // item_class = ITEMCLAS code (WEAPON, ENHANCER, ...)
    int type = 0;                        // 1-based ITEMTYPE row (0xb bow, 0xd arrow, 0x15 red potion...)
    int range = 0;                       // +0x32 weapon reach
    int damage = 0, inc_damage = 0;      // +0x4e / +0x52 (potions: amount)
    int min_level = 1, max_level = 1;    // +0x36 / +0x38
    int quantity = 1, max_quantity = 1;
    int primary_effect = 0;              // +0x4a DamageType (value = damage/inc_damage)
    struct Extra { int type, value, inc; };
    std::vector<Extra> extra;            // AdditionalEffectType/Value/IncValue
    // EffectiveRequirementType/Value/IncValue/EffectiveWhen (+0x245 n, +0x249 16-byte recs: type, value, inc,
    // BELOW 0 / OVER 1; ItemRes_parse 0x4a0160). Tested by World::item_applies (0x4a42f0) on enhancers only.
    struct Requirement { int type, value, inc; bool over; };
    std::vector<Requirement> require;
    int usable_unit = -1;                // UsableUnit unit class index, -1 any
    bool use_immediately = false;        // +0x23d UseImmediately (POTION)
    bool show_armory = false;            // +0x1e8 ShowArmoryInterface
    bool can_drop = true;                // +0x25c CanDrop (ItemRes_parse default 1; _stricmp keys)
    int width = 1, height = 1;           // +0x24 / +0x26 Width / Height: bag-grid footprint (0x4577a0)
    int defense = 0;                     // +0x56 Defense (ARMOR: armour level reached, 0x6d8ac0)
    int price = 0;                       // +0x2e Price (GOLD: value per piece, Unit_auto_pickup 0x49da30)
    bool pickable = false;               // +0x1c4 Pickable (Unit_auto_pickup 0x49da30)
    std::string message;                 // +0x18 MessageName: SMS pickup / gift text (FUN_005a6660 format, %1 = quantity)
    std::string description, detail_file;  // Description (tooltip, "\n" = break) / DetailDescriptionFilename (item_des/)
    // 0x4a4200: value of `effect` for an instance of `level` (nullopt if the item lacks it).
    std::optional<int> effect_value(int effect, int level) const;
    int base_damage(int level) const { return damage + (level - min_level) * inc_damage; }  // 0x4a41c0
    std::string model, attach_bone;      // Model (item/<model>) / AttachBoneName: ThrowItem_launch 0x5133e0 spawn bone
};

struct AnimScript {
    std::string code;
    int start_time = 0, end_time = 0, range = 0, start_force_time = 0;
    int v0 = 0, v1 = 0, h0 = 0, h1 = 0;  // vertical / horizontal angle limits (deg)
    float gravity = 3.0f;                // GravityLevel
    int force_h = 3000, force_v = 500;   // AttackForce / VerticalAttackForce
};

struct ClipInfo {
    float length_ms = 0;
    std::vector<formats::AnimEvent> events;  // times relative to clip start
    // Root track (FUN_00417f20, built at move-table load for clips of root_motion_action records):
    // "Bip01" model-space x/y relative to the key at the clip start, keys {ms from clip start, dx, dy}
    // after the colinear merge of 0x418820 (|error| <= 1 unit), at most 50 (0x25c-byte record).
    // The original also overwrites those Bip01 keys' x/y with the start key's, so the drawn clip
    // stays in place while the unit body moves (renderers subtract root_offset).
    std::vector<std::array<float, 3>> root;
    bool root_built = false;
};
// Actions whose clips carry root motion: FUN_00415090 list == Unit_process_movement 0x52daa0 list
// (CHAIN, ATTACK, JATTACKB, ENGATK..ENGATK4, ATTACKED, ATTKED2, ATTKEDR, THROW, THROWN, STANDUP,
// RUSH, DOWNATK, JUMP_B/R/L).
bool root_motion_action(int action);
// FUN_00418a00: root offset at t ms (linear from 0 before the first key, held after the last).
void root_offset(const ClipInfo& c, float t, float& x, float& y);

// One animgrp record (0x99 bytes; columns of STD.SET <UNIT> table).
struct MoveRecord {
    int position = 0, movement = 0;
    std::string combo;
    char combo_avail = 'N', chain_avail = ' ';
    int item_using = 1, special = 0, energy_pt = 0, damage = 0;
    int blend = 0, start_time = 0, end_time = 0;
    std::string upper_file, upper_name;
    int script = -1;                       // index into GameData::scripts
    std::string sound, sound2;
    int sound_time = 0, sound_time2 = 0;
    bool show_crit = false;
    int clip = -1;                         // index into GameData::clips (-1: no clip)
};

struct MoveTable {
    std::string code;
    std::vector<MoveRecord> rows;
};

struct UnitClass {
    std::string code, name, short_name, title, model, animation, script_file;
    std::string cfile;           // +0x100 CFileName: HUD portrait stem (MAINGAME "<cfile>1", party "JOANC2"...)
    int unit_class = 1;          // +0x002 CITIZEN0 LIGHT_INFANTRY1 HEAVY_INFANTRY2 POLEMAN3 MACEMAN4 ARCHER5 CROSSBOW_MAN6 CAVALRY7 SIEGE_WEAPON8 CART9
    int special = 0;             // +0x118 NORMAL0 MAIN1 HORSE2 ... AUTOBOW12 CART13 SPECIAL14
    char sex = 'M';              // +0x117 Sex
    int ranking = 0;             // +0x149
    unsigned trigger_mask = 0;   // +0x11c
    bool hero = false;           // +0x120
    int script_duration = 0;     // +0x121
    int height = 0;              // +0x109
    float walk = 0, run = 0, shift_l = 0, shift_r = 0, shift_b = 0;
    int hp = 0, ep = 0, life = 0, experience = 0, gold = 0;
    int resource = 0;            // RecruitWorkshop: +0x165 Resource (SIEGE_WEAPON only): iron / wood workshop cost
    int earmor_id = 0, earmor_item = 0, pweapon = 0, sweapon = 0, prange = 0, srange = 0;  // items: 1-based, 0 none
    std::string drop_list, upgrade_list;
    int str = 0, dex = 0, def = 0, lead = 0;
    int archery = 0, horse_riding = 0, swimming = 0, elephant_riding = 0, wide_animal = 0, preaching = 0,
        power_jump = 0, disguise = 0, persuade = 0, lock_picking = 0, mountain = 0;   // +0x183..+0x197
    int level = 1, loyalty = 0, morale = 0, reputation = 0, behavior = 0, falling = 20;
    int kingdom = 0;
    // CollisionType Cylinder (+0x1c3): centre z +0x21d, radius +0x221, height +0x225 (actor +0xe4.. shape, 0x6a1bb0).
    float cyl_radius = 15, cyl_height = 80, cyl_center_z = 40;
    int move_table = -1;         // index into GameData::move_tables (lazy, see GameData::moves)
    struct AttachPoint { float x = 0, y = 0, z = 0, dir = 0; int usage = 0, item = 0; };  // usage M1 F2 B3
    std::vector<AttachPoint> attach_points;   // AttachPointCount (+0x1ad..)
};

struct UpItem {
    int type = 0;                // 1 STRENGTH .. 0x10 HORSE_ATTACK_B (0x531ba0)
    std::string code, para1, require, desc;   // desc = UPITEM DESC (combo page name, 0x6ef9e0)
    int p1 = 0, cost = 0;
    bool skill() const { return type >= 8 && type <= 16; }   // paid with skill points (0x531ed0)
};

struct LevelRow { int exp = 0, att = 0, ski = 0; };

class GameData {
public:
    // Loads everything except move tables / clips (lazy). nullopt + err on missing files.
    static std::unique_ptr<GameData> load(const std::filesystem::path& game_dir, std::string* err = nullptr);

    Consts c;
    ThrowConsts throwi[6];
    std::vector<UnitClass> units;
    std::vector<ItemDef> items;          // [0] = null item; item ids are indices
    std::vector<AnimScript> scripts;
    std::vector<UpItem> upitems;
    std::map<std::string, std::vector<std::pair<int, int>>> upgrade_lists;  // code -> {level, upitem}
    std::map<std::string, std::vector<std::pair<int, int>>> drop_lists;     // code -> {item, weight}
    std::vector<LevelRow> levels;        // levels[L-1] = row L (EXPERIENCE needed to leave level L)
    std::vector<std::string> nations;    // 1-based ids: nations[id-1]
    std::vector<MoveTable> move_tables;
    std::vector<ClipInfo> clips;

    int unit_index(std::string_view code) const;   // -1
    int item_index(std::string_view code) const;   // 0 = none
    int script_index(std::string_view code) const; // -1
    int nation_id(std::string_view code) const;    // 0 = none
    const LevelRow* level_row(int level) const { return level >= 1 && level <= int(levels.size()) ? &levels[level - 1] : nullptr; }
    // Move table of a unit class, loaded (with its AM2 clips) on first use. nullptr if none.
    const MoveTable* moves(int cls);
    const MoveTable* moves(int cls) const { return cls >= 0 && units[cls].move_table >= 0 ? &move_tables[units[cls].move_table] : nullptr; }
    const ClipInfo* clip(const MoveRecord& r) const { return r.clip >= 0 ? &clips[r.clip] : nullptr; }
    // Item_setup_collision 0x41e850 sphere {cx, cy, cz, r} of an item's actor: ITEMTYPE 13..18 (arrows, bolts) centre 0
    // radius 10, every other item its model's bounding sphere (model +0x34..+0x40). Cached.
    const std::array<float, 4>& item_sphere(int item);
    // Model-space origin of skeleton node `bone` (case-insensitive) in move r's clip at t ms after the clip start.
    bool bone_origin(const MoveRecord& r, const std::string& bone, float t, std::array<float, 3>& out);

private:
    formats::Vfs vfs_{"."};
    std::optional<formats::ResArchive> stdset_;
    std::map<std::string, int> clip_index_;   // "file|clip" (upper) -> clips index
    std::map<std::string, std::shared_ptr<formats::Animation>> am2_;
    std::map<std::string, std::shared_ptr<formats::Skeleton>> am3_;
    std::map<int, std::array<float, 4>> item_sphere_;
    std::map<std::string, std::string> animate_;  // ANIMATE code -> DBF name
    int load_clip(const std::string& file, const std::string& name);
    void build_root(int clip, const std::string& file, const std::string& name);   // FUN_00417f20
};

}  // namespace joa::game
