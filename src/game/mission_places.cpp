#include "game/mission_places.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <type_traits>

#include "formats/res_archive.hpp"
#include "formats/world_text.hpp"
#include "game/mission.hpp"

namespace joa::game {

namespace {

constexpr double kDegToRad = 0.017453292519943295;  // 0x3f91df46a2529d39

// FUN_007c07b0 (_stricmp): byte-wise compare of the lower-cased strings.
int icmp(std::string_view a, std::string_view b) {
    for (size_t i = 0;; ++i) {
        const int ca = i < a.size() ? std::tolower(static_cast<unsigned char>(a[i])) : 0;
        const int cb = i < b.size() ? std::tolower(static_cast<unsigned char>(b[i])) : 0;
        if (ca != cb || ca == 0) return ca - cb;
    }
}
bool ieq(std::string_view a, std::string_view b) { return icmp(a, b) == 0; }

float atof_(const std::string& s) { return float(std::atof(s.c_str())); }
int atoi_(const std::string& s) { return std::atoi(s.c_str()); }

std::optional<formats::BlockText> read_blocks(const formats::Vfs& vfs, const std::string& path) {
    auto bytes = vfs.read(path);
    if (!bytes) return std::nullopt;
    return formats::parse_block_text(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
}

// Positional field reader: the original reads block lines in order (FileTxt), checking only some keys.
struct Fields {
    const std::vector<formats::KvLine>& f;
    size_t i = 0;
    bool done() const { return i >= f.size(); }
    const std::string& key() const { static const std::string e; return done() ? e : f[i].key; }
    const std::string& value() { static const std::string e; return done() ? e : f[i++].value; }
    // "ENTRANCE_END;" / "PARKING_END;" lines (kept with an empty key by the block parser).
    bool at(std::string_view marker) const { return !done() && (ieq(f[i].key, marker) || (f[i].key.empty() && ieq(f[i].value, marker))); }
};

// Box of the loaders (FUN_0055a090 / FUN_0055ae40): AABB of the W x L rectangle turned by `a`,
// z from Z-64 to Z+H+64.
void world_box(Vec3 p, float a, float w, float l, float h, Vec3& mn, Vec3& mx) {
    const double c = std::fabs(std::cos(double(a))), s = std::fabs(std::sin(double(a)));
    const double hx = s * (l / 2.0f) + c * (w / 2.0f), hy = s * (w / 2.0f) + c * (l / 2.0f);
    mn = {float(p.x - hx), float(p.y - hy), p.z - 64.0f};
    mx = {float(hx + p.x), float(hy + p.y), h + 64.0f + p.z};
}

bool aabb_contains(Vec3 mn, Vec3 mx, Vec3 p) {  // AABB_contains 0x60d5f0, inclusive
    return mn.x <= p.x && p.x <= mx.x && mn.y <= p.y && p.y <= mx.y && mn.z <= p.z && p.z <= mx.z;
}

std::string clip32(std::string s) {
    if (s.size() > 32) s.resize(32);
    return s;
}

Unit* live(World& w, int id) {
    Unit* u = w.unit(id);
    return u && !u->deleted ? u : nullptr;
}

// --- save helpers (visit-based, same layout rules as World::serialize) ---
struct Out {
    ByteWriter& w;
    template <class T> void operator()(T& x) {
        if constexpr (std::is_arithmetic_v<T>) w.put(x);
        else if constexpr (std::is_same_v<T, std::string>) w.str(x);
        else if constexpr (requires { x.size(); x.resize(0); x[0]; }) {
            w.u32(std::uint32_t(x.size()));
            for (auto& e : x) (*this)(e);
        } else x.visit(*this);
    }
};
struct In {
    formats::ByteReader& r;
    template <class T> void operator()(T& x) {
        if constexpr (std::is_arithmetic_v<T>) x = r.get<T>();
        else if constexpr (std::is_same_v<T, std::string>) {
            const auto s = r.bytes(r.u32());
            x.assign(reinterpret_cast<const char*>(s.data()), s.size());
        } else if constexpr (requires { x.size(); x.resize(0); x[0]; }) {
            const std::uint32_t n = r.u32();
            if (!r.ok() || n > r.remaining()) return r.fail();
            x.resize(n);
            for (auto& e : x) (*this)(e);
        } else x.visit(*this);
    }
};

}  // namespace

// ------------------------------------------------------------------------------------------
// Loading
// ------------------------------------------------------------------------------------------

bool Places::load(Mission& m, const formats::Vfs& vfs, const std::string& map_dir, const std::string& stem, std::string*) {
    m_ = &m;
    *this = Places{};
    m_ = &m;
    GameData& gd = m.data();
    nations_ = int(gd.nations.size());
    relations_.assign(size_t(nations_ * nations_), Relation{});

    // RECRUIT.RES (RecruitRes_load 0x4ca760 / RecruitRes_parse_record 0x4ca980): record id =
    // 1-based entry order; Code, optional Location / Level / Population.
    if (auto bytes = vfs.read("UNIT\\RECRUIT.RES"))
        if (auto res = formats::ResArchive::parse(std::move(*bytes)))
            for (const auto& e : res->entries()) {
                if (e.name.empty()) continue;
                const auto d = res->data(e);
                RecruitRec rec;
                for (const auto& kv : formats::parse_kv_text(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()))) {
                    if (ieq(kv.key, "Code")) rec.code = clip32(kv.value);
                    else if (ieq(kv.key, "Location")) rec.location = kv.value.substr(0, 0x80);   // RecruitWorkshop
                    else if (ieq(kv.key, "Level")) rec.level = atoi_(kv.value);
                    else if (ieq(kv.key, "Population")) rec.population = atoi_(kv.value);
                    else if (const int t = gd.unit_index(kv.key); t >= 0) {   // RecruitWorkshop: "CODE : time : crew"
                        const size_t c = kv.value.find(':');
                        rec.entries.push_back({t + 1, int(std::atof(kv.value.c_str()) * 1000.0f),
                                               c == std::string::npos ? 0 : atoi_(kv.value.substr(c + 1))});
                    }
                }
                recruit_.push_back(std::move(rec));
            }

    // World nodes (Battle::load_world): every node's .pla / .trk, in .wor order, shifted by
    // the node origin (FUN_0047ad40, Level +0x44/+0x48).
    std::vector<WorldNode> nodes = world_nodes(vfs, map_dir, m.info().step);
    if (nodes.empty()) nodes.push_back({stem, 0.0f, 0.0f});
    std::string world = m.info().step.world_file;
    if (const auto p = world.find_last_of("\\/"); p != std::string::npos) world = world.substr(p + 1);

    for (const WorldNode& wn : nodes) {
        const std::string& node = wn.name;
        const float ox = float(wn.ox), oy = float(wn.oy);
        if (auto pla = read_blocks(vfs, map_dir + "\\" + node + ".pla")) {
            for (const auto& b : pla->blocks) {
                Fields f{b.fields};
                if (ieq(b.type, "CreatePlace")) {  // FUN_0055a090
                    Place p;
                    p.recno = int(places_.size()) + 1;
                    p.model = f.value();
                    p.name = clip32(f.value());
                    const std::string sc = f.value();
                    p.scale = ieq(sc, "VILLAGE") ? 1 : ieq(sc, "TOWN") ? 2 : ieq(sc, "CASTLE") ? 3 : ieq(sc, "UMBRELLA") ? 4 : 0;
                    p.pos.x = atof_(f.value()) + ox, p.pos.y = atof_(f.value()) + oy, p.pos.z = atof_(f.value());
                    const double k = ieq(f.key(), "HD") ? kDegToRad : 1.0;
                    p.angle = float(std::atof(f.value().c_str()) * k);
                    const float w = atof_(f.value()), l = atof_(f.value()), h = atof_(f.value());
                    while (!f.done() && !f.at("ENTRANCE_END")) {
                        Vec3 e{atof_(f.value()) + ox, atof_(f.value()) + oy, atof_(f.value())};
                        if (p.entrances.size() < 8) p.entrances.push_back(e);  // FUN_004c22b0
                    }
                    ++f.i;
                    while (!f.done() && !f.at("PARKING_END")) {
                        Vec3 e{atof_(f.value()) + ox, atof_(f.value()) + oy, atof_(f.value())};
                        const double dk = ieq(f.key(), "PARKING_DIR") ? kDegToRad : 1.0;
                        const float dir = float(std::atof(f.value().c_str()) * dk);
                        if (p.parking.size() < 13) p.parking.push_back(e), p.parking_dir.push_back(dir);  // FUN_00438420
                    }
                    ++f.i;
                    const std::string nk = f.key();
                    const std::string nv = f.value();
                    if (ieq(nk, "NATION")) p.nation = atoi_(nv);
                    else if (ieq(nk, "NATION_ID")) p.nation = atoi_(nv);  // FUN_004b67e0: nation recno == id
                    else if (ieq(nk, "Kingdom")) p.nation = gd.nation_id(nv);
                    if (p.angle == 0.0f) world_box(p.pos, p.angle, w, l, h, p.box_min, p.box_max);
                    else p.box_min = {-w * 0.5f, -l * 0.5f, -64.0f}, p.box_max = {w * 0.5f, l * 0.5f, h + 64.0f};
                    places_.push_back(std::move(p));
                } else if (ieq(b.type, "CreateFacility")) {  // FUN_0055ae40
                    Facility fc;
                    const std::string cls = f.value();
                    static constexpr const char* kKinds[] = {"BARRACK", "SHOP", "RECRUIT", "STABLE", "WEAPON", "GRANARY",
                                                             "HOUSE", "PUB", "CHAPEL", "POST", "GUARD", "KING", "PRISON",
                                                             "HALL", "MONASTERY"};
                    if (ieq(cls, "WORKSHOP")) fc.kind = Facility::Workshop;
                    else
                        for (int i = 0; i < 15; ++i)
                            if (ieq(cls, kKinds[i])) fc.kind = i + 1;
                    // FUN_0046cef0 only creates BARRACK / SHOP / RECRUIT / WORKSHOP objects.
                    if (fc.kind != Facility::Barrack && fc.kind != Facility::Shop && fc.kind != Facility::Recruit &&
                        fc.kind != Facility::Workshop)
                        continue;
                    fc.recno = int(facilities_.size()) + 1;
                    fc.place = place_by_name(clip32(f.value()));
                    fc.name = clip32(f.value());
                    fc.pos.x = atof_(f.value()) + ox, fc.pos.y = atof_(f.value()) + oy, fc.pos.z = atof_(f.value());
                    const double k = ieq(f.key(), "HD") ? kDegToRad : 1.0;
                    fc.angle = float(std::atof(f.value().c_str()) * k);
                    const float w = atof_(f.value()), l = atof_(f.value()), h = atof_(f.value());
                    while (!f.done() && !f.at("ENTRANCE_END")) {
                        Vec3 e{atof_(f.value()) + ox, atof_(f.value()) + oy, atof_(f.value())};
                        if (fc.entrances.size() < 2) fc.entrances.push_back(e);  // FUN_0046d750
                    }
                    ++f.i;
                    world_box(fc.pos, 0.0f, w, l, h, fc.box_min, fc.box_max);  // always unrotated
                    if (fc.kind == Facility::Workshop && ieq(f.key(), "RECRUIT_LIST")) {  // FUN_0046b840
                        if (const int list = recruit_list_by_code(f.value())) fc.recruit_list = list;
                        const std::string t = f.value();
                        if (ieq(t, "NORMAL")) fc.workshop_type = 0;
                        else if (ieq(t, "IRON")) fc.workshop_type = 1;
                        else if (ieq(t, "WOOD")) fc.workshop_type = 2;
                        fc.resource = atoi_(f.value());
                    }
                    if (Place* p = place(fc.place)) p->facilities.push_back(fc.recno);  // FUN_004c39d0
                    const int list = fc.kind == Facility::Workshop ? fc.recruit_list : 0;
                    facilities_.push_back(std::move(fc));
                    if (list) set_workshop_list(int(facilities_.size()), list);   // RecruitWorkshop: products
                }
            }
        }
        // Tracks (FUN_004ebb40 / FUN_004ebd50). Track recno = order of CreateTrack blocks; the
        // original writes past the array `size`, only a size of 0 stops the file.
        if (auto trk = read_blocks(vfs, map_dir + "\\" + node + ".trk")) {
            bool open = false;
            for (const auto& b : trk->blocks) {
                if (ieq(b.type, "CreateTrackArray")) {
                    if (atoi_(b.fields.empty() ? std::string() : b.fields[0].value) == 0) break;
                    open = true;
                    continue;
                }
                if (!ieq(b.type, "CreateTrack")) continue;
                if (!open) break;
                Fields f{b.fields};
                Track t;
                t.recno = int(tracks_.size()) + 1;
                t.map_name = f.value();
                t.code = f.value();
                const int n = atoi_(f.value());
                t.points.resize(size_t(std::max(0, n)));
                std::vector<int> adj_cap(t.points.size());
                for (int i = 0; i < n; ++i) {
                    const int r = atoi_(f.value());
                    TrackPoint tp;
                    tp.recno = r;
                    tp.type = atoi_(f.value());
                    tp.pos.x = float(int(atof_(f.value()))) + ox, tp.pos.y = float(int(atof_(f.value()))) + oy, tp.pos.z = float(int(atof_(f.value())));
                    const int na = atoi_(f.value());
                    if (r >= 1 && r <= n) adj_cap[size_t(r - 1)] = na, t.points[size_t(r - 1)] = std::move(tp);
                }
                while (!f.done() && ieq(f.key(), "edge_start")) {
                    const int a = atoi_(f.value()), c = atoi_(f.value());
                    if (a < 1 || a > n || c < 1 || c > n) continue;
                    if (int(t.points[size_t(a - 1)].adj.size()) < adj_cap[size_t(a - 1)]) t.points[size_t(a - 1)].adj.push_back(c);
                    if (int(t.points[size_t(c - 1)].adj.size()) < adj_cap[size_t(c - 1)]) t.points[size_t(c - 1)].adj.push_back(a);
                }
                tracks_.push_back(std::move(t));
            }
        }
    }

    // Territories (<world>.ter, FUN_00547590 / FUN_004f9330), then qsort by name and renumber.
    if (auto ter = read_blocks(vfs, map_dir + "\\" + world + ".ter")) {
        for (const auto& b : ter->blocks) {
            if (!ieq(b.type, "CreateTerritory")) continue;
            Fields f{b.fields};
            Territory t;
            t.name = clip32(f.value());
            t.center_x = atoi_(f.value()), t.center_y = atoi_(f.value());
            t.coord_x = atoi_(f.value()), t.coord_y = atoi_(f.value());
            const std::string k = f.value();
            t.owner = gd.nation_id(k);
            if (t.owner == 0 && ieq(k, "Neutral")) t.owner = -1;
            if (ieq(f.key(), "DisableBeingAttacked")) t.no_attack = std::uint8_t(atoi_(f.value()));
            if (ieq(f.key(), "ScrName")) t.scr_name = f.value().substr(0, 8);
            if (ieq(f.key(), "Type")) {
                const std::string ty = f.value();
                static constexpr const char* kTypes[] = {"camp", "town", "city", "castle", "church", "forest"};
                for (int i = 0; i < 6; ++i)
                    if (ieq(ty, kTypes[i])) t.type = i;
            }
            const std::string sc = f.value();
            static constexpr const char* kScales[] = {"settlement", "small_camp", "large_camp", "castle", "paris"};
            for (int i = 0; i < 5; ++i)
                if (ieq(sc, kScales[i])) t.scale = i;
            const int n = atoi_(f.value());
            for (int i = 0; i < n; ++i) {
                Territory::Area a;
                const std::string shape = f.value();
                a.shape = ieq(shape, "circle") ? 1 : ieq(shape, "quadrilateral") ? 2 : 0;
                if (a.shape == 0) a.x[0] = atoi_(f.value()), a.y[0] = atoi_(f.value()), a.x[1] = atoi_(f.value()), a.y[1] = atoi_(f.value());
                else if (a.shape == 1) a.x[0] = atoi_(f.value()), a.y[0] = atoi_(f.value()), a.r = atoi_(f.value());
                else
                    for (int q = 0; q < 4; ++q) a.x[q] = atoi_(f.value()), a.y[q] = atoi_(f.value());
                t.areas.push_back(a);
            }
            territories_.push_back(std::move(t));
        }
        std::stable_sort(territories_.begin(), territories_.end(),
                         [](const Territory& a, const Territory& b) { return icmp(a.name, b.name) < 0; });
        for (size_t i = 0; i < territories_.size(); ++i) territories_[i].recno = int(i) + 1;
    }

    sync_ai(m);
    return true;
}

void Places::sync_ai(Mission& m) const {
    AiSystem& ai = m.ai();
    for (const Place& p : places_) {
        AiPlace& a = ai.places[p.recno];
        a.centre = p.pos, a.min = p.box_min, a.max = p.box_max;
        a.slots = p.parking;
        a.slot_facing = p.parking_dir;
        a.occupant.resize(p.parking.size(), 0);
    }
    for (const Facility& f : facilities_) {
        AiPlace& z = ai.zones[f.recno];
        z.centre = f.pos, z.min = f.box_min, z.max = f.box_max;
    }
    ai.tracks.clear();
    for (const Track& t : tracks_) {
        auto& v = ai.tracks[t.recno];
        for (const TrackPoint& p : t.points) v.push_back(p.pos);
    }
}

// ------------------------------------------------------------------------------------------
// Lookups
// ------------------------------------------------------------------------------------------

const Place* Places::place(int recno) const { return recno >= 1 && recno <= int(places_.size()) ? &places_[size_t(recno - 1)] : nullptr; }
Place* Places::place(int recno) { return recno >= 1 && recno <= int(places_.size()) ? &places_[size_t(recno - 1)] : nullptr; }
bool Places::inside(int recno, Vec3 pos) const {
    const Place* p = place(recno);
    return p && aabb_contains(p->box_min, p->box_max, pos);
}
int Places::place_by_name(std::string_view name) const {
    for (const Place& p : places_)
        if (ieq(p.name, name)) return p.recno;
    return 0;
}
bool Places::facility_valid(int recno) const { return recno >= 1 && recno <= int(facilities_.size()); }
const Facility* Places::facility(int recno) const { return facility_valid(recno) ? &facilities_[size_t(recno - 1)] : nullptr; }
Facility* Places::facility(int recno) { return facility_valid(recno) ? &facilities_[size_t(recno - 1)] : nullptr; }
int Places::facility_by_name(std::string_view name) const {
    for (int i = int(facilities_.size()); i >= 1; --i)
        if (ieq(facilities_[size_t(i - 1)].name, name)) return i;
    return 0;
}
int Places::track_id(std::string_view code) const {
    for (const Track& t : tracks_)
        if (ieq(t.code, code)) return t.recno;
    return 0;
}
const Track* Places::track(int id) const { return id >= 1 && id <= int(tracks_.size()) ? &tracks_[size_t(id - 1)] : nullptr; }
int Places::territory_by_name(std::string_view name) const {
    for (int i = int(territories_.size()); i >= 1; --i)
        if (ieq(territories_[size_t(i - 1)].name, name)) return i;
    return 0;
}
const Territory* Places::territory(int recno) const {
    return recno >= 1 && recno <= int(territories_.size()) ? &territories_[size_t(recno - 1)] : nullptr;
}
Territory* Places::territory(int recno) { return recno >= 1 && recno <= int(territories_.size()) ? &territories_[size_t(recno - 1)] : nullptr; }
int Places::recruit_list_by_code(std::string_view code) const {
    for (size_t i = 0; i < recruit_.size(); ++i)
        if (ieq(recruit_[i].code, code)) return int(i) + 1;
    return 0;
}
const Places::RecruitRec* Places::recruit_rec(int id) const {
    return id >= 1 && id <= int(recruit_.size()) ? &recruit_[size_t(id - 1)] : nullptr;
}

Relation& Places::relation(int a, int b) {
    static Relation none;
    if (a < 1 || b < 1 || a > nations_ || b > nations_) return none = Relation{};
    return relations_[size_t((a - 1) * nations_ + (b - 1))];
}

// Nation::set_relation_status FUN_00499770(b, status, recursive). The friendship cap at +0x12
// and the +2 treaty flags (FUN_004b4610) have no reader and are not modelled.
void Places::set_relation_status(Mission& m, int a, int b, int status) {
    const int today = int(m.world().now() / 3600000);  // DAT_00a47bb0 day number; 1 game day = 3600 s
    auto one = [&](int x, int y, bool recursive) {
        if (x == y) return false;
        Relation& r = relation(x, y);
        if (!recursive && r.status == 1 && status == 0 && r.status_day + 5 > today) return false;
        r.status = status;
        r.status_day = today;
        if (status == 4 || status == 3 || status == 1) m.world().set_hostile(x, y, false);  // FUN_004b44e0(y, 0)
        else if (status == 0) m.world().set_hostile(x, y, true);
        return true;
    };
    if (one(a, b, false)) one(b, a, true);
}

// ------------------------------------------------------------------------------------------
// Per tick
// ------------------------------------------------------------------------------------------

void Places::tick(Mission& m) {
    World& w = m.world();
    AiSystem& ai = m.ai();
    constexpr int dt = World::kTickMs;  // DAT_00a500be
    // place_array FUN_004c55d0 -> Place::process FUN_004c3330.
    for (Place& p : places_) {
        AiPlace& a = ai.places[p.recno];
        for (size_t i = 0; i < a.occupant.size(); ++i) {
            if (a.occupant[i] == 0) continue;
            if (Unit* u = live(w, a.occupant[i])) {
                if (!aabb_contains(p.box_min, p.box_max, u->pos)) {
                    a.occupant[i] = 0;
                    AiUnit& s = ai.st(*u);
                    s.place = 0, s.place_slot = 0;  // unit +4 / +6
                }
            } else {
                a.occupant[i] = 0;
            }
        }
        p.timer += dt;
        if (unsigned(p.timer) > 59999u) {  // population regrowth, one per minute
            if (p.pop_max > p.pop) p.pop += 1;
            p.timer = 0;
        }
    }
    // facility_array FacilityArray_process 0x46d4a0: vtbl+0x10 of each facility, ascending.
    for (Facility& f : facilities_) {
        if (f.kind == Facility::Barrack) tick_barrack(m, f);
        else if (f.kind == Facility::Recruit) tick_recruit(m, f);   // RecruitWorkshop: RecruitFacility_process 0x468600
        else if (f.kind == Facility::Workshop) tick_workshop(m, f); // RecruitWorkshop: build queue 0x46a040
        // SHOP 0x469710 (hourly restock of the buy-interface stock) only acts on interface data.
    }
    tick_territories(m);
}

// SpawnBarrack_process 0x467590: at most one unit per tick.
void Places::tick_barrack(Mission& m, Facility& f) {
    SpawnBarrack& b = f.barrack;
    if (b.leader_type == 0 && b.groups == 0) return;
    World& w = m.world();
    AiSystem& ai = m.ai();
    GameData& gd = m.data();
    const Place* owner = place(f.place);
    if (!owner) return;  // the original dereferences a null owner here
    auto rect_clear = [&] {  // Facility_is_spawn_rect_clear: any non-deleted unit in the x/y rect
        for (int i = int(w.units().size()); i >= 1; --i) {
            const Unit& u = w.units()[size_t(i - 1)];
            if (!u.deleted && f.box_min.x <= u.pos.x && u.pos.x <= f.box_max.x && f.box_min.y <= u.pos.y && u.pos.y <= f.box_max.y)
                return false;
        }
        return true;
    };
    auto spawn_pos = [&] {  // SpawnBarrack_get_spawn_pos: rect centre, terrain height
        Vec3 p{(f.box_min.x + f.box_max.x) / 2.0f, (f.box_min.y + f.box_max.y) / 2.0f, f.pos.z};
        if (w.terrain() && f.pos.z + 888.0f + 64.0f != f.box_max.z) p.z = w.terrain()->height(p.x, p.y);
        return p;
    };
    auto spawn = [&](int type) -> Unit* {
        if (type < 1 || type > int(gd.units.size())) return nullptr;
        const int id = w.spawn_unit(gd.units[size_t(type - 1)].code, owner->nation, {}, f.angle / 0.017453292f);
        Unit* u = live(w, id);
        if (!u) return nullptr;
        u->ai_controlled = true;  // +0x102 = 1
        ai.st(*u).spawned = 1;
        return u;
    };
    auto add_order = [&](Unit& u) {
        if (b.order.action == 0) return;
        Order o = b.order;
        o.expire = w.now() + b.order_ms;
        ai.st(u).orders.add(o);
    };
    auto finish = [&](Unit& u, int level) {
        w.set_level(u, level);
        if (u.rank < 4) w.ai_auto_upgrade(u);
        w.set_hp(u, w.max_hp(u));
    };
    const bool training = m.scenario().mode == 2;

    if (b.leader_type != 0) {
        if (b.leader == 0) {
            if (!rect_clear()) return;
            Unit* L = spawn(b.leader_type);
            if (!L) return;
            L->short_name = b.title;
            L->name = b.name;
            L->pos = spawn_pos();
            L->team = b.team;
            if (gd.units[size_t(L->cls)].ranking > 2) {
                ai.create_team(*L);
                for (int i = int(w.units().size()); i >= 1; --i) {
                    Unit& u = w.units()[size_t(i - 1)];
                    if (!u.deleted && u.team == L->team && ai.leader_of(u) == 0) ai.join_team(u, L->id);
                }
            }
            if (training) L->m.training = 2;
            add_order(*L);
            b.leader = L->id;
            finish(*L, b.leader_level);
            return;
        }
        if (!live(w, b.leader)) {
            b.reset();
            return;
        }
    }
    if (b.groups == 0) {
        b.reset();
        return;
    }
    if (b.count[0] == 0) {
        if (b.groups < 2) b.reset();
        else {  // pop group 0: counts and types shift, levels do not (CONFIRMED quirk)
            --b.groups;
            std::copy(b.count.begin() + 1, b.count.begin() + 1 + b.groups, b.count.begin());
            std::copy(b.type.begin() + 1, b.type.begin() + 1 + b.groups, b.type.begin());
        }
        return;
    }
    if (!rect_clear()) return;
    Unit* M = spawn(b.type[0]);
    if (!M) return;
    M->team = b.team;
    for (int i = int(w.units().size()); i >= 1; --i) {  // Unit_find_leader_by_team_id 0x51fe60
        const Unit& u = w.units()[size_t(i - 1)];
        if (u.deleted || u.team != M->team) continue;
        if (const int l = ai.leader_of(u)) {
            ai.join_team(*M, l);
            break;
        }
    }
    M->pos = spawn_pos();
    add_order(*M);
    if (live(w, b.follow)) ai.set_task(*M, Task{task::FOLLOW_UNIT, 5, b.follow, 0, 0, 0, 0});
    finish(*M, b.level[0]);
    if (training) M->m.training = 2;
    --b.count[0];
}

// TerritoryArray_process 0x4e4bb0 (GAMEPLAY_AI_TROOPS.md Part D §5).
void Places::tick_territories(Mission& m) {
    if (territories_.empty()) return;
    World& w = m.world();
    const GameData& gd = w.data();
    auto inside = [](const Territory& t, float x, float y) {
        for (const Territory::Area& a : t.areas) {
            bool in = false;
            if (a.shape == 0) in = float(a.x[0]) <= x && x <= float(a.x[1]) && float(a.y[0]) <= y && y <= float(a.y[1]);
            else if (a.shape == 1) {
                const float dx = x - float(a.x[0]), dy = y - float(a.y[0]);
                in = dy * dy + dx * dx < float(a.r * a.r);
            } else {
                in = true;
                for (int k = 0; k < 4 && in; ++k) {
                    const int n = (k + 1) & 3;
                    in = (x - float(a.x[k])) * float(a.y[n] - a.y[k]) - (y - float(a.y[k])) * float(a.x[n] - a.x[k]) < 0.0f;
                }
            }
            if (in) return true;
        }
        return false;
    };
    // (a) player unit: enter / leave messages (throttled 4000 ms), current territory (+8).
    if (const Unit* pl = live(w, w.player_id())) {
        for (int i = int(territories_.size()); i >= 1; --i) {
            Territory& t = territories_[size_t(i - 1)];
            const bool in = inside(t, pl->pos.x, pl->pos.y);
            if (unsigned(w.now() - territory_msg_ms_) > 3999u && in != (t.player_inside != 0)) {
                m.emit(MissionEvent::Kind::Text, t.recno, in ? 1 : 0, t.name);  // news message by territory type
                territory_msg_ms_ = w.now();
            }
            t.player_inside = in;
            if (in) player_territory_ = t.recno;
        }
    }
    // (b) one owned territory per tick (round robin): under attack when a foreign fighting unit is inside.
    const int n = int(territories_.size());
    if (territory_cursor_ > n) territory_cursor_ = n;
    else if (territory_cursor_ == 0) territory_cursor_ = 1;
    int k = territory_cursor_;
    do {
        Territory& t = territories_[size_t(k - 1)];
        if (t.owner != -1 && t.no_attack == 0) {
            bool hit = false;
            if (!t.areas.empty())
                for (int i = int(w.units().size()); i >= 1 && !hit; --i) {
                    const Unit& u = w.units()[size_t(i - 1)];
                    if (u.deleted || u.lod_tier >= 4 || u.nation == t.owner || u.cls < 0) continue;
                    const UnitClass& c = gd.units[size_t(u.cls)];
                    if (c.special == 2 || c.special == 7 || c.unit_class == 0 || c.unit_class == 8 || c.unit_class == 9) continue;
                    if (u.no_react || w.dead(u)) continue;
                    if (u.nation != 0 && relation(u.nation, t.owner).status == 4) continue;
                    hit = inside(t, u.pos.x, u.pos.y);
                }
            if (hit && t.under_attack == 0) t.attack_ms = w.now();
            t.under_attack = hit;
            territory_cursor_ = territory_cursor_ + 1 > n ? 1 : territory_cursor_ + 1;
            return;
        }
        k = k + 1 > n ? 1 : k + 1;
    } while (k != territory_cursor_);
}

// ------------------------------------------------------------------------------------------
// Save / load
// ------------------------------------------------------------------------------------------

void Places::save(ByteWriter& w) const {
    Places& self = const_cast<Places&>(*this);
    Out o{w};
    o(self.places_), o(self.facilities_), o(self.territories_), o(self.relations_);
    w.i32(territory_msg_ms_), w.i32(territory_cursor_), w.i32(player_territory_);
    // Dynamic place state kept in AiSystem's hook map: view angle and parking occupants.
    for (const Place& p : places_) {
        AiPlace* a = nullptr;
        if (m_)
            if (auto it = m_->ai().places.find(p.recno); it != m_->ai().places.end()) a = &it->second;
        w.i32(a ? a->view_angle : 0);
        std::vector<int> occ = a ? a->occupant : std::vector<int>{};
        o(occ);
    }
}

bool Places::load_state(formats::ByteReader& r) {
    In in{r};
    in(places_), in(facilities_), in(territories_), in(relations_);
    territory_msg_ms_ = r.i32(), territory_cursor_ = r.i32(), player_territory_ = r.i32();
    std::vector<std::pair<int, std::vector<int>>> dyn(places_.size());
    for (auto& d : dyn) d.first = r.i32(), in(d.second);
    if (!r.ok()) return false;
    if (m_) {
        sync_ai(*m_);
        for (size_t i = 0; i < places_.size(); ++i) {
            AiPlace& a = m_->ai().places[places_[i].recno];
            a.view_angle = dyn[i].first;
            a.occupant = dyn[i].second;
            a.occupant.resize(a.slots.size(), 0);
        }
    }
    return true;
}

}  // namespace joa::game
