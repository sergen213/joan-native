// Equipment visuals (headless, NullRenderer): unit.res model levels / item.res model levels picked by
// UnitDb, Joan's body mesh following the armour item (0x6d8ac0 -> Unit_set_model_level 0x511060), carried
// weapon swap, an arrow in flight drawn with its level-2 mesh pointing along its velocity, a dropped item
// and a box's spilled content drawn on the floor and gone once walked over (Unit_auto_pickup 0x49da30), the
// broken box itself playing its "break" clip in place of its static .stg mesh, then gone with the map item,
// MessageName formats (0x5a6660). Exit 77 without the original data.
#include "check.hpp"
#include "engine/hud_effects.hpp"
#include "engine/renderer_null.hpp"
#include "engine/session.hpp"
#include "engine/world_view.hpp"
#include "formats/model_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace joa;
using namespace joa::engine;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    const formats::Vfs vfs(dir ? dir : ".");
    if (!vfs.exists("map/scenario1/S1L1.gri") || !vfs.exists("unit/unit.res")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }

    // --- model selection --------------------------------------------------------------------------
    const UnitDb db(vfs);
    const auto model = [&](const char* code, int level) { const auto l = db.look(code, level); return l ? lower(l->model) : std::string(); };
    const auto item = [&](const char* code, int level) { const auto i = db.item(code, level); return i ? lower(i->model) : std::string(); };
    CHECK(model("JOAN_OF_ARC", 0) == "unit/joan_armorc.am1");   // JOAN_ARMOR_LV1 (Damage 0)
    CHECK(model("JOAN_OF_ARC", 1) == "unit/joan_of_arc.am1");   // JOAN_ARMOR_LV2 (Damage 1)
    CHECK(model("JOAN_OF_ARC", 2) == "unit/joan_armorb.am1");   // JOAN_ARMOR_LV3 (Damage 2)
    CHECK(model("JEAN_DE_METZ", 0) == "unit/jean_de_metz.am1");
    CHECK(model("CANNON", 1) == "unit/broken_cannon.am1");
    CHECK(db.look("CANNON", 1) && db.look("CANNON", 1)->anim_code == "BROKEN_CANNON");
    CHECK(item("JOAN_SWORD_LV1", 1) == "item/sw_warbrand.smo" && db.item("JOAN_SWORD_LV1")->bone == "bip01 R Hand");
    CHECK(item("JOAN_SWORD_LV2", 1) == "item/sw_flamberge.smo");
    CHECK(item("ARROW", 0) == "item/arrow.am1");
    CHECK(item("ARROW", 2) == "item/arrow01_hold.smo");
    CHECK(item("CROSSBOW_ARROW", 2) == "item/crossbow_arrow_shootout.smo");
    CHECK(item("CANNONBALL", 2) == "item/cannonball.smo");
    CHECK(item("SMALLRED", 0) == "item/smallred.am1" && db.item("SMALLRED", 0)->bone.empty());
    CHECK(db.item_emitter("JOAN_SWORD_LV1", 2) == "weapon_eaten");
    CHECK(db.item_emitter("ARROW_EXPLOSIVE", 3) == "explosion_arrow");
    CHECK(item_anim("item/SmallRed.AM1") == "item/anim/SmallRed.am2" && item_anim("item/sw_Warbrand.smo").empty());

    // --- live mission 1 -----------------------------------------------------------------------------
    NullRenderer r;
    Session s(vfs);
    s.seed = 7;
    std::string err;
    CHECK(s.new_game(1, 1, &err));
    game::Mission& m = *s.mission();
    m.autosave = false;
    game::World& w = m.world();
    WorldView view(r, vfs, m);
    std::vector<DrawItem> draws;
    std::vector<game::MissionEvent> events;  // mission events of the ticks since the last clear
    const auto tick = [&](int n = 1) {
        for (int i = 0; i < n; ++i) {
            if (m.ui().cut_scene_status()) m.ui().skip_cut_scene(m);
            s.tick({});
            view.sync(w, s.previous(), s.current());
            view.on_events(s.take_world_events(), s.current());
            for (game::MissionEvent& e : s.take_mission_events()) events.push_back(std::move(e));
            FrameView fv;
            draws.clear();
            CameraPose pose;
            if (const game::Unit* p = w.unit(w.player_id())) {
                pose.target = to_engine(p->pos.x, p->pos.y, p->pos.z + 55.0f);
                pose.eye = pose.target + glm::vec3(0, 60, 190);
            }
            view.frame(0.5f, pose, float(kGameTickSeconds), fv, draws);
        }
    };
    tick(5);
    game::Unit& joan = *w.unit(w.player_id());
    const int joan_id = joan.id;
    std::fprintf(stderr, "joan model %s armour %d level %d\n", view.actor_model(joan_id).c_str(), joan.armor_level, joan.model_level);
    CHECK(lower(view.actor_model(joan_id)) == "unit/joan_armorc.am1");

    // Armour LV3: consumed, armour level 3, mesh joan_armorB; a lower armour afterwards changes nothing.
    const auto bag_index = [&](const char* code) {
        for (std::size_t i = 0; i < joan.inv.bag.size(); ++i)
            if (w.data().items[std::size_t(joan.inv.bag[i].item)].code == code) return int(i);
        return -1;
    };
    CHECK(w.give_item(joan, "JOAN_ARMOR_LV3", 0, 1));
    CHECK(w.use_item(joan, bag_index("JOAN_ARMOR_LV3")));
    CHECK(joan.armor_level == 3 && joan.model_level == 2 && bag_index("JOAN_ARMOR_LV3") < 0);
    tick();
    CHECK(lower(view.actor_model(joan_id)) == "unit/joan_armorb.am1");
    CHECK(w.give_item(joan, "JOAN_ARMOR_LV2", 0, 1) && w.use_item(joan, bag_index("JOAN_ARMOR_LV2")));
    CHECK(joan.armor_level == 3 && joan.model_level == 2);

    // Sword LV2 (Flamberge) replaces the Warbrand in the right hand.
    const int attached = view.unit_stats().items;
    CHECK(w.give_item(joan, "JOAN_SWORD_LV2", 0, 1) && w.equip(joan, bag_index("JOAN_SWORD_LV2")));
    tick();
    CHECK(view.unit_stats().items > attached && view.unit_stats().items_missing == 0);

    // An arrow in flight: drawn with ARROW's level-2 mesh, interpolated, gone when it ends.
    joan.inv.ammo = {w.data().item_index("ARROW"), 10, 1};
    joan.aim_dir = {std::cos(joan.yaw_rad()), std::sin(joan.yaw_rad()), 0.0f};
    const int shot = w.spawn_projectile(joan);
    CHECK(shot > 0);
    tick();
    std::fprintf(stderr, "shot %d model %s shots drawn %d\n", shot, view.shot_model(shot).c_str(), view.item_stats().shots);
    CHECK(lower(view.shot_model(shot)) == "item/arrow01_hold.smo" && view.item_stats().shots >= 1);
    // The arrowhead flies first: the mesh's head end (the narrow tip; the fletching end carries the vanes)
    // turned by the drawn matrix points along the velocity (ThrowItem_launch 0x5133e0 yaw = velocity heading).
    {
        formats::Mesh mesh;
        std::string merr;
        const auto bytes = vfs.read("item/arrow01_hold.smo");
        CHECK(bytes && formats::parse_mesh(*bytes, false, mesh, merr));
        float lo = 1e9f, hi = -1e9f;
        for (const auto& sm : mesh.submeshes)
            for (const auto& v : sm.vertices) lo = std::min(lo, v.pos[0]), hi = std::max(hi, v.pos[0]);
        // Widest cross-section (|y|, |z| off the shaft) within the last 25 % of each end.
        float lo_w = 0, hi_w = 0;
        for (const auto& sm : mesh.submeshes)
            for (const auto& v : sm.vertices) {
                const float r = std::hypot(v.pos[1], v.pos[2]);
                if (v.pos[0] < lo + 0.25f * (hi - lo)) lo_w = std::max(lo_w, r);
                if (v.pos[0] > hi - 0.25f * (hi - lo)) hi_w = std::max(hi_w, r);
            }
        const float head = hi_w < lo_w ? 1.0f : -1.0f;  // mesh +X or -X (original axes) is the arrowhead
        const auto& pv = s.current().projectiles;
        const auto p = std::find_if(pv.begin(), pv.end(), [&](const game::ProjectileView& q) { return q.id == shot; });
        CHECK(p != pv.end());
        if (p != pv.end()) {
            const glm::vec3 fwd = glm::mat3(view.shot_frame(shot)) * to_engine(head, 0.0f, 0.0f);
            const glm::vec3 vel = to_engine(p->vel.x, p->vel.y, p->vel.z);
            std::fprintf(stderr, "arrow mesh x [%.1f, %.1f] end widths %.2f / %.2f, head %+.0f X, head . velocity %.3f\n", lo, hi,
                         lo_w, hi_w, head, glm::dot(glm::normalize(fwd), glm::normalize(vel)));
            CHECK(glm::dot(glm::normalize(fwd), glm::normalize(vel)) > 0.9f);
        }
    }
    tick(150);
    CHECK(view.shot_model(shot).empty());

    // A floor item 200 units ahead is drawn; walked over (moved under Joan) it is picked up and disappears.
    const game::Vec3 ahead{joan.pos.x + 200.0f, joan.pos.y, joan.pos.z};
    const int drop = w.add_item("SMALLRED", 0, 1, ahead);
    CHECK(drop > 0);
    tick();
    std::fprintf(stderr, "drop %d model %s drops drawn %d missing %d\n", drop, view.drop_model(drop).c_str(), view.item_stats().drops,
                 view.item_stats().missing);
    CHECK(lower(view.drop_model(drop)) == "item/smallred.am1" && view.item_stats().drops == 1 && w.item(drop));
    w.item(drop)->pos = joan.pos;
    tick();
    CHECK(!w.item(drop) && bag_index("SMALLRED") >= 0);
    CHECK(view.drop_model(drop).empty() && view.item_stats().drops == 0);
    CHECK(view.item_stats().missing == 0);

    // Box contents (MapObjects box_drop FUN_00463ed0): a S1L1 box with a Treasure broken by the player's
    // arrows spills its content as a map item with a floor marker, drawn under key -recno; walked over it is
    // picked up (Unit_auto_pickup walks the map floor markers too), deleted, and the SMS line event posted.
    game::MapObjects& o = m.objects();
    const auto floor_items = [&] {
        std::vector<int> v;
        for (int r = 1; r <= o.size(); ++r)
            if (const game::MapItem* it = o.item(r); it && it->on_floor && it->res > 0 && it->res < int(w.data().items.size()) &&
                                                     w.data().items[std::size_t(it->res)].pickable)
                v.push_back(r);
        return v;
    };
    int box = 0;
    for (int r = 1; r <= o.size() && !box; ++r)
        if (const game::MapItem* it = o.item(r); it && it->cls == 10 && it->content && it->modify && o.res(it->res) &&
                                                 !o.res(it->res)->open_with_interact)
            box = r;
    CHECK(box > 0);
    // The box's own mesh is a static .stg object of the scene, tied to the map item by WorldView.
    const std::vector<PlacedObject>& objs = view.scene().objects();
    const auto box_obj = std::find_if(objs.begin(), objs.end(), [&](const PlacedObject& p) {
        const game::Vec3 b = box ? o.item(box)->pos : game::Vec3{};
        return std::hypot(p.at.x - b.x, p.at.y - b.y, p.at.z - b.z) < 0.5f;
    });
    CHECK(box_obj != objs.end() && !box_obj->hidden && view.map_item_clip(box).empty());
    const int box_res = box ? o.item(box)->res : 0;
    const std::vector<int> placed = floor_items();
    const auto spilled = [&] {
        std::vector<int> v;
        for (int r : floor_items())
            if (std::find(placed.begin(), placed.end(), r) == placed.end()) v.push_back(r);
        return v;
    };
    int spill = 0;
    for (int i = 0; i < 300 && box && !spill; ++i) {
        game::Trigger t;
        t.type = 2, t.target_kind = 2, t.target_id = o.item(box) ? o.item(box)->modify : 0, t.src_kind = 1, t.sender = joan.id;
        if (o.item(box)) o.on_trigger(m, t);  // an ally's arrow (Box vtbl 0x9d37cc, type 2)
        tick();
        if (!spilled().empty()) spill = spilled().front();
    }
    CHECK(spill > 0);
    // Broken (stage 1 -> 0, 0x48a1f0): the static box gives way to its AM1 playing the stage AnimName "break";
    // once the clip is over the item is deleted (dead 0x48b900) and nothing of it is drawn. Content dropped once.
    std::fprintf(stderr, "box %d stage %d clip '%s'\n", box, o.item(box) ? o.item(box)->stage : -1, view.map_item_clip(box).c_str());
    CHECK(o.item(box) && o.item(box)->stage == 0 && lower(view.map_item_clip(box)) == "break" && box_obj != objs.end() && box_obj->hidden);
    int wait = 0;
    for (; wait < 300 && o.item(box) && o.item(box)->res == box_res; ++wait) tick();
    std::fprintf(stderr, "box %d deleted after %d ticks, spilled %zu\n", box, wait, spilled().size());
    CHECK((!o.item(box) || o.item(box)->res != box_res) && view.map_item_clip(box).empty() && box_obj->hidden);
    CHECK(spilled().size() == 1);
    if (spill > 0) {
        const game::MapItem& it = *o.item(spill);
        const std::string code = w.data().items[std::size_t(it.res)].code;
        std::fprintf(stderr, "box %d spilled %s (map item %d) model %s, drops drawn %d\n", box, code.c_str(), spill,
                     view.drop_model(-spill).c_str(), view.item_stats().drops);
        CHECK(!view.drop_model(-spill).empty() && view.item_stats().drops == 1);
        o.item(spill)->pos = joan.pos;
        events.clear();
        tick();
        CHECK(!o.item(spill) && view.drop_model(-spill).empty() && view.item_stats().drops == 0);
        CHECK(std::any_of(events.begin(), events.end(), [&](const game::MissionEvent& e) { return e.text == "sms_pickup " + code; }));
    }
    CHECK(view.item_stats().missing == 0);

    // MessageName formats (0x5a6660): %1j picks the N-th word, %1p singular for 0 / 1.
    const std::string bread = w.data().items[std::size_t(w.data().item_index("fd_bread_L"))].message;  // "Large Bread"
    CHECK(ui_format(bread, {"1"}) == "a loaf of bread" && ui_format(bread, {"2"}) == "two loaves of bread");
    CHECK(ui_format("a quiver with %1d %1p{bolt|bolts}", {"12"}) == "a quiver with 12 bolts");
    CHECK(ui_format("%1q{none|one|many}", {"0"}) == "none" && ui_format("%1j{a|b}", {"3"}).empty());
    return test_result();
}
