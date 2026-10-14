#include "game/mission_state.hpp"

namespace joa::game {

const char* event_kind_name(MissionEvent::Kind k) {
    static constexpr const char* kNames[] = {
        "objective_add", "objective_update", "objective_finish", "objective_fail", "objective_show", "objective_hide",
        "victory", "defeat", "game_over_message", "attention_message", "dialog", "choice_dialog", "dialog_closed",
        "letter", "title_screen", "brief", "buy_interface", "scenario_part", "cut_scene", "camera", "sound", "music",
        "minimap", "ui", "ending", "save_game", "text", "transition", "interact_prompt", "interact"};
    return kNames[static_cast<int>(k)];
}

void MissionState::emit(MissionEvent::Kind k, std::uint64_t tick, std::int32_t a, std::int32_t b, std::string text) {
    events.push_back({k, tick, a, b, std::move(text)});
}

int MissionState::add_end_condition() {
    if (static_cast<int>(end.size()) >= kMaxEndConditions) return 0;
    end.reserve(kMaxEndConditions);  // fixed array in the original: record pointers stay valid
    end.emplace_back();              // memset + enabled = 1
    return static_cast<int>(end.size());
}

int MissionState::del_end_condition() {
    if (end.empty()) return 0;
    end.pop_back();  // removes the LAST record only
    return static_cast<int>(end.size());
}

EndCondition* MissionState::end_condition(int index1) {
    return index1 >= 1 && index1 <= static_cast<int>(end.size()) ? &end[index1 - 1] : nullptr;
}

bool MissionState::process_end_conditions(std::uint8_t& finish, std::uint32_t now, std::uint64_t tick, int scenario_no,
                                          const std::function<void(std::int32_t)>& award_exp) {
    if (finish == 0 && !end.empty()) {
        bool allwin = true;
        for (auto& r : end)
            if (r.has_des && (r.type == EndCondition::Win || r.type == EndCondition::Side) && r.finish == 1 && !r.awarded) {
                r.awarded = 1;
                gold += r.gold;
                if (r.exp && award_exp) award_exp(r.exp);
            }
        for (const auto& r : end)
            if (r.enabled && r.has_des && r.type == EndCondition::Win && r.finish != 1) {
                allwin = false;
                break;
            }
        for (const auto& r : end)
            if (r.has_des && r.enabled && r.type == EndCondition::Lose && r.finish == 2) {
                finish = 2;
                allwin = false;
            }
        if (finish == 2) emit(MissionEvent::Kind::Defeat, tick);
        if (allwin) {
            if (win_start_ms == 0) win_start_ms = now;
        } else {
            win_start_ms = 0;
        }
    }
    // Unsigned >= compare, blocked while a cut scene runs.
    if (finish == 0 && win_start_ms != 0 && now - win_start_ms >= static_cast<std::uint32_t>(win_delay_ms) && !cut_scene) {
        finish = 1;
        for (auto& r : end)
            if (r.enabled && r.type == EndCondition::Imperative) r.finish = 1;
        if (scenario_no != 5)
            for (auto& r : end)
                if (r.enabled && r.has_des && r.type == EndCondition::Side && r.finish == 0) r.finish = 2;
        emit(MissionEvent::Kind::Victory, tick, scenario_no);
        return true;
    }
    return false;
}

void MissionState::save(ByteWriter& w) const {
    w.u32(static_cast<std::uint32_t>(end.size()));
    for (const auto& r : end) {
        w.u8(static_cast<std::uint8_t>(r.type));
        w.u8(r.has_des);
        w.str(r.des);
        w.str(r.territory_link);
        w.i16(r.finish), w.i16(r.para1), w.i16(r.para2), w.i16(r.enabled);
        w.i32(r.gold), w.i32(r.exp), w.u8(r.awarded);
    }
    w.i32(win_delay_ms), w.u32(win_start_ms), w.u8(cut_scene), w.i32(gold), w.u32(play_ms), w.u32(passed_ms);
    w.i32(command_points);
    for (std::int32_t v : {rescue_count, rescue_limit, reputation, citizen_killed, fcitizen_killed, enemy_bribed,
                           enemy_bribed_cost, gold_crates, food_crates, food_picked, total_food, citizen_saved,
                           bribed_enemy_killed, house_burnt, kill_citizen_limit, kill_citizen_limit_2})
        w.i32(v);
    w.u8(food_can_be_picked), w.u8(objective_menu_opened), w.u8(read_letter_once);
    w.u32(static_cast<std::uint32_t>(scenario_generals.size()));
    for (auto g : scenario_generals) w.i16(g);
    for (const auto* l : {&item_hints_a, &item_hints_b}) {
        w.u32(static_cast<std::uint32_t>(l->size()));
        for (auto v : *l) w.u16(v);
    }
    w.str(scenario_name), w.str(scenario_description);
    w.i32(requested_step);
    w.u64(ending);
    w.i32(date_jdn), w.i32(day), w.i32(month), w.i32(year), w.f32(minute), w.f32(min_start);
    w.f32(fog_distance), w.i16(snow_level), w.f32(weather_start[0]), w.f32(weather_start[1]);
    w.u8(charmove_detect_block), w.u8(clock_running), w.u8(game_over), w.str(game_over_text);
    w.u8(bribe_no_gold_shown);
}

bool MissionState::load(formats::ByteReader& r) {
    auto str = [&] {
        const auto n = r.u32();
        auto b = r.bytes(n);
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    };
    const auto n = r.u32();
    if (n > kMaxEndConditions) return false;
    end.assign(n, {});
    for (auto& e : end) {
        e.type = static_cast<std::int8_t>(r.u8());
        e.has_des = r.u8();
        e.des = str();
        e.territory_link = str();
        e.finish = r.i16(), e.para1 = r.i16(), e.para2 = r.i16(), e.enabled = r.i16();
        e.gold = r.i32(), e.exp = r.i32(), e.awarded = r.u8();
    }
    win_delay_ms = r.i32(), win_start_ms = r.u32(), cut_scene = r.u8(), gold = r.i32(), play_ms = r.u32(), passed_ms = r.u32();
    command_points = r.i32();
    for (std::int32_t* v : {&rescue_count, &rescue_limit, &reputation, &citizen_killed, &fcitizen_killed, &enemy_bribed,
                            &enemy_bribed_cost, &gold_crates, &food_crates, &food_picked, &total_food, &citizen_saved,
                            &bribed_enemy_killed, &house_burnt, &kill_citizen_limit, &kill_citizen_limit_2})
        *v = r.i32();
    food_can_be_picked = r.u8(), objective_menu_opened = r.u8(), read_letter_once = r.u8();
    scenario_generals.resize(std::min<std::uint32_t>(r.u32(), 31));
    for (auto& g : scenario_generals) g = r.i16();
    for (auto* l : {&item_hints_a, &item_hints_b}) {
        const auto m = r.u32();
        if (m > 0x10000) return false;
        l->resize(m);
        for (auto& v : *l) v = r.u16();
    }
    scenario_name = str(), scenario_description = str();
    requested_step = r.i32();
    ending = r.get<std::uint64_t>();
    date_jdn = r.i32(), day = r.i32(), month = r.i32(), year = r.i32(), minute = r.f32(), min_start = r.f32();
    fog_distance = r.f32(), snow_level = r.i16(), weather_start[0] = r.f32(), weather_start[1] = r.f32();
    charmove_detect_block = r.u8(), clock_running = r.u8(), game_over = r.u8();
    game_over_text = str();
    bribe_no_gold_shown = r.u8();
    events.clear();
    return r.ok();
}

}  // namespace joa::game
