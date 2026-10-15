// Places, facilities, territories, tracks and nation relations of a mission map (place_array
// 0xa484d0, facility_array 0xa5d100, territory 0xa52940, track arrays of trackres.cpp, nation
// relation records of nation array 0xa57f80). Loaded from every world node's .pla (CreatePlace
// FUN_0055a090 / CreateFacility FUN_0055ae40) and .trk (FUN_004ebb40), and the scenario's
// <stem>w.ter (FUN_00547590); processed once per tick in Game_process_tick order (place
// FUN_004c55d0, facility FacilityArray_process 0x46d4a0, territory TerritoryArray_process 0x4e4bb0).
// CONTRACT (owned by the places slice; Mission relies on load/tick/save/load_state, the other
// slices on the public lookups below).
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "formats/vfs_bytes.hpp"
#include "formats/vfs_fs.hpp"
#include "game/orders_queue.hpp"
#include "game/savegame_container.hpp"
#include "game/unit_state.hpp"

namespace joa::game {

class Mission;

// Place (0x1f6 bytes). Recnos are 1-based in load order.
struct Place {
    int recno = 0;                  // +0x00
    int scale = 0;                  // +0x04 PLACE_SCALE: VILLAGE 1, TOWN 2, CASTLE 3, UMBRELLA 4, else 0
    std::string model, name;        // MODEL_NAME, +0x05 PLACE_NAME (32 chars)
    int nation = 0;                 // +0x26 owner nation (0 none)
    Vec3 pos;                       // +0x2a
    float angle = 0;                // +0x36 radians (HD)
    // +0x3a area tested by AABB_contains 0x60d5f0. CONFIRMED quirk: places with HD != 0 keep the
    // box in local coordinates (centred on the origin), so their area is effectively elsewhere.
    Vec3 box_min, box_max;
    int timer = 0;                  // +0x5a population regrowth timer (ms)
    std::vector<Vec3> entrances;    // +0x5e/+0x62, max 8
    std::vector<Vec3> parking;      // +0xc2/+0xc6, max 13
    std::vector<float> parking_dir; // +0x162 radians
    int pop = 400, pop_max = 400;   // +0x52 / +0x56 (owner Town fields of the facilities)
    int spawn_count = 0;            // +0x1ca barrack units queued for this place
    int captured = 0;               // +0x1ce
    std::vector<int> facilities;    // +0x1ee linked facility recnos in link order
    template <class V> void visit(V& v) {
        v(recno), v(scale), v(model), v(name), v(nation), v(pos), v(angle), v(box_min), v(box_max), v(timer),
            v(entrances), v(parking), v(parking_dir), v(pop), v(pop_max), v(spawn_count), v(captured), v(facilities);
    }
};

// Spawn-barrack sub-object (+0x79..+0x13c, reset = memset(+0x79, 0, 0xc3)).
struct SpawnBarrack {
    int leader = 0;                 // +0x79 spawned leader recno
    int leader_type = 0;            // +0x7b unit type id (script id = class index + 1)
    int leader_level = 0;           // +0x7d
    int team = 0;                   // +0x7f
    std::string name, title;        // +0x81 / +0xc1 -> unit +0x3d / +0x7f
    Order order;                    // +0x101 action, +0x105 prio, +0x106.. p1..p4
    int order_ms = 0;               // +0x11e duration
    int groups = 0;                 // +0x122 (max 4)
    std::array<int, 4> type{}, count{}, level{};   // +0x124 / +0x12c / +0x134
    int follow = 0;                 // +0x13c (outside the reset range)
    void reset() { const int f = follow; *this = {}; follow = f; }
    template <class V> void visit(V& v) {
        v(leader), v(leader_type), v(leader_level), v(team), v(name), v(title), v(order), v(order_ms), v(groups);
        for (int i = 0; i < 4; ++i) v(type[size_t(i)]), v(count[size_t(i)]), v(level[size_t(i)]);
        v(follow);
    }
};

struct Facility {
    enum Kind : int { Barrack = 1, Shop = 2, Recruit = 3, Workshop = 16 };
    int recno = 0;
    int kind = 0;                   // +0x04 FacilityKind_from_name 0x46d820
    std::string name;               // +0x08 FACILITY_NAME
    int place = 0;                  // +0x6d owner place recno (0: PLACE_NAME not found)
    Vec3 pos;                       // +0x29
    float angle = 0;                // +0x35 radians
    Vec3 box_min, box_max;          // +0x39 spawn rect (always unrotated)
    std::vector<Vec3> entrances;    // +0x51/+0x55, max 2
    SpawnBarrack barrack;           // kind 1
    // shop (kind 2, f_shop.cpp): +0x89 sell list id, +0x81 can regen. The stock itself (and its
    // hourly restock, FUN_00469710) is buy-interface data and not modelled.
    int sell_list = 0, can_regen = 0;
    // recruit (kind 3): +0x81/+0x85 RECRUIT.RES lists, +0x91 spawn barrack, +0x93 available,
    // +0x97 can recruit, +0x98 level
    int list1 = 0, list2 = 0, spawn_barrack = 0, available = 0, can_recruit = 1, recruit_level = 1;
    // workshop (kind 16): +0xec RECRUIT.RES list, +0x79 type (NORMAL 0 IRON 1 WOOD 2), +0x2f0a resource
    int recruit_list = 0, workshop_type = 0, resource = 0;
    // RecruitWorkshop: recruit order queue (RecruitFacility_queue_order 0x468cd0, fed to the spawn barrack one
    // unit per free tick by RecruitFacility_process 0x468600). Team 9999 = no troop.
    struct Queued {
        int type = 0, team = 0, count = 0;
        template <class V> void visit(V& v) { v(type), v(team), v(count); }
    };
    std::vector<Queued> queue;
    // RecruitWorkshop: siege workshop (f_work.cpp). +0x7d capacity (12, FUN_00469fa0), products +0xa2 (max 4):
    // type +0xa4, cost +0xac (class Gold), crew +0xb4 (builders needed), build ms +0xbc; queued +0xcc / built +0xdc
    // per product; build queue +0x112 (max 0x50, 0x93-byte entries), +0x2f04 entry being built (-1), +0x2f06
    // last queue time.
    struct Product {
        int type = 0, cost = 0, crew = 0, ms = 0, queued = 0, built = 0;
        template <class V> void visit(V& v) { v(type), v(cost), v(crew), v(ms), v(queued), v(built); }
    };
    struct Build {
        int type = 0, team = 0, building = 0;
        std::uint32_t queued_ms = 0, start_ms = 0;
        int ms = 0;
        template <class V> void visit(V& v) { v(type), v(team), v(building), v(queued_ms), v(start_ms), v(ms); }
    };
    int capacity = 12, current = -1;
    std::uint32_t last_queue_ms = 0;
    std::vector<Product> products;
    std::vector<Build> builds;
    template <class V> void visit(V& v) {
        v(recno), v(kind), v(name), v(place), v(pos), v(angle), v(box_min), v(box_max), v(entrances), v(barrack),
            v(sell_list), v(can_regen), v(list1), v(list2), v(spawn_barrack), v(available),
            v(can_recruit), v(recruit_level), v(recruit_list), v(workshop_type), v(resource);
        v(queue), v(capacity), v(current), v(last_queue_ms), v(products), v(builds);   // RecruitWorkshop
    }
};

// Territory (array 0xa52940, sorted by name after loading, FUN_004e5960).
struct Territory {
    struct Area {
        int shape = 0;              // +0x48+i: 0 rectangle, 1 circle, 2 quadrilateral
        int x[4] = {}, y[4] = {};   // +0x4c.. / +0x5c.. (rectangle: x[0..1], y[0..1])
        int r = 0;                  // +0xcc
        template <class V> void visit(V& v) {
            v(shape);
            for (int i = 0; i < 4; ++i) v(x[i]), v(y[i]);
            v(r);
        }
    };
    int recno = 0;
    std::string name;               // +0x04
    int scale = 0, type = 0;        // +0x25 / +0x26
    int center_x = 0, center_y = 0, coord_x = 0, coord_y = 0;  // +0x28..+0x34
    int owner = 0;                  // +0x38 nation, -1 Neutral
    std::string scr_name;           // +0x3c
    int no_attack = 0;              // +0x45 DisableBeingAttacked
    int under_attack = 0;           // +0x46
    int player_inside = 0;          // +0x47
    int attack_ms = 0;              // +0xdc
    std::vector<Area> areas;        // +0x27 count
    template <class V> void visit(V& v) {
        v(recno), v(name), v(scale), v(type), v(center_x), v(center_y), v(coord_x), v(coord_y), v(owner), v(scr_name),
            v(no_attack), v(under_attack), v(player_inside), v(attack_ms), v(areas);
    }
};

struct TrackPoint {
    int recno = 0, type = 0;
    Vec3 pos;                       // stored as ints by the original (+8/+0xc/+0x10)
    std::vector<int> adj;           // neighbour point recnos
    template <class V> void visit(V& v) { v(recno), v(type), v(pos), v(adj); }
};
struct Track {
    int recno = 0;
    std::string map_name, code;     // +0x04 / +0x25
    std::vector<TrackPoint> points;
    template <class V> void visit(V& v) { v(recno), v(map_name), v(code), v(points); }
};

// Nation relation record (nation +0x55 + (b-1)*0xf4) fields the scripts touch.
struct Relation {
    int status = -1;                // +3 (-1: never set by a script)
    int status_day = 0;             // +4
    int friendship = 0;             // +8
    template <class V> void visit(V& v) { v(status), v(status_day), v(friendship); }
};

class Places {
public:
    // map_dir: game path of the map directory (e.g. "MAP\\scenario1"); stem: map stem ("S1L1").
    bool load(Mission& m, const formats::Vfs& vfs, const std::string& map_dir, const std::string& stem, std::string* err);
    void tick(Mission& m);
    void save(ByteWriter& w) const;
    bool load_state(formats::ByteReader& r);

    // --- lookups for the other slices ------------------------------------------------------
    const Place* place(int recno) const;                 // nullptr when invalid (FUN_00456ca0)
    Place* place(int recno);
    bool inside(int place_recno, Vec3 pos) const;        // AABB_contains(place+0x3a); false if invalid
    int place_by_name(std::string_view name) const;      // FUN_004c54a0 (stricmp, ascending)
    bool facility_valid(int recno) const;                // !FUN_0044cf80
    const Facility* facility(int recno) const;
    Facility* facility(int recno);
    int facility_by_name(std::string_view name) const;   // FUN_0046d200 (stricmp, from top)
    int track_id(std::string_view code) const;           // FUN_004ec320 (stricmp, ascending), 0 none
    bool track_exists(std::string_view code) const { return track_id(code) != 0; }
    const Track* track(int id) const;
    int territory_by_name(std::string_view name) const;  // FUN_004e4b20 (from top)
    const Territory* territory(int recno) const;
    static bool in_territory(const Territory& t, float x, float y);  // inside one of its areas
    Territory* territory(int recno);
    int recruit_list_by_code(std::string_view code) const;  // RecruitRes_find_by_code 0x4cad00
    int player_territory() const { return player_territory_; }  // player unit +8

    std::vector<Place>& places() { return places_; }
    std::vector<Facility>& facilities() { return facilities_; }
    std::vector<Territory>& territories() { return territories_; }

    // --- nation relations (FUN_0042a1c0, FUN_00499770) -----------------------------------------
    Relation& relation(int a, int b);
    void set_relation_status(Mission& m, int a, int b, int status);

    // RECRUIT.RES (unit\recruit.res) record subset.
    struct RecruitRec {
        std::string code;
        int level = -1, population = -1;
        // RecruitWorkshop: RecruitRes_parse_record 0x4ca980 +0x2b Location, +0x23 entries "CODE : time : crew"
        // (unit type id 1-based, time in s -> ms as Workshop_set_recruit_list 0x46b730 (x1000), crew = int field).
        std::string location;
        struct Entry { int type = 0, ms = 0, crew = 0; };
        std::vector<Entry> entries;
    };
    const RecruitRec* recruit_rec(int id) const;

    // RecruitWorkshop: barracks screen commit (BarrUI_confirm 0x6bdc00 -> RecruitFacility_queue_order 0x468cd0):
    // queue `count` units of `type` (1-based) for `team` (9999 = no troop) on a RECRUIT facility; gold and the
    // owner place's population are the screen's bookkeeping. Returns false for a non-recruit facility.
    bool recruit(int facility, int type, int team, int count);
    // Siege workshop screen commit (FUN_006ea1a0 -> 0x46ab40): queue one weapon of `type` (team 9999). Fails
    // (as the original, silently) without a live player, an unknown product, too few builders or 20 queued.
    bool build_siege(Mission& m, int facility, int type);
    void set_workshop_list(int facility, int list);   // Workshop_set_recruit_list 0x46b730

private:
    void sync_ai(Mission& m) const;                      // places / zones / tracks hooks of AiSystem
    void tick_barrack(Mission& m, Facility& f);
    void tick_recruit(Mission& m, Facility& f);          // RecruitWorkshop: RecruitFacility_process 0x468600
    void tick_workshop(Mission& m, Facility& f);         // RecruitWorkshop: workshop process 0x46a040
    void tick_territories(Mission& m);

    Mission* m_ = nullptr;
    std::vector<Place> places_;
    std::vector<Facility> facilities_;
    std::vector<Territory> territories_;
    std::vector<Track> tracks_;
    std::vector<RecruitRec> recruit_;
    std::vector<Relation> relations_;  // [a][b] flattened, nations 1..n
    int nations_ = 0;
    int territory_msg_ms_ = 0;         // territory array +0x21
    int territory_cursor_ = 0;         // territory array +0x1d
    int player_territory_ = 0;
};

// RecruitWorkshop: the barracks / siege workshop screen copy and its rules, shared by the engine screens
// (engine hud_recruit / hud_workshop) and the campaign bot. Barracks: open BarrUI_open 0x6bda00, aw_r / aw_l
// BarrUI_handle_input 0x6bf630, commit BarrUI_confirm 0x6bdc00; workshop: open FUN_006ea020, aw_r / aw_l
// FUN_006ec480, commit FUN_006ea1a0. Rows are the facility's RECRUIT.RES entries (barracks, slots below) or its
// products (workshop).
struct FacilityOrder {
    // Barracks slots: 0 light inf, 1 heavy inf, 2 maceman, 3 poleman, 4 archer, 5 crossbowman (by class code).
    static constexpr const char* kRecruitCodes[6] = {"FRENCH_LIGHT_INFANTRY", "FRENCH_HEAVY_INFANTRY", "FRENCH_MACEMAN",
                                                     "FRENCH_POLEMAN", "FRENCH_ARCHER", "FRENCH_CROSSBOW_MAN"};
    static FacilityOrder barracks(Mission& m, int facility, int recruiter);
    static FacilityOrder siege_workshop(Mission& m, int facility);
    int row_type(Mission& m, int row) const;   // the row's unit type (1-based), 0 none
    int row_price(Mission& m, int row) const;  // class Gold +0x161 (workshop IRON / WOOD: Resource +0x165)
    int slot(Mission& m, int row) const;       // barracks slot of the row (-1 other class) / the workshop row
    bool more(Mission& m, int row);            // aw_r: one more if the rules allow it
    bool less(Mission& m, int row);            // aw_l
    void commit(Mission& m);                   // queue the new units, write money / population back

    // Per slot (barracks) / product (workshop): +0x42 troop / +0x52 built, +0x5a / +0x62 new.
    std::array<int, 6> have{}, add{};
    int money = 0;      // +0x3e gold / workshop +0x41 gold or the facility's iron / wood
    int cost = 0;       // +0x7e / +0x45
    int citizens = 0;   // +0x72 able citizens (clamp(pop - 0.95 pop_max, 0, 20)), barracks
    int recruited = 0;  // +0x76
    int total = 0;      // +0x7a troop size incl. pending / workshop +0x49 weapons (built + new)
    int max_troop = 0;  // the recruiter's troop limit (0 without one)
    int recruiter = 0;  // DAT_00a48959 recruiting unit (Interact event a: player on R, selected RTS leader)
    int facility = 0;
    bool workshop = false;
};

}  // namespace joa::game
