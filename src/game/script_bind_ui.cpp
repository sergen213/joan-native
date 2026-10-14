// UI / camera / dialog / sound builtins (SCRIPT_VM.md §8 "ui" slice) on MissionUi. Executor VAs per
// handler; confidence follows the spec table (C = CONFIRMED, H = HIGH) unless noted.
#include <cstdlib>

#include "game/mission.hpp"
#include "game/script_bind.hpp"

namespace joa::game {

namespace {

using K = MissionEvent::Kind;

Value I(std::int32_t v) { return Value::integer(v); }
float fi(const BuiltinCall& c, size_t n) { return static_cast<float>(c.i(n)); }  // (float)int arg
bool b8(const BuiltinCall& c, size_t n) { return (c.i(n) & 0xff) != 0; }         // byte arg != 0
std::uint8_t u8(const BuiltinCall& c, size_t n) { return static_cast<std::uint8_t>(c.i(n)); }

bool live_unit(Mission& m, int id) {
    const Unit* u = m.world().unit(id);
    return u && !u->deleted;
}

// Calling script file stem (FUN_006509a0 up to '.'): voice lines are looked up by (stem, text number).
std::string stem(const BuiltinCall& c) {
    std::string_view s = c.script;
    if (const auto p = s.find_last_of("\\/"); p != std::string_view::npos) s.remove_prefix(p + 1);
    return std::string(s.substr(0, s.find('.')));
}

int tag_number(std::string_view raw) { return raw.size() > 1 ? std::atoi(std::string(raw.substr(1)).c_str()) : 0; }

// FUN_006380b0 + copy of min(len, 400) chars; `found` = the EX.res table had the tag.
std::string text_of(const BuiltinCall& c, size_t n, bool* found = nullptr) {
    std::string t = c.localized(n);
    if (found) *found = c.text && t != c.s(n);
    if (t.size() > 400) t.resize(400);
    return t;
}

// 293 set_music_side_environment_enable 0x65adb0 (C)
Value f293(Mission& m, const BuiltinCall& c) {
    m.ui().music_side_env = c.i(0) != 0;
    m.emit(K::Music, m.ui().music_side_env, 0, "side_environment");
    return I(1);
}

// 294 set_cut_scene_flag 0x65ade0 (H): 1 -> letterbox cut scene without camera, else end.
Value f294(Mission& m, const BuiltinCall& c) {
    MissionUi& ui = m.ui();
    return I(c.i(0) == 1 ? ui.cut_scene_start(m, true, false, 0, nullptr, 0, 0, 0, 0) : ui.cut_scene_end(m, false, 0));
}

// 295 get_cut_scene_flag 0x65b130 (C): Game+0x744.
Value f295(Mission& m, const BuiltinCall&) { return I(m.state().cut_scene); }

// 296 start_cut_scene 0x65aef0 / 0x65ae60 (H); 297 start_cut_scene_2: last arg = black-screen byte.
Value start_cut(Mission& m, const BuiltinCall& c, size_t short_argc, std::uint8_t black) {
    if (c.args.size() == short_argc)  // (no_camera, move, unit, dist, a1, a2) -> FUN_006cb9e0
        return I(m.ui().cut_scene_start(m, b8(c, 0), b8(c, 1), c.i(2), nullptr, fi(c, 3), fi(c, 4), fi(c, 5), black));
    const float pos[3] = {fi(c, 2), fi(c, 3), fi(c, 4)};  // (no_camera, move, x, y, z, ...) -> FUN_006cad50
    return I(m.ui().cut_scene_start(m, b8(c, 0), b8(c, 1), 0, pos, fi(c, 5), fi(c, 6), fi(c, 7), black));
}
Value f296(Mission& m, const BuiltinCall& c) { return start_cut(m, c, 6, 0); }
Value f297(Mission& m, const BuiltinCall& c) { return start_cut(m, c, 7, u8(c, c.args.size() == 7 ? 6 : 8)); }
// WAIT predicate FUN_0065af50 (296/297/302).
bool p_cut(Mission& m, const BuiltinCall&) { return m.ui().cut_scene_wait_done(); }

// 298 end_cut_scene 0x65aff0 (H); 299 end_cut_scene_2.
Value f298(Mission& m, const BuiltinCall& c) { return I(m.ui().cut_scene_end(m, b8(c, 0), 0)); }
Value f299(Mission& m, const BuiltinCall& c) { return I(m.ui().cut_scene_end(m, b8(c, 0), u8(c, 1))); }

// 300 get_cut_scene_status 0x65afd0 (C)
Value f300(Mission& m, const BuiltinCall&) { return I(m.ui().cs.status); }

// 301 start_cut_scene_camera 0x6537a0 (H): camera script of the unit's map piece (Level+0x60).
// HYPOTHESIS: Level+0x60 exists when the map has <stem>.cm; the return (FUN_00428850 short) is 1.
Value f301(Mission& m, const BuiltinCall& c) {
    const int unit = static_cast<std::int16_t>(c.i(0));
    if (!live_unit(m, unit) || !m.vfs().exists(m.map_dir() + "\\" + m.info().stem() + ".cm")) return I(0);
    MissionUi& ui = m.ui();
    ui.camera.flag5 = static_cast<std::int16_t>(c.i(4));  // FUN_00654a80
    ui.camera.speed = c.f(3);                              // FUN_00428a70
    ui.camera.flag6 = static_cast<std::int16_t>(c.i(5));  // FUN_00654ab0
    ui.play_camera_script(m, std::string(c.s(1)), m.now() + static_cast<std::uint32_t>(c.i(2)));
    return I(1);
}
// WAIT FUN_00653a90: a playing camera script finishes the wait only by ending, or immediately (and is
// stopped, camera back on the player) when no cut scene runs.
bool p301(Mission& m, const BuiltinCall&) {
    MissionUi& ui = m.ui();
    if (!ui.camera.playing) return true;
    if (m.state().cut_scene) return false;
    ui.camera.playing = false;  // FUN_004289e0
    ui.camera.flag6 = 0;
    m.emit(K::Camera, m.world().controller().unit, 0, "player");
    return true;
}

// 302 change_camera_in_cut_scene 0x65b030 / 0x65b070 (H)
Value f302(Mission& m, const BuiltinCall& c) {
    if (c.args.size() == 5)
        return I(m.ui().cut_scene_camera(m, b8(c, 0), c.i(1), nullptr, fi(c, 2), fi(c, 3), fi(c, 4)));
    const float pos[3] = {fi(c, 1), fi(c, 2), fi(c, 3)};
    return I(m.ui().cut_scene_camera(m, b8(c, 0), 0, pos, fi(c, 4), fi(c, 5), fi(c, 6)));
}

// 303 is_cut_scene_status_black_screen 0x65b0e0 (C), also its WAIT predicate.
bool p303(Mission& m, const BuiltinCall&) { return !m.ui().cs.active || m.ui().cs.status == 7; }
Value f303(Mission& m, const BuiltinCall& c) { return I(p303(m, c)); }

// 304 start_buy_interface 0x653890 (H): shop facility + seller unit.
Value f304(Mission& m, const BuiltinCall& c) {
    const int shop = static_cast<std::int16_t>(c.i(0)), seller = static_cast<std::int16_t>(c.i(1));
    if (!m.places().facility_valid(shop) || !live_unit(m, seller)) return I(0);
    MissionUi& ui = m.ui();
    ui.buy_open = true;
    ui.buy_shop = shop;
    ui.buy_seller = seller;
    ui.interface_ms = m.now();
    m.emit(K::BuyInterface, shop, seller);
    return I(1);
}
bool p304(Mission& m, const BuiltinCall&) { return !m.ui().buy_open; }  // FUN_00653c20

// 305 close_inventory_interface 0x653930 (C)
Value f305(Mission& m, const BuiltinCall&) {
    if (m.ui().inventory_open) {
        m.ui().inventory_open = false;
        m.emit(K::Ui, 0, 0, "close_inventory");
    }
    return I(1);
}

// 306 show_objective 0x653970 (H): objectives panel of the main game interface (FUN_00707ce0).
Value f306(Mission& m, const BuiltinCall&) {
    m.emit(K::ObjectiveShow);
    return I(1);
}

// 307 start_scenario_part_interface 0x6539a0 (C)
Value f307(Mission& m, const BuiltinCall& c) {
    m.ui().scenario_part = u8(c, 0);
    m.emit(K::ScenarioPart, m.ui().scenario_part);
    return I(1);
}

// 308 minimap_set_way_point 0x6539e0 -> FUN_00718010 (C). Slots 1..30 (unchecked in the original).
Value f308(Mission& m, const BuiltinCall& c) {
    const int slot = static_cast<std::int16_t>(c.i(0)) - 1;
    const auto type = static_cast<std::int8_t>(c.i(4));
    if (slot < 0 || slot >= 30) return I(1);
    MissionUi::Waypoint& w = m.ui().waypoints[static_cast<size_t>(slot)];
    if (type != w.type) {
        w.type = type;
        if (type != 0) {
            w.pos[0] = c.f(1), w.pos[1] = c.f(2), w.pos[2] = c.f(3);
            w.blink = true;
            w.blink_ms = m.now();
            w.flag = u8(c, 5);
        }
        m.emit(K::Minimap, slot + 1, type, "way_point");
    }
    return I(1);
}

// 309 minimap_set_way_point_follow_unit 0x653a50 -> FUN_007180f0 (C)
Value f309(Mission& m, const BuiltinCall& c) {
    const int slot = static_cast<std::int16_t>(c.i(0) - 1);
    if (slot < 0 || slot >= 30) return I(1);
    MissionUi::Waypoint& w = m.ui().waypoints[static_cast<size_t>(slot)];
    w.follow = c.i(1);
    w.flag = u8(c, 2);
    m.emit(K::Minimap, slot + 1, w.follow, "way_point_follow");
    return I(1);
}

// --- DIALOG executor 0x65fd70 -----------------------------------------------------------------

// 316 add_dialog (C): (speaker, text[, flag]) or (speaker, text, x1, y1, x2, y2[, flag]).
// EX.res entry animation fields (+8/+0xe) are not exposed by ScriptText: 0.
Value f316(Mission& m, const BuiltinCall& c) {
    MissionUi& ui = m.ui();
    ui.del_all_dialogs(m);
    bool found = false;
    std::string text = text_of(c, 1, &found);
    const int voice = found ? tag_number(c.s(1)) : 0;
    const size_t argc = c.args.size();
    if (argc == 2 || argc == 3) return I(ui.add_dialog(m, c.i(0), std::move(text), stem(c), voice, 0, 0, nullptr));
    if (argc == 6 || argc == 7) {
        const std::int16_t rect[4] = {static_cast<std::int16_t>(c.i(2)), static_cast<std::int16_t>(c.i(3)),
                                      static_cast<std::int16_t>(c.i(4)), static_cast<std::int16_t>(c.i(5))};
        return I(ui.add_dialog(m, c.i(0), std::move(text), stem(c), voice, 0, 0, rect));
    }
    return I(0);  // node value unchanged
}
// WAIT FUN_00660e60: blocks only inside a (not ending) cut scene until every box is typed.
bool p316(Mission& m, const BuiltinCall&) {
    const MissionUi& ui = m.ui();
    return ui.dialogs_done() || !m.state().cut_scene || ui.cs.ending;
}

// 317 del_dialog 0x63e1d0 (C); 318 del_all_dialog 0x63d150 (C).
Value f317(Mission& m, const BuiltinCall& c) {
    m.ui().del_dialog(m, c.i(0));
    return I(0);
}
Value f318(Mission& m, const BuiltinCall&) {
    m.ui().del_all_dialogs(m);
    return I(0);
}

// 319 add_choice_dialog (C): (speaker, question, choice1, enabled1, ...).
Value f319(Mission& m, const BuiltinCall& c) {
    MissionUi& ui = m.ui();
    ui.del_all_dialogs(m);
    std::string text = text_of(c, 1);
    const int voice = c.text ? tag_number(c.s(1)) : 0;
    const int id = ui.add_dialog(m, c.i(0), std::move(text), stem(c), voice, 0, 0, nullptr);
    if (MissionUi::DialogBox* d = ui.dialog(id)) {
        for (size_t k = 3; k <= c.args.size(); k += 2) {
            d->choices.push_back({text_of(c, k - 1), c.i(k) != 0});
            d->has_choices = true;
        }
        m.emit(K::ChoiceDialog, id, static_cast<std::int32_t>(d->choices.size()), d->text);
    }
    ui.site_dialog[c.site_id] = id;
    return I(id);
}
// WAIT FUN_0063d860(id): finished once the boxes are typed and the player picked a choice.
bool p319(Mission& m, const BuiltinCall& c) {
    MissionUi& ui = m.ui();
    const auto it = ui.site_dialog.find(c.site_id);
    const MissionUi::DialogBox* d = it == ui.site_dialog.end() ? nullptr : ui.dialog(it->second);
    if (!d || !d->has_choices) return true;
    return d->done && d->choice != 0;
}

// 320 get_choice 0x63d770 (C)
Value f320(Mission& m, const BuiltinCall& c) {
    const MissionUi::DialogBox* d = m.ui().dialog(c.i(0));
    return I(d ? d->choice : 0);
}

// 321 dialog_text (C). Deviation: an unknown tag returns the tag (the original copies EX.res entry 0).
Value f321(Mission&, const BuiltinCall& c) { return Value::string(text_of(c, 0)); }

// 322 set_dialog_voice (C): dialog +0x340 = voice of (script stem, atoi(tag+1)).
Value f322(Mission& m, const BuiltinCall& c) {
    if (MissionUi::DialogBox* d = m.ui().dialog(c.i(0))) {
        d->voice = tag_number(c.s(1));
        d->voice_stem = stem(c);
        m.emit(K::Sound, d->id, d->voice, d->voice_stem);
    }
    return I(1);
}

// 329 display_title_screen 0x65c420 -> FUN_00761680 (C); WAIT FUN_0065c470.
Value f329(Mission& m, const BuiltinCall& c) {
    MissionUi::Title& t = m.ui().title;
    t.active = true;
    t.start_ms = m.now();
    t.duration = c.i(1), t.fade_in = c.i(2), t.fade_out = c.i(3);
    t.text = std::string(c.s(0).substr(0, 30));
    m.emit(K::TitleScreen, t.duration, 1, t.text);
    return I(1);
}
bool p329(Mission& m, const BuiltinCall&) { return !m.ui().title.active; }

// 344 attention_call_message 0x65cea0 (C): icon 'M0000' (flag 0) or 'TMESS' (tutorial).
Value f344(Mission& m, const BuiltinCall& c) {
    const char* icon = c.i(1) == 0 ? "M0000" : "TMESS";
    return I(m.ui().add_attention_message(m, text_of(c, 0), icon, 0, stem(c), 0));
}

// 345 attention_call_message_2 0x65cf00 / 0x65cf40 (C): speaker name (portrait) or unit recno.
Value f345(Mission& m, const BuiltinCall& c) {
    const int voice = tag_number(c.s(0));
    if (c.args.size() > 1 && c.args[1].type == ScriptType::String)
        return I(m.ui().add_attention_message(m, text_of(c, 0), std::string(c.s(1)), 0, stem(c), voice));
    return I(m.ui().add_attention_message(m, text_of(c, 0), {}, c.i(1), stem(c), voice));
}

// 346 set_attention_call_message_voice 0x65cf80 (C). Returns 1 for the original's message pointer.
Value f346(Mission& m, const BuiltinCall& c) {
    MissionUi::AttentionMessage* a = m.ui().attention_message(c.i(0));
    if (!a) return I(0);
    bool found = false;
    text_of(c, 1, &found);
    a->voice = found ? tag_number(c.s(1)) : 0;
    a->voice_stem = stem(c);
    return I(1);
}

// 356 display_string 0x65d9c0 (H) / 357 display_number 0x65da50 (C): one-frame screen text.
Value f356(Mission& m, const BuiltinCall& c) {
    m.emit(K::Text, c.i(1), c.i(2), std::string(c.s(0)));
    return I(1);
}
Value f357(Mission& m, const BuiltinCall& c) {
    const long long v = c.i(0);  // FUN_0058fca0(v, 4): negative in parentheses
    m.emit(K::Text, c.i(1), c.i(2), v < 0 ? "(" + std::to_string(-v) + ")" : std::to_string(v));
    return I(1);
}

// 364 get_key_string 0x65e0a0 (C) -> KeyMap_get_name(DAT_00a5b9f8, action) 0x4cb5b0.
struct KeyRow {
    int action;
    const char* name[3];  // input scheme 0 keyboard, 1 joystick, 3 keyboard+mouse (STD.SET defaults)
};
constexpr KeyRow kKeys[] = {
    {2, {"Not Available", "JOYPAD10", "Not Available"}}, {3, {"Shift", "Not Available", "Not Available"}},
    {4, {"Up", "Not Available", "W"}},                    {5, {"Down", "Not Available", "S"}},
    {6, {"Not Available", "Not Available", "A"}},         {7, {"Not Available", "Not Available", "D"}},
    {8, {"Left", "Not Available", "Not Available"}},      {9, {"Right", "Not Available", "Not Available"}},
    {12, {"E", "JOYPAD2", "Q"}},                          {13, {"Q", "JOYPAD4", "Left mouse button"}},
    {14, {"W", "JOYPAD3", "Right mouse button"}},         {15, {"S", "Not Available", "C"}},
    {17, {"R", "JOYPAD8", "R"}},                          {18, {"Z", "JOYPAD9", "Z"}},
    {19, {"F3", "JOYPAD5", "F3"}},                        {23, {"F2", "F2", "F2"}},
    {24, {"`", "`", "`"}},                                {25, {"1", "1", "1"}},
    {26, {"2", "2", "2"}},                                {27, {"3", "3", "3"}},
    {35, {"F1", "JOYPAD7", "F1"}},                        {36, {"Not Available", "Not Available", "Not Available"}},
    {40, {"F12", "F12", "F12"}},                          {48, {"NUMPAD+", "NUMPAD+", "NUMPAD+"}},
    {49, {"NUMPAD-", "NUMPAD-", "NUMPAD-"}},
};
constexpr struct {
    const char* name;
    int action;
} kKeyNames[] = {
    {"A_ATTACK", 13},     {"B_ATTACK", 14},        {"DEFEND", 12},         {"ARROW_SHOOTING", 18},
    {"INTERACT", 17},     {"RIDE", 17},            {"CHARACTER_INTERFACE", 35}, {"DOWN_ATTACK", 12},
    {"USE_HEAL_ITEM", 19}, {"COMBO_INTERFACE", 36}, {"SWITCH_CHARACTER", 24}, {"FORWARD", 4},
    {"BACKWARD", 5},      {"TURN_LEFT", 8},        {"TURN_RIGHT", 9},      {"SHIFT_LEFT", 6},
    {"SHIFT_RIGHT", 7},   {"SHIFT", 3},            {"SHIFT_CAMERA", 2},    {"ZOOM_IN", 48},
    {"ZOOM_OUT", 49},     {"ENERGY_ATTACK", 15},   {"SWITCH_JOAN", 25},    {"SWITCH_JEAN", 26},
    {"SWITCH_LA_HIRE", 27}, {"GAME_OBJECTIVE_KEY", 40}, {"RTS_MODE", 23},
};
Value f364(Mission& m, const BuiltinCall& c) {
    std::string& buf = m.ui().site_text[c.site_id];  // node-owned buffer: unknown names leave it unchanged
    const std::string_view name = c.s(0);
    for (const auto& k : kKeyNames) {
        if (name != k.name) continue;
        World& w = m.world();
        const int mode = w.game_mode == 4 ? 4 : w.controller().mode;
        const int col = mode == 0 ? 0 : mode == 1 ? 1 : mode == 3 ? 2 : -1;
        buf.clear();
        if (col >= 0 && m.ui().key_name) buf = m.ui().key_name(k.action);
        else if (col >= 0)
            for (const KeyRow& r : kKeys)
                if (r.action == k.action) buf = r.name[col];
        break;
    }
    return Value::string(buf);
}

// 366 start_kill_interface 0x65e810 / 369 _2 0x65e8c0 (H); 367 / 370 end (H). The per-unit HP-bar
// display flags the original toggles (unit+0x31d +0x52) are render state.
Value kill_start(Mission& m, const BuiltinCall& c, int n) {
    MissionUi::KillInterface& k = m.ui().kill[n];
    if (!k.active) {
        k.active = true;
        k.mode = static_cast<std::int8_t>(c.args.empty() ? 0 : c.i(0));
        m.emit(K::Ui, n + 1, k.mode, "kill_interface_start");
    }
    return I(1);
}
Value kill_end(Mission& m, int n) {
    MissionUi::KillInterface& k = m.ui().kill[n];
    if (k.active || n == 0) {  // FUN_006dff10 clears unconditionally, FUN_006e01d0 only when open
        k.active = false;
        m.emit(K::Ui, n + 1, 0, "kill_interface_end");
    }
    return I(1);
}
Value f366(Mission& m, const BuiltinCall& c) { return kill_start(m, c, 0); }
Value f367(Mission& m, const BuiltinCall&) { return kill_end(m, 0); }
Value f369(Mission& m, const BuiltinCall& c) { return kill_start(m, c, 1); }
Value f370(Mission& m, const BuiltinCall&) { return kill_end(m, 1); }

// 378 set_minimap_icon_display_enable 0x65ec90 (C). HYPOTHESIS: the minimap is ready and has the
// icon (the icon table is minimap resource data the mission layer does not load): returns 1.
Value f378(Mission& m, const BuiltinCall& c) {
    m.ui().minimap_icons[std::string(c.s(0))] = u8(c, 1);
    m.emit(K::Minimap, c.i(1), 0, "icon " + std::string(c.s(0)));
    return I(1);
}

// 382 set_minimap_rotation 0x65eda0 (C) / 383 set_minimap_display_gold_enable 0x65edd0 (C)
Value f382(Mission& m, const BuiltinCall& c) {
    m.ui().minimap_rotation = static_cast<std::int16_t>(c.i(0));
    m.emit(K::Minimap, c.i(0), 0, "rotation");
    return I(1);
}
Value f383(Mission& m, const BuiltinCall& c) {
    m.ui().minimap_gold = static_cast<std::int16_t>(c.i(0));
    m.emit(K::Minimap, c.i(0), 0, "gold");
    return I(1);
}

// 385 show_scenario_brief 0x65ee60 (C); WAIT FUN_0065eea0 (interface mode back to 0).
Value f385(Mission& m, const BuiltinCall&) {
    m.ui().brief_open = true;
    m.ui().interface_ms = m.now();
    m.emit(K::Brief, 1);
    return I(1);
}
bool p385(Mission& m, const BuiltinCall&) { return !m.ui().brief_open; }

// 386 turn_camera 0x65eee0 (H): synchronous turn of the camera to the player's heading.
Value f386(Mission& m, const BuiltinCall&) {
    m.emit(K::Camera, m.world().controller().unit, 0, "turn");
    return I(1);
}

// 390 is_cut_scene_finished 0x65f600 (C), also its WAIT predicate.
bool p390(Mission& m, const BuiltinCall& c) {
    const MissionUi& ui = m.ui();
    const int allow_skip = c.args.empty() ? 0 : c.i(0);
    return !((m.state().cut_scene || ui.cs.active) && (allow_skip == 0 || !ui.cs.ending));
}
Value f390(Mission& m, const BuiltinCall& c) { return I(p390(m, c)); }

// 393-396 minimap vision areas 0x65f740.. -> FUN_007196e0.. (C). Areas 0..9 (unchecked originally).
MissionUi::VisionArea* area(Mission& m, const BuiltinCall& c) {
    const int a = c.i(0);
    return a >= 0 && a < 10 ? &m.ui().vision[static_cast<size_t>(a)] : nullptr;
}
Value vision_set(Mission& m, const BuiltinCall& c, std::uint8_t shape, size_t n) {
    if (MissionUi::VisionArea* v = area(m, c)) {
        v->enabled = true;
        v->shape = shape;
        if (shape == 1) {
            v->c[0] = c.i(1), v->c[1] = c.i(2), v->radius = c.i(3);
        } else {
            for (size_t i = 0; i < n; ++i) v->c[i] = c.i(i + 1);
        }
        m.emit(K::Minimap, c.i(0), shape, "vision_area");
    }
    return I(1);
}
Value f393(Mission& m, const BuiltinCall& c) { return vision_set(m, c, 0, 4); }
Value f394(Mission& m, const BuiltinCall& c) { return vision_set(m, c, 1, 0); }
Value f395(Mission& m, const BuiltinCall& c) { return vision_set(m, c, 2, 8); }
Value f396(Mission& m, const BuiltinCall& c) {
    if (MissionUi::VisionArea* v = area(m, c)) {
        v->enabled = false;
        m.emit(K::Minimap, c.i(0), -1, "vision_area");
    }
    return I(1);
}

// 398 show_letter_interface 0x65f910 (C): opening the letter stops the game clock (DAT_00a500d2);
// HYPOTHESIS: closing it restarts the clock. WAIT FUN_0065f950.
Value f398(Mission& m, const BuiltinCall& c) {
    MissionUi& ui = m.ui();
    ui.letter_open = true;
    ui.letter_id = static_cast<std::int16_t>(c.i(0));
    ui.clock_paused = true;
    ui.interface_ms = m.now();
    m.emit(K::Letter, ui.letter_id, 1);
    return I(1);
}
bool p398(Mission& m, const BuiltinCall&) { return !m.ui().letter_open; }

// 404 is_attention_call_message_shown 0x65fbf0 (C): 0 while a live message has this text.
Value f404(Mission& m, const BuiltinCall& c) {
    const std::string text = text_of(c, 0);
    for (auto it = m.ui().messages.rbegin(); it != m.ui().messages.rend(); ++it)
        if (it->text == text) return I(0);
    return I(1);
}

// 405 set_end_cut_scene_type 0x65fc90 (C): DAT_00a5d386 = controller +6 (black-screen end).
Value f405(Mission& m, const BuiltinCall& c) {
    m.ui().cs.black_end = u8(c, 0);
    return I(1);
}

}  // namespace

void bind_ui(BuiltinTable& t) {
    t.call[293] = &f293, t.call[294] = &f294, t.call[295] = &f295, t.call[296] = &f296, t.call[297] = &f297;
    t.call[298] = &f298, t.call[299] = &f299, t.call[300] = &f300, t.call[301] = &f301, t.call[302] = &f302;
    t.call[303] = &f303, t.call[304] = &f304, t.call[305] = &f305, t.call[306] = &f306, t.call[307] = &f307;
    t.call[308] = &f308, t.call[309] = &f309;
    t.call[316] = &f316, t.call[317] = &f317, t.call[318] = &f318, t.call[319] = &f319, t.call[320] = &f320;
    t.call[321] = &f321, t.call[322] = &f322, t.call[329] = &f329;
    t.call[344] = &f344, t.call[345] = &f345, t.call[346] = &f346, t.call[356] = &f356, t.call[357] = &f357;
    t.call[364] = &f364, t.call[366] = &f366, t.call[367] = &f367, t.call[369] = &f369, t.call[370] = &f370;
    t.call[378] = &f378, t.call[382] = &f382, t.call[383] = &f383, t.call[385] = &f385, t.call[386] = &f386;
    t.call[390] = &f390, t.call[393] = &f393, t.call[394] = &f394, t.call[395] = &f395, t.call[396] = &f396;
    t.call[398] = &f398, t.call[404] = &f404, t.call[405] = &f405;

    t.poll[296] = &p_cut, t.poll[297] = &p_cut, t.poll[302] = &p_cut;
    t.poll[301] = &p301, t.poll[303] = &p303, t.poll[304] = &p304, t.poll[316] = &p316, t.poll[319] = &p319;
    t.poll[329] = &p329, t.poll[385] = &p385, t.poll[390] = &p390, t.poll[398] = &p398;

    t.unimplemented[379] = "get_minimap_icon_display_enable: unused by shipped scripts; the icon table is minimap resource data";
}

}  // namespace joa::game
