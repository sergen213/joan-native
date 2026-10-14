// Campaign order and the progression the original carries between missions (profile.cpp,
// Save/Player.dat). Spec: docs/SAVES_AND_CAMPAIGN.md; RE: GAMEPLAY_AI_TROOPS.md Part D §4.
//
// Campaign = scenario/scenario_01..08.scr (file number = campaign index; StepNo 1 everywhere):
//   1 S1L1, 2 S3L1, 3 S2L1, 4 S4L1, 5 S5L1, 6 S9L1, 7 S7L1, 8 S6L1.
// Win -> game-end menu CONTINUE (FUN_00720ca0): store heroes (FUN_0051ff00), scenario generals
// (FUN_004cdbc0), item-hint lists, gold, play time and command points into the profile, set
// DAT_00a58f95 = 1, then start scenario no+1 (no == 8 -> ending). The next mission's scripts
// get the heroes back through load_unit(code) (FUNC 86 -> FUN_004c91c0), which is a no-op
// (returns 0) until the profile has been written once.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "formats/scenario_def.hpp"
#include "formats/save_sav.hpp"
#include "formats/vfs_bytes.hpp"
#include "formats/vfs_fs.hpp"
#include "game/savegame_container.hpp"
#include "game/unit_state.hpp"

namespace joa::game {

class GameData;

struct CampaignMission {
    int no = 0;                 // 1..8
    std::string code;           // "SCENARIO_01" (label only)
    formats::ScenarioStep step;
    std::string stem() const;   // ScriptFileName, e.g. "S1L1"
};

// Reads scenario/scenario_01..08.scr. Empty + err when any file is missing.
std::vector<CampaignMission> load_campaign(const formats::Vfs& vfs, std::string* err = nullptr);

// One profile entry (0x77 bytes: char code[0x21], Unit* stored, short member_count, Unit*
// members[]). The original never writes member_count (FUN_004c90a0 stores only the hero), so
// troops are not carried; only the hero unit is.
struct HeroRecord {
    std::string code;  // unit class code (unit.res Code), lookup key (stricmp)
    Unit unit;         // the FUN_004c96f0 subset, see copy_persistent_unit
};

struct CampaignProfile {
    bool continued = false;               // DAT_00a58f95: profile written by a CONTINUE
    std::vector<HeroRecord> heroes;
    std::vector<std::uint16_t> item_hints_a, item_hints_b;  // profile +0x28/+0x2c, +0x30/+0x34
    // +0x38. A new profile holds 100 (shipped Player.dat +0x38 = 100; the mission-1 autosave's
    // Game object has gold 100 right after the profile restore of Battle::load_world).
    std::int32_t gold = 100;
    std::uint32_t play_ms = 0;            // +0x3c
    std::int32_t command_points = 0;      // +0x40
    int difficulty = 1;                   // Config 0xa57fc2: 0 easy, 1 normal, 2 hard, 3 very hard

    HeroRecord* find(std::string_view code);
    void save(ByteWriter& w) const;
    bool load(formats::ByteReader& r);
};

// FUN_004c96f0(src, dst): dst is a freshly initialised unit of src's class; copies name, title,
// rank, kingdom, race, team id, the +0x11c..+0x16e block (lives, level, xp, skill/attribute
// points, hp/ep and their maxima, base stats, armor, loyalty, morale, reputation, mode,
// attack behaviour), the 0x3b bytes at +0x16f (skill columns archery..mountain), the upgrade list
// (+0x1aa), the inventory (+0x1c1) and +0x3c5 (UNKNOWN, not modelled).
void copy_persistent_unit(const Unit& src, Unit& dst);

// CampaignProfile <-> original Save/Player.dat (CONTAINERS_WORLD_SAVES.md §7.2). Each hero becomes
// one entry whose 0x432 Unit image holds the copy_persistent_unit subset (other bytes zero: the
// original writes stale heap pointers there) and whose inventory bag items are re-placed first-fit
// into the original containers (ENHANCER -> +0x12, else +0x16 then +0x1a; items that fit nowhere are
// dropped). Not in Player.dat: `continued` (DAT_00a58f95, set only by CONTINUE) and `difficulty`
// (config byte); import leaves them at their defaults.
formats::PlayerProfile to_player_dat(const CampaignProfile& p, const GameData& gd, std::string_view name = "Player");
CampaignProfile from_player_dat(const formats::PlayerProfile& f, const GameData& gd);

// Unit field visitor -> bytes (uses Unit::visit); shared by HeroRecord and save import.
void write_unit(ByteWriter& w, const Unit& u);
bool read_unit(formats::ByteReader& r, Unit& u);

}  // namespace joa::game
