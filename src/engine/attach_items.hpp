#pragma once
// Unit look-up for rendering: unit/unit.res record -> body mesh, idle clip (STD.SET ANIMATE
// tables), shadow model and the equipment meshes hung on skeleton bones (item/item.res).
// Evidence: docs/GRAPHICS_ROADMAP.md "First original scene".
#include "formats/res_archive.hpp"
#include "formats/vfs_fs.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::engine {

struct UnitLook {
    std::string model;   // game path, e.g. "unit/english_archer.am1"
    std::string anim;    // game path of the idle clip's file, e.g. "anim/english_archer_share.am2"
    std::string idle;    // clip in `anim`, e.g. "F_IDLE"
    std::string shadow;  // game path of ShadowModel ("model/shadow_bitmap.SMO"), empty = none
    struct Item {
        std::string model;  // game path, "item/e_lisword.smo"
        std::string bone;   // skeleton node name, "bip01 R Hand"
    };
    std::vector<Item> items;
};

class UnitDb {
public:
    explicit UnitDb(const formats::Vfs& vfs);
    // `code` is the unit.res record (== upper-case .uni UnitFile / script unit code).
    std::optional<UnitLook> look(std::string_view code) const;

private:
    std::optional<formats::ResArchive> units_, items_, std_set_;
};

}  // namespace joa::engine
