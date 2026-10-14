// Mission map objects (item array 0xa460a0 + modify-item array 0xa50530).
//
// Sources (joan.exe): map object loader Level_load_objects 0x4f3df0 via WorldS_parse_objects
// 0x44e5f0 (create=0 pass of FUN_00439ff0 per .wor CreateNode, FUN_0047ad40), map triggers
// WorldS_parse_create_trigger 0x450ea0, Item_create 0x49ea90, per-class vtables (gate 0x9d388c,
// obstacle 0x9d3ab4, house 0x9d397c, wall 0x9d3c88, box 0x9d37cc, switch 0x9d3bec, path 0x9d3b50,
// keg 0x9d3a18, base 0x9d3d78), modify_item process 0x4afb20, Trigger_process case 2 0x4ef090.
//
// Deviations / HYPOTHESIS (documented, not silently different):
// - Only gameplay items are kept: the create=0 pass creates items for classes in 0x4a4050
//   (all but TERRAIN 5, WAVE 12, ARMOR 16; never ITEMTYPE 6 grass). TERRAIN scenery that the
//   render pass (create=1) also puts in the original item array is the renderer's business, and
//   unit inventory items are not in this array, so recnos differ from the original's (scripts only
//   use recnos returned by the builtins). LIFO slot reuse follows DynArrayB 0x5857a0.
// - Object animations / fades are not simulated by a scene graph: their duration comes from the
//   model's AM2 clip (f_open / f_close / stage AnimName) and the actor fade constants
//   (actor+0x30: -0.3 gate kind 2, -0.2 obstacle/house, -0.5 box open).
// - FUN_0049a010 mode 1 (gate: segment tests against the gate box) is approximated by the same
//   footprint/height test as mode 0 without the angle window (HYPOTHESIS).
// - Gate_set_open_collision: the unit-proximity refusal is reproduced; the path-mesh polygon is
//   the one containing the gate position (find_poly_xy), toggled via NavMesh::set_blocked.
// - Map PLAY_ANIMATION / PLAY_AMB_SOUND triggers are cosmetic: they are evaluated here (not in
//   World's one-shot trigger list) and emit MissionEvents.
#include "game/mission_objects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>

#include "formats/anim_clip.hpp"
#include "formats/res_archive.hpp"
#include "formats/world_text.hpp"
#include "game/mission.hpp"
#include "game/nav_mesh.hpp"

namespace joa::game {
namespace {

constexpr float kDeg = 0.017453292f;

// STD.SET ITEMCLAS / ITEMTYPE (1-based rows, CONFIRMED from data).
constexpr const char* kClasses[] = {"GATE", "OBSTACLE", "HOUSE", "WALL", "TERRAIN", "WEAPON", "POTION", "GOLD", "ENHANCER", "BOX",
                                    "LADDER", "WAVE", "SWITCH", "PATH", "MISSION", "ARMOR", "MISC", "KEG", "CROSS", "PTERRAIN"};
constexpr const char* kTypes[] = {
    "Castle Gate", "Obstacle", "House", "Castle Wall", "Normal Terrain Object", "Grass Terrain object",
    "Permanent Terrain Object", "Normal Left Hand Weapon", "Normal Right Hand Weapon", "Normal Two Hand Weapon",
    "Normal Range Weapon", "Normal Crossbow Weapon", "Normal Arrow", "Projectile Arrow", "Explosive Arrow", "Normal Bolt",
    "Projectile Bolt", "Explosive Bolt", "Cannonball", "Normal Stone", "Red Potion", "Green Potion", "Blue Potion",
    "Yellow Potion", "All Potion", "Gold", "Amulet", "Gemstone", "Ring", "Box", "Normal Ladder", "Switch", "Path",
    "Mission item", "Mission Letter", "Armor", "Miscellaneous Object", "Keg", "Cross"};

enum Cls { GATE = 1, OBSTACLE = 2, HOUSE = 3, WALL = 4, BOX = 10, LADDER = 11, SWITCH = 13, PATH = 14, MISSION = 15, KEG = 18, CROSS = 19 };

// FUN_004a4050: classes that get a modify-item (and that the create=0 map pass instantiates).
bool modify_class(int c) { return c >= 1 && c <= 20 && c != 5 && c != 12 && c != 16; }

std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string_view::npos ? std::string() : std::string(s.substr(a, b - a + 1));
}
std::string first_token(std::string_view s) {
    std::string t = trim(s);
    if (auto p = t.find_first_of(" \t"); p != std::string::npos) t.resize(p);
    return t;
}
int index_of(std::string_view v, const char* const* names, int n) {
    for (int i = 0; i < n; ++i)
        if (formats::iequals(v, names[i])) return i + 1;
    return 0;
}

// FUN_00494dc0 (unknown names -> 0).
int trigger_type(std::string_view name) {
    static constexpr std::pair<const char*, int> kNames[] = {
        {"TRIGGER_ATTACK", 0x1}, {"TRIGGER_ARROW_ATTACK", 0x2}, {"TRIGGER_FADEOUT", 0x4}, {"TRIGGER_FADEIN", 0x8},
        {"TRIGGER_THROW", 0x10}, {"TRIGGER_GATE", 0x20}, {"TRIGGER_STONE_ATTACK", 0x40}, {"TRIGGER_FOG", 0x80},
        {"TRIGGER_PLAY_ANIMATION", 0x100}, {"TRIGGER_INTERACT", 0x200}, {"TRIGGER_DOWN_ATTACK", 0x400},
        {"TRIGGER_PLAY_SOUND", 0x800}, {"TRIGGER_PLAY_AMB_SOUND", 0x1000}, {"TRIGGER_EXPLOSIVE_ARROW_ATTACK", 0x2000},
        {"TRIGGER_EXPLOSIVE_KEG_ATTACK", 0x4000}};
    for (auto [n, v] : kNames)
        if (formats::iequals(name, n)) return v;
    return 0;
}

// ItemRes_parse 0x4a0160: the positional header + class block + the keyword properties we read.
void parse_res(ObjRes& r, std::string_view text, const std::string& rec_name) {
    std::vector<std::pair<std::string, std::string>> kv;
    for (const auto& l : formats::parse_kv_text(text)) kv.emplace_back(trim(l.key), trim(l.value));
    r.code = rec_name;
    for (std::size_t i = 0; i < kv.size(); ++i) {
        const std::string& k = kv[i].first;
        const std::string& v = kv[i].second;
        auto is = [&](const char* n) { return formats::iequals(k, n); };
        if (is("ItemClass")) r.cls = index_of(first_token(v), kClasses, 20);
        else if (is("ItemType")) r.type = index_of(v, kTypes, 39);
        else if (is("Code")) { if (!v.empty()) r.code = first_token(v); }
        else if (is("Model")) r.model = first_token(v);
        else if (is("Name")) {
            const std::string n = v;
            const auto p = n.find_last_of('\\');
            r.basename = p == std::string::npos ? n : n.substr(p + 1);
        } else if (is("StructionType")) {
            static constexpr const char* kS[] = {"TOWER", "WALL", "GATE", "WOODGATE", "WOODDOOR", "TRUNK"};
            r.struction = index_of(first_token(v), kS, 6);
        } else if (is("HP")) r.hp = std::atoi(v.c_str());
        else if (is("notBlock")) r.not_block = std::atoi(v.c_str());
        else if (is("NeedKey")) r.need_key = std::atoi(v.c_str()) & 0xff;
        else if (is("ModelLevel")) {
            const bool margins = r.cls == GATE || r.cls == OBSTACLE || r.cls == HOUSE || r.type == 4;
            const bool boxes = r.cls == BOX || r.cls == KEG;
            if (r.type == 37) {   // MISC object: n, then one AnimName (+0x87), played by ItemAnim
                if (i + 1 < kv.size()) r.stages.push_back({0, 0, first_token(kv[++i].second), {}});
                continue;
            }
            if (!margins && !boxes) continue;
            const int n = std::max(0, std::atoi(v.c_str()));
            std::size_t j = i + 1;
            auto key = [&](const char* n2) { return j < kv.size() && formats::iequals(kv[j].first, n2); };
            for (int e = 0; e < n && j < kv.size(); ++e) {
                ObjStage s;
                if (margins) s.margin = std::atoi(kv[j++].second.c_str());   // positional LevelMargin
                if (key("AnimName")) s.kind = 0, s.anim = first_token(kv[j++].second);
                else if (key("ModelName")) {
                    s.kind = 1, s.model = first_token(kv[j++].second);
                    if (key("AnimName")) s.kind = 2, s.anim = first_token(kv[j++].second);
                } else if (margins && key("AddModelName")) s.kind = 3, s.model = first_token(kv[j++].second);
                else if (margins && key("ModelName2")) s.kind = 4, s.model = first_token(kv[j++].second);
                r.stages.push_back(std::move(s));
            }
            i = j - 1;
        } else if (is("Width")) r.width = std::atoi(v.c_str());
        else if (is("Height")) r.height = std::atoi(v.c_str());
        else if (is("Quantity")) r.quantity = std::atoi(v.c_str());
        else if (is("MinLevel")) r.min_level = std::atoi(v.c_str());
        else if (is("Damage")) r.damage = std::atoi(v.c_str());
        else if (is("Pickable")) r.pickable = std::atoi(v.c_str()) != 0;
        else if (is("UniqueItem")) r.unique = std::atoi(v.c_str()) != 0;
        else if (is("OpenWithInteract")) r.open_with_interact = std::atoi(v.c_str()) != 0;
    }
}

std::optional<formats::BlockText> read_blocks(const formats::Vfs& vfs, const std::string& path) {
    auto b = vfs.read(path);
    if (!b) return std::nullopt;
    return formats::parse_block_text(std::string_view(reinterpret_cast<const char*>(b->data()), b->size()));
}

// Sorted like NTFS FindFirstFile (case-insensitive name order).
std::vector<std::filesystem::directory_entry> sorted_dir(const std::filesystem::path& d) {
    std::vector<std::filesystem::directory_entry> v;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(d, ec)) v.push_back(e);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return upper(a.path().filename().string()) < upper(b.path().filename().string());
    });
    return v;
}

// Save visitors (plain data).
struct Out {
    ByteWriter& w;
    void operator()(int v) { w.i32(v); }
    void operator()(float v) { w.f32(v); }
    void operator()(bool v) { w.u8(v ? 1 : 0); }
    void operator()(const std::string& s) { w.str(s); }
    void operator()(Vec3& p) { w.f32(p.x), w.f32(p.y), w.f32(p.z); }
};
struct In {
    formats::ByteReader& r;
    void operator()(int& v) { v = r.i32(); }
    void operator()(float& v) { v = r.f32(); }
    void operator()(bool& v) { v = r.u8() != 0; }
    void operator()(std::string& s) {
        const std::uint32_t n = r.u32();
        if (n > r.remaining()) { r.fail(); return; }
        auto b = r.bytes(n);
        s.assign(reinterpret_cast<const char*>(b.data()), b.size());
    }
    void operator()(Vec3& p) { p.x = r.f32(), p.y = r.f32(), p.z = r.f32(); }
};
void put_ints(ByteWriter& w, const std::vector<int>& v) {
    w.u32(std::uint32_t(v.size()));
    for (int x : v) w.i32(x);
}
bool get_ints(formats::ByteReader& r, std::vector<int>& v) {
    const std::uint32_t n = r.u32();
    if (n > r.remaining() / 4) return false;
    v.resize(n);
    for (int& x : v) x = r.i32();
    return r.ok();
}

// Unit_anim_is_idle_class (IDLE_CLASS FUN_00508ab0).
bool idle_class(int a) { return (a >= 1 && a <= 6) || a == 8 || a == 0x43 || a == 0x59 || a == 0x5a; }

}  // namespace

// ---------------------------------------------------------------------------------------------
// ItemRes

void MapObjects::load_res(Mission& m, const formats::Vfs& vfs) {
    (void)m;
    res_.assign(1, ObjRes{});
    res_index_.clear();
    // item\ITEM.RES records first (same order and ids as GameData::items).
    if (auto p = vfs.resolve("item/item.res"))
        if (auto a = formats::ResArchive::open(*p))
            for (const auto& e : a->entries()) {
                ObjRes r;
                parse_res(r, a->text(e.name).value_or(""), e.name);
                res_.push_back(std::move(r));
            }
    // FUN_0049fdf0: ITEM\*.pro, then each subdirectory recursively.
    std::function<void(const std::filesystem::path&, const std::string&)> walk = [&](const std::filesystem::path& d,
                                                                                    const std::string& rel) {
        const auto entries = sorted_dir(d);
        for (const auto& e : entries) {
            if (!e.is_regular_file() || !formats::iequals(e.path().extension().string(), ".pro")) continue;
            auto bytes = formats::read_file(e.path());
            if (!bytes) continue;
            ObjRes r;
            r.dir = rel;
            parse_res(r, std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), e.path().stem().string());
            res_.push_back(std::move(r));
        }
        for (const auto& e : entries)
            if (e.is_directory()) walk(e.path(), rel + e.path().filename().string() + "\\");
    };
    if (auto root = vfs.resolve("item")) walk(*root, "");
    for (int i = int(res_.size()) - 1; i >= 1; --i) res_index_[upper(res_[i].code)] = i;   // first id wins
    sell_lists_.clear();
    if (auto p = vfs.resolve("item/sellitem.res"))
        if (auto a = formats::ResArchive::open(*p))
            for (const auto& e : a->entries()) {
                std::string code = e.name;
                for (const auto& l : formats::parse_kv_text(a->text(e.name).value_or("")))
                    if (formats::iequals(trim(l.key), "Code")) { code = first_token(l.value); break; }
                sell_lists_.push_back(upper(code));
            }
}

int MapObjects::res_by_code(std::string_view code) const {
    auto it = res_index_.find(upper(code));
    return it == res_index_.end() ? 0 : it->second;
}

int MapObjects::sell_list(std::string_view code) const {
    const std::string c = upper(code);
    for (std::size_t i = 0; i < sell_lists_.size(); ++i)
        if (sell_lists_[i] == c) return int(i) + 1;
    return 0;
}

// Length of a clip of the model shown at `stage` (FUN_004a3f20: stage model, else Model), from
// ITEM\<dir>\anim\<model>.am2, else ANIM\<model>.am2.
int MapObjects::clip_ms(int res_id, int stage, std::string_view clip) {
    const ObjRes* r = res(res_id);
    if (!r || !vfs_ || clip.empty()) return 0;
    const std::string& model =
        stage >= 1 && stage <= int(r->stages.size()) && !r->stages[stage - 1].model.empty() ? r->stages[stage - 1].model : r->model;
    const std::string done = upper(r->dir + model) + ":";
    if (!clip_cache_.count(done)) {
        clip_cache_[done] = 1;
        const auto dot = model.rfind('.');
        if (dot != std::string::npos && formats::iequals(model.substr(dot), ".am1")) {
            const std::string stem = model.substr(0, dot);
            auto bytes = vfs_->read("item\\" + r->dir + "anim\\" + stem + ".am2");
            if (!bytes) bytes = vfs_->read("anim\\" + stem + ".am2");
            formats::Animation a;
            std::string err;
            if (bytes && formats::parse_animation(*bytes, a, err))
                for (const auto& c : a.clips) clip_cache_[done + upper(c.name)] = int(c.end - c.start);
        }
    }
    auto it = clip_cache_.find(done + upper(clip));
    return it == clip_cache_.end() ? 0 : it->second;
}

// ---------------------------------------------------------------------------------------------
// Item array

int MapObjects::count() const {
    int n = 0;
    for (const auto& it : items_) n += !it.deleted;
    return n;
}

// Item_create 0x49ea90 + the class init (vtbl[8]) + modify-item add 0x4afa80.
int MapObjects::create(Mission& m, int id) {
    (void)m;
    const ObjRes* r = res(id);
    if (!r) return 0;
    MapItem it;
    it.res = id;                       // Item_init 0x4178e0
    it.cls = r->cls, it.type = r->type;
    it.damage = r->damage;
    const int stages = int(r->stages.size());
    switch (r->cls) {
    case GATE:                         // 0x48c360
        it.hp = it.max_hp = r->hp, it.stage = stages, it.need_key = r->need_key, it.interact = 0x200;
        break;
    case OBSTACLE: case HOUSE: case WALL:   // 0x43d730 / 0x48eb80 / 0x449a30
        it.hp = it.max_hp = r->hp, it.stage = stages;
        break;
    case BOX:                          // 0x489fb0
        it.stage = stages, it.need_key = r->need_key, it.can_delete = 1;
        if (r->open_with_interact) it.interact = 0x200;
        break;
    case KEG: it.stage = stages; break;   // 0x48f850
    case MISSION: case CROSS: it.name = r->basename; break;
    case LADDER: it.interact = 0x200; break;
    default: break;
    }
    int recno;
    if (!free_.empty()) recno = free_.back(), free_.pop_back(), items_[recno - 1] = std::move(it);
    else items_.push_back(std::move(it)), recno = int(items_.size());
    if (modify_class(r->cls)) {
        int mr;
        if (!modify_free_.empty()) mr = modify_free_.back(), modify_free_.pop_back(), modify_[mr - 1] = recno;
        else modify_.push_back(recno), mr = int(modify_.size());
        items_[recno - 1].modify = mr;
    }
    return recno;
}

void MapObjects::del_item(Mission& m, int recno) {   // FUN_0049f370
    (void)m;
    if (!valid(recno)) return;
    if (const int mr = items_[recno - 1].modify; mr >= 1 && mr <= int(modify_.size()) && modify_[mr - 1] == recno) {
        modify_[mr - 1] = 0;
        modify_free_.push_back(mr);
    }
    items_[recno - 1] = MapItem{};
    items_[recno - 1].deleted = true;
    free_.push_back(recno);
}

int MapObjects::find_by_name(std::string_view name) const {
    for (int r = int(items_.size()); r >= 1; --r)
        if (!items_[r - 1].deleted && formats::iequals(items_[r - 1].name, name)) return r;
    return 0;
}

int MapObjects::find_by_code(std::string_view code) const {
    for (int r = int(items_.size()); r >= 1; --r)
        if (!items_[r - 1].deleted)
            if (const ObjRes* x = res(items_[r - 1].res); x && formats::iequals(x->code, code)) return r;
    return 0;
}

int MapObjects::modify_item(int mr) const {
    return mr >= 1 && mr <= int(modify_.size()) ? modify_[mr - 1] : 0;
}

void MapObjects::set_owner_in_box(Vec3 mn, Vec3 mx, int nation) {
    for (int mr = int(modify_.size()); mr >= 1; --mr) {
        const int ir = modify_[mr - 1];
        if (!ir) continue;
        MapItem& it = items_[ir - 1];
        if (it.cls != WALL && it.cls != GATE) continue;
        const Vec3 p = it.pos;
        if (p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y && p.z >= mn.z && p.z <= mx.z) it.nation = nation;
    }
}

void MapObjects::box_content(Mission& m, int recno, std::string_view code, int level) {
    if (!valid(recno) || items_[recno - 1].cls != BOX || code.empty()) return;
    MapItem& b = items_[recno - 1];
    const auto& lists = m.data().drop_lists;
    if (auto dl = lists.find(upper(code)); dl != lists.end()) {   // DropList_roll 0x464030
        int total = 0;
        for (auto& [itm, w] : dl->second) total += w;
        if (total <= 0) return;
        int x = m.world().rng().next(total) + 1;
        for (auto e = dl->second.rbegin(); e != dl->second.rend(); ++e)
            if ((x -= e->second) < 1) {
                b.content = e->first;
                b.content_level = res(e->first) ? res(e->first)->min_level : 0;
                return;
            }
    } else if (const int c = res_by_code(code)) {
        b.content = c;
        b.content_level = level == -1 ? res(c)->min_level : level;
    }
}

void MapObjects::change_in_box(Mission& m, Vec3 mn, Vec3 mx, int old_res, int new_res, int nation) {
    for (int mr = int(modify_.size()); mr >= 1; --mr) {
        const int ir = mr <= int(modify_.size()) ? modify_[mr - 1] : 0;
        if (!ir || items_[ir - 1].res != old_res) continue;
        const MapItem o = items_[ir - 1];
        if (o.pos.x < mn.x || o.pos.x > mx.x || o.pos.y < mn.y || o.pos.y > mx.y || o.pos.z < mn.z || o.pos.z > mx.z) continue;
        del_item(m, ir);
        const int ni = create(m, new_res);
        if (!ni) continue;
        MapItem& n = items_[ni - 1];
        n.pos = o.pos, n.dir = o.dir, n.pitch = o.pitch, n.roll = o.roll;
        n.nation = nation;
    }
}

bool MapObjects::hostile(Mission& m, int a, int b) const {   // FUN_004b6860
    if (a == b) return false;
    if (a == 0 || b == 0) return true;
    return m.world().hostile(a, b);
}

// ---------------------------------------------------------------------------------------------
// Map load

bool MapObjects::load(Mission& m, const formats::Vfs& vfs, const std::string& map_dir, const std::string& stem, std::string* err) {
    vfs_ = &vfs;
    nav_ = m.nav();
    load_res(m, vfs);
    if (res_.size() <= 1) {
        if (err) *err = "item resources not found";
        return false;
    }
    items_.clear(), free_.clear(), modify_.clear(), modify_free_.clear(), triggers_.clear(), clip_cache_.clear();
    install_hooks(m);

    // FUN_0047ad40 node origins; NodeFile == NodeName + ".stg" in every shipped .wor.
    const std::vector<WorldNode> nodes = world_nodes(vfs, map_dir, m.info().step);
    if (nodes.empty()) {
        if (err) *err = "no world nodes for " + map_dir + "\\" + stem + "w.wor";
        return false;
    }
    GameData& gd = m.data();
    std::vector<std::pair<const formats::TextBlock*, const WorldNode*>> trig;
    std::vector<formats::BlockText> stgs;
    stgs.reserve(nodes.size());
    for (const WorldNode& n : nodes) {
        auto stg = read_blocks(vfs, map_dir + "\\" + n.name + ".stg");
        if (!stg) continue;
        stgs.push_back(std::move(*stg));
        for (const auto& b : stgs.back().blocks) {
            if (formats::iequals(b.type, "CreateTrigger")) { trig.push_back({&b, &n}); continue; }
            if (!formats::iequals(b.type, "CreateObject")) continue;
            const std::string* path = b.get("Path");
            const std::string* file = b.get("ObjectFile");
            const int id = res_by_code((path ? *path : std::string()) + (file ? *file : std::string()));
            const ObjRes* r = res(id);
            if (!r || !modify_class(r->cls) || r->type == 6) continue;   // render-only scenery (create=1 pass)
            const int ir = create(m, id);
            MapItem& it = items_[ir - 1];
            if (const std::string* nm = b.get("ObjectName")) it.name = nm->substr(0, 63);
            it.pos = {float(b.number("X")) + n.ox, float(b.number("Y")) + n.oy, float(b.number("Z"))};
            it.dir = float(b.number("HD")) * kDeg;   // "HD"/"VD" labels: degrees
            it.pitch = float(b.number("VD")) * kDeg;
            if (const std::string* k = b.get("Kingdom")) it.nation = gd.nation_id(trim(*k));
            it.on_floor = r->pickable || r->cls == GATE || r->cls == WALL;   // 0x52-byte marker (0x4a4180 / Pickable)
            it.level = r->min_level, it.quantity = r->quantity;
            if (const std::string* tr = b.get("Treasure"); tr && r->cls == BOX) {   // Treasure / TreasureLevel
                const std::string* lv = b.get("TreasureLevel");
                box_content(m, ir, trim(*tr), lv ? std::atoi(lv->c_str()) : -1);
            }
            if (const std::string* sc = b.get("Scale")) it.scale = float(std::atof(sc->c_str()));
            if (const std::string* is = b.get("InitState"); is && it.cls == GATE) it.state = std::atoi(is->c_str()) & 0xff;
            if (const std::string* fs = b.get("FinalState"); fs && it.cls == GATE) it.final_state = std::atoi(fs->c_str()) & 0xff;
            if (const std::string* tt = b.get("TriggerType")) it.interact = trigger_type(trim(*tt));
            if (const std::string* tn = b.get("TriggerItemName"); tn && it.cls == SWITCH) it.lever_name = trim(*tn);
            if (it.cls == GATE && it.state == 1) it.blocking = false;
            if (it.cls == GATE) gate_nav(it);
        }
    }
    // Map triggers (create=1 pass, after the items exist). Values positional and int (atoi).
    for (auto [b, n] : trig) {
        const auto& f = b->fields;
        if (f.size() < 5) continue;
        MapTrigger t;
        t.type = trigger_type(trim(f[0].value));
        t.pos = {float(std::atoi(f[1].value.c_str())) + n->ox, float(std::atoi(f[2].value.c_str())) + n->oy,
                 float(std::atoi(f[3].value.c_str()))};
        t.radius = float(std::atoi(f[4].value.c_str()));
        const std::string extra = f.size() > 5 ? trim(f[5].value) : std::string();
        if (t.type == 0x100) {
            const int ir = find_by_name(extra);
            if (!ir || !items_[ir - 1].modify) continue;
            t.target = items_[ir - 1].modify;
        } else if (t.type == 0x800 || t.type == 0x1000) {
            t.code = extra;
        } else {
            continue;   // FADEIN/FADEOUT/FOG/GATE: absent from shipped maps
        }
        triggers_.push_back(std::move(t));
    }
    return true;
}

void MapObjects::install_hooks(Mission& m) {
    m.world().object_trigger = [this, &m](const Trigger& t) { on_trigger(m, t); };
    AiSystem& ai = m.ai();
    ai.item = [this](int id) -> std::optional<AiItem> {
        const MapItem* it = item(id);
        if (!it) return std::nullopt;
        AiItem a;
        a.pos = it->pos;
        if (const ObjRes* r = res(it->res)) a.code = r->code;
        a.nation = it->nation;
        a.gate = it->cls == GATE, a.wall = it->cls == WALL, a.box = it->cls == BOX;
        a.hp = (a.gate || a.wall || it->cls == OBSTACLE || it->cls == HOUSE) ? it->hp : (a.box ? it->stage : 1);
        a.gate_open = a.gate ? it->state : -1;
        return a;
    };
    ai.item_ids = [this]() {
        std::vector<int> ids;
        for (int r = int(items_.size()); r >= 1; --r)
            if (!items_[r - 1].deleted) ids.push_back(r);
        return ids;
    };
    // GATE_OPEN order 0x4a71f0: post_event(0x20, item modify recno, unit, u.pos, 1.0, now, now).
    ai.on_gate_request = [this, &m](int unit, int id, int) {
        const MapItem* it = item(id);
        const Unit* u = m.world().unit(unit);
        if (it && u && !u->deleted) post(m, 0x20, 2, it->modify, 1, unit, u->pos, 1.0f);
    };
}

void MapObjects::post(Mission& m, int type, int kind, int target, int src_kind, int sender, Vec3 pos, float radius, int param) {
    Trigger t;
    t.type = type, t.target_kind = kind, t.target_id = target;
    t.src_kind = src_kind;
    t.sender = sender, t.pos = pos, t.radius = radius;
    t.start = t.end = m.world().now();
    t.param = param;
    m.world().post_trigger(t);
}

// ---------------------------------------------------------------------------------------------
// Tick: modify_item 0x4afb20 (from top: dead -> delete, else process) + cosmetic map triggers.

void MapObjects::tick(Mission& m) {
    for (int mr = int(modify_.size()); mr >= 1; --mr) {
        const int ir = mr <= int(modify_.size()) ? modify_[mr - 1] : 0;
        if (!ir) continue;
        if (dead(m, items_[ir - 1])) del_item(m, ir);
        else process(m, ir);
    }
    const int now = m.world().now();
    const Unit* player = m.world().unit(m.world().player_id());
    for (MapTrigger& t : triggers_) {
        if (t.type == 0x100) {   // ItemAnim_on_trigger 0x433670
            const int ir = modify_item(t.target);
            if (!ir || now < items_[ir - 1].busy_until) continue;
            const ObjRes* r = res(items_[ir - 1].res);
            if (!r || r->stages.empty() || r->stages[0].anim.empty()) continue;
            bool near = false;
            for (const Unit& u : m.world().units()) {
                if (u.deleted || idle_class(u.action)) continue;
                const float dx = t.pos.x - u.pos.x, dy = t.pos.y - u.pos.y, dz = t.pos.z - u.pos.z;
                if (dx * dx + dy * dy + dz * dz <= t.radius * t.radius) { near = true; break; }
            }
            if (!near) continue;
            items_[ir - 1].busy_until = now + clip_ms(items_[ir - 1].res, items_[ir - 1].stage, r->stages[0].anim);
            m.emit(MissionEvent::Kind::Ui, ir, 0, "item_anim " + r->stages[0].anim);
        } else {                 // SE_play_trigger 0x4c0680 (listener = player unit)
            bool in = false;
            if (player && !player->deleted) {
                const float dx = t.pos.x - player->pos.x, dy = t.pos.y - player->pos.y, dz = t.pos.z - player->pos.z;
                in = t.radius == 0 || dx * dx + dy * dy + dz * dz <= t.radius * t.radius;
            }
            if (in && !t.inside) m.emit(MissionEvent::Kind::Sound, 0, t.type, t.code);
            t.inside = in;
        }
    }
}

bool MapObjects::dead(Mission& m, const MapItem& it) const {
    const ObjRes* r = res(it.res);
    if (!r || r->stages.empty() || it.stage != 0) return false;
    const int k0 = r->stages[0].kind;
    const bool done = k0 == 1 || ((k0 == 0 || k0 == 2) && m.world().now() >= it.busy_until);
    switch (it.cls) {
    case GATE: case OBSTACLE: case HOUSE: return it.hp == 0 && done;   // 0x48e270 / 0x4928f0 / 0x48f380
    case BOX: return it.can_delete && done;                           // 0x48b900
    case KEG: return done;                                            // 0x4902c0
    default: return false;                                            // wall 0x492e00, switch/path/base: never
    }
}

void MapObjects::process(Mission& m, int ir) {
    MapItem& it = items_[ir - 1];
    const int now = m.world().now();
    if (it.cls == GATE) {   // 0x48c4d0
        if (it.state == 2) {
            if (now >= it.busy_until) it.state = 0;
        } else if (it.state == 3) {
            if (now >= it.busy_until) gate_collision(m, ir, false), items_[ir - 1].state = 1;
        } else if (it.state == 4 && gate_collision(m, ir, true)) {
            MapItem& g = items_[ir - 1];
            g.busy_until = now + clip_ms(g.res, g.stage, "f_close");
            g.state = 2;
            m.emit(MissionEvent::Kind::Ui, ir, 2, "gate_close");
        }
    } else if (it.cls == BOX) {   // 0x48ba70 (chests opened by interact)
        const ObjRes* r = res(it.res);
        if (!r || !r->open_with_interact || it.stage != 0 || now < it.busy_until) return;
        if (it.fade_until == 0) {
            it.fade_until = now + 2000;   // actor+0x30 = -0.5
            if (it.content) box_drop(m, ir);
        } else if (now > it.fade_until) {
            it.can_delete = 1;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Trigger receivers

bool MapObjects::footprint(const MapItem& it, Vec3 p, int r) const {
    const ObjRes* x = res(it.res);
    if (!x) return false;
    const float dx = p.x - it.pos.x, dy = p.y - it.pos.y;
    const float c = std::cos(-it.dir), s = std::sin(-it.dir);
    const float lx = c * dx - s * dy, ly = s * dx + c * dy;
    return float(-x->width / 2 - r) <= lx && lx <= float(x->width / 2 + r) && float(-x->height / 2 - r) <= ly &&
           ly <= float(x->height / 2 + r);
}

void MapObjects::on_trigger(Mission& m, const Trigger& t) {
    if (t.target_kind != 2) return;
    if (t.target_id <= 0) {
        for (int mr = int(modify_.size()); mr >= 1; --mr) {
            const int ir = mr <= int(modify_.size()) ? modify_[mr - 1] : 0;
            if (ir && footprint(items_[ir - 1], t.pos, int(t.radius))) deliver(m, ir, t);
        }
    } else if (const int ir = modify_item(t.target_id)) {
        deliver(m, ir, t);
    }
}

void MapObjects::deliver(Mission& m, int ir, const Trigger& t) {
    if (!valid(ir)) return;
    switch (items_[ir - 1].cls) {
    case GATE: gate_event(m, ir, t); break;
    case WALL: wall_event(m, ir, t); break;
    case BOX: box_event(m, ir, t); break;
    case OBSTACLE: obstacle_event(m, ir, t, false); break;
    case HOUSE: obstacle_event(m, ir, t, true); break;
    case KEG: keg_event(m, ir, t); break;
    case SWITCH: lever_event(m, ir, t); break;
    default: break;   // ItemAnim 0x433670 reacts to 0x100 only (map triggers, tick); path 0x491de0 no-op
    }
}

bool MapObjects::hit_test(Mission& m, const MapItem& it, int unit, int mode) const {
    World& w = m.world();
    const Unit* u = w.unit(unit);
    if (!u || u->deleted || !u->m.active) return false;
    int reach = 0;
    for (const auto& h : u->inv.melee)
        if (h.item && h.item < int(m.data().items.size())) reach = std::max(reach, m.data().items[h.item].range);
    if (!hostile(m, it.nation, u->nation)) return false;
    const AnimScript* s = w.script_of(w.move(*u));
    if (!s) return false;
    const int r = s->range + reach;
    if (!footprint(it, u->pos, r) || std::fabs(u->pos.z - it.pos.z) > float(r)) return false;
    if (mode != 0) return true;   // HYPOTHESIS: gate segment tests approximated by the footprint
    const float a = w.rel_angle(*u, it.pos);
    return float(std::min(s->h0, s->h1)) <= a && a <= float(std::max(s->h0, s->h1));
}

bool MapObjects::interact_ok(Mission& m, const MapItem& it, int unit) const {
    const Unit* u = m.world().unit(unit);
    if (!u || u->deleted || !u->m.active) return false;
    if (it.nation != 0 && it.nation != u->nation) {
        if (it.cls != GATE) return false;
        // FUN_0041af80: angle of the unit relative to the gate facing; the front half is refused.
        float a = 0;
        if (u->pos.x != it.pos.x || u->pos.y != it.pos.y) {
            a = std::fmod(it.dir / kDeg - std::atan2(u->pos.y - it.pos.y, u->pos.x - it.pos.x) / kDeg, 360.0f);
            if (a > 180.0f) a -= 360.0f;
            if (a < -180.0f) a += 360.0f;
        }
        if (-90.0f < a && a < 90.0f) return false;
        if (it.max_hp < 1) return false;
        if (const ObjRes* r = res(it.res); r && r->struction == 5) return false;
    }
    if (it.cls == GATE && it.max_hp > 0 && it.hp == 0) return false;
    const float dx = u->pos.x - it.pos.x, dy = u->pos.y - it.pos.y, dz = u->pos.z - it.pos.z;
    return dx * dx + dy * dy + dz * dz <= 40000.0f;
}

// Stage transition shared by the destructibles (vtbl[0x18](stage-1) + stage animation / fade).
bool MapObjects::stage_down(Mission& m, int ir, int fade_ms, bool wall_kinds) {
    MapItem& it = items_[ir - 1];
    const ObjRes* r = res(it.res);
    if (!r || it.stage < 1 || it.stage > int(r->stages.size())) return false;
    const ObjStage& s = r->stages[it.stage - 1];
    const int now = m.world().now();
    switch (s.kind) {
    case 0: case 2: {
        it.stage -= 1;
        it.busy_until = now + std::max(clip_ms(it.res, it.stage, s.anim), s.kind == 2 || it.cls != GATE ? fade_ms : 0);
        break;
    }
    case 1: it.stage -= 1; break;
    case 3: if (!wall_kinds) return false; it.stage -= 2; break;   // 0x492e20 break segment
    case 4: if (!wall_kinds) return false; it.stage -= 1; break;
    default: return false;
    }
    m.emit(MissionEvent::Kind::Ui, ir, it.stage, "item_stage");
    return true;
}

void MapObjects::gate_set_open(Mission& m, int ir, bool open) {
    const int now = m.world().now();
    if (open) {
        MapItem& g = items_[ir - 1];
        g.busy_until = now + clip_ms(g.res, g.stage, "f_open");
        g.state = 3;
        m.emit(MissionEvent::Kind::Ui, ir, 3, "gate_open");
    } else if (!gate_collision(m, ir, true)) {
        items_[ir - 1].state = 4;
    } else {
        MapItem& g = items_[ir - 1];
        g.busy_until = now + clip_ms(g.res, g.stage, "f_close");
        g.state = 2;
        m.emit(MissionEvent::Kind::Ui, ir, 2, "gate_close");
    }
}

bool MapObjects::gate_collision(Mission& m, int ir, bool close) {
    MapItem& g = items_[ir - 1];
    const ObjRes* r = res(g.res);
    if (close && r && !r->not_block) {
        const GameData& gd = m.data();
        for (const Unit& u : m.world().units()) {
            if (u.deleted || u.cls < 0) continue;
            const UnitClass& c = gd.units[u.cls];
            if (c.unit_class != 8 && u.mount == 0 && c.special != 2) continue;
            const float dx = u.pos.x - g.pos.x, dy = u.pos.y - g.pos.y;
            if (dx * dx + dy * dy <= 90000.0f) return false;
        }
    }
    g.blocking = close;
    gate_nav(g);
    return true;
}

void MapObjects::gate_nav(const MapItem& g) {
    const ObjRes* r = res(g.res);
    if (!nav_ || (r && r->not_block)) return;
    if (const int poly = nav_->find_poly_xy(g.pos.x, g.pos.y, g.pos.z); poly >= 0) nav_->set_blocked(poly, g.blocking);
}

void MapObjects::gate_event(Mission& m, int ir, const Trigger& t) {
    World& w = m.world();
    MapItem& g = items_[ir - 1];
    const ObjRes* r = res(g.res);
    if (!r) return;
    const int n = int(r->stages.size());
    auto destroyed_check = [&] {
        MapItem& x = items_[ir - 1];
        if (x.stage == 0 && n != 0 && x.hp == 0) gate_collision(m, ir, false), m.emit(MissionEvent::Kind::Ui, ir, 0, "item_destroyed");
    };
    // Gate_check_stage 0x48e430: threshold[stage] < hp -> keep stage (still returns 1).
    auto check_stage = [&] {
        MapItem& x = items_[ir - 1];
        if (r->stages[x.stage - 1].margin < x.hp) return true;
        return stage_down(m, ir, 3333, false);   // kind 2: actor fade -0.3
    };
    auto take = [&](float dmg) {
        MapItem& x = items_[ir - 1];
        x.hp = std::max(0, int(float(x.hp) - dmg));
        if (x.stage == 0 || dmg == 0.0f || !check_stage()) return;
        destroyed_check();
    };
    switch (t.type) {
    case 0x200: {
        if (!g.interact || t.src_kind != 1 || !interact_ok(m, g, t.sender)) return;
        if (!g.need_key) {
            if (g.state == 0) gate_set_open(m, ir, true);
            else if (g.state == 1) gate_set_open(m, ir, false);
        }
        if (items_[ir - 1].state != 0) return;
        if (Unit* u = w.unit(t.sender)) u->m.interact_item = ir;   // +0x2e5
        return;
    }
    case 1: {
        g.attacked = 1;
        if (g.invincible || t.src_kind != 1 || g.state != 0 || !hit_test(m, g, t.sender, 1)) return;
        Unit* u = w.unit(t.sender);
        if (!u || u->cls < 0) return;
        const UnitClass& c = m.data().units[u->cls];
        const bool lahire = formats::iequals(c.code, "LAHIRE");
        if (!lahire && c.special != 11) return;
        float dmg = w.melee_attack_power(*u) * 0.5f;   // Gate_take_melee 0x48c8c0
        if (lahire) dmg *= 3.0f;
        take(dmg);
        return;
    }
    case 0x20:
        if (g.state == 0) gate_set_open(m, ir, true);
        else if (g.state == 1) gate_set_open(m, ir, false);
        return;
    case 0x40: case 0x2000: {
        g.attacked = 1;
        const Unit* u = w.unit(t.sender);
        if (g.invincible || t.src_kind != 1 || g.state != 0 || !u) return;
        take(w.ranged_base_damage(*u) * 0.5f);   // Gate_take_ranged 0x48c990
        return;
    }
    case 0x4000: {
        g.attacked = 1;
        if (g.invincible || t.src_kind != 2 || g.state != 0) return;
        take(valid(t.sender) ? float(items_[t.sender - 1].damage) : 0.0f);   // Gate_take_keg 0x48ca30
        return;
    }
    default: return;   // arrows (2) ignored
    }
}

void MapObjects::wall_event(Mission& m, int ir, const Trigger& t) {
    World& w = m.world();
    MapItem& wl = items_[ir - 1];
    const ObjRes* r = res(wl.res);
    if (!r || wl.invincible) return;
    float dmg = 0;
    if (t.type == 1) {
        if (t.src_kind != 1 || !hit_test(m, wl, t.sender, 0)) return;
        const Unit* u = w.unit(t.sender);
        if (!u || u->cls < 0 || m.data().units[u->cls].special != 11) return;   // battering ram only
        dmg = w.melee_attack_power(*u) * 0.5f;   // Wall_take_melee 0x451f20
    } else if (t.type == 0x40 || (t.type == 0x2000 && m.scenario_no() == 4)) {
        const Unit* u = w.unit(t.sender);
        if (t.src_kind != 1 || !u) return;
        dmg = w.ranged_base_damage(*u) * 0.5f;   // Wall_take_ranged 0x492b10
    } else if (t.type == 0x4000) {
        if (t.src_kind != 2) return;
        dmg = valid(t.sender) ? float(items_[t.sender - 1].damage) : 0.0f;   // Wall_take_keg 0x492bb0
    } else {
        return;
    }
    wl.hp = std::max(0, int(float(wl.hp) - dmg));
    if (wl.stage == 0 || dmg == 0.0f || wl.stage > int(r->stages.size()) || wl.hp > r->stages[wl.stage - 1].margin) return;
    stage_down(m, ir, 0, true);
    if (items_[ir - 1].stage == 0 && items_[ir - 1].hp == 0) m.emit(MissionEvent::Kind::Ui, ir, 0, "item_destroyed");
}

void MapObjects::box_drop(Mission& m, int ir) {
    const MapItem b = items_[ir - 1];
    if (b.content) {
        const ObjRes* c = res(b.content);
        const auto& owned = m.state().item_hints_a;   // FUN_00472740 (Game+0x71c unique items obtained)
        if (c && !(c->unique && std::find(owned.begin(), owned.end(), std::uint16_t(b.content)) != owned.end())) {
            const int ni = create(m, b.content);
            MapItem& d = items_[ni - 1];
            d.pos = b.pos, d.dir = b.dir, d.nation = b.nation;
            d.on_floor = true, d.level = b.content_level, d.quantity = c->quantity;
            m.emit(MissionEvent::Kind::Ui, ni, ir, "item_drop");
        }
    }
}

void MapObjects::box_event(Mission& m, int ir, const Trigger& t) {
    World& w = m.world();
    const ObjRes* r = res(items_[ir - 1].res);
    if (!r) return;
    const bool openable = r->open_with_interact;
    const Unit* player = w.unit(w.player_id());
    const int player_nation = player ? player->nation : 0;
    auto food = [&] {
        if (formats::iequals(items_[ir - 1].name, "s7_food")) ++m.state().food_crates;   // DAT_00a591be
    };
    auto broken = [&] {   // stage reached 0 by an attack: drop content, sound, food counter
        if (items_[ir - 1].stage != 0) return;
        box_drop(m, ir);
        food();
    };
    auto from_ally = [&] {
        const Unit* u = t.src_kind == 1 && t.sender ? w.unit(t.sender) : nullptr;
        return u && !u->deleted && u->nation == player_nation;
    };
    switch (t.type) {
    case 1:
        if (openable || t.src_kind != 1 || !hit_test(m, items_[ir - 1], t.sender, 0) || !from_ally()) return;
        if (items_[ir - 1].stage == 0 || !stage_down(m, ir, 0, false)) return;
        broken();
        return;
    case 2: case 0x40: case 0x2000:
        if (openable || !from_ally() || items_[ir - 1].stage == 0 || !stage_down(m, ir, 0, false)) return;
        broken();
        return;
    case 0x4000:
        if (openable || t.src_kind != 2 || items_[ir - 1].stage == 0 || !stage_down(m, ir, 0, false)) return;
        broken();
        return;
    case 0x20:
        if (t.src_kind == 1) {   // add_gate_trigger on a box: one stage, then the interact path
            if (items_[ir - 1].stage != 0) {
                items_[ir - 1].can_delete = 0;
                if (!stage_down(m, ir, 0, false)) return;
            }
            food();
        }
        [[fallthrough]];
    case 0x200: {
        MapItem& b = items_[ir - 1];
        if (openable && b.stage >= 1 && !b.need_key && t.src_kind == 1) {   // FUN_0048ba00
            b.can_delete = 0;
            if (!stage_down(m, ir, 0, false)) return;
            food();
        }
        if (t.src_kind != 1 || items_[ir - 1].stage == 0) return;
        if (Unit* u = w.unit(t.sender)) u->m.interact_item = ir;   // +0x2e5
        return;
    }
    default: return;
    }
}

void MapObjects::obstacle_event(Mission& m, int ir, const Trigger& t, bool house) {
    World& w = m.world();
    const ObjRes* r = res(items_[ir - 1].res);
    if (!r) return;
    float dmg = 0;
    if (t.type == 0x4000) {
        if (t.src_kind != 2) return;
        dmg = valid(t.sender) ? float(items_[t.sender - 1].damage) : 0.0f;   // 0x452b20 / 0x48ee90
    } else if (house) {
        return;   // houses react to kegs only
    } else if (t.type == 1) {
        if (t.src_kind != 1 || !hit_test(m, items_[ir - 1], t.sender, 0)) return;
        const Unit* u = w.unit(t.sender);
        if (!u || u->cls < 0 || !formats::iequals(m.data().units[u->cls].code, "LAHIRE")) return;
        dmg = w.melee_attack_power(*u) * 0.5f;   // 0x44a5f0
    } else if (t.type == 0x40 || t.type == 0x2000) {
        const Unit* u = w.unit(t.sender);
        if (t.src_kind != 1 || !u) return;
        dmg = w.ranged_base_damage(*u) * 0.5f;   // 0x452280
    } else {
        return;
    }
    MapItem& o = items_[ir - 1];
    o.hp = std::max(0, int(float(o.hp) - dmg));
    if (o.stage == 0 || dmg == 0.0f || o.stage > int(r->stages.size())) return;
    if (o.hp <= r->stages[o.stage - 1].margin && !stage_down(m, ir, 5000, false)) return;   // fade -0.2
    const MapItem& x = items_[ir - 1];
    if (x.stage == 0 && x.hp == 0) m.emit(MissionEvent::Kind::Ui, ir, 0, "item_destroyed");   // collision off
}

void MapObjects::keg_event(Mission& m, int ir, const Trigger& t) {
    const bool hit = (t.type == 0x2000 && t.src_kind == 1) || (t.type == 0x4000 && t.src_kind == 2);
    const ObjRes* r = res(items_[ir - 1].res);
    if (!hit || !r || items_[ir - 1].stage == 0 || items_[ir - 1].stage > int(r->stages.size())) return;
    const int kind = r->stages[items_[ir - 1].stage - 1].kind;
    if (!stage_down(m, ir, 0, false)) return;
    if (kind == 0 || kind == 1) {   // explosion: objects then units, radius 200, source = keg
        const MapItem& k = items_[ir - 1];
        post(m, 0x4000, 2, 0, 2, ir, k.pos, 200.0f, k.damage);
        post(m, 0x4000, 1, 0, 2, ir, k.pos, 200.0f, k.damage);
        m.emit(MissionEvent::Kind::Sound, ir, 0x4000, "keg_explode");
    }
}

void MapObjects::lever_event(Mission& m, int ir, const Trigger& t) {
    MapItem& l = items_[ir - 1];
    if (!l.lever_name.empty()) l.lever_target = find_by_name(l.lever_name), l.lever_name.clear();
    if (t.type != 0x200 || !l.interact || !valid(l.lever_target) || t.src_kind != 1 || !interact_ok(m, l, t.sender)) return;
    Trigger o;
    o.type = l.interact, o.target_kind = 2, o.target_id = items_[l.lever_target - 1].modify;
    o.src_kind = 2, o.sender = ir, o.pos = l.pos, o.radius = 1.0f;
    o.start = o.end = m.world().now();
    m.world().post_trigger(o);
}

// ---------------------------------------------------------------------------------------------
// Save / load (derived ItemRes and hooks come from load()).

void MapObjects::save(ByteWriter& w) const {
    Out o{w};
    w.u32(std::uint32_t(items_.size()));
    for (MapItem it : items_) it.visit(o);
    put_ints(w, free_);
    put_ints(w, modify_);
    put_ints(w, modify_free_);
    w.u32(std::uint32_t(triggers_.size()));
    for (MapTrigger t : triggers_) t.visit(o);
}

bool MapObjects::load_state(formats::ByteReader& r) {
    In in{r};
    std::uint32_t n = r.u32();
    if (n > r.remaining()) return false;
    items_.assign(n, MapItem{});
    for (MapItem& it : items_) it.visit(in);
    if (!get_ints(r, free_) || !get_ints(r, modify_) || !get_ints(r, modify_free_)) return false;
    n = r.u32();
    if (n > r.remaining()) return false;
    triggers_.assign(n, MapTrigger{});
    for (MapTrigger& t : triggers_) t.visit(in);
    for (const MapItem& it : items_)
        if (!it.deleted && it.cls == GATE) gate_nav(it);
    return r.ok();
}

}  // namespace joa::game
