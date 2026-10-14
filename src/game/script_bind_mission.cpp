// Mission slice of the builtin table: objectives / end conditions, objective counters, game
// clock and date, weather/fog, save_game, scenario name/step, global gameplay flags.
// Per-id spec: docs/reverse-engineering/SCRIPT_VM.md §8 (executor VAs in the comments).
// Confidence CONFIRMED unless a comment says HYPOTHESIS.
#include <algorithm>
#include <cmath>

#include "game/mission.hpp"
#include "game/script_bind.hpp"

namespace joa::game {
namespace {

using K = MissionEvent::Kind;

Value one() { return Value::integer(1); }

// Localised text, capped like the original 0x191-byte buffers (FUN_006380b0 + strncpy 400).
std::string loc(const BuiltinCall& c, size_t n) {
    std::string s = c.localized(n);
    if (s.size() > 400) s.resize(400);
    return s;
}

// --- calendar (FUN_0057f990 / FUN_0057fef0 / FUN_0057fff0 / FUN_0057fa80 / FUN_00580060) ----------
constexpr int kCum[14] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};  // DAT_00a15198
constexpr int kJdnBase = 0x1a4451;  // 1721425: JDN of day 0 before 0001-01-01

bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// Day of year, -1 when the date is invalid (FUN_0057fef0).
int day_of_year(int y, int m, int d) {
    if (y < 0 || m < 1 || m > 12 || d < 1) return -1;
    const int L = leap(y);
    if (d > kCum[m + 1] - kCum[m] + (m == 2 ? L : 0)) return -1;
    return kCum[m] + d + (m < 3 ? 0 : L);
}

int days_before_year(int y) {  // FUN_0057fff0
    --y;
    return y / 400 + y * 365 + y / 4 - y / 100;
}

int calculate_date(int y, int m, int d) {  // FUN_0057f990
    const int doy = day_of_year(y, m, d);
    return doy < 1 ? -1 : days_before_year(y) + doy + kJdnBase;
}

// FUN_0057fa80 with 'Y' / 'M': year or month of a JDN, -1 outside (0x1a4450, 0x51fe2d).
// The original estimates the year with ftol and corrects it once each way; the loop below yields
// the same (exact) result.
int jdn_part(int jdn, char part) {
    if (!(jdn > 0x1a4450 && jdn < 0x51fe2d)) return -1;
    const int n = jdn - kJdnBase;  // 1 = 0001-01-01
    int y = int(static_cast<long long>(n) * 400 / 146097) + 1;
    while (n - days_before_year(y) < 1) --y;
    while (n - days_before_year(y) > 365 + leap(y)) ++y;
    if (part == 'Y') return y;
    const int doy = n - days_before_year(y);
    const int adj = doy < 60 ? 0 : leap(y);  // FUN_00580060
    for (int k = 2; k <= 13; ++k)
        if (doy <= kCum[k] + adj) return k - 1;
    return -1;
}

// GameClock_new_day 0x4953d0 (day capped at 30, quirk). ponytail: the nation daily process
// (0x4b68d0), weather roll (0x45b160) and 0x4e42e0 are not reproduced; they draw from the shared
// RNG, so a mission that crosses midnight diverges from the original's stream there.
void next_day(MissionState& s) {
    if (++s.day > 30) s.day = 30;
    ++s.date_jdn;
    if (const int mo = jdn_part(s.date_jdn, 'M'); mo != s.month) s.day = 1, s.month = mo;
    if (const int yr = jdn_part(s.date_jdn, 'Y'); yr != s.year) s.month = 1, s.year = yr;
}

// --- end conditions -------------------------------------------------------------------------
EndCondition* ec(Mission& m, const BuiltinCall& c) { return m.state().end_condition(c.i(0)); }

// 0x65a3c0 / 0x65a400: localised description into E+1.
void set_des(Mission& m, const BuiltinCall& c) {
    if (EndCondition* e = ec(m, c)) e->des = loc(c, 1), e->has_des = true;
}

// --- game over --------------------------------------------------------------------------------
// call_gameover_msg 0x65ebd0 (H).
Value call_gameover_msg(Mission& m, const BuiltinCall& c) {
    World& w = m.world();
    MissionState& s = m.state();
    m.ui().clear_attention_messages();  // FUN_004ae6c0 -> FUN_004734a0
    if (!s.game_over) {                 // GameOver_lose_freeze_units 0x40a141
        s.game_over = 1;
        if (Unit* p = w.unit(w.player_id()); p && !w.dead(*p)) w.stop(*p);
        for (int r = int(w.units().size()); r > 0; --r)
            if (Unit* u = w.unit(r); u && r != w.player_id()) m.ai().set_idle(*u), w.stop(*u);
        // ponytail: Ctrl_range_exit (leave the aim mode by control mode) is not modelled.
    }
    s.game_over_text = loc(c, 0).substr(0, 255);  // GameOverBox_set_text (strncpy 255)
    if (Unit* p = w.unit(w.player_id()); p && !w.dead(*p)) w.stop(*p);
    m.emit(K::GameOverMessage, 0, 0, s.game_over_text);
    return one();
}

// set_victory_display_flag 0x65f6a0 (H): argument unused.
Value set_victory_display_flag(Mission& m, const BuiltinCall&) {
    MissionState& s = m.state();
    s.clock_running = 0;  // DAT_00a500d2 = 0, speed saved, FUN_0042f930(0): game speed 0
    if (!s.game_over) {   // VictoryScreen_open + clear count times 1..3
        m.emit(K::Ui, 0, 0, "VICTORY_SCREEN");
        for (auto& t : m.scenario().count_time) t = 0;
    }
    return one();
}

}  // namespace

void advance_clock(MissionState& s, std::uint32_t dt_ms) {
    if (!s.clock_running) return;  // `_DAT_00a500cc != 0.0` (game speed)
    s.minute = float(double(float(dt_ms)) * double(0.00040000002f) + double(s.minute));
    if (1440.0f <= s.minute) {
        s.minute = s.min_start;
        next_day(s);
    }
}

void bind_mission(BuiltinTable& t) {
    auto& f = t.call;
    // 2 set_snow_level 0x651100: weather +0x20 = (short)level.
    f[2] = [](Mission& m, const BuiltinCall& c) { return m.state().snow_level = std::int16_t(c.i(0)), one(); };
    // 3 start_weather_effect 0x651130 (H): records the minute effect 1/2 started. HYPOTHESIS: the
    // weather-object guard byte [effect+1] is not modelled (always 0).
    f[3] = [](Mission& m, const BuiltinCall& c) {
        if (c.i(0) == 1 || c.i(0) == 2) m.state().weather_start[c.i(0) - 1] = m.state().minute;
        return one();
    };
    f[4] = &call_gameover_msg;
    // 8 get_read_letter_once 0x653110: (char)DAT_00a46059.
    f[8] = [](Mission& m, const BuiltinCall&) { return Value::integer(std::int8_t(m.state().read_letter_once)); };
    // 161 string_to_order_para 0x6511f0 / Order_pack_string_paras 0x4bbac0: 12 chars into 3
    // big-endian dwords; chars are sign-extended before the shift (quirk for bytes >= 0x80).
    f[161] = [](Mission&, const BuiltinCall& c) {
        const int part = c.i(1);
        if (part < 1 || part > 3) return Value::integer(0);
        const std::string_view s = c.s(0).substr(0, c.s(0).find('\0'));  // C string
        std::uint32_t v = 0;
        for (size_t k = size_t(part - 1) * 4; k < s.size() && k < size_t(part) * 4; ++k)
            v |= std::uint32_t(std::int32_t(static_cast<signed char>(s[k]))) << ((3 - k % 4) * 8);
        return Value::integer(std::int32_t(v));
    };
    // 205 set_game_start_date 0x659550: FUN_00495290(y,m,d), then min_start and the clamped
    // start minute (FUN_00590b00). FUN_004a5630 (lighting reselect) is the renderer's job.
    f[205] = [](Mission& m, const BuiltinCall& c) {
        MissionState& s = m.state();
        s.year = c.i(0), s.month = c.i(1), s.day = c.i(2);
        s.date_jdn = calculate_date(s.year, s.month, s.day);
        const double x = c.i(3), lo = c.i(4), hi = c.i(5);
        s.min_start = float(c.i(4));
        s.minute = float(lo <= x ? (x <= hi ? x : hi) : lo);
        return one();
    };
    // 209 calculate_date 0x6596f0.
    f[209] = [](Mission&, const BuiltinCall& c) { return Value::integer(calculate_date(c.i(0), c.i(1), c.i(2))); };
    f[210] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().date_jdn); };       // 0x659720
    f[211] = [](Mission& m, const BuiltinCall&) { return Value::real(m.state().minute / 60.0f); };    // 0x659740
    // 212 get_game_minute 0x659790: ftol(fmod(minute, 60)) stored as raw int bits in a FLOAT.
    f[212] = [](Mission& m, const BuiltinCall&) {
        const auto v = static_cast<std::int32_t>(std::fmod(double(m.state().minute), 60.0));
        return Value{ScriptType::Float, static_cast<std::uint32_t>(v), {}};
    };
    f[213] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().day); };    // 0x6597d0
    f[214] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().month); };  // 0x6597f0
    f[215] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().year); };   // 0x659810
    // 216 change_scenario_step 0x659830: DAT_00a58faf; the stage switch runs in the scenario tick.
    f[216] = [](Mission& m, const BuiltinCall& c) { return m.state().requested_step = c.i(0), one(); };
    f[218] = [](Mission& m, const BuiltinCall& c) { return m.state().fog_distance = float(c.i(0)), one(); };  // 0x659890

    // --- objective counters (0x659a00..0x659f20) ---
    f[227] = [](Mission& m, const BuiltinCall& c) { return m.state().rescue_count = c.i(0), one(); };
    f[228] = [](Mission& m, const BuiltinCall& c) { return m.state().rescue_limit = c.i(0), one(); };
    f[229] = [](Mission& m, const BuiltinCall& c) { return m.state().reputation += c.i(0), one(); };
    f[230] = [](Mission& m, const BuiltinCall& c) { return m.state().reputation = c.i(0), one(); };
    f[231] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().reputation); };
    f[232] = [](Mission& m, const BuiltinCall&) { return m.state().citizen_killed = 0, one(); };  // reset, no arg
    f[233] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().citizen_killed); };
    f[234] = [](Mission& m, const BuiltinCall&) { return m.state().fcitizen_killed = 0, one(); };
    f[235] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().fcitizen_killed); };
    f[236] = [](Mission& m, const BuiltinCall&) { return m.state().enemy_bribed = 0, one(); };  // cost total kept
    f[237] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().enemy_bribed); };
    f[238] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().enemy_bribed_cost); };
    f[239] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().gold_crates); };
    f[241] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().food_crates); };
    f[242] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().food_picked); };
    f[243] = [](Mission& m, const BuiltinCall& c) { return m.state().food_can_be_picked = std::uint8_t(c.i(0)), one(); };
    f[244] = [](Mission& m, const BuiltinCall& c) { return m.state().total_food = c.i(0), one(); };
    f[245] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().total_food); };
    f[246] = [](Mission& m, const BuiltinCall&) { return m.state().citizen_saved = 0, one(); };
    f[247] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().citizen_saved); };
    f[248] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().citizen_saved++); };
    // 249 add_scenario_general FUN_004cdd40: (short)id > 0 and count < 31.
    f[249] = [](Mission& m, const BuiltinCall& c) {
        auto& g = m.state().scenario_generals;
        if (const auto id = std::int16_t(c.i(0)); id > 0 && g.size() < 31) g.push_back(id);
        return one();
    };
    // 250 remove_scenario_general FUN_004cdc40: removes the first match; when absent the LAST
    // entry is dropped (quirk). Deviation: an empty list stays empty (original count goes to -1).
    f[250] = [](Mission& m, const BuiltinCall& c) {
        auto& g = m.state().scenario_generals;
        if (const auto id = std::int16_t(c.i(0)); id > 0) {
            if (auto it = std::find(g.begin(), g.end(), id); it != g.end()) g.erase(it);
            else if (!g.empty()) g.pop_back();
        }
        return one();
    };
    f[251] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().bribed_enemy_killed); };
    f[252] = [](Mission& m, const BuiltinCall&) { return m.state().house_burnt = 0, one(); };
    f[253] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.state().house_burnt); };

    // --- end conditions (0x65a350..0x65a840). Out-of-range indexes are ignored (the original
    // writes unchecked; the record is memset by the next add anyway) and read as 0. ---
    f[263] = [](Mission& m, const BuiltinCall&) {
        const int idx = m.state().add_end_condition();
        if (idx) m.emit(K::ObjectiveAdd, idx);
        return Value::integer(idx);
    };
    f[264] = [](Mission& m, const BuiltinCall&) {
        const int n = int(m.state().end.size());
        if (n) m.emit(K::ObjectiveHide, n);
        return Value::integer(m.state().del_end_condition());
    };
    f[265] = [](Mission& m, const BuiltinCall& c) {
        if (EndCondition* e = ec(m, c)) e->type = std::int8_t(c.i(1));
        return one();
    };
    f[266] = [](Mission& m, const BuiltinCall& c) { return set_des(m, c), one(); };
    f[267] = [](Mission& m, const BuiltinCall& c) {
        m.emit(K::Sound, 0, 0, "OBJECTIVE_UPDATE");
        set_des(m, c);
        m.emit(K::ObjectiveUpdate, c.i(0));
        return one();
    };
    f[268] = [](Mission& m, const BuiltinCall& c) {
        if (EndCondition* e = ec(m, c)) e->territory_link = std::string(c.s(1));
        return one();
    };
    // 269 set_end_condition_finish 0x65a4a0: WIN/LOSE/NORMAL (stricmp), only when it changes.
    f[269] = [](Mission& m, const BuiltinCall& c) {
        EndCondition* e = ec(m, c);
        const std::string_view s = c.s(1);
        const int v = formats::iequals(s, "WIN") ? 1 : formats::iequals(s, "LOSE") ? 2 : formats::iequals(s, "NORMAL") ? 0 : -1;
        if (!e || v < 0 || e->finish == v) return one();
        e->finish = std::int16_t(v);
        if (v == 1) m.emit(K::Sound, 0, 0, "OBJECTIVE_FINISH"), m.emit(K::ObjectiveFinish, c.i(0));
        else m.emit(v == 2 ? K::ObjectiveFail : K::ObjectiveShow, c.i(0));
        return one();
    };
    f[270] = [](Mission& m, const BuiltinCall& c) { const EndCondition* e = ec(m, c); return Value::integer(e ? e->finish : 0); };
    f[271] = [](Mission& m, const BuiltinCall& c) {
        if (EndCondition* e = ec(m, c)) e->para1 = std::int16_t(c.i(1));
        return one();
    };
    f[272] = [](Mission& m, const BuiltinCall& c) {
        if (EndCondition* e = ec(m, c)) e->para2 = std::int16_t(c.i(1));
        return one();
    };
    // 273 set_end_condition_enabled 0x65a720: short field compared with the int argument.
    f[273] = [](Mission& m, const BuiltinCall& c) {
        EndCondition* e = ec(m, c);
        const int v = c.i(1);
        if (!e) return one();
        if (e->enabled == 0 && v != 0) m.emit(K::Sound, 0, 0, "OBJECTIVE_ADD");
        if (e->enabled != v) {
            e->enabled = std::int16_t(v);
            m.emit(v == 0 ? K::ObjectiveHide : K::ObjectiveShow, c.i(0));
        }
        return one();
    };
    f[274] = [](Mission& m, const BuiltinCall& c) { const EndCondition* e = ec(m, c); return Value::integer(e ? e->enabled : 0); };
    f[275] = [](Mission& m, const BuiltinCall& c) {
        if (EndCondition* e = ec(m, c)) e->gold = c.i(1);
        return Value::integer(c.i(1));
    };
    f[276] = [](Mission& m, const BuiltinCall& c) {
        if (EndCondition* e = ec(m, c)) e->exp = c.i(1);
        return Value::integer(c.i(1));
    };
    f[289] = [](Mission& m, const BuiltinCall& c) { return m.state().scenario_name = loc(c, 0), one(); };         // 0x65ab10
    f[290] = [](Mission& m, const BuiltinCall& c) { return m.state().scenario_description = loc(c, 0), one(); };  // 0x65ab50
    // 315 save_game 0x65bed0: Mission writes the save after the tick.
    f[315] = [](Mission& m, const BuiltinCall& c) {
        m.state().save_request_name = std::string(c.s(0));
        m.state().save_request_desc = std::string(c.s(1));
        return one();
    };
    // 331 set_stop_unit_process_flag 0x65c500: DAT_00a596ab (char) gates UnitArray_process.
    f[331] = [](Mission& m, const BuiltinCall& c) { return m.world().stop_unit_process = (c.i(0) & 0xff) != 0, one(); };
    // 365 / 368 set_kill_citizen_limit(_2) 0x65e7d0 / 0x65e880: stored only when > 0.
    f[365] = [](Mission& m, const BuiltinCall& c) {
        if (c.i(0) > 0) m.state().kill_citizen_limit = c.i(0);
        return Value::integer(c.i(0) > 0);
    };
    f[368] = [](Mission& m, const BuiltinCall& c) {
        if (c.i(0) > 0) m.state().kill_citizen_limit_2 = c.i(0);
        return Value::integer(c.i(0) > 0);
    };
    f[380] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.world().game_mode); };       // 0x65ed50
    f[381] = [](Mission& m, const BuiltinCall&) { return Value::integer(m.world().game_mode == 4); };  // 0x65ed70
    // 384 set_critical_attack_enabled 0x65ee30: DAT_00a4f858 (char) gates Player_critCheck.
    f[384] = [](Mission& m, const BuiltinCall& c) { return m.world().critical_attack_enabled = (c.i(0) & 0xff) != 0, one(); };
    // 391 set_charmove_detect_block_flag 0x65f670: DAT_00a5ba64 (char).
    f[391] = [](Mission& m, const BuiltinCall& c) { return m.state().charmove_detect_block = std::uint8_t(c.i(0)), one(); };
    f[392] = &set_victory_display_flag;
    f[402] = [](Mission& m, const BuiltinCall& c) { return m.state().win_delay_ms = c.i(0), one(); };  // 0x65fba0
    f[403] = [](Mission& m, const BuiltinCall&) { return Value::integer(std::int8_t(m.state().objective_menu_opened)); };
    // 406 show_ending 0x65fcc0 (H): (char)ending 0 or 1 starts the ending sequence (FUN_006d19e0).
    f[406] = [](Mission& m, const BuiltinCall& c) {
        if (const auto e = static_cast<signed char>(c.i(0)); e >= 0 && e < 2) {
            m.state().ending = 1 + std::uint64_t(e);
            m.emit(K::Ending, e);
        }
        return one();
    };
}

// Objective counters the engine increments on death: Unit_die 0x7c6240 (killer) and
// Unit_dieNoKiller 0x64f320 (killer 0), both reported by World::unit_die as Ev::Death
// (unit = victim, other = killer).
void process_world_events(Mission& m, const std::vector<GameEvent>& events) {
    World& w = m.world();
    MissionState& s = m.state();
    const GameData& gd = m.data();
    const bool training = m.scenario().mode == 2;
    for (const GameEvent& e : events) {
        // World::unit_die emits the retreat survival (+0x114 == 1) as this custom event; the
        // bribed-kill check of Unit_die runs before that branch.
        const bool death = e.type == Ev::Death;
        if (!death && !(e.type == Ev::Custom && e.name == "MISSION_EVENT_DEFEAT")) continue;
        if (e.unit < 1 || e.unit > int(w.units().size())) continue;
        const Unit& v = w.units()[std::size_t(e.unit - 1)];
        const Unit* k = w.unit(e.other);
        // Training mode skips units flagged +0x10c == 1 unless the killer is not flagged.
        if (training && v.m.training == 1 && (!k || k->m.training != 0)) continue;
        // Unit_die: killer bribed (+0x2eb == 2) and victim nation named "English".
        if (k && k->m.bribe == 2 && v.nation >= 1 && v.nation <= int(gd.nations.size()) &&
            formats::iequals(gd.nations[std::size_t(v.nation - 1)], "English"))
            ++s.bribed_enemy_killed;
        // Citizen class (+0x2 == 0) not hostile to the player (0x5094d0); female on Sex 'F'.
        if (death && v.cls >= 0 && gd.units[std::size_t(v.cls)].unit_class == 0 && !w.hostile_to_player(v)) {
            ++s.citizen_killed;
            if (gd.units[std::size_t(v.cls)].sex == 'F') ++s.fcitizen_killed;
        }
    }
}

}  // namespace joa::game
