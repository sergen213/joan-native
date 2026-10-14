// In-game menu (interfac/igamemenu: init FUN_006f2d70, layout FUN_006f43e0, input FUN_006f3c90, button draw
// FUN_006f4c40) and the INGAME_B backdrop shared by every menu opened during a mission (FUN_006f1940 init,
// FUN_006f24d0 slide-in, FUN_006f26e0 slide-out, FUN_006f21c0 draw order). Confirm dialogs: G_POPUP box
// FUN_006d3cb0.
#include "engine/menu_options_store.hpp"
#include "engine/menu_saveload.hpp"
#include "engine/ui_screens.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace joa::engine {

float ui_slide_time(float d, float v0, float a) { return a == 0.0f ? d / v0 : (v0 - std::sqrt(std::max(v0 * v0 + 2.0f * a * d, 0.0f))) / -a; }

float ui_slide(float t, float delay, float dist, float v0, float a) {
    float s = std::max(t - delay, 0.0f);
    if (a < 0.0f) s = std::min(s, v0 / -a);  // velocity reaches 0: the widget stops
    return std::min(v0 * s + 0.5f * a * s * s, dist);
}

float ui_fade(float t, float delay, float rate) { return std::clamp((t - delay) * rate, 0.0f, 1.0f); }

void menu_heading_bars(Ui& ui, int lc, std::string_view heading, float top_x, float bottom_x) {
    ui.sprite("COMMELE2", "longbar", {top_x, float(180 + lc)});
    ui_text_in_rect(ui, ui.font("Goudy_bold_18"), heading, {int(top_x), 180 + lc, int(top_x) + 1024, 210 + lc}, kUiWhite);
    ui.sprite("COMMELE2", "longbar", {bottom_x, float(712 + lc)});
}

float ingame_backdrop_time(const MenuModel& m) { return m.ingame_backdrop_closed ? m.time : 1e9f; }

// FUN_006f21c0 draws layer 10/5 parts (woodtex, sphere = 25% white/black shading, ring, left/right halves) and
// layer 1 parts (titlebar stone band, "In-Game Menu" title bar = title + Goudy14(22) label + arrows placed
// by FUN_006f2ac0, top, bottom). en2d layer 1 is in front of the menu pages (in-game menu = layer 5,
// savemenu = layer 2): s04_07_ingame_menu shows the bottom stone over the BACK longbar.
// Slide-in FUN_006f24d0: left/right 5120 px/s after 100 ms, top 920 px/s, bottom 320 px/s; woodtex/sphere/ring
// fade at 5/s, title parts at 10/s after 200 ms. Slide-out FUN_006f26e0: everything back after 100 ms except
// the title parts (fade out at 10/s at once).
void ingame_backdrop_back(Ui& ui, float t, float e) {
    const bool out = e >= 0.0f;
    const float side = out ? 512.0f - ui_slide(e, 0.1f, 512, 5120, 0) : ui_slide(t, 0.1f, 512, 5120, 0);
    const glm::vec4 c{1, 1, 1, out ? 1.0f - ui_fade(e, 0.1f, 5) : ui_fade(t, 0, 5)};
    ui.sprite("INGAME_B", "woodtex", {0, 0}, c);
    ui.sprite("INGAME_B", "sphere", {40, 24}, c);
    ui.sprite("INGAME_B", "ring", {0, 0}, c);
    ui.sprite("INGAME_B", "left", {side - 512, 0});
    ui.sprite("INGAME_B", "right", {1024 - side, 0});
}

void ingame_backdrop_front(Ui& ui, float t, float e, std::string_view label) {
    const bool out = e >= 0.0f;
    const glm::vec4 c{1, 1, 1, out ? 1.0f - ui_fade(e, 0, 10) : ui_fade(t, 0.2f, 10)};
    ui.sprite("INGAME_B", "titlebar", {0, 0}, c);
    ui.sprite("INGAME_B", "title", {312, 48}, c);
    const UiFont* f = ui.font("Goudy14(22) white");
    if (label.empty()) label = ui.text("IGAMEMNU", 15);
    const int tw = f ? f->metrics.measure(label) : 0;
    ui_text_in_rect(ui, f, label, {312, 50, 708, 68}, c);
    ui.sprite("INGAME_B", "l_arrow", {float((396 - tw) / 2 - 68 + 310), 54}, c);
    ui.sprite("INGAME_B", "r_arrow", {float((396 + tw) / 2 + 314), 54}, c);
    const float top = out ? -ui_slide(e, 0.1f, 184, 920, 0) : ui_slide(t, 0, 184, 920, 0) - 184;
    const float bottom = out ? 704 + ui_slide(e, 0.1f, 64, 320, 0) : 768 - ui_slide(t, 0, 64, 320, 0);
    ui.sprite("INGAME_B", "top", {0, top});
    ui.sprite("INGAME_B", "bottom", {0, bottom});
}

void menu_ingame_backdrop(Ui& ui, const MenuModel& m, std::string_view heading) {
    const float t = ingame_backdrop_time(m);
    ingame_backdrop_back(ui, t);
    if (!heading.empty()) menu_heading_bars(ui, -48, heading);  // page layer, behind the layer-1 front parts
    ingame_backdrop_front(ui, t);
}

namespace {
// Key shown in "Load Game (%1s)" etc.: KeyMap_get_name(map, DEFAULTK record) of the current scheme.
std::string key_name(Ui& ui, const MenuModel& m, int action) { return action_key_name(ui.vfs(), m, action); }

std::string format1(std::string_view fmt, std::string_view arg) {
    std::string s(fmt);
    if (auto p = s.find("%1s"); p != std::string::npos) s.replace(p, 3, arg);
    return s;
}

// 0x633500 with a clip extent: the sprite repeats from (x, y) and the last copy is cut at w x h (no stretch).
void tile(Ui& ui, const UiSprite* s, glm::vec4 r) {
    if (!s) return;
    for (float oy = 0; oy < r.w; oy += s->size.y)
        for (float ox = 0; ox < r.z; ox += s->size.x) {
            const glm::vec2 part{std::min(s->size.x, r.z - ox), std::min(s->size.y, r.w - oy)};
            const glm::vec2 uv1 = s->uv0 + (s->uv1 - s->uv0) * part / s->size;
            ui.image(s->texture, {r.x + ox, r.y + oy, part.x, part.y}, s->uv0, uv1);
        }
}

// In-game menu slide (FUN_006f43e0 in, FUN_006f38a0 out): row i 600 px from above, in-delay 44 ms * (8 - i),
// out-delay 44 ms * i; FRAME in at once / out after 352 ms; LONGBAR from x 1024 (out after 150 ms);
// BACK fades in at 6/s after 200 ms, out at 6/s.
constexpr float kRowOut = 0.044f;
float ingame_menu_out_time() { return std::max(8 * kRowOut + ui_slide_time(600), 0.15f + ui_slide_time(1024)); }
}  // namespace

int ui_confirm_popup(Ui& ui, std::string_view body) {
    const UiFont* f = ui.font("Goudy14(22) white");
    if (!f) return 0;
    const std::vector<std::string> lines = wrap_text(f->metrics, body, 361);
    const int lh = f->metrics.line_height(), n = int(lines.size());
    const int w = 456;                                                // 0x1c8: no title, preset width -1
    const int h = std::max(0x60, 0x30 + n * lh - 2 + n * 2 + 0x14 + 0x24);
    const int x = (1024 - w) / 2, y = (768 - h) / 2;
    const float fx = float(x), fy = float(y), fw = float(w), fh = float(h);
    auto sp = [&](const char* name) { return ui.sprite("GPOPUP", name); };
    tile(ui, sp("frame_tl"), {fx, fy, 48, 48});
    tile(ui, sp("frame_t"), {fx + 48, fy, fw - 96, 48});
    tile(ui, sp("frame_tr"), {fx + fw - 48, fy, 48, 48});
    tile(ui, sp("frame_l"), {fx, fy + 48, 48, fh - 96});
    tile(ui, sp("frame_c"), {fx + 48, fy + 48, fw - 96, fh - 96});
    tile(ui, sp("frame_r"), {fx + fw - 48, fy + 48, 48, fh - 96});
    tile(ui, sp("frame_bl"), {fx, fy + fh - 48, 48, 48});
    tile(ui, sp("frame_b"), {fx + 48, fy + fh - 48, fw - 96, 48});
    tile(ui, sp("frame_br"), {fx + fw - 48, fy + fh - 48, 48, 48});
    for (int i = 0; i < n; ++i) ui.text(f, lines[i], {x + w / 2.0f, float(y + 0x18 + i * (lh + 2))}, kUiWhite, TextAlign::Center);
    int result = 0;
    for (int i = 0; i < 2; ++i) {
        const int bx = (-0xbd * 2 + 0x1d9) / 2 + 0x11c + i * 0xbd, by = y + h - 0x3c;
        const glm::vec4 r{float(bx), float(by), 172, 36};
        const bool hot = ui.hover(r);
        ui.sprite("GPOPUP", hot && ui.input().mouse_down ? "b_dw" : hot ? "b_ov" : "b_ot", {r.x, r.y});
        ui_text_in_rect(ui, f, ui.text("G_POPUP", i + 1), {bx, by, bx + 171, by + 35}, kUiWhite);
        if (ui.clicked(r)) result = i + 1;
    }
    if (ui.key(SDL_SCANCODE_RETURN) || ui.key(SDL_SCANCODE_KP_ENTER)) result = 1;
    if (ui.key(SDL_SCANCODE_ESCAPE)) result = 2;
    return result;
}

void menu_ingame(Ui& ui, MenuModel& m, std::vector<UiCommand>& out) {
    const float bt = ingame_backdrop_time(m);
    if (m.ingame_backdrop_closed && bt >= kIngameBackdropIn) m.ingame_backdrop_closed = false;
    const bool exiting = m.ingame_exit >= 0;
    const float e = exiting ? m.time - m.ingame_exit_time : -1.0f;
    const float out_end = ingame_menu_out_time();
    const float be = exiting && m.ingame_exit == 8 ? e - out_end : -1.0f;  // backdrop slide-out (resume only)

    if (exiting && e >= out_end && (m.ingame_exit != 8 || be >= kIngameBackdropOut)) {
        // FUN_006f43e0 close: DAT_00a52934 picks the next page (4 load, 3 save, 6 brief, 7 log, 5 options).
        const int item = m.ingame_exit;
        m.ingame_exit = -1;
        switch (item) {
            case 0: m.previous = MenuScreen::InGame, m.screen = MenuScreen::Load, m.save_selected = -1, m.save_state = 0; break;
            case 1: m.previous = MenuScreen::InGame, m.screen = MenuScreen::Save, m.save_selected = -1, m.save_state = 0; break;
            case 2: out.push_back({UiCommand::Type::MissionBrief}); break;
            case 3: out.push_back({UiCommand::Type::MessageLog}); break;
            case 5: m.previous = MenuScreen::InGame, m.screen = MenuScreen::Options, m.options_tab = OptionsTab::Root; break;
            default: m.ingame_backdrop_closed = true, out.push_back({UiCommand::Type::Resume}); break;
        }
        return;
    }

    ingame_backdrop_back(ui, bt, be);
    if (m.ingame_confirm) {
        ingame_backdrop_front(ui, bt);
        // FUN_006f3c90: body = IGAMEMNU 12 (restart) or 10 (quit), "\n", 11.
        const std::string body = std::string(ui.text("IGAMEMNU", m.ingame_confirm == 3 ? 12 : 10)) + "\n" +
                                 std::string(ui.text("IGAMEMNU", 11));
        if (const int r = ui_confirm_popup(ui, body)) {
            if (r == 1) {
                out.push_back({m.ingame_confirm == 3   ? UiCommand::Type::RestartMission
                               : m.ingame_confirm == 2 ? UiCommand::Type::QuitToMain
                                                       : UiCommand::Type::Quit});
                m.ingame_backdrop_closed = true;  // FUN_006f4a10 + FUN_006f28c0: instant reset
            }
            m.ingame_confirm = 0;
        }
        return;
    }
    const float t = m.time;
    const bool live = !exiting && t >= 8 * kRowOut + ui_slide_time(600);  // input once every row has landed
    auto row_y = [&](int i) {
        const float settled = float(114 + 68 * i);
        return exiting ? settled - ui_slide(e, kRowOut * float(i), 600) : settled - 600 + ui_slide(t, kRowOut * float(8 - i), 600);
    };
    const float frame_y = exiting ? 110 - ui_slide(e, 8 * kRowOut, 600) : -490 + ui_slide(t, 0, 600);
    ui.sprite("COMMELE2", "frame", {292, frame_y});
    // Final slide targets (FUN_006f43e0): x 256, y 114 + 68 i, 512x64; label raised 2 px, black normally /
    // white when hovered or pressed (FUN_006f4c40, CM_OT/CM_OV/CM_DW). Font: decomp pairs 0xa489c8 with
    // "Goudy12(18) black", but the reference glyphs match Goudy14(22) metrics exactly (measured).
    // Label IGAMEMNU n, %1s = key of DEFAULTK record `action` (0x6f3330: 0x26 load, 0x25 save, 0x28 brief,
    // 0x29 message log, 0x27 options).
    struct Item { int text, action; };
    static constexpr Item kItems[8] = {{1, 38}, {2, 37}, {3, 40}, {13, 41}, {4, 0}, {5, 39}, {6, 0}, {7, 0}};
    const UiFont* f = ui.font("Goudy14(22) white");
    int hit = -1;
    for (int i = 0; i < 8; ++i) {
        const float y = std::round(row_y(i));
        const glm::vec4 r{256, y, 512, 64};
        const bool hot = live && ui.hover(r), down = hot && ui.input().mouse_down;
        ui.sprite("COMMELE2", down ? "cm_dw" : hot ? "cm_ov" : "cm_ot", {256, y});
        const Item& it = kItems[i];
        const std::string label = it.action ? format1(ui.text("IGAMEMNU", it.text), key_name(ui, m, it.action))
                                            : std::string(ui.text("IGAMEMNU", it.text));
        ui_text_in_rect(ui, f, label, {256, int(y) - 2, 767, int(y) + 61}, hot ? kUiWhite : ui_rgba(0, 0, 0));
        if (hot && ui.clicked(r)) hit = i;
    }
    ui.sprite("COMMELE2", "longbar", {exiting ? ui_slide(e, 0.15f, 1024) : 1024 - ui_slide(t, 0, 1024), 712});
    const float back_alpha = exiting ? 1.0f - ui_fade(e, 0, 6) : ui_fade(t, 0.2f, 6);
    const bool back = ui_text_button(ui, ui.font("Goudy14(22) white"), ui.text("IGAMEMNU", 14), {462, 712, 561, 742}, back_alpha);
    ingame_backdrop_front(ui, bt, be);
    if (!live) return;
    if (hit == 4 || hit == 6 || hit == 7) m.ingame_confirm = hit == 4 ? 3 : hit == 6 ? 2 : 1;  // popup, no slide
    else if (hit >= 0) m.ingame_exit = hit, m.ingame_exit_time = t;
    else if (back || ui.key(SDL_SCANCODE_ESCAPE)) m.ingame_exit = 8, m.ingame_exit_time = t;
}

}  // namespace joa::engine
