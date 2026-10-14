#include "game/campaign.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "formats/vfs_fs.hpp"
#include "game/unitclass_db.hpp"

namespace joa::game {

std::string CampaignMission::stem() const { return step.script_file; }

std::vector<CampaignMission> load_campaign(const formats::Vfs& vfs, std::string* err) {
    std::vector<CampaignMission> out;
    for (int no = 1; no <= 8; ++no) {
        char path[40];
        std::snprintf(path, sizeof path, "SCENARIO\\scenario_%02d.scr", no);  // FUN_004ccfb0
        auto bytes = vfs.read(path);
        if (!bytes) {
            if (err) *err = std::string("missing ") + path;
            return {};
        }
        auto def = formats::parse_scenario_def(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), err);
        if (!def || def->steps.empty()) return {};
        out.push_back({no, def->code, def->steps.front()});
    }
    return out;
}

HeroRecord* CampaignProfile::find(std::string_view code) {
    for (auto& h : heroes)
        if (formats::iequals(h.code, code)) return &h;  // FUN_004c81a0 stricmp
    return nullptr;
}

void copy_persistent_unit(const Unit& s, Unit& d) {
    d.name = s.name;  // +0x3d, 0x42 bytes
    d.title = s.title;
    d.rank = s.rank, d.kingdom = s.kingdom, d.race = s.race, d.team = s.team;
    // memcpy +0x11c..+0x16e
    d.max_lives = s.max_lives, d.lives = s.lives, d.level = s.level, d.xp = s.xp;
    d.skill_points = s.skill_points, d.attr_points = s.attr_points, d.level_arrow = s.level_arrow, d.ts_level = s.ts_level;
    d.hp = s.hp, d.hp_trail = s.hp_trail, d.base_max_hp = s.base_max_hp, d.ep = s.ep, d.base_max_ep = s.base_max_ep;
    d.ts_ep_regen = s.ts_ep_regen, d.str = s.str, d.dex = s.dex, d.def = s.def, d.lead = s.lead;
    d.armor_level = s.armor_level, d.armor_item = s.armor_item, d.loyalty = s.loyalty, d.morale = s.morale;
    d.reputation = s.reputation, d.mode = s.mode, d.attack_behaviour = s.attack_behaviour;
    // 0x3b bytes at +0x16f (skill ints archery..mountain + 15 unmodelled bytes)
    d.archery = s.archery, d.horse_riding = s.horse_riding, d.swimming = s.swimming;
    d.elephant_riding = s.elephant_riding, d.wide_animal = s.wide_animal, d.preaching = s.preaching;
    d.power_jump = s.power_jump, d.disguise = s.disguise, d.persuade = s.persuade;
    d.lock_picking = s.lock_picking, d.mountain = s.mountain;
    d.upgrades = s.upgrades;  // FUN_00531230
    d.inv = s.inv;            // FUN_00497a10
    d.model_level = s.model_level;  // +0x3c5
}

namespace {
struct Writer {
    ByteWriter& w;
    void operator()(int v) { w.i32(v); }
    void operator()(float v) { w.f32(v); }
    void operator()(bool v) { w.u8(v); }
    void operator()(std::string& s) { w.str(s); }
    template <class T>
    void operator()(std::vector<T>& v) {
        w.u32(static_cast<std::uint32_t>(v.size()));
        for (auto& e : v) (*this)(e);
    }
    void operator()(std::vector<bool>&) = delete;
    template <class T>
        requires std::is_class_v<T>
    void operator()(T& t) {
        t.visit(*this);
    }
};

struct Reader {
    formats::ByteReader& r;
    void operator()(int& v) { v = r.i32(); }
    void operator()(float& v) { v = r.f32(); }
    void operator()(bool& v) { v = r.u8() != 0; }
    void operator()(std::string& s) {
        const auto n = r.u32();
        auto b = r.bytes(n);
        s.assign(reinterpret_cast<const char*>(b.data()), b.size());
    }
    template <class T>
    void operator()(std::vector<T>& v) {
        const auto n = r.u32();
        if (n > r.remaining()) return r.fail();
        v.resize(n);
        for (auto& e : v) (*this)(e);
    }
    template <class T>
        requires std::is_class_v<T>
    void operator()(T& t) {
        t.visit(*this);
    }
};
}  // namespace

void write_unit(ByteWriter& w, const Unit& u) {
    Unit copy = u;  // visit() is non-const
    Writer wr{w};
    copy.visit(wr);
}

bool read_unit(formats::ByteReader& r, Unit& u) {
    Reader rd{r};
    u.visit(rd);
    return r.ok();
}

void CampaignProfile::save(ByteWriter& w) const {
    w.u8(continued);
    w.u32(static_cast<std::uint32_t>(heroes.size()));
    for (const auto& h : heroes) {
        w.str(h.code);
        write_unit(w, h.unit);
    }
    for (const auto* l : {&item_hints_a, &item_hints_b}) {
        w.u32(static_cast<std::uint32_t>(l->size()));
        for (auto v : *l) w.u16(v);
    }
    w.i32(gold), w.u32(play_ms), w.i32(command_points), w.i32(difficulty);
}

bool CampaignProfile::load(formats::ByteReader& r) {
    continued = r.u8() != 0;
    const auto n = r.u32();
    if (n > 1024) return false;
    heroes.assign(n, {});
    for (auto& h : heroes) {
        const auto len = r.u32();
        auto b = r.bytes(len);
        h.code.assign(reinterpret_cast<const char*>(b.data()), b.size());
        if (!read_unit(r, h.unit)) return false;
    }
    for (auto* l : {&item_hints_a, &item_hints_b}) {
        const auto m = r.u32();
        if (m > r.remaining()) return false;
        l->resize(m);
        for (auto& v : *l) v = r.u16();
    }
    gold = r.i32(), play_ms = r.u32(), command_points = r.i32(), difficulty = r.i32();
    return r.ok();
}

// ---- Save/Player.dat (CONTAINERS_WORLD_SAVES.md §7.2) ----------------------------------------
namespace {
using formats::PlayerBytes;

// Unit image fields carried by the profile (copy_persistent_unit subset; offsets CONFIRMED,
// GAMEPLAY_CHARACTER_COMBAT.md Part A §1). size 2 = short (sign-extended on import).
struct Field {
    std::uint16_t off;
    std::uint8_t size;
    int Unit::*m;
};
constexpr Field kFields[] = {
    {0x031, 2, &Unit::team}, {0x104, 4, &Unit::rank}, {0x108, 2, &Unit::kingdom}, {0x10a, 2, &Unit::race},
    {0x11c, 4, &Unit::max_lives}, {0x120, 4, &Unit::lives}, {0x124, 2, &Unit::level}, {0x126, 4, &Unit::xp},
    {0x12a, 2, &Unit::skill_points}, {0x12c, 2, &Unit::attr_points}, {0x131, 4, &Unit::ts_level},
    {0x135, 4, &Unit::hp}, {0x139, 4, &Unit::hp_trail}, {0x13d, 4, &Unit::base_max_hp}, {0x145, 4, &Unit::ep},
    {0x149, 4, &Unit::base_max_ep}, {0x151, 4, &Unit::ts_ep_regen}, {0x155, 2, &Unit::str}, {0x157, 2, &Unit::dex},
    {0x159, 2, &Unit::def}, {0x15b, 2, &Unit::lead}, {0x15d, 2, &Unit::armor_level}, {0x15f, 2, &Unit::armor_item},
    {0x161, 2, &Unit::loyalty}, {0x163, 2, &Unit::morale}, {0x165, 2, &Unit::reputation}, {0x167, 4, &Unit::mode},
    {0x16b, 4, &Unit::attack_behaviour},
    {0x16f, 4, &Unit::archery}, {0x173, 4, &Unit::horse_riding}, {0x177, 4, &Unit::swimming},
    {0x17b, 4, &Unit::elephant_riding}, {0x17f, 4, &Unit::wide_animal}, {0x183, 4, &Unit::preaching},
    {0x187, 4, &Unit::power_jump}, {0x18b, 4, &Unit::disguise}, {0x18f, 4, &Unit::persuade},
    {0x193, 4, &Unit::lock_picking}, {0x197, 4, &Unit::mountain}, {0x3c5, 4, &Unit::model_level},
};

void put(PlayerBytes& b, std::size_t off, std::int32_t v, std::size_t size) { std::memcpy(b.data() + off, &v, size); }
std::int32_t get(const PlayerBytes& b, std::size_t off, std::size_t size) {
    if (size == 2) {
        std::int16_t v;
        std::memcpy(&v, b.data() + off, 2);
        return v;
    }
    std::int32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
void put_str(PlayerBytes& b, std::size_t off, const std::string& s, std::size_t n) {  // strncpy, NUL kept
    std::memcpy(b.data() + off, s.data(), std::min(s.size(), n - 1));
}
std::string get_str(const PlayerBytes& b, std::size_t off, std::size_t n) {
    return formats::ByteReader::cstr(std::span(b).subspan(off, n));
}

// Bag geometry (0x457290 args: rows 1, cell bytes = width, cols = height) for inventory +0x12/+0x16/+0x1a.
constexpr int kBagW[3] = {4, 6, 6}, kBagH[3] = {1, 8, 4};

// FUN_004575a0 first fit (y outer, x inner) + FUN_00458090 grid fill + FUN_004576f0 append. RecruitWorkshop: an item
// placed in this bag (ItemInstance bag/col/row) keeps its cell when it is free.
bool place(formats::PlayerItemBag& bag, int g, const ItemInstance& it, int w, int h) {
    const int W = kBagW[g], H = kBagH[g];
    auto free = [&](int x, int y) {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx)
                if (bag.grid[size_t(yy * W + xx)]) return false;
        return true;
    };
    auto put_at = [&](int x, int y) {
        const auto idx = std::uint8_t(bag.items.size() + 1);
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx) bag.grid[size_t(yy * W + xx)] = idx;
        PlayerBytes r(0x18, 0);
        put(r, 0, it.item, 2), put(r, 2, it.quantity, 2), put(r, 4, it.level, 2);
        r[0x12] = std::uint8_t(w), r[0x13] = std::uint8_t(h), r[0x15] = std::uint8_t(x), r[0x16] = std::uint8_t(y);
        r[0x17] = idx;
        bag.items.push_back(std::move(r));
        return true;
    };
    if (it.bag == g && it.col >= 0 && it.row >= 0 && it.col <= W - w && it.row <= H - h && free(it.col, it.row))
        return put_at(it.col, it.row);
    for (int y = 0; y <= H - h; ++y)
        for (int x = 0; x <= W - w; ++x)
            if (free(x, y)) return put_at(x, y);
    return false;
}

formats::PlayerUnit export_unit(const Unit& u, const GameData& gd) {
    formats::PlayerUnit p;
    p.raw.assign(0x432, 0);
    put(p.raw, 0x3b, u.cls + 1, 2);
    put_str(p.raw, 0x3d, u.name, 0x42);
    put_str(p.raw, 0xc1, u.title, 0x41);
    p.raw[0x130] = u.level_arrow;
    for (const auto& f : kFields) put(p.raw, f.off, u.*f.m, f.size);
    p.block_16f.assign(p.raw.begin() + 0x16f, p.raw.begin() + 0x1aa);
    if (!u.upgrades.empty()) {
        p.upgrade_ptr = 1;  // any non-zero value: the reader allocates a fresh list
        put(p.raw, 0x1aa, 1, 4);
        for (const auto& e : u.upgrades)
            p.upgrades.push_back({std::int16_t(e.upitem + 1), std::int16_t(e.unlocked), std::int16_t(e.applied),
                                  std::int16_t(e.combo_level)});
    }
    put(p.raw, 0x1c1, 1, 4);
    auto& inv = p.inv.emplace();
    inv.raw.assign(0x1e, 0);
    put(inv.raw, 0, u.inv.weapon_mode, 2);
    const ItemInstance* slots[4] = {&u.inv.melee[0], &u.inv.melee[1], &u.inv.ranged, &u.inv.ammo};
    for (size_t s = 0; s < 4; ++s) {
        if (!slots[s]->item) continue;
        put(inv.raw, 2 + 4 * s, 1, 4);
        inv.slots[s].assign(0x12, 0);
        put(inv.slots[s], 0, slots[s]->item, 2), put(inv.slots[s], 2, slots[s]->quantity, 2);
        put(inv.slots[s], 4, slots[s]->level, 2);
    }
    for (int g = 0; g < 3; ++g) {
        put(inv.raw, size_t(0x12 + 4 * g), 1, 4);
        inv.bags[size_t(g)].grid.assign(size_t(kBagW[g] * kBagH[g]), 0);
    }
    for (const auto& it : u.inv.bag) {
        if (it.item <= 0 || it.item >= int(gd.items.size())) continue;
        const ItemDef& d = gd.items[size_t(it.item)];
        // ponytail: items that fit nowhere are dropped, as the original's add-item fails for them.
        if (d.item_class == "ENHANCER") place(inv.bags[0], 0, it, d.width, d.height);
        else if (!place(inv.bags[1], 1, it, d.width, d.height)) place(inv.bags[2], 2, it, d.width, d.height);
    }
    return p;
}

Unit import_unit(const formats::PlayerUnit& p) {
    Unit u;
    u.cls = get(p.raw, 0x3b, 2) - 1;
    u.name = get_str(p.raw, 0x3d, 0x42);
    u.title = get_str(p.raw, 0xc1, 0x41);
    u.level_arrow = p.raw[0x130] != 0;
    for (const auto& f : kFields) u.*f.m = get(p.raw, f.off, f.size);
    for (const auto& e : p.upgrades) u.upgrades.push_back({e[0] - 1, e[1], e[2], e[3]});
    if (!p.inv) return u;
    const auto& inv = *p.inv;
    u.inv.weapon_mode = get(inv.raw, 0, 2);
    ItemInstance* slots[4] = {&u.inv.melee[0], &u.inv.melee[1], &u.inv.ranged, &u.inv.ammo};
    for (size_t s = 0; s < 4; ++s)
        if (inv.slots[s].size() == 0x12) *slots[s] = {get(inv.slots[s], 0, 2), get(inv.slots[s], 2, 2), get(inv.slots[s], 4, 2)};
    for (int g = 0; g < int(inv.bags.size()); ++g)   // RecruitWorkshop: + the item's bag cell (+0x15 col, +0x16 row)
        for (const auto& r : inv.bags[size_t(g)].items)
            if (r.size() == 0x18) u.inv.bag.push_back({get(r, 0, 2), get(r, 2, 2), get(r, 4, 2), g, r[0x15], r[0x16]});
    return u;
}
}  // namespace

formats::PlayerProfile to_player_dat(const CampaignProfile& p, const GameData& gd, std::string_view name) {
    formats::PlayerProfile f;
    f.header.assign(0x48, 0);
    put_str(f.header, 0, std::string(name), 0x20);
    put(f.header, 0x20, std::int32_t(p.heroes.size()), 4);
    put(f.header, 0x28, std::int32_t(p.item_hints_a.size()), 4);
    put(f.header, 0x30, std::int32_t(p.item_hints_b.size()), 4);
    put(f.header, 0x38, p.gold, 4);
    put(f.header, 0x3c, std::int32_t(p.play_ms), 4);
    put(f.header, 0x40, p.command_points, 4);
    f.block2.assign(f.header.begin(), f.header.begin() + 0x20);
    for (const auto& h : p.heroes) {
        formats::PlayerEntry e;
        e.head.assign(0x20, 0);
        put_str(e.head, 0, h.code, 0x21);  // FUN_004c80c0 strncpy 0x21; only 0x20 bytes are written
        e.head2 = e.head;
        e.unit = export_unit(h.unit, gd);
        f.entries.push_back(std::move(e));
    }
    f.list_a = p.item_hints_a, f.list_b = p.item_hints_b;
    f.trailing = std::uint16_t(p.command_points);
    return f;
}

CampaignProfile from_player_dat(const formats::PlayerProfile& f, const GameData& gd) {
    CampaignProfile p;
    for (const auto& e : f.entries) {
        HeroRecord h{e.code(), import_unit(e.unit)};
        if (h.unit.cls < 0 || h.unit.cls >= int(gd.units.size())) h.unit.cls = gd.unit_index(h.code);
        p.heroes.push_back(std::move(h));
    }
    p.item_hints_a = f.list_a, p.item_hints_b = f.list_b;
    p.gold = f.gold();
    p.play_ms = f.play_ms();
    p.command_points = std::int16_t(f.trailing);  // FUN_004c8bb0 overwrites +0x40 with the trailing short
    return p;
}

}  // namespace joa::game
