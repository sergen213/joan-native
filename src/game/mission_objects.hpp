// Mission map objects: the item array 0xa460a0 (named props, gates, walls, boxes, kegs, levers,
// box drops) with its modify-item array 0xa50530, loaded from the map's .stg CreateObject /
// CreateTrigger blocks and created by scripts (add_item, add_item_box). Unit drops stay in
// World::items(); unit inventories are not part of this array (deviation, see the .cpp header).
// Processed once per tick before the units (modify_item FUN_004afb20); World delivers
// target_kind-2 triggers to on_trigger (modify-item vtbl[0x48]).
// CONTRACT (owned by the objects slice; Mission relies on load/tick/save/load_state).
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "formats/vfs_bytes.hpp"
#include "formats/vfs_fs.hpp"
#include "game/savegame_container.hpp"
#include "game/unit_state.hpp"

namespace joa::game {

class Mission;
class NavWorld;
struct Trigger;

// ItemRes record (0x2bf bytes, ItemRes_parse 0x4a0160): the fields the mission layer reads.
// Ids: item/item.res records first (== GameData item ids), then every ITEM\**\*.pro file in
// FindFirstFile order (ItemRes_load 0x49fbb0 / 0x49fdf0).
struct ObjStage {           // damage stage i (1-based stage s uses entry s-1)
    int margin = 0;         // +0x67 HP threshold ("LevelMargin")
    int kind = 0;           // +0x97 0 AnimName, 1 ModelName, 2 Model+Anim, 3 AddModelName, 4 ModelName2
    std::string anim;       // +0x87
    std::string model;      // +0x77 model shown while this stage is current (FUN_004a3f20)
};
struct ObjRes {
    std::string code;       // +0x10 (ItemRes_find_by_name key)
    std::string dir, model; // ITEM\<dir>\ and Model file (.pro objects only; animation lookup)
    std::string basename;   // +0x14 basename of the Name field (MISSION/CROSS items copy it to +0x0c)
    int cls = 0, type = 0;  // +0x00 ITEMCLAS / +0x02 ITEMTYPE (1-based)
    int width = 0, height = 0;  // +0x24 / +0x26 footprint (ModItem_footprint_contains 0x41c5e0)
    int quantity = 1;       // +0x2a
    int min_level = 0;      // +0x36
    int damage = 0;         // +0x4e (-> item +0x72)
    int hp = 0;             // +0x5e
    int not_block = 0;      // +0x3e
    int need_key = 0;       // +0x66
    int struction = 0;      // +0x1c StructionType
    std::vector<ObjStage> stages;  // +0x62 count
    bool pickable = false, unique = false, open_with_interact = false;  // +0x1c4 / +0x25b / +0x25e
    std::string description;  // +0x20 Description (interact prompt text "Press %1s to ..."), raw cp1252
};

// Item (base 0xc2 bytes + per-class tail). Offsets of the original in the comments.
struct MapItem {
    int res = 0;            // +0x0a
    int cls = 0, type = 0;  // +0x4e / +0x50
    int nation = 0;         // +0x08 owner nation
    int modify = 0;         // +0x06 modify-item recno (0 none)
    std::string name;       // +0x0c (64)
    Vec3 pos;               // +0x56
    float dir = 0, pitch = 0, roll = 0;  // +0x62 / +0x66 / +0x6a radians
    float scale = 1;        // +0x6e
    int damage = 0;         // +0x72
    int interact = 0;       // +0x7e trigger bits (0x200 = can interact)
    bool on_floor = false;  // +0xa2 floor marker object present (0x52-byte pickup record)
    int level = 0, quantity = 0;  // marker item level / quantity (FUN_00456cd0)
    int stage = 0;          // +0x52 damage stage (counts down to 0)
    int hp = 0, max_hp = 0; // gate/obstacle/house +0xc2/+0xc6, wall +0xc6/+0xca
    int need_key = 0;       // gate +0xce, box +0xc8
    int invincible = 0;     // gate +0xcf, wall +0xe6
    int attacked = 0;       // gate +0xd0
    int state = 0;          // gate +0xd1: 0 closed, 1 open, 2 closing, 3 opening, 4 close pending
    int final_state = 0;    // gate +0xd2
    int content = 0, content_level = 0;  // box +0xc2 / +0xc4
    int can_delete = 0;     // box +0xc9
    int busy_until = 0;     // end of the current object animation / fade (game ms)
    int fade_until = 0;     // box open fade (actor+0x30 -0.5)
    bool blocking = true;   // gate passage closed in the path mesh (Gate_set_open_collision)
    std::string lever_name; // switch +0xc2 (TriggerItemName)
    int lever_target = 0;   // switch +0x103
    bool deleted = false;
    template <class V> void visit(V& v) {
        v(res), v(cls), v(type), v(nation), v(modify), v(name), v(pos), v(dir), v(pitch), v(roll), v(scale), v(damage),
            v(interact), v(on_floor), v(level), v(quantity), v(stage), v(hp), v(max_hp), v(need_key), v(invincible),
            v(attacked), v(state),
            v(final_state), v(content), v(content_level), v(can_delete), v(busy_until), v(fade_until), v(blocking),
            v(lever_name), v(lever_target), v(deleted);
    }
};

// Permanent .stg CreateTrigger (PLAY_ANIMATION 0x100 on a modify item, PLAY_AMB_SOUND 0x1000).
struct MapTrigger {
    int type = 0;
    int target = 0;         // 0x100: modify recno
    Vec3 pos;
    float radius = 0;
    std::string code;       // 0x1000: sound code
    bool inside = false;    // 0x1000: listener inside last tick (stands in for "already playing")
    template <class V> void visit(V& v) { v(type), v(target), v(pos), v(radius), v(code), v(inside); }
};

class MapObjects {
public:
    bool load(Mission& m, const formats::Vfs& vfs, const std::string& map_dir, const std::string& stem, std::string* err);
    void tick(Mission& m);
    void save(ByteWriter& w) const;
    bool load_state(formats::ByteReader& r);

    // --- item array (1-based recnos, LIFO slot reuse like DynArrayB) ----------------------------
    bool valid(int recno) const { return recno >= 1 && recno <= int(items_.size()) && !items_[recno - 1].deleted; }
    MapItem* item(int recno) { return valid(recno) ? &items_[recno - 1] : nullptr; }
    const MapItem* item(int recno) const { return valid(recno) ? &items_[recno - 1] : nullptr; }
    int size() const { return int(items_.size()); }
    int count() const;                                   // live items
    int create(Mission& m, int res);                     // ItemArray_create 0x49ea90; 0 if res invalid
    void del_item(Mission& m, int recno);                // FUN_0049f370 (no-op if invalid)
    int find_by_name(std::string_view name) const;       // FUN_0049f420 (from top, stricmp)
    int find_by_code(std::string_view code) const;       // FUN_0049f4b0 (from top)
    int modify_item(int modify_recno) const;             // modify recno -> item recno (0 none)
    int modify_count() const { return int(modify_.size()); }   // modify-item array 0xa50534 size
    const std::vector<MapTrigger>& triggers() const { return triggers_; }  // .stg PLAY_ANIMATION / PLAY_AMB_SOUND

    // --- ItemRes ---------------------------------------------------------------------------------
    const ObjRes* res(int id) const { return id >= 1 && id < int(res_.size()) ? &res_[id] : nullptr; }
    int res_by_code(std::string_view code) const;        // ItemRes_find_by_name 0x4a36c0 (0 none)
    int sell_list(std::string_view code) const;          // FUN_004ce900 (1-based, 0 none)

    // --- behaviour ---------------------------------------------------------------------------------
    void on_trigger(Mission& m, const Trigger& t);       // TriggerSys case 2 (+ vtbl[0x48] per class)
    // 0x6542b0/0x654560 tail: modify items from top, class 4 or 1, pos inside [mn,mx] -> nation.
    void set_owner_in_box(Vec3 mn, Vec3 mx, int nation);
    // Box content from a drop list (FUN_00463ed0 + DropList_roll 0x464030, level = MinLevel) or an
    // item code (level, -1 = MinLevel). No-op for non-boxes / unknown codes.
    void box_content(Mission& m, int recno, std::string_view code, int level = -1);
    // FUN_004c4440 (change_all_item_in_place): modify items from top with ItemRes == old_res and
    // pos inside [mn,mx] are recreated as new_res at the same pos/angles, owner = nation.
    void change_in_box(Mission& m, Vec3 mn, Vec3 mx, int old_res, int new_res, int nation);
    bool hostile(Mission& m, int a, int b) const;        // Diplomacy FUN_004b6860

private:
    void load_res(Mission& m, const formats::Vfs& vfs);
    int clip_ms(int res_id, int stage, std::string_view clip);   // clip length of the stage's model
    bool footprint(const MapItem& it, Vec3 p, int r) const;   // ModItem_footprint_contains 0x41c5e0
    void deliver(Mission& m, int ir, const Trigger& t);
    void gate_event(Mission& m, int ir, const Trigger& t);    // Gate_on_event 0x48caf0
    void wall_event(Mission& m, int ir, const Trigger& t);    // Wall_on_event 0x54ffd0
    void box_event(Mission& m, int ir, const Trigger& t);     // 0x48a1f0
    void obstacle_event(Mission& m, int ir, const Trigger& t, bool house);  // 0x493680 / 0x48ef50
    void keg_event(Mission& m, int ir, const Trigger& t);     // Keg_on_event 0x48fa50
    void lever_event(Mission& m, int ir, const Trigger& t);   // Lever_on_trigger 0x479270
    void gate_set_open(Mission& m, int ir, bool open);        // Gate_set_open 0x48d2b0
    bool gate_collision(Mission& m, int ir, bool close);      // Gate_set_open_collision 0x48d4b0
    void gate_nav(const MapItem& g);                          // path-mesh poly under the gate <- g.blocking
    bool stage_down(Mission& m, int ir, int fade_ms, bool wall_kinds);
    void box_drop(Mission& m, int ir);
    bool dead(Mission& m, const MapItem& it) const;           // vtbl[0x44]
    void process(Mission& m, int ir);                         // vtbl[0xc]
    bool hit_test(Mission& m, const MapItem& it, int unit, int mode) const;   // FUN_0049a010
    bool interact_ok(Mission& m, const MapItem& it, int unit) const;          // FUN_00428600
    void install_hooks(Mission& m);
    void post(Mission& m, int type, int kind, int target, int src_kind, int sender, Vec3 pos, float radius, int param = 0);

    std::vector<ObjRes> res_;                     // [0] = null record
    std::map<std::string, int> res_index_;        // upper(code) -> first id
    std::vector<std::string> sell_lists_;         // upper Code per sellitem.res record
    std::map<std::string, int> clip_cache_;       // "<res>:<CLIP>" -> ms (derived, not saved)
    const formats::Vfs* vfs_ = nullptr;
    NavWorld* nav_ = nullptr;                     // Mission::nav() (null without a .pfd)

    std::vector<MapItem> items_;
    std::vector<int> free_;                       // DynArrayB free stack (LIFO)
    std::vector<int> modify_;                     // modify recno-1 -> item recno (0 free)
    std::vector<int> modify_free_;
    std::vector<MapTrigger> triggers_;
};

}  // namespace joa::game
