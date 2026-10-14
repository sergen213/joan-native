// Options menu (interfac/optmenui.cpp). Owner object init FUN_0073ba80, page switch FUN_0073cc00 (slide
// targets), draw FUN_0073d2c0, input FUN_0073d830. Pages: video 0x748b10/0x74bc50/0x74c2e0/0x74a700, audio
// 0x740440/0x741cd0/0x7420e0/0x7413e0, input 0x7435e0/0x747bc0/0x7480e0/0x745b10, remap 0x748540/0x748830,
// game 0x742470/0x743170/0x7433d0/0x742c80. lc = -48 in game (DAT_00a500c2 == 9).
// Widgets: big button 0x73ee40 (COMMELE3 cs_*), row label 0x73e990 (MENU_B s_ot), slider 0x760050/0x760b70,
// combo type 2 0x6b29b0/0x6b3ec0 (COMMELE2 page_*/aw_*), text button 0x73e130, remap rows 0x73f490/0x73fde0.
// The OPTMENU/OPTMENUB/OPTMENUF tables are loaded (0x73c740) but none of their records is ever drawn.
// Page switches slide the old page out and the new one in (start = reset positions 0x73ded0/0x74b6b0/...,
// targets/delays = the enter/leave functions); closed form on model.time since the switch.
#include "engine/game_input.hpp"  // joypad_index (pad remap capture)
#include "engine/menu_options_store.hpp"
#include "engine/menu_saveload.hpp"
#include "engine/ui_screens.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace joa::engine {
namespace {

constexpr int kRowY[8] = {0xf3, 0x127, 0x15b, 399, 0x1c3, 0x1f7, 0x22b, 0x25f};  // label y; control y = label - 7
constexpr int kCtrlX = 0x1c0;                                                     // slider / combo x
constexpr int kRemap = 5;                                                         // page id of the remap page

glm::vec4 white(float a) { return {1, 1, 1, a}; }
glm::vec4 black(float a) { return {0, 0, 0, a}; }

// __ftol (truncation) as the page inits convert config floats to slider steps (0x748d8c, double math).
int ftol(double v) { return int(v); }

const UiFont* button_font(Ui& ui) { return ui.font("Goudy14(22) white"); }  // DAT_00a51608

// Widget slide (shared ui_slide = 0x61e020 + 0x61f450: 3500 px/s, -1500 px/s^2, snaps at the target);
// speed 0x477fff00 = instant jump once the delay is over.
glm::vec2 slide(glm::vec2 a, glm::vec2 b, float t, int delay_ms, bool instant) {
    const float delay = float(delay_ms) / 1000.0f, d = glm::length(b - a);
    if (t <= delay || d <= 0) return t <= delay ? a : b;
    const float dist = instant ? d : ui_slide(t, delay, d);
    return dist >= d ? b : glm::round(a + (b - a) * (dist / d));
}

// One page being drawn: entering (slides start -> target) or leaving (target -> start), t = s since the switch.
struct Ctx {
    Ui& ui;
    MenuModel& m;
    std::vector<UiCommand>& out;
    int l;
    float t;
    bool in;
    glm::vec2 pos(glm::vec2 start, glm::vec2 target, int din, int dout, bool instant = false) const {
        return in ? slide(start, target, t, din, instant) : slide(target, start, t, dout, instant);
    }
    // Fade 0x61f1b0 + 0x61f980: alpha towards 1 (enter) / 0 (leave) at `rate` per second after the delay.
    float alpha(int din, int dout, float rate) const {
        const float v = ui_fade(t, float(in ? din : dout) / 1000.0f, rate);
        return in ? v : 1.0f - v;
    }
};

// Draws `set/name` at (x, y) cropped to the 1024x768 canvas: the original screen clips widgets that slide
// partly off it (row strips at x = -100, the FRAME above y = 0), so nothing reaches widescreen pillars.
void sprite_clipped(Ui& ui, const char* set, const char* name, glm::vec2 p, float a = 1.0f) {
    const UiSprite* s = ui.sprite(set, name);
    if (!s || a <= 0) return;
    const glm::vec2 lo = glm::max(p, glm::vec2(0)), hi = glm::min(p + s->size, glm::vec2(1024, 768));
    if (hi.x <= lo.x || hi.y <= lo.y) return;
    const glm::vec2 du = (s->uv1 - s->uv0) / s->size;
    ui.image(s->texture, {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y}, s->uv0 + (lo - p) * du, s->uv0 + (hi - p) * du, white(a));
}

// Text only while fully on the canvas (sliding widgets would otherwise leak into the pillars).
void text_on_canvas(Ui& ui, const UiFont* f, std::string_view s, glm::vec2 p, glm::vec4 color) {
    if (f && p.x >= 0 && p.x + float(f->metrics.measure(s)) <= 1024) ui.text(f, s, p, color, TextAlign::Left, 1.0f, TextOp::Diffuse);
}

// 0x626130 centring inside the inclusive rect.
void text_in_rect(Ui& ui, const UiFont* f, std::string_view s, glm::vec4 r, glm::vec4 color) {
    if (!f) return;
    const int w = f->metrics.measure(s);
    const float x = std::max(r.x, r.x + std::floor((r.z - r.x + 1 - float(w)) / 2));
    text_on_canvas(ui, f, s, {x, r.y + std::floor((r.w - r.y + 1 - float(f->metrics.line_height())) / 2)}, color);
}

// 0x73e990: MENU_B s_ot at (x, y) (x = -100 when settled), label right-aligned to x2 - 40 (0x626080), top y + 8;
// DIAMONDs at (x2 - 35, y + 14) and (x2 - w - 67, y + 14) while the row is highlighted.
void row_label(Ui& ui, std::string_view s, glm::vec2 p, bool hot, float a) {
    sprite_clipped(ui, "MENU_B", "s_ot", p, a);
    const UiFont* f = button_font(ui);
    if (!f) return;
    const int w = f->metrics.measure(s);
    text_on_canvas(ui, f, s, {p.x + 472 - float(w) + 1, p.y + 8}, white(a));
    if (hot) {
        ui.sprite("MENU_B", "diamond", {p.x + 477, p.y + 14}, white(a));
        ui.sprite("MENU_B", "diamond", {p.x + 512 - float(w) - 67, p.y + 14}, white(a));
    }
}

// 0x6211d0 sprite button (ot / ov hover / dw held). Returns true on release inside while `live`; `held` = pressed now.
bool arrow(Ui& ui, const char* base, glm::vec2 p, float a, bool* held = nullptr, bool live = true) {
    const UiSprite* ot = ui.sprite("COMMELE2", std::string(base) + "_ot");
    if (!ot) return false;
    const glm::vec4 r{p.x, p.y, ot->size.x, ot->size.y};
    const bool down = ui.hover(r) && ui.input().mouse_down;
    if (held) *held = down;
    ui.sprite("COMMELE2", std::string(base) + (down ? "_dw" : ui.hover(r) ? "_ov" : "_ot"), p, white(a));
    return live && ui.clicked(r);
}

// Slider 0x760050 (values 0..steps-1): PAGE_OT/PAGE_OV box (464x52), arrows only while highlighted,
// hl_bar cropped to value * 300 / (steps - 1) + 3 px x 25 rows at (+76, +10) (src h 0x18 inclusive, measured),
// hl_mark at (+74 + that, +8). Input 0x760df0: arrow click = 1 step; held >= 300 ms repeats every
// 2000 / (range * 0.4) ms; dragging on the scroll bar (+80, +12)-(+380, +36) sets the value.
int slider(Ctx& c, glm::vec2 p, int v, int steps, bool hot, float a) {
    Ui& ui = c.ui;
    const int range = steps - 1;
    ui.sprite("COMMELE2", hot ? "page_ov" : "page_ot", p, white(a));
    if (hot) {
        const float dt = ui.dt() * 1000.0f, interval = float(ftol(2000.0 / (range * 0.4)));
        for (const auto& [base, x, dir] : {std::tuple{"aw_l", 0x23, -1}, std::tuple{"aw_r", 0x186, 1}}) {
            bool held = false;
            if (arrow(ui, base, p + glm::vec2(x, 0xb), a, &held)) v += dir, c.m.slider_repeat_ms = 0;
            if (!held) continue;
            if (c.m.slider_hold_ms < 300) {
                c.m.slider_hold_ms += dt;
            } else if ((c.m.slider_repeat_ms += dt) >= interval) {
                v += dir;
                c.m.slider_repeat_ms -= interval;
            }
        }
        v += ui.nav_adjust();  // GamepadPlay: pad left/right on the focused row step the slider
        v = std::clamp(v, 0, range);
    }
    if (ui.input().mouse_down && ui.hover({p.x + 0x50, p.y + 0xc, 300, 24}))
        v = std::clamp(int(std::lround((ui.mouse().x - (p.x + 0x50)) * float(range) / 300.0f)), 0, range);
    const int off = v * 300 / range;
    if (const UiSprite* bar = ui.sprite("COMMELE2", "hl_bar")) {
        const glm::vec2 frac{float(off + 3) / bar->size.x, 25.0f / bar->size.y};
        ui.image(bar->texture, {p.x + 0x4c, p.y + 10, float(off + 3), 25}, bar->uv0, bar->uv0 + (bar->uv1 - bar->uv0) * frac, white(a));
    }
    ui.sprite("COMMELE2", "hl_mark", {p.x + 0x4a + float(off), p.y + 8}, white(a));
    return v;
}

// Combo type 2 0x6b29b0: PAGE box with the item centred at (+232 - w/2, +13); arrows cycle (0x6b4280, wraps).
// index is 1-based as the original's 0x80 field.
int combo(Ui& ui, glm::vec2 p, std::initializer_list<std::string_view> items, int index, bool hot, float a) {
    const int n = int(items.size());
    ui.sprite("COMMELE2", hot ? "page_ov" : "page_ot", p, white(a));
    if (arrow(ui, "aw_l", p + glm::vec2(0x23, 0xb), a, nullptr, hot)) index = index < 2 ? n : index - 1;  // hot: not a leaving page
    if (arrow(ui, "aw_r", p + glm::vec2(0x186, 0xb), a, nullptr, hot)) index = index < n ? index + 1 : 1;
    if (const int d = hot ? ui.nav_adjust() : 0) index = (index - 1 + d + n) % n + 1;  // GamepadPlay: pad left/right cycle
    index = std::clamp(index, 1, n);
    if (const UiFont* f = button_font(ui)) {
        const std::string_view s = items.begin()[index - 1];
        ui.text(f, s, {p.x + float(0x1d0 / 2 - f->metrics.measure(s) / 2), p.y + 0xd}, white(a), TextAlign::Left, 1.0f, TextOp::Diffuse);
    }
    return index;
}

// Page row i of n: label slides in from x = -612 (500 ms; out at 0), its control drops from (448, 62) to
// (448, label - 7), bottom row first (enter 550 + 50 k ms, leave 50 k ms); both fade at 4/s.
struct Row {
    glm::vec2 label, ctrl;
    float la, ca;
    bool hot;
};
Row row(Ctx& c, int i, int n) {
    const float y = float(kRowY[i] + c.l);
    const int k = n - 1 - i;
    Row r;
    r.label = c.pos({-612, y}, {-100, y}, 500, 0);
    r.ctrl = c.pos({kCtrlX, float(0x3e + c.l)}, {kCtrlX, y - 7}, 550 + 50 * k, 50 * k);
    r.la = c.alpha(500, 0, 4);
    r.ca = c.alpha(550 + 50 * k, 50 * k, 4);
    r.hot = c.in && c.ui.hover({0, r.ctrl.y, 1024, 52});
    return r;
}

int combo_row(Ctx& c, int i, int n, std::string_view label, std::initializer_list<std::string_view> items, int index) {
    const Row r = row(c, i, n);
    row_label(c.ui, label, r.label, r.hot, r.la);
    return combo(c.ui, r.ctrl, items, index, r.hot, r.ca);
}

int slider_row(Ctx& c, int i, int n, std::string_view label, int v, int steps) {
    const Row r = row(c, i, n);
    row_label(c.ui, label, r.label, r.hot, r.la);
    return slider(c, r.ctrl, v, steps, r.hot, r.ca);
}

// Text button 0x73e130 at (x1, 712 + lc)-(x2, 742 + lc): label centred in the rect raised 1 px, COMMELE2 mark
// at both sides (y + 8) while hovered, black label while held. Appears after 500 ms (fade 6/s), fades at once.
bool text_button(Ctx& c, int x1, int x2, std::string_view s) {
    Ui& ui = c.ui;
    const float a = c.alpha(500, 0, 6), y1 = float(0x2c8 + c.l), y2 = y1 + 30;
    if (a <= 0) return false;
    const glm::vec4 r{float(x1), y1, float(x2 - x1 + 1), y2 - y1 + 1};
    const bool hot = c.in && ui.hover(r), held = hot && ui.input().mouse_down;
    const UiFont* f = button_font(ui);
    text_in_rect(ui, f, s, {float(x1), y1 - 1, float(x2), y2 - 1}, held ? black(a) : white(a));
    if (hot && f) {
        const int w = f->metrics.measure(s);
        ui.sprite("COMMELE2", "mark", {float((x1 + x2 - w) / 2 - 20), y1 + 8}, white(a));
        ui.sprite("COMMELE2", "mark", {float((x1 + x2 + w) / 2 + 6), y1 + 8}, white(a));
    }
    return c.in && ui.clicked(r);
}

void set_page(MenuModel& m, OptionsTab tab) {
    m.options_prev = int(m.options_tab);
    m.options_tab = tab;
    m.time = 0;
}

// Original combos/sliders write the config on every change; the game applies and persists on OptionsChanged.
// The page's leave function (0x74c2e0/0x7420e0/0x7480e0/0x7433d0) plays BUTTON_SLIDE_MANY.
void leave_page(Ctx& c) {
    set_page(c.m, OptionsTab::Root);
    c.ui.sound("BUTTON_SLIDE_MANY");
    c.out.push_back({UiCommand::Type::OptionsChanged, 0, {}, {}});
}

void root_page(Ctx& c) {
    Ui& ui = c.ui;
    const UiFont* f = button_font(ui);
    // 0x73cc00 case 0: buttons drop from (256, 62) to (256, 320/400/480/560 + lc), bottom first
    // (500..650 ms; leave 0..150 ms), fade 6/s; rect 512x64, COMMELE3 cs_* 512x48.
    for (int i = 0; i < 4; ++i) {
        const int k = 3 - i;
        const glm::vec2 p = c.pos({256, float(0x3e + c.l)}, {256, float(320 + 80 * i + c.l)}, 500 + 50 * k, 50 * k);
        const float a = c.alpha(500 + 50 * k, 50 * k, 6);
        if (a <= 0) continue;
        const glm::vec4 r{p.x, p.y, 512, 64};
        const bool hot = c.in && ui.hover(r);
        ui.sprite("COMMELE2", hot ? (ui.input().mouse_down ? "cs_dw" : "cs_ov") : "cs_ot", p, white(a));
        const std::string_view s = ui.text("OPTMENU", 35 + i);
        text_in_rect(ui, f, s, {p.x, p.y - 5, p.x + 512, p.y + 64 - 20}, white(a));
        if (hot && f) {
            const int w = f->metrics.measure(s);
            ui.sprite("MENU_B", "diamond", {p.x - 32 + float((512 - w) / 2), p.y + 17}, white(a));
            ui.sprite("MENU_B", "diamond", {p.x + 10 + float((512 + w) / 2), p.y + 17}, white(a));
        }
        // 0x73d830: IF_SETTING_BUTTON_CLICK, then 0x73cc00 case 0 (root leave) BUTTON_SLIDE_MANY.
        if (c.in && ui.clicked(r)) set_page(c.m, OptionsTab(1 + i)), ui.sound("IF_SETTING_BUTTON_CLICK"), ui.sound("BUTTON_SLIDE_MANY");
    }
    // BACK / Esc -> 0x73ca80 -> 0x73cc00(6): root leave (case 0) and close (case 6) each play BUTTON_SLIDE_MANY;
    // the close slide runs in options_screen, the screen switches when it ends.
    if (text_button(c, 0x1ce, 0x232, ui.text("OPTMENU", 39)) || (c.in && ui.shortcut(SDL_SCANCODE_ESCAPE))) {
        c.m.options_closing = true, c.m.options_prev = 0, c.m.time = 0;
        ui.sound("BUTTON_SLIDE_MANY"), ui.sound("BUTTON_SLIDE_MANY");
    }
}

void video_page(Ctx& c) {
    GameOptions& o = c.m.options;
    auto opt = [&](int i) { return c.ui.text("OPTMENU", i); };
    // Slider <-> config mappings of 0x74a700 (inverse as the init 0x748b10).
    int s = ftol(double(o.brightness) / 0.02 + 50);
    if (int n = slider_row(c, 0, 8, opt(22), s, 101); n != s) o.brightness = float(n - 50) * 0.02f;
    s = ftol(o.contrast > 1.0f ? (double(o.contrast) - 1.0) / 0.04 + 50 : (double(o.contrast) - 0.5) / 0.01);
    if (int n = slider_row(c, 1, 8, opt(23), s, 101); n != s) o.contrast = n < 51 ? float(n) * 0.01f + 0.5f : float(n - 50) * 0.04f + 1.0f;
    const double g = o.gamma;
    s = ftol(g > 3.0 ? (g - 3.0) / 0.12 + 75 : g > 1.0 ? (g - 1.0) / 0.08 + 50 : (g - 0.5) / 0.01);
    if (int n = slider_row(c, 2, 8, opt(24), s, 101); n != s)
        o.gamma = n < 51 ? float(n) * 0.01f + 0.5f : n < 76 ? float(n - 50) * 0.08f + 1.0f : float(n - 75) * 0.12f + 3.0f;
    o.draw_distance = slider_row(c, 3, 8, opt(25), o.draw_distance, 21);
    o.dynamic_grass = combo_row(c, 4, 8, opt(28), {opt(2), opt(1)}, o.dynamic_grass + 1) == 2;
    s = o.grass_distance / 20;
    if (int n = slider_row(c, 5, 8, opt(26), s, 101); n != s) {
        o.grass_distance = n * 20;
        o.dynamic_grass = n != 0;
    }
    o.grass_density = slider_row(c, 6, 8, opt(27), o.grass_density, 101);
    o.shadow = combo_row(c, 7, 8, opt(29), {opt(18), opt(19), opt(20), opt(21)}, o.shadow + 1) - 1;
    if (text_button(c, 0x183, 0x1e7, opt(33))) {  // DEFAULT: Config_set_defaults, video fields
        const GameOptions d;
        o.shadow = d.shadow, o.brightness = d.brightness, o.contrast = d.contrast, o.gamma = d.gamma;
        o.draw_distance = d.draw_distance, o.dynamic_grass = d.dynamic_grass, o.grass_distance = d.grass_distance;
        o.grass_density = d.grass_density;
    }
    if (text_button(c, 0x219, 0x27d, opt(39)) || (c.in && c.ui.shortcut(SDL_SCANCODE_ESCAPE))) leave_page(c);
}

void audio_page(Ctx& c) {
    GameOptions& o = c.m.options;
    auto opt = [&](int i) { return c.ui.text("OPTMENU", i); };
    o.music_volume = slider_row(c, 0, 4, opt(3), o.music_volume, 101);              // config 0x3A
    o.environment_volume = slider_row(c, 1, 4, opt(30), o.environment_volume, 101);  // 0x3C
    o.sound_volume = slider_row(c, 2, 4, opt(4), o.sound_volume, 101);              // 0x41
    o.sound_3d = combo_row(c, 3, 4, opt(5), {opt(2), opt(1)}, o.sound_3d + 1) == 2;  // 0x43
    if (text_button(c, 0x183, 0x1e7, opt(33))) {
        const GameOptions d;
        o.music_volume = d.music_volume, o.environment_volume = d.environment_volume, o.sound_volume = d.sound_volume;
        o.sound_3d = d.sound_3d;
    }
    if (text_button(c, 0x219, 0x27d, opt(39)) || (c.in && c.ui.shortcut(SDL_SCANCODE_ESCAPE))) leave_page(c);
}

void input_page(Ctx& c) {
    GameOptions& o = c.m.options;
    auto opt = [&](int i) { return c.ui.text("OPTMENU", i); };
    // Gamepad/Joystick is only listed when a pad is present (0x7435e0).
    o.input_method = (o.input_method == 2 || c.ui.input().pad.connected
                          ? combo_row(c, 0, 5, opt(15), {opt(12), opt(13), opt(14)}, o.input_method + 1)
                          : combo_row(c, 0, 5, opt(15), {opt(12), opt(13)}, o.input_method + 1)) - 1;
    int s = ftol(double(o.mouse_sensitivity) * 10 + 10);
    if (int n = slider_row(c, 1, 5, opt(16), s, 21); n != s) o.mouse_sensitivity = float(n - 10) / 10.0f;
    o.invert_mouse = combo_row(c, 2, 5, opt(17), {opt(9), opt(10)}, o.invert_mouse + 1) == 2;
    s = ftol(o.riding_camera_angle);
    if (int n = slider_row(c, 3, 5, opt(49), s, 0xb4); n != s) o.riding_camera_angle = float(n);
    o.riding_mode = combo_row(c, 4, 5, opt(52), {opt(53), opt(54)}, o.riding_mode + 1) - 1;
    if (text_button(c, 0x1b5, 0x24b, opt(33))) {  // DEFAULT (0x745b10): scheme 3, sens 0, 90 deg, not inverted
        const GameOptions d;
        o.input_method = d.input_method, o.mouse_sensitivity = d.mouse_sensitivity, o.invert_mouse = d.invert_mouse;
        o.riding_camera_angle = d.riding_camera_angle;
    }
    if (text_button(c, 0xd4, 0x16a, opt(11))) {  // REMAP KEYS -> page 5
        if (c.m.bindings.empty()) c.m.bindings = default_bindings(c.ui.vfs());
        c.m.remap_backup = c.m.bindings;
        c.m.remap_selected = -1;
        c.m.screen = MenuScreen::Remap;
        c.ui.sound("BUTTON_SLIDE_MANY");  // input page leave 0x7480e0
    } else if (text_button(c, 0x296, 0x32c, opt(39)) || (c.in && c.ui.shortcut(SDL_SCANCODE_ESCAPE))) {
        leave_page(c);
    }
}

void game_page(Ctx& c) {
    GameOptions& o = c.m.options;
    auto opt = [&](int i) { return c.ui.text("OPTMENU", i); };
    o.difficulty = combo_row(c, 0, 2, opt(46), {opt(6), opt(7), opt(8), opt(51)}, o.difficulty + 1) - 1;
    o.melee_auto_target = combo_row(c, 1, 2, opt(55), {opt(1), opt(2)}, o.melee_auto_target ? 1 : 2) == 1;
    if (text_button(c, 0x1ce, 0x232, opt(39)) || (c.in && c.ui.shortcut(SDL_SCANCODE_ESCAPE))) leave_page(c);
}

void remap_page(Ctx& c) {
    Ui& ui = c.ui;
    MenuModel& m = c.m;
    if (m.bindings.empty()) m.bindings = default_bindings(ui.vfs());  // sample state / no runner load
    const UiFont* f = button_font(ui);
    auto opt = [&](int i) { return ui.text("OPTMENU", i); };
    const int method = m.options.input_method;

    // Category combo drops from (116, 62) to (116, 236 + lc) last (1050 ms, 0x748540); rows = DEFAULTK ids per
    // category (0x7478a0) whose current binding name is neither "Not Available" nor "Escape", 10 slots max.
    const glm::vec2 cp = c.pos({0x74, float(0x3e + c.l)}, {0x74, float(0xec + c.l)}, 1050, 0);
    int cat = combo(ui, cp, {opt(40), opt(41), opt(42), opt(43), opt(44), opt(50), opt(56)}, m.remap_category + 1,
                    c.in && ui.hover({0, cp.y, 1024, 52}), c.alpha(1050, 0, 6)) - 1;
    if (c.in && m.remap_selected < 0) cat = (cat + ui.nav_page() + 7) % 7;  // GamepadPlay: LB / RB change the category
    if (cat != m.remap_category) m.remap_category = cat, m.remap_selected = -1;
    constexpr int kFirst[7] = {1, 0xb, 0x16, 0x23, 0x2a, 0x2f, 0x35}, kLast[7] = {10, 0x15, 0x20, 0x29, 0x2e, 0x33, 0x37};
    std::vector<KeyBindingRow*> rows;
    for (KeyBindingRow& r : m.bindings) {
        if (r.action < kFirst[cat] || r.action > kLast[cat] || rows.size() == 10) continue;
        const std::string* n = binding_name(ui.vfs(), r, method);
        if (!n || (*n != "Not Available" && *n != "Escape")) rows.push_back(&r);
    }
    if (m.remap_selected >= int(rows.size())) m.remap_selected = -1;

    for (int i = 0; i < int(rows.size()); ++i) {
        // Slots drop from y = 70 and key boxes come in from x = 1024, bottom row first (550 + 50 k ms; leave
        // 50 + 50 i ms, 0x748830), fade 6/s.
        const float y = float(300 + i * 0x28 + c.l);
        const int k = 9 - i;
        const glm::vec2 sp = c.pos({0x74, float(0x46 + c.l)}, {0x74, y}, 550 + 50 * k, 50 + 50 * i);
        const glm::vec2 kp = c.pos({1024, y}, {0x244, y}, 550 + 50 * k, 50 + 50 * i);
        const float a = c.alpha(550 + 50 * k, 50 + 50 * i, 6);
        const bool sel = m.remap_selected == i;
        // Action slot 0x73f490: 464x30, SLOT_OT black text / SLOT_OV white (hover, selected) / SLOT_DW held.
        const glm::vec4 slot{sp.x, sp.y, 0x1d0, 30};
        const bool hot = c.in && ui.hover(slot);
        ui.sprite("COMMELE2", hot && ui.input().mouse_down ? "slot_dw" : hot || sel ? "slot_ov" : "slot_ot", sp, white(a));
        text_in_rect(ui, f, rows[i]->label, {sp.x, sp.y - 2, sp.x + 0x1d0, sp.y + 30 - 2}, hot || sel ? white(a) : black(a));
        // Key box 0x73fde0: 326x40, TB_OT / TB_OV when selected, white text.
        const glm::vec4 box{kp.x, kp.y, 0x146, 40};
        sprite_clipped(ui, "COMMELE2", sel ? "tb_ov" : "tb_ot", kp, a);
        const std::string* n = binding_name(ui.vfs(), *rows[i], method);
        const std::string_view key = sel ? opt(45) : n ? std::string_view(*n) : std::string_view();
        text_in_rect(ui, f, key, {kp.x, kp.y - 2, kp.x + 0x146, kp.y + 40 - 2}, white(a));
        if (sel && f) {
            for (const auto& [x1, x2, s] : {std::tuple{sp.x, sp.x + 0x1d0, std::string_view(rows[i]->label)},
                                            std::tuple{kp.x, kp.x + 0x146, key}}) {
                const float w = float(f->metrics.measure(s));
                ui.sprite("COMMELE2", "mark", {std::floor((x1 + x2 - w) / 2) - 20, y + 8});
                ui.sprite("COMMELE2", "mark", {std::floor((x1 + x2 + w) / 2) + 6, y + 8});
            }
        }
        if (c.in && m.remap_selected < 0 && (ui.clicked(slot) || ui.clicked(box))) {
            m.remap_selected = i;
            return;  // the selecting click is not captured as the new binding
        }
    }

    // Capture (0x745b10): Escape cancels; Shift / Ctrl / Alt (modifier edges) always go through RemapKey_assign
    // scheme 3 with DIK 0x2a / 0x1d / 0x38 (as the original, whatever the scheme); other keys use the current
    // scheme; in the keyboard & mouse scheme left / right clicks bind mouse buttons 0 / 1. The selection ends
    // even when the assignment is refused (unknown key, Main Game Menu).
    if (m.remap_selected >= 0) {
        if (!c.in) return;
        const int action = rows[std::size_t(m.remap_selected)]->action;
        const UiInput& in = ui.input();
        const int sc = in.key_pressed, scheme = key_scheme(method);
        const int mod = sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT ? 0x2a
                        : sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL ? 0x1d
                        : sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT   ? 0x38
                                                                               : 0;
        if (in.pad.button >= 0 && (scheme == 1 || in.pad.button == SDL_GAMEPAD_BUTTON_START)) {
            // GamepadPlay, the original's joystick capture (0x745b10 device 1): a pad button binds J_JOYPAD in the
            // Gamepad/Joystick scheme; Start (fixed Main Game Menu) cancels; other schemes ignore pad buttons.
            const int n = joypad_index(SDL_GamepadButton(in.pad.button));
            if (in.pad.button == SDL_GAMEPAD_BUTTON_START) m.remap_selected = -1;
            else if (scheme == 1 && n) remap_assign(ui.vfs(), m.bindings, 1, action, 1, n - 1), m.remap_selected = -1;
        } else if (ui.key(SDL_SCANCODE_ESCAPE)) {
            m.remap_selected = -1;
        } else if (mod) {
            remap_assign(ui.vfs(), m.bindings, 3, action, 0, mod);
            m.remap_selected = -1;
        } else if (sc != 0) {
            if (const int dik = sdl_to_dik(sc)) remap_assign(ui.vfs(), m.bindings, scheme, action, 0, dik);
            m.remap_selected = -1;
        } else if (scheme == 3 && (in.mouse_pressed || in.right_pressed)) {
            remap_assign(ui.vfs(), m.bindings, 3, action, 2, in.mouse_pressed ? 0 : 1);
            m.remap_selected = -1;
        }
        return;
    }

    if (text_button(c, 0xd4, 0x16a, opt(31))) {  // CONFIRM: RemapKey_save_config, back to the input page
        m.screen = MenuScreen::Options;
        m.options_prev = kRemap;
        ui.sound("BUTTON_SLIDE_MANY");  // remap leave 0x748830
        c.out.push_back({UiCommand::Type::OptionsChanged, 0, {}, {}});
    }
    if (text_button(c, 0x1b5, 0x24b, opt(33)))  // DEFAULT: RemapKey_reset_scheme(current scheme)
        for (KeyBindingRow& r : m.bindings) {
            if (method == 0) r.code[0] = r.def[0];
            else if (method == 2) r.code[1] = r.def[1], r.code[2] = r.def[2];
            else r.code[3] = r.def[3], r.code[4] = r.def[4];
        }
    if (text_button(c, 0x296, 0x32c, opt(32)) || (c.in && ui.shortcut(SDL_SCANCODE_ESCAPE))) {  // CANCEL: KeyMap_apply_config
        if (!m.remap_backup.empty()) m.bindings = m.remap_backup;
        m.screen = MenuScreen::Options;
        m.options_prev = kRemap;
        ui.sound("BUTTON_SLIDE_MANY");
    }
}

void draw_page(Ctx& c, int page) {
    switch (page) {
        case 0: root_page(c); break;
        case 1: video_page(c); break;
        case 2: audio_page(c); break;
        case 3: input_page(c); break;
        case 4: game_page(c); break;
        default: remap_page(c); break;
    }
}

// FRAME (COMMELE2 "frame" 440x552, stored rotated) y per page; root/remap/closed park it at -448.
// Enter 0x74bc50 etc.: 500 ms; leaving to root/remap: video 400, audio 200, input 250, game 100 ms.
constexpr int kFrameY[6] = {-0x1c0, 0x6c, -100, -0x30, -0xcc, -0x1c0};
constexpr int kFrameOut[6] = {0, 400, 200, 250, 100, 0};

// Backdrop, FRAME, LONGBARs + title, the leaving page, the page, the foreground (0x73d2c0 order).
// Close (BACK/Esc on the root page, 0x73cc00 case 6, MenuModel::options_closing): the root buttons leave as on a
// page switch (0..150 ms), BACK fades out at 6/s at once, the LONGBARs slide out to x = -1024 / +1024 after
// 200 ms; 0x73db40 switches screens once the bottom root button is up and faded and both bars are out.
void options_screen(Ui& ui, MenuModel& m, std::vector<UiCommand>& out, int page, int prev) {
    const int l = m.in_game ? -48 : 0;
    const float t = m.time;
    const bool closing = m.options_closing;
    if (!ui.input().mouse_down) m.slider_hold_ms = m.slider_repeat_ms = 0;
    if (m.in_game && m.ingame_backdrop_closed && ingame_backdrop_time(m) >= kIngameBackdropIn) m.ingame_backdrop_closed = false;
    if (m.in_game) ingame_backdrop_back(ui, ingame_backdrop_time(m));  // INGAME_B layer 0; layer 1 on top later
    else menu_frontend_backdrop(ui, m);
    // BUTTON_SLIDE_MANY: opening (0x73c9d0 -> 0x73cc00 case 0 leave) and once the entered page's first widget
    // delay runs out (ticks 0x73db40 / 0x74b550 / 0x741960 / 0x746ff0 / 0x742ee0: 500 ms; remap row 0: 1000 ms).
    if (!closing && prev < 0) ui.sound_at("BUTTON_SLIDE_MANY", t, 0.0f);
    if (!closing) ui.sound_at("BUTTON_SLIDE_MANY", t, page == kRemap ? 1.0f : 0.5f);

    const int from = prev < 0 ? 0 : prev;
    const int fdelay = page != 0 && page != kRemap ? 500 : kFrameOut[from];
    sprite_clipped(ui, "COMMELE2", "frame", slide({0x1cc, float(kFrameY[from] + l)}, {0x1cc, float(kFrameY[page] + l)}, t, fdelay, false));
    // LONGBARs at (0, 180 + lc) / (0, 712 + lc); on opening they come in from x = -1024 / +1024 after 250 ms.
    const float bx = closing ? slide({0, 0}, {-1024, 0}, t, 200, false).x : prev < 0 ? slide({-1024, 0}, {0, 0}, t, 250, false).x : 0;
    sprite_clipped(ui, "COMMELE2", "longbar", {bx, float(180 + l)});
    text_in_rect(ui, ui.font("Goudy_bold_18"), ui.text("OPTMENU", page == kRemap ? 34 : 34 + page),
                 {bx, float(180 + l), bx + 1024, float(210 + l)}, kUiWhite);
    sprite_clipped(ui, "COMMELE2", "longbar", {-bx, float(712 + l)});

    if (closing || (prev >= 0 && prev != page && t < 2.0f)) {  // the old page slides out on a scratch copy (no effects)
        MenuModel ghost = m;
        std::vector<UiCommand> sink;
        Ctx g{ui, ghost, sink, l, t, false};
        draw_page(g, closing ? 0 : prev);
    }
    if (!closing) {
        Ctx c{ui, m, out, l, t, true};
        draw_page(c, page);
    }

    // Layers above the page: in game the INGAME_B stone/title layer (en2d layer 1); front end (0x70a8b0) the
    // logo bar and stone ring overlays (the options page is drawn between gold_r and logo_bar).
    if (m.in_game) {
        ingame_backdrop_front(ui, ingame_backdrop_time(m));
    } else {
        ui.sprite("MENU1024", "logo_bar", {0, 16});
        ui.sprite("MENU1024", "logo", {288, 26});
        ui.sprite("MENU1024", "alab_s", {0, 0});
        ui.sprite("MENU1024", "alab_l", {0, 0});
        ui.sprite("MENU1024", "alab_r", {836, 0});
    }
    if (closing && t >= 0.2f + ui_slide_time(1024)) m.screen = m.previous, m.options_closing = false, m.options_prev = -1;
}

}  // namespace

void menu_options(Ui& ui, MenuModel& m, std::vector<UiCommand>& out) {
    options_screen(ui, m, out, int(m.options_tab), m.options_prev);
}

void menu_remap(Ui& ui, MenuModel& m, std::vector<UiCommand>& out) {
    options_screen(ui, m, out, kRemap, int(OptionsTab::Input));
}

}  // namespace joa::engine
