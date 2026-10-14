// In-game HUD and HUD overlays (interfac/maingame, minimap, msglog, objmenu; 1024x768 design units).
// RE: HUD draw FUN_006f9b60 (player panel + party portraits), minimap FUN_0070e500 / icons FUN_00716010 /
// view FUN_00716be0 / tiles FUN_0071b6a0 (minires.cpp), objectives FUN_00704810, attention message
// FUN_004d8c10, unit name billboards FUN_00520270 / FUN_00520e80, message log FUN_006e1570 / FUN_006e22e0,
// quick-load prompt = G_POPUP box FUN_006d3cb0. Positions are the exe's 1024x768 constants (H-n / W-n
// expressions anchored to that edge); evidence and measurements: work/ui/notes_HudGame.md.
#include "engine/hud_brief.hpp"
#include "engine/hud_crit.hpp"
#include "engine/hud_effects.hpp"
#include "engine/hud_exp.hpp"
#include "engine/hud_mapobj.hpp"
#include "engine/menu_saveload.hpp"
#include "engine/ui_screens.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace joa::engine {
namespace {

const glm::vec4 kHighlight = ui_rgba(220, 32, 32);  // '_' markup colour, measured from s04_05_after_moves

// font::text_right 0x626080: right edge x inclusive (left = x - width + 1).
void text_right(Ui& ui, const UiFont* f, std::string_view s, int x, int y, glm::vec4 c) {
    ui.text(f, s, {float(x + 1), float(y)}, c, TextAlign::Right);
}

// Horizontal part of a bar sprite: `frac` of its width, kept from the left (or from the right for CHP).
void bar(Ui& ui, const UiSprite* s, glm::vec2 pos, float frac, bool keep_right = false, glm::vec4 color = kUiWhite) {
    if (!s) return;
    const float w = std::floor(s->size.x * std::clamp(frac, 0.0f, 1.0f));
    const float du = (s->uv1.x - s->uv0.x) * w / s->size.x;
    if (keep_right) ui.image(s->texture, {pos.x + s->size.x - w, pos.y, w, s->size.y}, {s->uv1.x - du, s->uv0.y}, s->uv1, color);
    else ui.image(s->texture, {pos.x, pos.y, w, s->size.y}, s->uv0, {s->uv0.x + du, s->uv1.y}, color);
}

std::string strip_marks(std::string_view s) {
    std::string r;
    for (char c : s)
        if (c != '_') r += c;
    return r;
}

// Greedy word wrap of '_'-markup text (0x626320 with markup on); widths ignore the '_' toggles.
std::vector<std::string> markup_wrap(const UiFontMetrics& m, std::string_view s, int width) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i <= s.size()) {
        const std::size_t j = std::min(s.find(' ', i), s.size());
        const std::string word(s.substr(i, j - i));
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && m.measure(strip_marks(next)) > width) lines.push_back(line), line = word;
        else line = next;
        i = j + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Draws one markup line; `hi` carries the highlight state across lines.
void markup_line(Ui& ui, const UiFont* f, std::string_view line, glm::vec2 pos, glm::vec4 c, bool& hi) {
    float x = pos.x;
    std::size_t i = 0;
    while (i <= line.size()) {
        const std::size_t j = std::min(line.find('_', i), line.size());
        if (j > i) x += float(ui.text(f, line.substr(i, j - i), {x, pos.y}, hi ? glm::vec4(glm::vec3(kHighlight), c.a) : c) + f->metrics.spacing);
        if (j < line.size()) hi = !hi;
        i = j + 1;
    }
}

}  // namespace

int ui_markup_block(Ui& ui, const UiFont* f, std::string_view s, glm::vec4 r, glm::vec4 c, int max_lines) {
    if (!f) return 0;
    const std::vector<std::string> lines = markup_wrap(f->metrics, s, int(r.z));
    const int first = std::max(0, int(lines.size()) - max_lines);  // FUN_004d8c10 scrolls to the last lines
    bool hi = false;
    for (int k = 0; k < first; ++k)
        for (char ch : lines[std::size_t(k)]) hi ^= ch == '_';
    for (std::size_t k = std::size_t(first); k < lines.size(); ++k)
        markup_line(ui, f, lines[k], {r.x, r.y + float((int(k) - first) * (f->metrics.line_height() + 2))}, c, hi);  // 0x626320 gap 2
    return int(lines.size());
}

int ui_markup_lines(const UiFontMetrics& m, std::string_view s, int width) { return int(markup_wrap(m, s, width).size()); }

namespace {

// Sprite rotated by `angle` (radians, clockwise on screen) about its centre `c`.
// ponytail: one 1-unit quad per texel (UiQuad has no rotation); add a rotated UiQuad if more sprites need it.
void sprite_rotated(Ui& ui, const UiSprite* s, glm::vec2 c, float angle) {
    if (!s) return;
    const glm::vec2 du = (s->uv1 - s->uv0) / s->size;
    const float cs = std::cos(angle), sn = std::sin(angle), h = s->size.x / 2.0f;
    for (int y = 0; y < int(s->size.y); ++y)
        for (int x = 0; x < int(s->size.x); ++x) {
            const glm::vec2 d{x + 0.5f - h, y + 0.5f - h};                 // destination offset
            const glm::vec2 src{cs * d.x + sn * d.y + h, -sn * d.x + cs * d.y + h};  // inverse rotation
            if (src.x < 0 || src.y < 0 || src.x >= s->size.x || src.y >= s->size.y) continue;
            const glm::vec2 uv = s->uv0 + glm::floor(src) * du;
            ui.image(s->texture, {c.x - h + float(x), c.y - h + float(y), 1, 1}, uv + du * 0.25f, uv + du * 0.75f);
        }
}

// Minimap tile grid at the current zoom (z<zoom>r<row>c<col>, 200x200 px each; FUN_0071b6a0 takes the max row/col);
// 0 = no minimap tiles.
glm::ivec2 minimap_tiles(Ui& ui, const HudModel& m) {
    const std::string table = m.scenario + "W";
    char name[16];
    auto has = [&](int r, int c) {
        std::snprintf(name, sizeof name, "z%dr%02dc%02d", m.minimap_zoom, r, c);
        return ui.has_sprite(table, name);
    };
    glm::ivec2 tiles{0};
    while (has(1, tiles.x + 1)) ++tiles.x;
    while (has(tiles.y + 1, 1)) ++tiles.y;
    return tiles;
}

// ---- bottom-left player panel (FUN_006f9b60, param & 1 / & 4) ----
void player_panel(Ui& ui, const HudModel& m) {
    ui.frame(Anchor::BottomLeft);
    const HudUnit& p = m.player;
    ui.sprite("MAINGAME", "panel", {0, 660});                          // (0, H-0x6c)
    if (!p.portrait.empty()) ui.sprite("MAINGAME", p.portrait, {15, 660});  // unit class name + "1"
    // Bars: the shown values the HUD tick chases (FUN_006fd8b0); the texts below use the real ones.
    bar(ui, ui.sprite("MAINGAME", "PHPBAR"), {141, 710}, p.hp_bar >= 0 ? p.hp_bar : p.hp);              // (0x8d, H-0x3a)
    bar(ui, ui.sprite("MAINGAME", "PEPBAR"), {141, 726}, p.energy_bar >= 0 ? p.energy_bar : p.energy);  // (0x8d, H-0x2a)
    if (p.level != 30) bar(ui, ui.sprite("MAINGAME", "PLVLBAR"), {141, 748}, p.xp_bar >= 0 ? p.xp_bar : p.xp);  // (0x8d, H-0x14)
    const UiFont* f = ui.font("verdana_12_regular_black_outline");      // font object 0xa50048
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d/%d", p.hp_value, p.hp_max);
    text_right(ui, f, buf, 0xf2, 768 - 0x3d, kUiWhite);
    if (p.level != 30) {
        std::snprintf(buf, sizeof buf, "%d%%", int(p.xp * 100.0f));
        text_right(ui, f, buf, 0xf2, 768 - 0x17, kUiWhite);
    }
    std::snprintf(buf, sizeof buf, "%d/%d", p.energy_value, p.energy_max);
    text_right(ui, f, buf, 0xf2, 768 - 0x2d, kUiWhite);
    ui_text_in_rect(ui, f, std::to_string(p.ammo), {0x68, 768 - 0x18, 0x7b, 768 - 0xd}, kUiWhite);
    ui_text_in_rect(ui, ui.font("verdana_12_gold_black_outline"), std::to_string(p.level), {0x69, 768 - 0x38, 0x7b, 768 - 0x2d},
                    kUiWhite);  // font object 0xa52848
}

// ---- bottom-right party portraits (FUN_006f9b60 loop over party, slot = 8 - count + i) ----
void party(Ui& ui, const HudModel& m, std::vector<UiCommand>& out) {
    ui.frame(Anchor::BottomRight);
    static constexpr int kFrameX[8] = {69, 161, 253, 345, 437, 529, 621, 713};  // DAT_009de64c (= portrait 0x9de68c)
    static constexpr int kHpX[8] = {83, 175, 267, 359, 451, 543, 635, 727};     // DAT_009de6ac
    static constexpr int kCountX[8] = {131, 223, 315, 407, 499, 591, 683, 775};  // DAT_009de6e4
    const int n = std::min(int(m.party.size()), 8);
    const UiFont* f = ui.font("verdana_12_regular_black_outline");
    const glm::vec2 scale = hud_minimap_scale(ui, m);  // minimap px per world unit
    for (int i = 0; i < n; ++i) {
        const HudUnit& u = m.party[std::size_t(i)];
        const int k = 8 - n + i;
        const float x = float(kFrameX[k] + 224);  // table - 800 + W
        // 0xffffffff, or 0x80ffffff when +0x2f / task-locked / outside the minimap view (FUN_00717a90: |d| < half the
        // 200 px view; true without a minimap) - every sprite and the count text except the HOLD / RESUME buttons.
        const glm::vec2 d = glm::abs(u.world - m.player_world) * scale;
        const glm::vec4 a = u.busy || (scale.x > 0 && (d.x >= 100 || d.y >= 100)) ? ui_rgba(255, 255, 255, 0x80) : kUiWhite;
        if (u.name != m.player.name || m.rts_mode) {
            ui.sprite("MAINGAME", "hr_frame", {x, 678}, a);                                  // (.., H-0x5a)
            // Button list 0x622cc0: HOLD_OT/DW/OV vs RESUMEOT/DW/OV (0x6f9b60 picks the set per unit state).
            const glm::vec4 r{x, 678, 24, 28};
            const bool hot = ui.hover(r), down = hot && ui.input().mouse_down;
            ui.sprite("MAINGAME", std::string(u.holding ? "hold_" : "resume") + (down ? "dw" : hot ? "ov" : "ot"), {x, 678});
            if (ui.clicked(r)) out.push_back({UiCommand::Type::HoldResume, i});
        }
        // HudGaps: hit flash / danger blink, RTS selection blink, morale pips, in the order of FUN_006f9b60.
        static constexpr int kMoralX[6] = {60, 51, 42, 33, 24, 15};                  // DAT_009de6cc
        const float ix = float(kFrameX[k] + 244), mx = float(kFrameX[k] + 230);      // t-0x30c+W (DICON) / t-0x31a+W (MHIT)
        const bool troops = u.units > 0;
        auto flash = [&](int idx) {  // DSWORD<idx> (1..9) at (.., H-0x8c); CHIT / MHIT <idx> for 1..3
            ui.sprite("MAINGAME", "Dsword" + std::to_string(idx), {ix, 628}, a);
            if (idx > 3) return;
            ui.sprite("MAINGAME", "chit" + std::to_string(idx), {x, 668}, a);
            if (troops) ui.sprite("MAINGAME", "mhit" + std::to_string(idx), {mx, 747}, a);
        };
        if (u.danger) {  // HP below INDANGER: frame steps every 100 ms, DICON hidden on every 3rd
            const int s = m.clock_ms / 100 % 9;
            ui.sprite("MAINGAME", "cptop", {x, 668}, a);
            if (s % 3 != 0) ui.sprite("MAINGAME", "Dicon", {ix, 628}, a);
            flash(s + 1);
        } else if (u.hit_ms >= 0) {  // hit: 1000 ms flash
            ui.sprite("MAINGAME", "Dicon", {ix, 628}, a);
            flash(u.hit_ms / 100 % 9 + 1);
        }
        // (DAT_00a500b2 - +0x147) / 100 % 8 (real ms since the character window opened); frames 1..3 drawn
        const int sel = int(std::max(0.0f, m.ui_ms - m.select_ms) / 100.0f) % 8;
        if (sel >= 1 && sel <= 3) {
            if (u.commander_selected) ui.sprite("MAINGAME", "cselect" + std::to_string(sel), {x, 668}, a);
            if (troops && u.troops_selected) ui.sprite("MAINGAME", "mselect" + std::to_string(sel), {mx, 747}, a);
        }
        if (u.units > 0) ui.sprite("MAINGAME", "cmbase", {float(kFrameX[k] + 224 + 2), 743}, a);  // (-0x31e+W, H-0x19)
        if (troops)
            for (int p = 0; p < std::min(6, u.morale * 6 / 100); ++p)
                ui.sprite("MAINGAME", "cmoral", {float(kMoralX[p] + kFrameX[k] + 224), 755}, a);  // (.. -800+W, H-0xd)
        ui.sprite("MAINGAME", "cptop", {x, 668}, a);                                            // (.., H-100)
        bar(ui, ui.sprite("MAINGAME", "chp"), {float(kHpX[k] + 224), 739}, u.hp, true, a);     // drains from the left
        if (!u.portrait.empty()) ui.sprite("MAINGAME", u.portrait, {x, 648}, a);               // (.., H-0x78)
        if (u.units > 0)
            ui_text_in_rect(ui, f, std::to_string(u.units), {kCountX[k] + 224, 768 - 0x23, kCountX[k] + 240, 768 - 0xc}, a);
    }
}

// ---- top-right minimap (FUN_0070e500): 199x199 view at (W-0xd3, 10) centred on the player ----
void minimap(Ui& ui, const HudModel& m, std::vector<UiCommand>& out) {
    ui.frame(Anchor::TopRight);
    constexpr float kX = 1024 - 0xd3, kY = 10, kW = 199, kH = 200;
    const std::string table = m.scenario + "W";
    const glm::ivec2 tiles = minimap_tiles(ui, m);
    if (tiles.x == 0) return;
    const glm::vec2 px_per_world = glm::vec2(tiles * 200) / m.map_size;  // tile world size = map size / grid
    char name[16];
    auto to_map = [&](glm::vec2 w) { return glm::vec2(w.x, m.map_size.y - w.y) * px_per_world; };  // FUN_0070fa70, y up
    const glm::vec2 off = glm::vec2(kX + 99.5f, kY + 100.0f) - to_map(m.player_world);            // screen = map + off
    // Clip a 200x200 tile (map px origin `o`) to the view. Outside the map: the "z<zoom>b" background tile.
    auto tile = [&](const UiSprite* s, glm::vec2 o) {
        if (!s) return;
        const glm::vec2 a = glm::max(o + off, glm::vec2(kX, kY)), b = glm::min(o + off + 200.0f, glm::vec2(kX + kW, kY + kH));
        if (a.x >= b.x || a.y >= b.y) return;
        const glm::vec2 t0 = (a - o - off) / 200.0f, t1 = (b - o - off) / 200.0f;
        ui.image(s->texture, {a.x, a.y, b.x - a.x, b.y - a.y}, glm::mix(s->uv0, s->uv1, t0), glm::mix(s->uv0, s->uv1, t1));
    };
    std::snprintf(name, sizeof name, "z%db", m.minimap_zoom);
    const UiSprite* back = ui.sprite(table, name);
    const glm::vec2 first = glm::floor((glm::vec2(kX, kY) - off) / 200.0f);
    for (int r = int(first.y); r * 200.0f + off.y < kY + kH; ++r)
        for (int c = int(first.x); c * 200.0f + off.x < kX + kW; ++c) {
            std::snprintf(name, sizeof name, "z%dr%02dc%02d", m.minimap_zoom, r + 1, c + 1);
            const bool inside = r >= 0 && c >= 0 && r < tiles.y && c < tiles.x;
            tile(inside ? ui.sprite(table, name) : back, {c * 200.0f, r * 200.0f});
        }
    // Icons centred, only when fully inside the view (FUN_00716010 bounds test with the half size). Unit dots
    // (FUN_0070fbe0): filled quad (x-1, y-1)-(x+1, y+1) via 0x632a70, colours DAT_00a6a5a8 red / 0xa6a59c green /
    // 0xa6a5b4 blue / 0xa6a5a0 white, all alpha 0xcc (set up at 0x70de70..0x70e0b7).
    static const glm::vec4 kDot[5] = {{}, ui_rgba(255, 0, 0, 0xcc), ui_rgba(0, 255, 0, 0xcc), ui_rgba(0, 0, 255, 0xcc),
                                      ui_rgba(255, 255, 255, 0xcc)};
    for (const HudMapIcon& ic : m.map_icons) {
        const glm::vec2 p = glm::round(to_map(ic.world) + off);
        if (ic.dot != HudMapIcon::Dot::None) {
            if (p.x - 1 >= kX && p.y - 1 >= kY && p.x + 1 <= kX + kW && p.y + 1 <= kY + kH) ui.fill({p.x - 1, p.y - 1, 3, 3}, kDot[int(ic.dot)]);
            continue;
        }
        const UiSprite* s = ui.sprite("MAPICON", ic.sprite);
        if (!s) continue;
        const glm::vec2 h = s->size / 2.0f;
        if (p.x - h.x >= kX && p.y - h.y >= kY && p.x + h.x <= kX + kW && p.y + h.y <= kY + kH) ui.sprite(s, p - h);
    }
    // Player arrow (icon type 2): MAPICON "cursor" points up; rotate to the heading (y up in the world).
    sprite_rotated(ui, ui.sprite("MAPICON", "cursor"), glm::round(to_map(m.player_world) + off), 1.5707964f - m.player_heading);
    hud_minimap_fog(ui, m, off, px_per_world);       // HudGaps mapobj: vision mask (FUN_0070e500 after the dots)
    ui.sprite("MAPICON", "frame", {1024 - 218, 6});  // measured in s04_04_mission1_hud (decomp W-0xd7 is clipped)
    hud_minimap_waypoints(ui, m, off, px_per_world);  // HudGaps mapobj: FUN_00718540 after FRAME
    // RTS only (FUN_00717240 / Minimap_screen_to_world 0x717700): left click = camera jump, right click = move order.
    const glm::vec4 view{kX, kY, kW, kH};
    if (m.rts_mode && ui.hover(view) && (ui.clicked(view, false) || ui.input().right_pressed)) {  // not an en2d button: no click SE
        const glm::vec2 p = (ui.mouse() - off) / px_per_world;
        out.push_back({UiCommand::Type::MinimapClick, ui.input().right_pressed ? 1 : 0, {}, {}, {p.x, m.map_size.y - p.y}});
    }
}

// ---- unit name billboards (FUN_00520e80 text "%s (%d)" verdana12_reg_outline + FUN_00520270 LifeBarre) ----
// ponytail: drawn as a 2D overlay scaled by bar_scale (1 = the s04_04 size); the original is a world-space billboard.
void name_bars(Ui& ui, const HudModel& m) {
    ui.frame(Anchor::TopLeft);
    const UiFont* f = ui.font("verdana12_reg_outline");
    const UiSprite frame{ui.texture("LifeBarre"), {0, 0}, {113.0f / 128.0f, 24.0f / 32.0f}, {54, 9}};
    for (const HudUnit& u : m.name_bars) {
        if (!u.on_screen) continue;
        const glm::vec2 c = glm::round(ui.to_design(u.screen));
        const float s = u.bar_scale;
        if (!u.workshop_bar) {
            const std::string label = u.name + " (" + std::to_string(u.level) + ")";
            ui.text(f, label, {c.x, c.y - std::round(18 * s)}, u.hostile ? ui_rgba(0xf5, 0x41, 0x21) : ui_rgba(0x4c, 0x7f, 0xff),
                    TextAlign::Center, s);
        }
        ui.sprite(&frame, glm::vec4(c.x - 27 * s, c.y, 54 * s, 9 * s));
        const float w = std::floor(50.0f * s * std::clamp(u.hp, 0.0f, 1.0f));
        ui.image(ui.texture(u.workshop_bar ? "bar_workshop" : u.hostile ? "bar_english" : "bar_french"), {c.x - 25 * s, c.y + 2 * s, w, 5 * s},
                 {0, 0}, {0.78125f * w / (50.0f * s), 0.625f});
    }
}

// ---- attention message panel above the bottom HUD (FUN_004d8c10, state 4/5) ----
void attention(Ui& ui, const HudMessage& msg) {
    ui.frame(Anchor::BottomLeft);
    ui.sprite("MESSAGE", "panel", {12, 548});
    ui.sprite("MESSAGE", "corn_tl", {4, 540});
    ui.sprite("MESSAGE", "corn_tr", {0x3d5, 540});
    ui.sprite("MESSAGE", "corn_bl", {4, 621});
    ui.sprite("MESSAGE", "corn_br", {0x3d5, 621});
    ui.sprite("MESSAGE", "icon_1", {28, 564});
    const UiSprite* icon = ui.sprite("MESSAGE", msg.icon);
    if (icon) ui.sprite(icon, glm::vec2(28, 564) + (glm::vec2(72) - icon->size) / 2.0f);  // M000x icons are 48x48
    ui.sprite("MESSAGE", "icon_2", {28, 564});
    // 40 chars/s = FUN_004ace90 typed += dt * 0.04 (GameUiBridge fills age = typed / 40).
    const std::string shown = msg.text.substr(0, std::size_t(std::max(0.0f, msg.age * 40.0f)));
    ui_markup_block(ui, ui.font("Goudy14(22) white"), shown, {0x68, 768 - 0xcc, 1024 - 0x1c - 0x68, 0xcc - 0x84}, kUiWhite, 3);
}

// ---- message log (FUN_006e1570 / FUN_006e22e0): MSGLOG book over the in-game backdrop ----
void message_log(Ui& ui, HudModel& m) {
    ui.frame(Anchor::Center);
    ingame_backdrop_back(ui);  // settled (no slide-in clock in HudModel)
    ingame_backdrop_front(ui);
    // Pages: every mission starts a new page, 4 entries per page; the count is rounded up to whole spreads.
    std::vector<std::pair<std::size_t, std::size_t>> pages;  // [first, last) entry per page
    for (std::size_t i = 0; i < m.message_log.size();) {
        std::size_t j = i;
        while (j < m.message_log.size() && j - i < 4 && m.message_log[j].mission == m.message_log[i].mission) ++j;
        pages.push_back({i, j});
        i = j;
    }
    const int total = std::max(2, int(pages.size() + 1) / 2 * 2);
    m.message_log_page = std::clamp(m.message_log_page / 2 * 2, 0, total - 2);
    ui.sprite("MSGLOG", "cover", {0x48, 0x88});
    const glm::vec4 ink = ui_rgba(0, 0, 0, 0xbb);
    const UiFont* page_font = ui.font("Goudy14(22) white");  // 0xa51608
    const UiFont* title = ui.font("Goudy_bold_18");          // 0xa48d48
    const UiFont* body = ui.font("Times_10");                 // 0xa56ff0
    for (int side = 0; side < 2; ++side) {
        const int page = m.message_log_page + side;
        const bool left = side == 0;
        ui.sprite("MSGLOG", left ? "page_l" : "page_r", {left ? 104.0f : 512.0f, 0x98});
        std::string label(ui.text("MSGLOG", 1));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%d", page + 1);
        label.replace(label.find("%1d"), 3, buf);
        std::snprintf(buf, sizeof buf, "%d", total);
        label.replace(label.find("%2d"), 3, buf);
        if (left) ui.text(page_font, label, {0xa4, 0x28c}, ink);
        else text_right(ui, page_font, label, 0x359, 0x28c, ink);
        if (page >= int(pages.size())) continue;
        const auto [first, last] = pages[std::size_t(page)];
        std::string head(ui.text("MSGLOG", 2));
        std::snprintf(buf, sizeof buf, "%d", m.message_log[first].mission);
        head.replace(head.find("%1d"), 3, buf);
        if (left) ui.text(title, head, {0x7c, 0xa0}, ink);
        else text_right(ui, title, head, 900, 0xa0, ink);
        const float x = left ? 0x7c : 0x214;
        for (std::size_t e = first; e < last; ++e) {
            const HudMessage& msg = m.message_log[e];
            const float y = float(int(e - first) * 0x70 + 0xca);
            ui.sprite("MESSAGE", "icon_1", {x, y});
            const bool small = msg.icon.size() >= 4 && std::equal(msg.icon.begin(), msg.icon.begin() + 4, "m000",
                                                                  [](char a, char b) { return std::tolower(a) == b; });
            ui.sprite("MESSAGE", msg.icon, {x + (small ? 10.0f : 0.0f), y + (small ? 10.0f : 0.0f)});
            ui.sprite("MESSAGE", "icon_2", {x, y});
            ui_markup_block(ui, body, msg.text, {x + 0x50, y, 0x15e - 0x50, 100}, ink);
        }
    }
    // Page arrows (COMMELE2 aw_l/aw_r buttons at (0x78, 0x28c) / (0x364, 0x28c)).
    auto arrow = [&](const char* base, float ax, bool enabled) {
        const glm::vec4 r{ax, 0x28c, 36, 28};
        const bool hot = enabled && ui.hover(r);
        ui.sprite("COMMELE2", std::string(base) + (hot && ui.input().mouse_down ? "_dw" : hot ? "_ov" : "_ot"), {r.x, r.y});
        return enabled && ui.clicked(r);
    };
    const int pad_page = ui.nav_page();  // GamepadPlay: LB / RB turn the page
    if (arrow("aw_l", 0x78, m.message_log_page > 0) || (pad_page < 0 && m.message_log_page > 0)) m.message_log_page -= 2;
    if (arrow("aw_r", 0x364, m.message_log_page + 2 < total) || (pad_page > 0 && m.message_log_page + 2 < total)) m.message_log_page += 2;
    ui.sprite("COMMELE2", "longbar", {0, 712});
    if (ui_text_button(ui, page_font, ui.text("IGAMEMNU", 14), {462, 712, 561, 742}) || ui.key(SDL_SCANCODE_ESCAPE))
        m.show_message_log = false;
}

// ---- mission brief topic menu (objmenu, "MISSION BRIEF" over the in-game backdrop) ----
void mission_brief(Ui& ui, HudModel& m) {
    ui.frame(Anchor::Center);
    std::string heading(ui.text("OBJMENU", 15));
    for (char& c : heading) c = char(std::toupper(static_cast<unsigned char>(c)));
    ingame_backdrop_back(ui);  // settled; == menu_ingame_backdrop with the slide-in finished
    menu_heading_bars(ui, -48, heading);
    ingame_backdrop_front(ui);
    // MISSION TIME / TOTAL TIME boxes (OBJMENU "tc"), measured at (8, 24) and (876, 24).
    const UiFont* small = ui.font("Goudy12(18) inner emboss");
    const UiFont* f14 = ui.font("Goudy14(22) white");
    for (int i = 0; i < 2; ++i) {
        const int x = i ? 876 : 8, t = i ? m.total_time : m.mission_time;
        ui.sprite("OBJMENU", "tc", {float(x), 24});
        ui_text_in_rect(ui, small, ui.text("OBJMENU", 76 + i), {x, 28, x + 139, 42}, kUiWhite);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
        ui_text_in_rect(ui, f14, buf, {x, 50, x + 139, 76}, kUiWhite);
    }
    // Topics: COMMELE2 cs_* buttons at (256, 210 + 63 i) (measured), label in (x, y-5)-(x+511, y+27) (0x73ee40).
    static constexpr int kTopics[7] = {4, 5, 6, 7, 8, 44, 63};
    static constexpr int kPages[7] = {1, 2, 3, 4, 5, 8, 9};  // objmenu +0x80 page per topic (FUN_00721a60)
    const UiFont* f = ui.font("Goudy14(22) white");  // matched visually against s04_06_mission_brief
    for (int i = 0; i < 7; ++i) {
        const int y = 210 + 63 * i;
        const bool enabled = i < 5 || (i == 5 ? m.brief_strategy : m.brief_siege);
        const glm::vec4 r{256, float(y), 512, 48};
        const bool hot = enabled && ui.hover(r);
        if (hot && ui.clicked(r)) m.brief_page = kPages[i], m.brief_time = 0.0f, m.brief_scroll = i == 0 ? -4 : 0, m.brief_select = -1;
        ui.sprite("COMMELE2", !enabled ? "cs_ds" : hot && ui.input().mouse_down ? "cs_dw" : hot ? "cs_ov" : "cs_ot", {r.x, r.y});
        ui_text_in_rect(ui, f, ui.text("OBJMENU", kTopics[i]), {256, y - 5, 767, y + 27},
                        enabled ? kUiWhite : ui_rgba(255, 255, 255, 0x60));
    }
    if (ui_text_button(ui, f14, ui.text("OBJMENU", 9), {462, 664, 561, 694}) || ui.key(SDL_SCANCODE_ESCAPE))
        m.show_mission_brief = false;
}

}  // namespace

glm::vec2 hud_minimap_scale(Ui& ui, const HudModel& m) {
    const glm::ivec2 tiles = minimap_tiles(ui, m);
    return tiles.x ? glm::vec2(tiles * 200) / m.map_size : glm::vec2(0.0f);
}

void hud_game(Ui& ui, HudModel& m, std::vector<UiCommand>& out) {
    m.ui_ms += ui.dt() * 1000.0f;  // DAT_00a500b2 (real ms): party selection blink
    // GamepadPlay: the full-screen pages and the quick-load popup are pad focus scopes.
    if (m.show_message_log) return ui.nav_modal(2001), message_log(ui, m);
    if (m.show_mission_brief) return ui.nav_modal(2100 + m.brief_page), m.brief_page ? hud_brief(ui, m) : mission_brief(ui, m);
    name_bars(ui, m);
    hud_target(ui, m);  // HudGaps: TUI target HP (first in FUN_006f9b60)
    if (m.show_objective) hud_objectives(ui, m);  // HudGaps mapobj: FUN_00704810 (fade, highlights, suffixes)
    if (!m.messages.empty()) attention(ui, m.messages.front());
    minimap(ui, m, out);
    player_panel(ui, m);
    hud_exp(ui, m);  // FUN_006fe660 / FUN_006ffed0 / ComboHUD_update after the PLVLBAR
    hud_crit(ui, m);  // HudGaps crit: Crit_drawHUD 0x45fae0 (after the panel text) + kill interface FUN_006df180
    party(ui, m, out);
    hud_interact(ui, m);  // HudGaps: GMSG indicator (FUN_006d23b0, late in Sys_disp_frame)
    if (m.show_quickload_prompt) {
        ui.frame(Anchor::Center);
        ui.nav_modal(2002);
        if (const int r = ui_confirm_popup(ui, ui.text("QSAVE", 1))) {
            out.push_back({r == 1 ? UiCommand::Type::QuickLoad : UiCommand::Type::QuickLoadCancel});
            m.show_quickload_prompt = false;
        }
    }
    ui.frame(Anchor::Center);
}

}  // namespace joa::engine
