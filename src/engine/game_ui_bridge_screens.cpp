// GameUiBridge overlays (1024x768 design units, Center frame): cut-scene letterbox / black wipe FUN_006ca230,
// dialog box FUN_0063b830 + choices (FUN_0063c8a0 / input FUN_0063c1c0), title FUN_00761340, loading screen
// FUN_00756430, win message FUN_006e6dc0 + game-end menu (objmenu win mode, input 0x720ca0), game-over box
// FUN_006b7160. Facts, addresses and confidence: docs/reverse-engineering/UI_SYSTEM.md "Game UI bridge".
#include "engine/game_ui_bridge.hpp"

#include "engine/menu_saveload.hpp"
#include "engine/hud_effects.hpp"
#include "engine/ui_screens.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace joa::engine {
namespace {

const glm::vec4 kBlack = ui_rgba(0, 0, 0);
const glm::vec4 kChoiceHot = ui_rgba(220, 32, 32);  // hovered choice redraw colour (HYPOTHESIS: the '_' red)

glm::vec4 black(float a) { return ui_rgba(0, 0, 0, int(std::clamp(a, 0.0f, 255.0f))); }
glm::vec4 white(float a) { return ui_rgba(255, 255, 255, int(std::clamp(a, 0.0f, 255.0f))); }

// The whole window in Center-frame design units (letterbox bars and black screens span pillar areas too).
glm::vec4 window_rect(const Ui& ui) {
    const UiLayout& l = ui.layout();
    const glm::vec2 a = ui.to_design(l.origin), b = ui.to_design(l.origin + l.size);
    return {a.x, a.y, b.x - a.x, b.y - a.y};
}

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// Win message / game-over box animation (WinMsg_animate 0x6e7cf0, FUN_006b7f20): one step per 50 ms through the
// shrinking G100 frames (bits 1..8), the settled G50 (0x10) and the glow pulse (0x20/0x40/0x60); text state
// (+0xd29 / +0x9e3) 1..3 during the pulse's fall, 4 = settled (buttons).
struct EndFrame { int bits, text; };
constexpr EndFrame kEndFrames[] = {{0x01, 0}, {0x03, 0}, {0x07, 0}, {0x0f, 0}, {0x1f, 0}, {0x1e, 0}, {0x1c, 0},
                                   {0x18, 0}, {0x30, 0}, {0x50, 0}, {0x70, 0}, {0x50, 1}, {0x30, 2}, {0x10, 3}};
EndFrame end_frame(float t) {
    const int i = int(t / 0.05f);
    return i < int(std::size(kEndFrames)) ? kEndFrames[i] : EndFrame{0x10, 4};
}

// Shrinking G100 frames + G50 + GGLOW of table `set` (VICT / GAMEOVER), all centred on the canvas.
void end_title(Ui& ui, std::string_view set, const glm::vec2 (&g100)[4], glm::vec2 g50, glm::vec2 glow, int bits) {
    static constexpr int kAlpha[4] = {0x7f, 0x99, 0xb2, 0xcc};
    const UiSprite* big = ui.sprite(set, "G100");
    for (int k = 0; k < 4; ++k)
        if (bits & (1 << k)) {
            const glm::vec2 s = g100[k];
            ui.sprite(big, glm::vec4((1024 - s.x) / 2, (768 - s.y) / 2, s.x, s.y), white(float(kAlpha[k])));
        }
    if (const int g = (bits >> 5) & 3) ui.sprite(set, "GGLOW", glow, white(g == 1 ? 0x7f : g == 2 ? 0xbf : 0xff));
    if (bits & 0x70) ui.sprite(set, "G50", g50);
}

// Wrapped centred end text (0x626e00, centred both ways as in s08_end) in (172,450)-(852,586), Goudy18(27) white
// (font obj 0xa46060, asm 0x6b798f / 0x6e758f).
void end_text(Ui& ui, std::string_view text, int state) {
    const UiFont* f = ui.font("Goudy18(27) white");
    if (state == 0 || text.empty() || !f) return;
    const float a = std::min(255.0f, float(25 * state + 25) * 255.0f / 100.0f);
    const float h = float(wrap_text(f->metrics, text, 852 - 172 + 1).size() * std::size_t(f->metrics.line_height()));
    ui.text_block(f, text, {172, std::floor(450 + (137 - h) / 2), 852 - 172 + 1, h + 1}, white(a), 0, TextAlign::Center);
}

// COMMELE MBUT button (draw cb 0x6b82d0): MBUT_CON frame at (x-4, y-4), MBUT_OT/OV/DW 200x24, label centred in
// Goudy12(18) black (0xa489c8) coloured 0xff003366, hover MBUT_MOT marks left/right of the label. True on click.
bool mbut(Ui& ui, std::string_view label, glm::vec2 p) {
    const glm::vec4 r{p.x, p.y, 200, 24};
    const bool hot = ui.hover(r), down = hot && ui.input().mouse_down;
    ui.sprite("COMMELE", "MBUT_CON", p - 4.0f);
    ui.sprite("COMMELE", down ? "MBUT_DW" : hot ? "MBUT_OV" : "MBUT_OT", p);
    const UiFont* f = ui.font("Goudy12(18) black");
    ui_text_in_rect(ui, f, label, {int(p.x), int(p.y), int(p.x) + 199, int(p.y) + 23}, ui_rgba(0, 0x33, 0x66));
    if (hot && f) {
        const float tw = float(f->metrics.measure(label));
        ui.sprite("COMMELE", "MBUT_MOT", {p.x + (200 - tw) / 2 - 25, p.y + 1});
        ui.sprite("COMMELE", "MBUT_MOT", {p.x + (200 + tw) / 2 + 1, p.y + 1});
    }
    return ui.clicked(r);
}

UiCommand command(UiCommand::Type t) {
    UiCommand c;
    c.type = t;
    return c;
}

}  // namespace

bool GameUiBridge::blocking(game::Mission& m) const {
    const game::MissionUi& u = m.ui();
    return u.cs.active || u.game_cut_scene || u.title.active || m.finish() != 0 || m.state().game_over;
}

void GameUiBridge::draw(Ui& ui, game::Mission& m, HudModel& hud, std::vector<UiCommand>& out) {
    ui.frame(Anchor::Center);
    game::MissionUi& mui = m.ui();
    post_effects(ui, m);  // HudGaps: bribe_no_gold attention message (needs the t_interf text)
    if (!hud.show_mission_brief && !hud.show_message_log) sms_line(ui, m), ui.frame(Anchor::Center);
    if (mui.cs.active) letterbox(ui, m);
    dialog_box(ui, m);
    if (mui.title.active) title_screen(ui, m);
    const bool lost = m.state().game_over || m.finish() == 2;
    if (lost) {
        over_time_ = over_time_ < 0 ? 0.0f : over_time_ + ui.dt();
        game_over(ui, m, out);
    } else {
        over_time_ = -1.0f;
    }
    if (!lost && m.finish() == 1 && !mui.cs.active && !mui.title.active) {
        if (end_menu_) {
            game_end_menu(ui, m, hud, out);
        } else {
            win_time_ = win_time_ < 0 ? 0.0f : win_time_ + ui.dt();
            win_message(ui, m, out);
        }
    } else {
        win_time_ = -1.0f, end_menu_ = false;
    }
}

// FUN_006ca230: letterbox bars h = (W*55/640) * fade/255 (88 px at 255), black with alpha = fade; status 6/8 =
// cut_scene_texture wipe at x = cs.wipe (512 wide, mirrored when wipe_flip) with black on its trailing side,
// 7 = black hold.
void GameUiBridge::letterbox(Ui& ui, game::Mission& m) {
    const auto& cs = m.ui().cs;
    const glm::vec4 win = window_rect(ui);
    if (cs.status == 7) return ui.fill(win, kBlack);
    if (cs.status == 6 || cs.status == 8) {
        const float x = cs.wipe;
        const glm::vec2 u0 = cs.wipe_flip ? glm::vec2(1, 0) : glm::vec2(0, 0), u1 = cs.wipe_flip ? glm::vec2(0, 1) : glm::vec2(1, 1);
        ui.image(ui.texture("cut_scene_texture"), {x, win.y, 512, win.w}, u0, u1);
        if (cs.status == 6) ui.fill({x + 512, win.y, win.x + win.z - x - 512, win.w}, kBlack);
        else if (x > win.x) ui.fill({win.x, win.y, x - win.x, win.w}, kBlack);
        if (cs.status == 6) return;
    }
    const float h = std::floor(1024.0f * 55.0f / 640.0f * cs.fade / 255.0f);
    if (h <= 1.0f) return;
    ui.fill({win.x, win.y, win.z, h - win.y}, black(cs.fade));
    ui.fill({win.x, 768 - h, win.z, win.y + win.w - (768 - h)}, black(cs.fade));
}

// FUN_0063b830 (only while cs.status == 2 && fade == 255): ICON_1 / portrait / ICON_2 at (28,689), text
// Goudy14(22) white (asm 0x63bae5) in manager (104,663) + box rect (15,30,848,36), typed at 40 chars/s
// (manager +2). A click / Space / Enter finishes the typing (FUN_0063c1c0); choices (once done) are laid out
// below the text at x 30, width manager w - 50, 5 px apart, and a click on one answers the dialog.
void GameUiBridge::dialog_box(Ui& ui, game::Mission& m) {
    game::MissionUi& mui = m.ui();
    game::MissionUi::DialogBox* d = nullptr;
    for (auto& b : mui.dialogs)
        if (!b.deleted) d = &b;
    if (!d) return void(typed_id_ = 0);
    if (d->id != typed_id_) typed_id_ = d->id, typed_ = 0.0f;
    if (mui.cs.status != 2 || mui.cs.fade != 255.0f) return;
    ui.nav_modal(3000 + (d->done && d->has_choices));  // GamepadPlay: South advances / picks the focused choice
    const UiFont* f = ui.font("Goudy14(22) white");
    if (!f) return;
    const glm::vec2 mgr{0x68, 768 - 0x69};
    const bool custom = d->rect[0] >= 0;
    const glm::vec4 r = custom ? glm::vec4(d->rect[0], d->rect[1], d->rect[2] - d->rect[0] + 1, d->rect[3] - d->rect[1] + 1)
                               : glm::vec4(15, 30, 0x37c - 0x2c, 0x69 - 0x45);
    const float len = float(d->text.size());
    const bool advance = ui.input().mouse_released || ui.key(SDL_SCANCODE_SPACE) || ui.key(SDL_SCANCODE_RETURN);
    typed_ = std::min(len, typed_ + ui.dt() * 40.0f);
    if (advance && !d->has_choices) typed_ = len;
    if (typed_ >= len && !voice_active && !d->done) mui.dialog_done(d->id);

    ui.sprite("MESSAGE", "icon_1", {28, 689});
    const std::string portrait = message_portrait(m, d->speaker, {});
    if (const UiSprite* p = ui.sprite("MESSAGE", portrait)) ui.sprite(p, glm::vec2(28, 689) + (glm::vec2(72) - p->size) / 2.0f);
    ui.sprite("MESSAGE", "icon_2", {28, 689});
    ui_markup_block(ui, f, std::string_view(d->text).substr(0, std::size_t(typed_)), {mgr.x + r.x, mgr.y + r.y, r.z, r.w}, kUiWhite, 2);

    if (!d->done || !d->has_choices || d->choice) return;
    const int lh = f->metrics.line_height() + 2;
    auto block_h = [&](std::string_view s, float w) { return ui_markup_lines(f->metrics, s, int(w)) * lh; };
    float y = mgr.y + r.y + (d->text.empty() ? 0.0f : float(block_h(d->text, r.z) + 4));
    const float x = mgr.x + 30, w = 0x37c - 0x32;
    for (std::size_t i = 0; i < d->choices.size(); ++i) {
        const auto& c = d->choices[i];
        if (!c.enabled && !mui.dialog_show_disabled) continue;
        const float h = float(block_h(c.text, w));
        const glm::vec4 hit{x - 20, y, w + 20, h};
        const bool hot = c.enabled && ui.hover(hit);
        ui_markup_block(ui, f, c.text, {x, y, w, h}, !c.enabled ? ui_rgba(255, 0, 0) : hot ? kChoiceHot : kUiWhite);
        if (hot && ui.clicked(hit)) mui.answer_dialog(d->id, int(i) + 1);
        y += h + 5;
    }
}

// FUN_00761340: black screen (alpha bg) + text in font_verdana_12_bold (asm 0x7614a8) scaled 5x, centred.
// Alpha: fade-in ramp, 255, fade-out ramp over the title's game-ms clock; an empty text keeps the screen black.
void GameUiBridge::title_screen(Ui& ui, game::Mission& m) {
    const auto& t = m.ui().title;
    const float now = float(m.now() - t.start_ms);
    float text_a = 255.0f, bg_a = 255.0f;
    if (now < float(t.fade_in) && t.fade_in > 0) {
        text_a = now * 255.0f / float(t.fade_in);
        if (t.text.empty()) bg_a = text_a;
    } else if (now > float(t.duration)) {
        text_a = bg_a = 0.0f;
    } else if (now > float(t.duration - t.fade_out) && t.fade_out > 0) {
        text_a = bg_a = (float(t.duration) - now) * 255.0f / float(t.fade_out);
    }
    ui.fill(window_rect(ui), black(bg_a));
    const UiFont* f = ui.font("font_verdana_12_bold");
    if (!f || t.text.empty()) return;
    const float h = float(f->metrics.line_height()) * 5.0f;
    ui.text(f, t.text, {512, std::floor((768 - h) / 2)}, white(text_a), TextAlign::Center, 5.0f);
}

// WinMsg draw FUN_006e6dc0: VICT G100 frames (0.9..0.6 of 1024x256) -> G50 (256,320) + GGLOW pulse, text VICTS n,
// then only button 4 "Next Mission" (412,694) which opens the game-end menu (GameEndMenu_open(2)).
void GameUiBridge::win_message(Ui& ui, game::Mission& m, std::vector<UiCommand>&) {
    static constexpr glm::vec2 kG100[4] = {{921, 230}, {819, 204}, {716, 179}, {614, 153}};
    const EndFrame fr = end_frame(win_time_);
    end_title(ui, "VICT", kG100, {256, 320}, {256, 320}, fr.bits);
    end_text(ui, ui.text("VICTS", m.scenario_no()), fr.text);
    if (fr.text == 4) ui.nav_modal(3102);  // GamepadPlay focus scope
    if (fr.text == 4 && mbut(ui, ui.text("VICT", 4), {412, 694})) end_menu_ = true;
}

// Game-end menu (objmenu win mode, FUN_007226a0(2)): in-game backdrop titled OBJMENU 14 "Mission De-brief",
// MISSION / TOTAL TIME boxes, RESTART MISSION (OBJMENU 10) / LOAD GAME (11) / CONTINUE (VICT 5); Esc does nothing.
// ponytail: the topic list (De-brief page 7, Objectives ...) is not drawn here; button row positions HYPOTHESIS.
void GameUiBridge::game_end_menu(Ui& ui, game::Mission&, HudModel& hud, std::vector<UiCommand>& out) {
    const std::string_view title = ui.text("OBJMENU", 14);
    ui.nav_modal(3103);  // GamepadPlay focus scope
    ingame_backdrop_back(ui);
    menu_heading_bars(ui, -48, upper(title));
    ingame_backdrop_front(ui, 1e9f, -1.0f, title);
    const UiFont* small = ui.font("Goudy12(18) inner emboss");
    const UiFont* f14 = ui.font("Goudy14(22) white");
    for (int i = 0; i < 2; ++i) {
        const int x = i ? 876 : 8, t = i ? hud.total_time : hud.mission_time;
        ui.sprite("OBJMENU", "tc", {float(x), 24});
        ui_text_in_rect(ui, small, ui.text("OBJMENU", 76 + i), {x, 28, x + 139, 42}, kUiWhite);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
        ui_text_in_rect(ui, f14, buf, {x, 50, x + 139, 76}, kUiWhite);
    }
    if (ui_text_button(ui, f14, ui.text("OBJMENU", 10), {150, 664, 349, 694})) out.push_back(command(UiCommand::Type::RestartMission));
    if (ui_text_button(ui, f14, ui.text("OBJMENU", 11), {412, 664, 611, 694})) out.push_back(command(UiCommand::Type::LoadMenu));
    if (ui_text_button(ui, f14, ui.text("VICT", 5), {675, 664, 874, 694})) out.push_back(command(UiCommand::Type::ContinueCampaign));
}

// FUN_006b7160: GAMEOVER G100 frames (1080x144 .. 720x96) -> G50 (212,344) + GGLOW (192,324), the script text
// (call_gameover_msg), then RESTART MISSION / LOAD GAME / QUIT TO MAIN (GAMEOVER 1..3) at (412, 564/604/644).
void GameUiBridge::game_over(Ui& ui, game::Mission& m, std::vector<UiCommand>& out) {
    static constexpr glm::vec2 kG100[4] = {{1080, 144}, {960, 128}, {840, 112}, {720, 96}};
    const EndFrame fr = end_frame(over_time_);
    end_title(ui, "GAMEOVER", kG100, {212, 344}, {192, 324}, fr.bits);
    end_text(ui, m.state().game_over_text, fr.text);
    if (fr.text != 4) return;
    ui.nav_modal(3101);  // GamepadPlay focus scope
    if (mbut(ui, ui.text("GAMEOVER", 1), {412, 564})) out.push_back(command(UiCommand::Type::RestartMission));
    if (mbut(ui, ui.text("GAMEOVER", 2), {412, 604})) out.push_back(command(UiCommand::Type::LoadMenu));
    if (mbut(ui, ui.text("GAMEOVER", 3), {412, 644})) out.push_back(command(UiCommand::Type::QuitToMain));
}

// LoadingScreen FUN_00756430 (stage machine +0x85 on real ms): M<n> scr (0,194) behind SL_BG1 vignette, black
// bands above/below, M<n> title (416,72) and name (276,352) [scenario 6: (152,352)], SL_COM bar: fill bg (408,710)
// clipped to the displayed percent, frame (410,712), head bar (405 + 2 pct, 708); "LOADING" (SCELOAD 1)
// Goudy_bold_18 (asm 0x75987d) at (450,744).
void GameUiBridge::draw_loading(Ui& ui, int mission_no, float progress) {
    static constexpr float kStageSec[8] = {3.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 2.0f, 0.5f};
    if (mission_no != load_mission_) load_mission_ = mission_no, load_stage_ = 0, load_time_ = 0.0f, load_shown_ = 0.0f;
    load_time_ += ui.dt();
    const float target = std::clamp(progress, 0.0f, 1.0f) * 100.0f;
    if (load_stage_ >= 4 && load_stage_ <= 5) load_shown_ = std::min(target, load_shown_ + ui.dt() * 20.0f);  // 1 % per 50 ms
    while (load_stage_ < 8) {
        const bool done = load_stage_ == 5 ? load_shown_ >= 100.0f : load_time_ >= kStageSec[load_stage_];
        if (!done) break;
        load_time_ = load_stage_ == 5 ? 0.0f : load_time_ - kStageSec[load_stage_];
        ++load_stage_;
    }
    const float k = load_stage_ < 8 && kStageSec[load_stage_] > 0 ? std::min(1.0f, load_time_ / kStageSec[load_stage_]) : 1.0f;

    ui.frame(Anchor::Center);
    ui.fill(window_rect(ui), kBlack);
    char set[4];
    std::snprintf(set, sizeof set, "M%d", std::clamp(mission_no, 1, 8));
    ui.sprite(set, "scr", {0, 194});
    ui.sprite("SL_BG1", "bg", {0, 0});
    ui.fill({0, 0, 1024, 194}, kBlack);
    ui.fill({0, 582, 1024, 194}, kBlack);
    // Middle band: opaque until stage 3 fades it out.
    if (load_stage_ <= 3) ui.fill({0, 194, 1024, 388}, black(load_stage_ < 3 ? 255.0f : 255.0f * (1.0f - k)));
    const float title_a = load_stage_ == 0 ? 0.0f : load_stage_ == 1 ? 255.0f * k : 255.0f;
    const float name_a = load_stage_ <= 1 ? 0.0f : load_stage_ == 2 ? 255.0f * k : 255.0f;
    ui.sprite(set, "title", {416, 72}, white(title_a));
    ui.sprite(set, "name", {mission_no == 6 ? 152.0f : 276.0f, 352}, white(name_a));
    if (load_stage_ >= 4 && load_stage_ <= 6) {
        const float a = load_stage_ == 6 ? 255.0f * (1.0f - k) : 255.0f;
        if (const UiSprite* fill = ui.sprite("SL_COM", "bg")) {
            const float w = std::floor(2.04f * load_shown_);
            const float du = (fill->uv1.x - fill->uv0.x) * w / fill->size.x;
            ui.image(fill->texture, {408, 710, w, fill->size.y}, fill->uv0, {fill->uv0.x + du, fill->uv1.y}, white(a));
        }
        ui.sprite("SL_COM", "frame", {410, 712}, white(a));
        ui.sprite("SL_COM", "bar", {405 + 2 * std::floor(load_shown_), 708}, white(a));
        ui.text(ui.font("Goudy_bold_18"), ui.text("SCELOAD", 1), {450, 744}, white(a));
    }
    if (load_stage_ >= 7) ui.fill(window_rect(ui), black(load_stage_ == 7 ? 255.0f * k : 255.0f));
}

}  // namespace joa::engine

namespace joa::engine {

// ---- SMS line (interfac\i_sms.cpp) ----------------------------------------------------------------------
// FUN_006e5700(text, jump, type) queues a line; FUN_006e5280 slides the front one: the QSAVE "QLC_BAR" bar at
// (x, 0x204) and the Goudy14(22) text, black (FUN_0042a260(0,0,0,0xff)), at (x, 0x206) (setup FUN_006e4f10)
// start at x = -text width, alpha 0; move to x 0 (2700 px/s, -1000 px/s^2) while fading in at 1.5/s; hold
// +0x17c = 3000 ms; the text leaves (v0 0, +1600 px/s^2, fade out 1.5/s) and the bar follows once the text is
// 20 % out; then the next line. Drawn by FUN_006e5140 in game (not in cut scenes / full-screen pages).
bool GameUiBridge::sms_event(const game::MissionEvent& e) {
    if (e.kind != game::MissionEvent::Kind::Ui || !(e.text.starts_with("sms_") || e.text == "food_blocked")) return false;
    sms_events_.push_back(e);
    return true;
}

void GameUiBridge::sms_line(Ui& ui, game::Mission& m) {
    const auto& items = m.data().items;
    for (const game::MissionEvent& e : sms_events_) {
        if (e.text == "food_blocked") {  // Unit_auto_pickup S7_food* without set_food_can_be_picked: 0x4ad710(MESSAGE 3)
            m.ui().add_attention_message(m, std::string(ui.text("MESSAGE", 3)), "M0000", 0, "", 0);
            continue;
        }
        std::string text;
        int type = 0;
        if (e.text == "sms_gold_crates") {  // FUN_004dd8a0 (SMS 11) / FUN_004dd8e0(count) (SMS 12), type 1
            text = ui_format(ui.text("SMS", e.a == 1 ? 11 : 12), {std::to_string(e.a)}), type = 1;
        } else if (e.text == "sms_siege_built") {  // RecruitWorkshop: workshop process 0x46a040 -> 0x6e5700(WORKSHOP 17)
            text = ui.text("WORKSHOP", 17);
        } else if (e.a > 0 && e.a < int(items.size())) {
            // 0x5a6660(MessageName +0x18, quantity / gold) then SMS 3 (FUN_004dd6a0) / SMS 4 (FUN_004dd6e0).
            const std::string what = ui_format(items[std::size_t(e.a)].message, {std::to_string(e.b)});
            text = ui_format(ui.text("SMS", e.text.starts_with("sms_given") ? 4 : 3), {what});
        }
        if (text.empty()) continue;
        bool restart = sms_.empty();
        if (type == 1) {  // param_3 == 1 drops the queued counter lines; jump (front is one): shown at once
            restart = restart || sms_.front().type == 1;
            std::erase_if(sms_, [](const Sms& s) { return s.type == 1; });
        }
        if (restart) sms_.push_front({text, type}), sms_state_ = 0, sms_time_ = 0.0f;
        else sms_.push_back({text, type});
    }
    sms_events_.clear();
    if (sms_.empty() || blocking(m)) return;

    const UiFont* f = ui.font("Goudy14(22) white");
    const float w = f ? float(f->metrics.measure(sms_.front().text)) : 0.0f;
    sms_time_ += ui.dt();
    const float t = sms_time_;
    float text_x = -w, bar_x = -w, text_a = 0.0f, bar_a = 0.0f;
    if (sms_state_ == 0) {  // states 0-2
        text_x = bar_x = -w + ui_slide(t, 0.0f, w, 2700.0f, -1000.0f);
        text_a = bar_a = ui_fade(t, 0.0f, 1.5f);
        if (text_a >= 1.0f && text_x >= 0.0f) sms_state_ = 1, sms_time_ = 0.0f;
    } else if (sms_state_ == 1) {  // state 3
        text_x = bar_x = 0.0f, text_a = bar_a = 1.0f;
        if (t >= 3.0f) sms_state_ = 2, sms_time_ = 0.0f;
    } else {  // states 4-5
        const float bar_t = ui_slide_time(0.2f * w, 0.0f, 1600.0f);
        text_x = -ui_slide(t, 0.0f, w, 0.0f, 1600.0f), text_a = 1.0f - ui_fade(t, 0.0f, 1.5f);
        bar_x = -ui_slide(t, bar_t, w, 0.0f, 1600.0f), bar_a = 1.0f - ui_fade(t, bar_t, 1.5f);
        if (bar_x <= -w && text_x <= -w) {  // FUN_006e5d00: next line
            sms_.pop_front(), sms_state_ = 0, sms_time_ = 0.0f;
            return;
        }
    }
    ui.frame(Anchor::BottomLeft);
    ui.sprite("QSAVE", "qlc_bar", {bar_x, 0x204}, {1, 1, 1, bar_a});
    if (f) ui.text(f, sms_.front().text, {text_x, 0x206}, ui_rgba(0, 0, 0, int(255.0f * text_a)));
}

}  // namespace joa::engine
