#include "game/savegame_import.hpp"

#include "formats/vfs_bytes.hpp"
#include "game/mission.hpp"

namespace joa::game {

namespace {
// Record-mode File object: every write is "u16 length + data", put_short is a raw u16.
struct Rec {
    formats::ByteReader r;
    std::span<const std::uint8_t> rec() { return r.bytes(r.u16()); }
    std::int16_t raw_short() { return r.i16(); }
};

template <class T>
T at(std::span<const std::uint8_t> b, std::size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}
std::string cstr(std::span<const std::uint8_t> b, std::size_t off, std::size_t n) {
    if (off >= b.size()) return {};
    return formats::ByteReader::cstr(b.subspan(off, std::min(n, b.size() - off)));
}
// Optional heap string written as raw u16 length + record (FUN_004841d0 / FUN_00483b90).
std::string opt_string(Rec& in, bool present) {
    if (!present) return {};
    in.raw_short();
    auto t = in.rec();
    return formats::ByteReader::cstr(t);
}

constexpr std::size_t kGame = 0x745, kScenario = 0x289, kStep = 0x1b2, kEndCon = 0x1a, kUnit = 0x432;
constexpr std::size_t kScen = 0x17;  // Scenario object inside Game (0xa58fa7 - 0xa58f90)

Unit decode_unit(std::span<const std::uint8_t> u) {
    Unit x;
    x.nation = at<std::int16_t>(u, 0x2);
    x.pos = {at<float>(u, 0xe), at<float>(u, 0x12), at<float>(u, 0x16)};
    x.yaw_goal = at<float>(u, 0x1e);
    x.yaw_deg = at<float>(u, 0x1a) / 0.017453292f;
    x.team = at<std::int16_t>(u, 0x31);
    x.cls = at<std::int16_t>(u, 0x3b) - 1;
    x.name = cstr(u, 0x3d, 0x42);
    x.short_name = cstr(u, 0x7f, 0x42);
    x.title = cstr(u, 0xc1, 0x41);
    x.rank = at<std::int32_t>(u, 0x104);
    x.kingdom = at<std::int16_t>(u, 0x108), x.race = at<std::int16_t>(u, 0x10a);
    x.retreat = at<std::int32_t>(u, 0x114);
    x.max_lives = at<std::int32_t>(u, 0x11c), x.lives = at<std::int32_t>(u, 0x120);
    x.level = at<std::int16_t>(u, 0x124), x.xp = at<std::int32_t>(u, 0x126);
    x.skill_points = at<std::int16_t>(u, 0x12a), x.attr_points = at<std::int16_t>(u, 0x12c);
    x.hp = at<std::int32_t>(u, 0x135), x.base_max_hp = at<std::int32_t>(u, 0x13d);
    x.ep = at<std::int32_t>(u, 0x145), x.base_max_ep = at<std::int32_t>(u, 0x149);
    x.str = at<std::int16_t>(u, 0x155), x.dex = at<std::int16_t>(u, 0x157);
    x.def = at<std::int16_t>(u, 0x159), x.lead = at<std::int16_t>(u, 0x15b);
    x.armor_level = at<std::int16_t>(u, 0x15d), x.armor_item = at<std::int16_t>(u, 0x15f);
    x.loyalty = at<std::int16_t>(u, 0x161), x.morale = at<std::int16_t>(u, 0x163), x.reputation = at<std::int16_t>(u, 0x165);
    x.mode = at<std::int32_t>(u, 0x167), x.attack_behaviour = at<std::int32_t>(u, 0x16b);
    x.medals = at<std::int16_t>(u, 0x1ae), x.kills = at<std::int16_t>(u, 0x1b0), x.officer_kills = at<std::int16_t>(u, 0x1b2);
    return x;
}
}  // namespace

std::optional<OriginalSave> import_original_save(std::span<const std::uint8_t> bytes, const GameData& gd, std::string* err) {
    auto fail = [&](std::string m) -> std::optional<OriginalSave> {
        if (err) *err = std::move(m);
        return std::nullopt;
    };
    auto sav = formats::parse_sav(bytes, err);
    if (!sav) return std::nullopt;
    if (sav->sections.size() != formats::SaveGame::kSections) return fail("unexpected section count");
    OriginalSave o;
    o.header = sav->header;

    // Section 4: Game object, Scenario, steps, end conditions.
    const auto& s4 = sav->sections[3];
    Rec in{formats::ByteReader(bytes.subspan(s4.offset, s4.size))};
    const auto g = in.rec();
    if (g.size() != kGame) return fail("section 4 is not a Game object");
    o.scenario_no = at<std::int32_t>(g, kScen + 0x0);
    o.step = at<std::int32_t>(g, kScen + 0x4);
    o.count_time[0] = at<std::uint32_t>(g, kScen + 0x1ca);
    o.count_time[1] = at<std::uint32_t>(g, kScen + 0x1ce);
    o.count_time[2] = at<std::uint32_t>(g, kScen + 0x1d2);
    o.passed_ms = at<std::uint32_t>(g, kScen + 0x1da);
    o.finish = g[0x308];
    const int ends = at<std::int16_t>(g, 0x309);
    o.gold = at<std::int32_t>(g, 0x734);
    o.win_delay_ms = at<std::int32_t>(g, 0x738);
    o.win_start_ms = at<std::uint32_t>(g, 0x73c);
    o.play_ms = at<std::uint32_t>(g, 0x740);

    const auto sc = in.rec();
    if (sc.size() != kScenario) return fail("section 4: bad Scenario record");
    const int steps = at<std::int32_t>(sc, 0xc);
    if (steps < 0 || steps > 16) return fail("section 4: bad step count");
    for (int k = 0; k <= steps; ++k) {  // step records, then the inline step
        const auto st = in.rec();
        if (st.size() != kStep) return fail("section 4: bad step record");
        const std::string name = opt_string(in, at<std::uint32_t>(st, 0x19e) != 0);
        opt_string(in, at<std::uint32_t>(st, 0x1a2) != 0);  // original bug: writes the name again
        if (k + 1 == o.step) {
            for (int j = 0; j < 4; ++j) o.bits[j] = at<std::uint32_t>(st, 0x18a + 4 * j);
            o.stage = at<std::int32_t>(st, 0x19a);
            o.time_limit = at<std::int32_t>(st, 0x1a6);
            o.time_remaining = at<std::int32_t>(st, 0x1aa);
            o.mode = at<std::int32_t>(st, 0x1ae);
            o.scenario_name = name;
        }
    }
    if (at<std::int16_t>(sc, 0x287) != 0) {
        o.notes.push_back("scenario generals present: unit records not decoded, end conditions/lists skipped");
    } else {
        for (int k = 0; k < ends && in.r.ok(); ++k) {
            const auto e = in.rec();
            if (e.size() != kEndCon) return fail("section 4: bad end-condition record");
            EndCondition c;
            c.type = static_cast<std::int8_t>(e[0]);
            c.has_des = at<std::uint32_t>(e, 1) != 0;
            c.des = opt_string(in, c.has_des);
            c.territory_link = opt_string(in, at<std::uint32_t>(e, 5) != 0);
            c.finish = at<std::int16_t>(e, 9), c.para1 = at<std::int16_t>(e, 0xb), c.para2 = at<std::int16_t>(e, 0xd);
            c.enabled = at<std::int16_t>(e, 0xf), c.gold = at<std::int32_t>(e, 0x11), c.exp = at<std::int32_t>(e, 0x15);
            c.awarded = e[0x19];
            o.end.push_back(std::move(c));
        }
        if (at<std::int32_t>(g, 0x720) || at<std::int32_t>(g, 0x72c))
            o.notes.push_back("item-hint lists not decoded (element layout unconfirmed)");
    }
    if (!in.r.ok()) return fail("section 4 truncated");

    // Section 13: unit records (u16 0x432 + raw Unit); keep heroes (rank > 3) of known classes.
    const auto& s13 = sav->sections[12];
    const auto units = bytes.subspan(s13.offset, s13.size);
    for (std::size_t p = 0; p + 2 + kUnit <= units.size(); ++p) {
        if (at<std::uint16_t>(units, p) != kUnit) continue;
        const auto u = units.subspan(p + 2, kUnit);
        Unit x = decode_unit(u);
        if (x.rank < 4 || x.rank > 5 || x.cls < 0 || x.cls >= int(gd.units.size()) || x.level < 1 || x.level > 30) continue;
        if (!gd.units[x.cls].hero) continue;
        bool dup = false;
        for (const auto& h : o.heroes) dup |= h.code == gd.units[x.cls].code;
        if (!dup) o.heroes.push_back({gd.units[x.cls].code, std::move(x)});
        p += kUnit;
    }
    o.notes.push_back("world (units, items, places, AI) is not decoded: the mission restarts from its map with the "
                      "decoded script flags, objectives and hero progression");
    return o;
}

void apply_original_save(Mission& m, const OriginalSave& o) {
    ScenarioState& ss = m.scenario();
    for (int j = 0; j < 4; ++j) ss.bits[j] = o.bits[j];
    ss.stage = o.stage;
    ss.mode = o.mode;
    ss.time_limit = o.time_limit;
    ss.time_remaining = o.time_remaining;
    for (int j = 0; j < 3; ++j) ss.count_time[j] = o.count_time[j];
    MissionState& ms = m.state();
    if (!o.end.empty()) ms.end = o.end;
    ms.gold = o.gold;
    ms.play_ms = o.play_ms;
    ms.passed_ms = o.passed_ms;
    ms.win_delay_ms = o.win_delay_ms;
    if (!o.scenario_name.empty()) ms.scenario_name = o.scenario_name;
    World& w = m.world();
    for (const auto& h : o.heroes)
        for (Unit& u : w.units()) {
            if (u.deleted || u.cls != h.unit.cls) continue;
            const Unit& s = h.unit;
            u.level = s.level, u.xp = s.xp, u.skill_points = s.skill_points, u.attr_points = s.attr_points;
            u.base_max_hp = s.base_max_hp, u.base_max_ep = s.base_max_ep, u.max_lives = s.max_lives, u.lives = s.lives;
            u.str = s.str, u.dex = s.dex, u.def = s.def, u.lead = s.lead;
            u.loyalty = s.loyalty, u.morale = s.morale, u.reputation = s.reputation;
            u.medals = s.medals, u.kills = s.kills, u.officer_kills = s.officer_kills;
            w.set_hp(u, s.hp);
            w.set_ep(u, s.ep);
            break;
        }
}

}  // namespace joa::game
