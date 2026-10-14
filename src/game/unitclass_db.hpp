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
    int jumpmaxd = 100, chainpro = 200, cri_time = 10000;
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
    int usable_unit = -1;                // UsableUnit unit class index, -1 any
    bool use_immediately = false;        // +0x23d UseImmediately (POTION)
    bool show_armory = false;            // +0x1e8 ShowArmoryInterface
    int width = 1, height = 1;           // +0x24 / +0x26 Width / Height: bag-grid footprint (0x4577a0)
    // 0x4a4200: value of `effect` for an instance of `level` (nullopt if the item lacks it).
    std::optional<int> effect_value(int effect, int level) const;
    int base_damage(int level) const { return damage + (level - min_level) * inc_damage; }  // 0x4a41c0
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
};

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
    int earmor_id = 0, earmor_item = 0, pweapon = 0, sweapon = 0, prange = 0, srange = 0;  // items: 1-based, 0 none
    std::string drop_list, upgrade_list;
    int str = 0, dex = 0, def = 0, lead = 0;
    int archery = 0, horse_riding = 0, swimming = 0, elephant_riding = 0, wide_animal = 0, preaching = 0,
        power_jump = 0, disguise = 0, persuade = 0, lock_picking = 0, mountain = 0;   // +0x183..+0x197
    int level = 1, loyalty = 0, morale = 0, reputation = 0, behavior = 0, falling = 20;
    int kingdom = 0;
    float cyl_radius = 15, cyl_height = 80;
    int move_table = -1;         // index into GameData::move_tables (lazy, see GameData::moves)
    struct AttachPoint { float x = 0, y = 0, z = 0, dir = 0; int usage = 0, item = 0; };  // usage M1 F2 B3
    std::vector<AttachPoint> attach_points;   // AttachPointCount (+0x1ad..)
};

struct UpItem {
    int type = 0;                // 1 STRENGTH .. 0x10 HORSE_ATTACK_B (0x531ba0)
    std::string code, para1, require;
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

private:
    formats::Vfs vfs_{"."};
    std::optional<formats::ResArchive> stdset_;
    std::map<std::string, int> clip_index_;   // "file|clip" (upper) -> clips index
    std::map<std::string, std::shared_ptr<formats::Animation>> am2_;
    std::map<std::string, std::string> animate_;  // ANIMATE code -> DBF name
    int load_clip(const std::string& file, const std::string& name);
};

}  // namespace joa::game
