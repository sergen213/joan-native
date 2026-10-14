#include "game/campaign.hpp"

#include <cstdio>
#include <type_traits>

#include "formats/vfs_fs.hpp"

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
    d.upgrades = s.upgrades;  // FUN_00531230
    d.inv = s.inv;            // FUN_00497a10
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

}  // namespace joa::game
