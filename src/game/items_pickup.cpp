// Unit_auto_pickup 0x49da30 (CONFIRMED structure, see items_pickup.hpp).
#include "game/items_pickup.hpp"

#include <cmath>

#include "game/mission.hpp"

namespace joa::game {

namespace {
// ponytail: the item actor's bounding radius (actor +0xc8 x scale) is a render-side value; a fixed
// 15 units (a dropped potion / sword model) stands in.
constexpr float kItemRadius = 15.0f;
}  // namespace

void install_auto_pickup(Mission& m) {
    m.world().auto_pickup = [&m](Unit& u) {
        World& w = m.world();
        // rank > 3 (+0x104), an active body in a loaded level (+0x339 / +0x2f5), LOD tier <= 3 (+0x22: within 8000
        // of the player), the player's nation (0x49da91: UnitArray_get(player) +2 == unit +2), alive.
        const Unit* pl = w.unit(w.player_id());
        if (u.rank <= 3 || !u.m.active || u.lod_tier > 3 || !pl || pl->nation != u.nation || w.dead(u)) return;
        const GameData& gd = w.data();
        const UnitClass& cls = gd.units[size_t(u.cls)];
        const float reach = cls.cyl_radius + kItemRadius;   // item radius + unit radius (actor +0xf0)
        const bool player = u.id == w.player_id();           // *unit == DAT_00a5b9e8: SMS lines only for the player
        MissionState& st = m.state();
        // One floor marker (+0 item, +2 quantity, +4 level) at `pos`; `marker` = World item id (> 0) or
        // -MapObjects recno. True = taken: the caller deletes the item (0x49f370).
        const auto take = [&](int item, int level, int quantity, const Vec3& pos, int marker) {
            if (item <= 0 || item >= int(gd.items.size())) return false;
            const ItemDef& d = gd.items[size_t(item)];
            const bool mission = d.item_class == "MISSION";
            // Pickable (+0x1c4); MISSION class items only for heroes (class +0x120).
            if (!d.pickable || (mission && !cls.hero)) return false;
            const float dx = u.pos.x - pos.x, dy = u.pos.y - pos.y;
            if (dx * dx + dy * dy > reach * reach || std::abs(u.pos.z - pos.z) > cls.cyl_height) return false;
            const auto picked = [&] {
                w.emit(Ev::Sound, u.id, 0, 0, "PICKUP");                // 0x494810 at the unit
                w.emit(Ev::Effect, u.id, marker, item, "PICKUP");       // item emitter (+0x17a), 5000 ms
            };
            if (d.type == 0x15 && d.damage < w.max_hp(u) - u.hp) {
                w.recover(u, item, true);   // red potion used at once when it fits (0x50b4f0(item, 0, 1): team share)
            } else if (mission && d.code == "S7_GOLDBAR") {   // FUN_007c07b0 (strcmp) "S7_GOLDBAR"
                ++st.gold_crates;                              // DAT_00a591b6: SMS 11 / 12 (FUN_006e5700 type 1)
                m.emit(MissionEvent::Kind::Ui, st.gold_crates, 0, "sms_gold_crates");
            } else if (mission && d.code.starts_with("S7_food")) {   // strncmp(code, "S7_food", 7)
                if (!st.food_can_be_picked) {                    // DAT_00a591c6 clear: MESSAGE 3 (0x4ad710), stays
                    m.emit(MissionEvent::Kind::Ui, item, 0, "food_blocked");
                    return false;
                }
                ++st.food_picked;                                // DAT_00a591c2
            } else if (d.item_class == "GOLD") {
                // DAT_00a596c4 += max(1, marker +6) x Price (+0x2e); marker +6 is 0 for every marker (0x456cd0).
                st.gold += d.price;
                if (player) m.emit(MissionEvent::Kind::Ui, item, d.price, "sms_pickup " + d.code);   // SMS 3
            } else if (!w.give_item(u, d.code, level, quantity)) {   // 0x495f40 (fails: item stays)
                return false;
            } else if (player) {
                m.emit(MissionEvent::Kind::Ui, item, quantity, "sms_pickup " + d.code);   // SMS 3 (MessageName, +2)
            }
            picked();
            return true;
        };
        for (WorldItem& it : w.items())
            if (!it.deleted && take(it.item, it.level, it.quantity, it.pos, it.id)) it.deleted = true;   // 0x49f370
        // Map items with a floor marker (box drops FUN_00463ed0, script add_item 0x651ad0): same marker list.
        MapObjects& o = m.objects();
        for (int r = 1; r <= o.size(); ++r)
            if (const MapItem* it = o.item(r); it && it->on_floor && take(it->res, it->level, it->quantity, it->pos, -r))
                o.del_item(m, r);
    };
}

}  // namespace joa::game
