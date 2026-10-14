#pragma once
// Unit look-up for rendering: unit/unit.res record -> body mesh, idle clip (STD.SET ANIMATE
// tables), shadow model and the equipment meshes hung on skeleton bones (item/item.res).
// Evidence: docs/GRAPHICS_ROADMAP.md "First original scene".
#include "formats/res_archive.hpp"
#include "formats/vfs_fs.hpp"
#include "formats/world_text.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::engine {

struct UnitLook {
    std::string model;   // game path, e.g. "unit/english_archer.am1"
    std::string anim;    // game path of the idle clip's file, e.g. "anim/english_archer_share.am2"
    std::string idle;    // clip in `anim`, e.g. "F_IDLE"
    std::string anim_code;  // unit.res Animation / AnimationName (STD.SET ANIMATE code) of the model level
    std::string shadow;  // game path of ShadowModel ("model/shadow_bitmap.SMO"), empty = none
    struct Item {
        std::string model;  // game path, "item/e_lisword.smo"
        std::string bone;   // skeleton node name, "bip01 R Hand"; empty = NONE (not carried)
    };
    std::vector<Item> items;
};

// Animated item meshes (bows, potions, pickup models) take their clips from item/anim/<name>.am2;
// empty for static .SMO meshes.
std::string item_anim(std::string_view model);

class UnitDb {
public:
    explicit UnitDb(const formats::Vfs& vfs);
    // `code` is the unit.res record (== upper-case .uni UnitFile / script unit code). `items` = the
    // class default equipment (PWeaponId/SWeaponId, else PRangeWeaponId). `level` = Unit model level
    // (+0x3c5): 0 Model/Animation, n the n-th ModelName/AnimationName (armour meshes, broken siege weapons).
    std::optional<UnitLook> look(std::string_view code, int level = 0) const;
    // Mesh of an item.res record at a model level (0 Model = floor/pickup mesh, 1 first ModelName =
    // carried mesh, 2 = projectile in flight) and its AttachBoneName.
    std::optional<UnitLook::Item> item(std::string_view code, int level = 1) const;
    // pscript name of EmitterName slot `slot` (0..3) of an item.res record, empty for none.
    std::string item_emitter(std::string_view code, int slot) const;

private:
    std::vector<formats::KvLine> item_record(std::string_view code) const;
    std::optional<formats::ResArchive> units_, items_, std_set_;
    // item.res records whose Code differs from the archive entry name (fd_bread_s in SMALL_BREAD): Code -> entry.
    std::vector<std::pair<std::string, std::string>> item_alias_;
};

}  // namespace joa::engine
