// LOAD A SAVED GAME / SAVE A GAME (interfac/savemenu). Positions are the final slide-in targets of
// FUN_00751750 (+ FUN_0074cb10 init), lc = -48 in game; draw order FUN_0074e170; input FUN_0074f230;
// slot draw FUN_007538b0, text buttons FUN_00754220, FILE INFORMATION bar FUN_00753440.
#include "engine/menu_saveload.hpp"
#include "engine/ui_screens.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace joa::engine {

void ui_text_in_rect(Ui& ui, const UiFont* f, std::string_view s, glm::ivec4 r, glm::vec4 color) {
    if (!f) return;
    const int x = std::max(r.x + ((r.z - r.x + 1) - f->metrics.measure(s)) / 2, r.x);
    const int y = r.y + ((r.w - r.y + 1) - f->metrics.line_height()) / 2;
    ui.text(f, s, {float(x), float(y)}, color, TextAlign::Left, 1.0f, TextOp::Diffuse);  // 0x626130 selectors (0,1,2,0)
}

namespace {
const glm::vec4 kBlack = ui_rgba(0, 0, 0);

// Thumbnail of a save: Save/<stem>.BMP next to an original .SAV (game path), <stem>.bmp next to a native .jsav
// (absolute path, Session::save).
std::string thumbnail(const SaveSlot& s) {
    std::filesystem::path p(s.file);
    if (p.extension() == ".jsav") return p.replace_extension(".bmp").string();
    return "Save/" + p.stem().string() + ".BMP";
}

// MARK diamonds left/right of a centred label of width w (0x6f5200 / 0x754220 / 0x7538b0), y = y1 + 8.
void marks(Ui& ui, glm::ivec4 r, int w) {
    ui.sprite("COMMELE2", "mark", {float((r.x + r.z - w) / 2 - 0x14), float(r.y + 8)});
    ui.sprite("COMMELE2", "mark", {float((r.x + r.z + w) / 2 + 6), float(r.y + 8)});
}

glm::vec4 hit_rect(glm::ivec4 r) { return {float(r.x), float(r.y), float(r.z - r.x + 1), float(r.w - r.y + 1)}; }

std::string page_text(std::string_view fmt, int page, int pages) {
    std::string s(fmt);
    if (auto p = s.find("%1d"); p != std::string::npos) s.replace(p, 3, std::to_string(page));
    if (auto p = s.find("%2d"); p != std::string::npos) s.replace(p, 3, std::to_string(pages));
    return s;
}

// Unicode -> Windows-1252 byte, 0 = not representable (0x80..0x9f are the cp1252 specials, the rest is Latin-1).
unsigned char cp1252(char32_t c) {
    static constexpr char16_t k80[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                         0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                         0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    if (c < 0x80 || (c >= 0xa0 && c < 0x100)) return static_cast<unsigned char>(c);
    for (int i = 0; i < 32; ++i)
        if (k80[i] && k80[i] == c) return static_cast<unsigned char>(0x80 + i);
    return 0;
}

// UTF-8 (SDL text input) -> cp1252 bytes; unrepresentable characters are dropped.
std::string to_cp1252(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i++]);
        const int n = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : b >= 0xc0 ? 1 : 0;
        char32_t c = n ? b & (0x3f >> n) : b;
        for (int k = 0; k < n && i < s.size(); ++k) c = (c << 6) | (static_cast<unsigned char>(s[i++]) & 0x3f);
        if (b >= 0x80 && b < 0xc0) continue;  // stray continuation byte
        if (const unsigned char ch = cp1252(c)) out += char(ch);
    }
    return out;
}

// Description text box, en2d txtbox2d as the Save screen sets it up (FUN_0074cb10: FUN_0062d4f0(0x11e, y, 0x2e2,
// obj+0x449d, 0x40, Goudy14(22) white, left aligned), moved to y 0x29f + lc on entry): keys FUN_0062dd10, mouse
// FUN_0062e490, draw FUN_0062d730. Characters 0x20..0xff except '~' insert at the caret (replacing the selection)
// while the text stays <= 63 bytes and narrower than the box (453 px); Backspace/Delete/Left/Right/Home/End edit;
// a press in the box places the caret (largest index whose x <= mouse, 0x62e9a0) and dragging selects (+0x84).
// Caret: 1 px line x1 + width(text[0..caret]) from y1 to y2 = y1 - 1 + line height, blinking with a 400 ms period
// (visible in its second half), restarted visible by every handled key (Misc_get_time - 200). Selection: line at y2.
void description_box(Ui& ui, MenuModel& m, const UiFont* f, int lc, float t) {
    if (!f) return;
    std::string& s = m.save_description;
    const int x1 = 286, x2 = 738, y1 = 671 + lc, y2 = y1 - 1 + f->metrics.line_height();
    const auto x_at = [&](int i) { return x1 + f->metrics.measure(std::string_view(s).substr(0, std::size_t(i))); };
    const auto caret_at = [&](float mx) {
        int i = int(s.size());
        while (i > 0 && float(x_at(i)) > mx) --i;
        return i;
    };
    int& caret = m.save_caret;
    int& anchor = m.save_anchor;
    caret = std::min(caret, int(s.size())), anchor = std::min(anchor, int(s.size()));  // 0x62ea70
    const auto erase_selection = [&] {
        const int lo = std::min(caret, anchor), hi = std::max(caret, anchor);
        s.erase(std::size_t(lo), std::size_t(hi - lo));
        caret = anchor = lo;
    };
    bool keyed = false;
    for (const char c : to_cp1252(ui.input().text)) {
        keyed = true;
        if (static_cast<unsigned char>(c) < 0x20 || c == '~' || s.size() >= 63) continue;
        const int lo = std::min(caret, anchor), hi = std::max(caret, anchor);
        std::string next = s.substr(0, std::size_t(lo)) + c + s.substr(std::size_t(hi));
        if (f->metrics.measure(next) < x2 - x1 + 1) s = std::move(next), caret = anchor = lo + 1;
    }
    switch (ui.input().key_pressed) {
        case SDL_SCANCODE_BACKSPACE:
            if (caret != anchor) erase_selection();
            else if (caret > 0) s.erase(std::size_t(--caret), 1), anchor = caret;
            keyed = true;
            break;
        case SDL_SCANCODE_DELETE:
            if (caret != anchor) erase_selection();
            else if (caret < int(s.size())) s.erase(std::size_t(caret), 1);
            keyed = true;
            break;
        case SDL_SCANCODE_LEFT: caret = anchor = std::max(caret - 1, 0), keyed = true; break;
        case SDL_SCANCODE_RIGHT: caret = anchor = std::min(caret + 1, int(s.size())), keyed = true; break;
        case SDL_SCANCODE_HOME: caret = anchor = 0, keyed = true; break;
        case SDL_SCANCODE_END: caret = anchor = int(s.size()), keyed = true; break;
        default: break;
    }
    const glm::vec4 box{float(x1), float(y1), float(x2 - x1 + 1), float(y2 - y1 + 1)};
    if (m.save_drag && !ui.input().mouse_down) m.save_drag = false;
    if (m.save_drag) caret = caret_at(ui.mouse().x);
    else if (ui.pressed(box)) caret = anchor = caret_at(ui.mouse().x), m.save_drag = true, keyed = true;
    if (keyed) m.save_blink = t - 0.2f;

    ui.text(f, s, {float(x1), float(y1)}, kUiWhite);
    if (std::fmod(t - m.save_blink, 0.4f) >= 0.2f)
        if (const int x = x_at(caret); x <= x2) ui.fill({float(x), float(y1), 1, float(y2 - y1 + 1)}, kUiWhite);
    if (caret != anchor) {
        const int a = x_at(std::min(caret, anchor)), b = x_at(std::max(caret, anchor));
        ui.fill({float(a), float(y2), float(b - a + 1), 1}, kUiWhite);
    }
}
}  // namespace

bool ui_text_button(Ui& ui, const UiFont* f, std::string_view s, glm::ivec4 r, float alpha) {
    if (!f || alpha <= 0.0f) return false;
    const glm::vec4 h = hit_rect(r);
    const bool live = alpha >= 1.0f, hot = live && ui.hover(h), down = hot && ui.input().mouse_down;
    if (hot) marks(ui, r, f->metrics.measure(s));
    ui_text_in_rect(ui, f, s, {r.x, r.y - 1, r.z, r.w - 1}, down ? kBlack : glm::vec4(1, 1, 1, alpha));
    return live && ui.clicked(h);
}

namespace {
// Slide-in chain of FUN_00751750 (each step waits for the previous widget's position), as start times:
// FRAME/LONGBARs at 0; slot 9 when FRAME is within 4 px of its target; slot i-1 when slot i has covered half
// its 230 + 40 i px; PIC with slot 5, INFOBAR/INFO with slot 1; page bar when slot 0 has covered 115 px;
// settled (state 1: input, file info, buttons fade in at 6/s) once page bar and INFOBAR have landed and faded.
struct SaveTimeline {
    float slot[10]{}, page = 0, settled = 0;
};
SaveTimeline save_timeline() {
    SaveTimeline s;
    s.slot[9] = ui_slide_time(796);
    for (int i = 9; i > 0; --i) s.slot[i - 1] = s.slot[i] + ui_slide_time((230.0f + 40.0f * float(i)) / 2.0f);
    s.page = s.slot[0] + ui_slide_time(115);
    s.settled = std::max(s.page + std::max(ui_slide_time(174), 1.0f / 12.0f), s.slot[1] + std::max(ui_slide_time(200), 0.25f));
    return s;
}
// Slide-out FUN_0074dbc0: buttons fade at 6/s; page bar up after 50 ms, slot i after 100 + 50 i ms (fade 12/s);
// INFOBAR/INFO with slot 6, PIC with slot 2 (fade 4/s); FRAME and LONGBARs after 600 ms.
constexpr float kSaveOutEnd = 0.6f;
}  // namespace

void menu_saveload(Ui& ui, MenuModel& m, std::vector<UiCommand>& out) {
    const bool load = m.screen == MenuScreen::Load;
    const int lc = m.in_game ? -48 : 0;
    const float t = m.time;
    const bool exiting = m.save_exit_time >= 0.0f;
    const float e = exiting ? t - m.save_exit_time : -1.0f;
    if (exiting && e >= kSaveOutEnd + ui_slide_time(1024)) {  // state 3: close (back to the in-game menu / main)
        m.save_exit_time = -1.0f;
        m.screen = m.previous;
        return;
    }
    const float bt = ingame_backdrop_time(m);
    if (m.in_game && m.ingame_backdrop_closed && bt >= kIngameBackdropIn) m.ingame_backdrop_closed = false;
    if (m.in_game) ingame_backdrop_back(ui, bt);
    else menu_frontend_backdrop(ui, m);

    const SaveTimeline tl = save_timeline();
    if (!exiting) ui.sound_at("BUTTON_SLIDE_MANY", t, 0.0f);  // FUN_00751750: first slide step
    const bool live = !exiting && t >= tl.settled;  // FUN_0074f230 acts only in state 1 (obj+0x4b02)
    // Distance along a slide of `dist` px: in = start time on the chain, out = delay after the exit click.
    auto pos = [&](float in_start, float out_delay, float dist) {
        return exiting ? dist - ui_slide(e, out_delay, dist) : ui_slide(t, in_start, dist);
    };
    auto alpha = [&](float in_start, float out_delay, float rate) {
        return exiting ? 1.0f - ui_fade(e, out_delay, rate) : ui_fade(t, in_start, rate);
    };

    const int count = int(m.saves.size());
    const int pages = (count + 9) / 10;
    m.save_page = std::clamp(m.save_page, 0, std::max(pages - 1, 0));
    if (m.save_selected >= count) m.save_selected = -1;
    const SaveSlot* sel = m.save_selected >= 0 ? &m.saves[std::size_t(m.save_selected)] : nullptr;
    const UiFont* f14 = ui.font("Goudy14(22) white");
    const UiFont* label = ui.font("Goudy12 Bold Black");
    const glm::vec4 wh = kUiWhite;

    ui.sprite("COMMELE2", "frame", {128, std::round(float(152 + lc) - 800 + pos(0, kSaveOutEnd, 800))});
    // 10 slots: x 116..580, y 300 + 40 i (+lc), from y 70 + lc; SLOT_OT / SLOT_OV (hover or selected) / SLOT_DW;
    // label Goudy14(22) raised 2 px, black, white when hovered/selected (selected also gets MARKs).
    int slot_hit = -1;
    for (int i = 0; i < 10; ++i) {
        const int idx = m.save_page * 10 + i;
        const float out_delay = 0.1f + 0.05f * float(i);
        const int y = int(std::round(70.0f + float(lc) + pos(tl.slot[i], out_delay, 230.0f + 40.0f * float(i))));
        const glm::vec4 c{1, 1, 1, alpha(tl.slot[i], out_delay, 12)};
        const glm::ivec4 r{116, y, 580, y + 36};
        const bool hot = live && m.save_state == 0 && ui.hover(hit_rect(r)), down = hot && ui.input().mouse_down;
        const bool selected = idx == m.save_selected;
        ui.sprite("COMMELE2", down ? "slot_dw" : hot || selected ? "slot_ov" : "slot_ot", {116, float(y)}, c);
        if (idx < count) {
            const std::string_view d = m.saves[std::size_t(idx)].description;
            if (selected && live) marks(ui, r, f14 ? f14->metrics.measure(d) : 0);
            ui_text_in_rect(ui, f14, d, {r.x, r.y - 2, r.z, r.w - 2}, (hot && !down) || selected ? c : glm::vec4(0, 0, 0, c.a));
        }
        if (hot && ui.clicked(hit_rect(r))) slot_hit = i;
    }

    // Page widget FUN_006b29b0 type 2, from (116, 62 + lc) to (116, 236 + lc): page_ot 464x52, arrows at
    // +0x23 / +0x186, +11.
    const int py = int(std::round(62.0f + float(lc) + pos(tl.page, 0.05f, 174)));
    const glm::vec4 pc{1, 1, 1, alpha(tl.page, 0.05f, 12)};
    ui.sprite("COMMELE2", "page_ot", {116, float(py)}, pc);
    int page_step = 0;
    for (int side = 0; side < 2; ++side) {
        const glm::vec4 r{side ? 506.0f : 151.0f, float(py + 11), 36, 28};
        const bool hot = live && m.save_state == 0 && ui.hover(r);
        const std::string name = std::string(side ? "aw_r_" : "aw_l_") + (hot && ui.input().mouse_down ? "dw" : hot ? "ov" : "ot");
        ui.sprite("COMMELE2", name, {r.x, r.y}, pc);
        if (hot && ui.clicked(r)) page_step = side ? 1 : -1;
    }
    if (!page_step && live && m.save_state == 0) page_step = ui.nav_page();  // GamepadPlay: LB / RB turn the page
    // Label 5 px above the page_ot centre (measured in s04_07_save_menu / s04_03_load_campaign).
    ui_text_in_rect(ui, f14, page_text(ui.text("SAVEMENU", 22), pages ? m.save_page + 1 : 0, pages), {116, py - 5, 580, py + 47}, pc);

    // PIC from y -168 + lc, INFOBAR from 480 + lc, INFO from 360 + lc.
    ui.sprite("SAVEMENU", "pic", {580, std::round(float(232 + lc) - 400 + pos(tl.slot[5], 0.2f, 400))}, {1, 1, 1, alpha(tl.slot[5], 0.2f, 4)});
    if (sel && live)
        if (TextureHandle tex = ui.bitmap(thumbnail(*sel)))  // thumbnail: 320x240 in a 512x256 BMP
            ui.image(tex, {601, float(253 + lc), 320, 240}, {0, 0}, {320.0f / 512.0f, 240.0f / 256.0f});
    // FILE INFORMATION bar (0x753440): INFOBAR + SAVEMENU 23 + DIAMONDs at y + 8.
    const int iy = int(std::round(480.0f + float(lc) + pos(tl.slot[1], 0.4f, 40)));
    const glm::vec4 ic{1, 1, 1, alpha(tl.slot[1], 0.4f, 4)};
    ui.sprite("SAVEMENU", "infobar", {612, float(iy)}, ic);
    const std::string_view info = ui.text("SAVEMENU", 23);
    const int iw = label ? label->metrics.measure(info) : 0;
    ui.sprite("SAVEMENU", "diamond", {float((612 + 911 - iw) / 2 - 28), float(iy + 8)}, ic);  // measured in s04_07_save_menu
    ui.sprite("SAVEMENU", "diamond", {float((612 + 911 + iw) / 2 + 8), float(iy + 8)}, ic);
    ui_text_in_rect(ui, label, info, {612, iy - 2, 911, iy + 25}, ic);
    ui.sprite("SAVEMENU", "info", {612, std::round(float(360 + lc) + pos(tl.slot[1], 0.4f, 200))}, ic);
    if (sel && live) {
        // FUN_0074e170 (original pairing kept: "SAVED TIME:" above the date, "SAVED DATE:" above the time).
        ui_text_in_rect(ui, f14, sel->time, {612, 614 + lc, 912, 642 + lc}, wh);
        ui_text_in_rect(ui, label, ui.text("SAVEMENU", 24), {612, 600 + lc, 912, 628 + lc}, wh);
        ui_text_in_rect(ui, f14, sel->date, {612, 574 + lc, 912, 602 + lc}, wh);
        ui_text_in_rect(ui, label, ui.text("SAVEMENU", 25), {612, 560 + lc, 912, 588 + lc}, wh);
        const int mission = sel->mission >= 1 && sel->mission <= 8 ? sel->mission : 0;
        ui.text_block(f14, ui.text("SAVEMENU", 10 + mission), {612, float(657 + lc), 301, 57}, wh, 0, TextAlign::Center);
        ui_text_in_rect(ui, label, ui.text("SAVEMENU", 21), {612, 640 + lc, 912, 668 + lc}, wh);
    }

    const float bars = std::round(pos(0, kSaveOutEnd, 1024));  // top LONGBAR from x -1024, bottom from 1024
    menu_heading_bars(ui, lc, ui.text("SAVEMENU", load ? 20 : 6), bars - 1024, 1024 - bars);

    // Bottom buttons by state (FUN_0074cb10 rects), fading in at 6/s once settled. Init passes 0xa48d48
    // (Goudy_bold_18) but the reference labels match Goudy14(22) metrics exactly (measured, s04_07_save_menu).
    const UiFont* fb = ui.font("Goudy14(22) white");
    const int by = 712 + lc;
    const float ba = exiting ? 1.0f - ui_fade(e, 0, 6) : ui_fade(t, tl.settled, 6);
    auto button = [&](int text, int x1) { return ui_text_button(ui, fb, ui.text("SAVEMENU", text), {x1, by, x1 + 150, by + 30}, ba); };
    const bool enter = live && (ui.key(SDL_SCANCODE_RETURN) || ui.key(SDL_SCANCODE_KP_ENTER));
    const bool escape = live && ui.shortcut(SDL_SCANCODE_ESCAPE);  // CANCEL / BACK key
    if (m.save_state == 2) {
        // Description entry (IN_FRAME / IN_BG moved to y 0x294 / 0x29e + lc, text box to 0x29f + lc).
        ui.sprite("SAVEMENU", "in_frame", {228, float(660 + lc)});
        ui.sprite("SAVEMENU", "in_bg", {286, float(670 + lc)});
        if (live) description_box(ui, m, f14, lc, t);
        else ui.text(f14, m.save_description, {286, float(671 + lc)}, wh);
    }
    if (m.save_state != 0) {
        if (button(4, 262) || enter) {
            if (m.save_state == 1) out.push_back({UiCommand::Type::DeleteSave, 0, sel->file, {}});
            else out.push_back({UiCommand::Type::Save, 0, sel ? sel->file : std::string(), m.save_description});
            m.save_state = 0, m.save_selected = -1;
        } else if (button(5, 612) || escape) {
            m.save_state = 0;
        }
    } else if (sel) {
        if (button(load ? 2 : 1, 237)) {
            if (load) out.push_back({UiCommand::Type::Load, 0, sel->file, {}});
            else m.save_state = 2, m.save_description = sel->description, m.save_blink = t - 0.2f;
        } else if (button(3, 437)) {
            m.save_state = 1;
        } else if (button(5, 637) || escape) {
            m.save_selected = -1;
        }
    } else {
        const bool save_new = !load && button(9, 262);
        if (save_new) m.save_state = 2, m.save_description.clear(), m.save_blink = t - 0.2f;
        else if (button(19, load ? 437 : 612) || escape) m.save_exit_time = t, ui.sound("BUTTON_SLIDE_MANY");  // slide out (FUN_0074dbc0), then back
    }
    if (m.in_game) {
        ingame_backdrop_front(ui, bt);
    } else {
        // Front-end layers drawn after the page (FUN_0070a8b0, see menu_main.cpp).
        ui.sprite("MENU1024", "alab_l", {0, 0});
        ui.sprite("MENU1024", "alab_r", {836, 0});
    }

    if (live && m.save_state == 0 && pages) {
        if (page_step) m.save_page = (m.save_page + page_step + pages) % pages, m.save_selected = -1;  // arrows wrap (0x6b4280)
        if (slot_hit >= 0) {
            const int idx = m.save_page * 10 + slot_hit;
            if (idx >= count) {
                m.save_selected = -1;
            } else if (idx == m.save_selected && load && t - m.save_click_time < 0.5f) {
                out.push_back({UiCommand::Type::Load, 0, m.saves[std::size_t(idx)].file, {}});  // double click
            } else {
                if (idx != m.save_selected) ui.bitmap(thumbnail(m.saves[std::size_t(idx)]), true);
                m.save_selected = idx;
            }
            m.save_click_time = t;
        }
    }
}

}  // namespace joa::engine
