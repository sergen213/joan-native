// Character details window: attribute panel, inventory page, attack-combo page. See hud_character.hpp.
#include "engine/hud_character.hpp"

#include "engine/menu_saveload.hpp"
#include "engine/ui_screens.hpp"

#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace joa::engine {
namespace {
using game::GameData;
using game::ItemDef;
using game::Unit;
using game::World;

// FUN_006f6280 row table DAT_00a277d8 (6 x 16 bytes): highlight at (x+8, y+8), value right edge (vx+8, vy+8),
// ADD button (vx+0xc, ay+8). Rows: hit points, energy points, strength, defense, dexterity, leadership.
struct Row { int x, y, vx, vy, ay; };
constexpr Row kRows[6] = {{8, 233, 254, 238, 238}, {8, 257, 254, 263, 263}, {8, 292, 254, 298, 298},
                          {8, 318, 254, 324, 324}, {8, 342, 254, 348, 348}, {8, 366, 254, 372, 372}};
constexpr int kSpend[6] = {6, 7, 1, 3, 2, 5};   // FUN_006f7780 -> Unit_spend_stat_point 0x50d640 type
constexpr glm::vec4 kCbButton{0xf6, 0x13, 44, 44};

// Inventory page geometry (FUN_006d6740 / FUN_006d8ac0). Icons carry an 8 px margin: drawn at cell - 8.
constexpr glm::ivec2 kGridOrigin[3] = {{0x21a, 0x3e}, {0x15f, 0x220}, {0x21a, 0x1a6}};  // personal, enhancers, troop
constexpr glm::vec4 kWeaponArea{0x15f, 0x3c, 0xa0, 0xa0}, kRangedArea{0x15f, 0x12a, 0x50, 0xa2},
    kAmmoArea{0x1d4, 0x130, 0x28, 0xa0};
// PadWindows: pad focus targets of the equipment slots melee0 / melee1 (the weapon area's halves), ranged, ammo.
constexpr glm::vec4 kPadSlots[4] = {{0x15f, 0x3c, 0x50, 0xa0}, {0x1af, 0x3c, 0x50, 0xa0}, kRangedArea, kAmmoArea};

constexpr const char* kNameFont = "Goudy_Bold_Gold_24";
constexpr const char* kNumFont = "18pt attribute numbers out";
constexpr const char* kNumOverFont = "18pt attribute numbers over";
constexpr const char* kBigNumFont = "24pt large numbers light";
constexpr const char* kSmallFont = "font_verdana_10";
constexpr const char* kListFont = "Goudy12(18) black";

bool inside(glm::vec2 p, glm::vec4 r) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.z && p.y < r.y + r.w; }
// PadWindows: the pad's focused rect in design units of the current frame (the pad "pointer"; pad focus only).
glm::vec4 pad_rect(const Ui& ui) {
    const glm::vec4 f = ui.nav_focus();
    const glm::vec2 a = ui.to_design({f.x, f.y}), b = ui.to_design({f.x + f.z, f.y + f.w});
    return {a, b - a};
}

// 0x626080: right-aligned, last pixel at x (left = x - w + 1); glyph colours from the atlas.
void text_right(Ui& ui, const char* font, const std::string& s, float x, float y) {
    ui.text(ui.font(font), s, {x + 1, y}, kUiWhite, TextAlign::Right);
}
// 0x626130: centred in the inclusive rect.
void text_centre(Ui& ui, const char* font, const std::string& s, glm::ivec4 r) {
    const UiFont* f = ui.font(font);
    if (!f) return;
    const int x = std::max(r.x + ((r.z - r.x + 1) - f->metrics.measure(s)) / 2, r.x);
    ui.text(f, s, {float(x), float(r.y + ((r.w - r.y + 1) - f->metrics.line_height()) / 2)}, kUiWhite);
}

// en2d button sprite state (0x6211d0 names: out, down, over).
std::string button(Ui& ui, glm::vec4 r, std::string_view base, std::string_view out = "_ot") {
    const bool hot = ui.hover(r);
    return std::string(base) + std::string(hot && ui.input().mouse_down ? "_dw" : hot ? "_ov" : out);
}

bool is_class(const ItemDef& d, std::string_view c) { return d.item_class == c; }
bool is_ammo(int type) { return type >= 0xd && type <= 0x12; }
// FUN_006de4d0 after every placement: weapons IE_SWORD, bows / crossbows IE_BOW, ammo IE_ARROW.
void item_sound(Ui& ui, const ItemDef& d) {
    if (d.type >= 8 && d.type <= 10) ui.sound("IE_SWORD");
    else if (d.type == 0xb || d.type == 0xc) ui.sound("IE_BOW");
    else if (is_ammo(d.type)) ui.sound("IE_ARROW");
}
// FUN_006d8ac0: bows (0xb) shoot arrows 0xd..0xf, crossbows (0xc) bolts 0x10..0x12.
bool ammo_fits(int ranged_type, int ammo_type) {
    if (!ranged_type || !ammo_type) return true;
    return ranged_type == 0xb ? ammo_type <= 0xf : ammo_type >= 0x10;
}

void icon(Ui& ui, const ItemDef& d, glm::vec2 cell, bool hot) {
    const std::string name = item_icon(d);
    TextureHandle t = hot ? ui.texture(name + "#M") : TextureHandle{};
    glm::ivec2 sz = hot ? ui.texture_size(name + "#M") : glm::ivec2(0);
    if (!t) t = ui.texture(name), sz = ui.texture_size(name);
    if (t) ui.image(t, {cell.x - 8, cell.y - 8, float(sz.x), float(sz.y)});
}

// Quantity (stacks) or "Lv%1d" (enhancers, INVENT 1) at the cell's top-left (FUN_006d6740).
void item_count(Ui& ui, const ItemDef& d, const game::ItemInstance& it, glm::vec2 at) {
    std::string s;
    if (it.quantity >= 2 || is_ammo(d.type)) s = std::to_string(it.quantity);
    else if (is_class(d, "ENHANCER")) {
        s = std::string(ui.text("INVENT", 1));
        if (auto p = s.find("%1d"); p != std::string::npos) s.replace(p, 3, std::to_string(it.level - d.min_level + 1));
    }
    if (!s.empty()) ui.text(ui.font(kSmallFont), s, at, kUiWhite);
}

// Tooltip body of an inventory item (FUN_006d6740 tail): Description with its values, then the right-click hint.
std::string item_tip(Ui& ui, const ItemDef& d, int level) {
    std::string hint;
    if (is_class(d, "POTION")) hint = ui.text("INVENT", 12);
    else if (is_class(d, "ARMOR")) hint = ui.text("INVENT", 13);
    else if (is_class(d, "CROSS")) hint = ui.text("INVENT", 14);
    else if (d.type == 0x23) hint = ui.text("INVENT", 15);
    if (d.description.empty()) return hint;
    return item_description(d, level) + (hint.empty() ? "" : "\n\n" + hint);
}

bool overlaps(const CharacterWindow::Cell& a, const CharacterWindow::Cell& b, const GameData& gd) {
    const ItemDef &da = gd.items[std::size_t(a.item)], &db = gd.items[std::size_t(b.item)];
    return a.grid == b.grid && a.col < b.col + db.width && b.col < a.col + da.width && a.row < b.row + db.height &&
           b.row < a.row + da.height;
}

bool fits(const GameData& gd, const std::vector<CharacterWindow::Cell>& cells, const CharacterWindow::Cell& c, int skip = -1) {
    const ItemDef& d = gd.items[std::size_t(c.item)];
    const glm::ivec2 g = grid_size(c.grid);
    if (c.col < 0 || c.row < 0 || c.col + d.width > g.x || c.row + d.height > g.y) return false;
    for (std::size_t i = 0; i < cells.size(); ++i)
        if (int(i) != skip && cells[i].grid >= 0 && overlaps(cells[i], c, gd)) return false;
    return true;
}

}  // namespace

glm::ivec2 grid_size(int grid) {
    return grid == 0 ? glm::ivec2(6, 8) : grid == 1 ? glm::ivec2(4, 1) : grid == 2 ? glm::ivec2(6, 4) : glm::ivec2(6, 13);
}

bool place_first_fit(const GameData& gd, std::vector<CharacterWindow::Cell>& cells, int grid, CharacterWindow::Cell& c) {
    const glm::ivec2 g = grid_size(grid);
    c.grid = grid;
    for (c.row = 0; c.row < g.y; ++c.row)
        for (c.col = 0; c.col < g.x; ++c.col)
            if (fits(gd, cells, c)) return true;
    c.grid = -1;
    return false;
}

std::string item_icon(const ItemDef& d) {
    // FUN_006d6740: the socketed Saint-Catherine-de-Fierbois swords (JOAN_SWORD_LV10B..BCDE) share
    // JOAN_SWORD_LV10_<number of sockets>.
    if (formats::iequals(d.name, "Saint-Catherine-de-Fierbois") && d.code.size() > 15)
        return "item/JOAN_SWORD_LV10_" + std::to_string(d.code.size() - 15);
    return "item/" + d.code;
}

std::string item_description(const ItemDef& d, int level) {
    // 0x5a4c30 arguments: %1 = ItemRes_baseValue, then the additional effects at `level` (value + inc x (level - min)).
    std::vector<int> args{d.base_damage(level)};
    for (const auto& x : d.extra) args.push_back(x.value + x.inc * (level - d.min_level));
    std::string out;
    const std::string& s = d.description;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\n', ++i;
        } else if (s[i] == '%' && i + 1 < s.size() && s[i + 1] == '%') {
            out += '%', ++i;
        } else if (s[i] == '%' && i + 2 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) && s[i + 2] == 'd') {
            const std::size_t a = std::size_t(s[i + 1] - '1');
            out += a < args.size() ? std::to_string(args[a]) : "0";
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

void CharacterWindow::draw(Ui& ui, game::Mission& m, HudModel& hud) {
    tip_title_.clear(), tip_body_.clear(), tip_at_ = glm::vec2(-1.0f);
    if (hud.armory_item) return armory(ui, m, hud);
    shop(ui, m);
    if (shop_facility_) return draw_tip(ui);
    if (!hud.show_character || !m.ui().inventory_open) {
        if (open_) m.ui().inventory_open = false, hud.show_character = false, open_ = false, held_ = {};
        if (!hud.show_character) return;
    }
    World& w = m.world();
    if (!open_) {   // FUN_006f7d80
        open_ = true;
        unit = w.player_id();
        m.ui().inventory_open = true;
        held_ = {};
    }
    Unit* u = w.unit(unit);
    if (!u || u->deleted) {
        hud.show_character = m.ui().inventory_open = open_ = false;
        return;
    }
    // Pointer rest clock for the label tooltips (+0xc/+0x10: shown 250 ms after the last move).
    const glm::vec2 mouse = ui.mouse();
    if (mouse != rest_mouse_) rest_mouse_ = mouse, rest_ = 0.0f;
    else rest_ += ui.dt();
    blink_ += ui.dt();
    if (!m.data().units[std::size_t(u->cls)].hero) combo_page = false;   // non-heroes: inventory only, no CB button
    // PadWindows: one focus scope for both pages (South on CB/WP keeps the focus there). LB / RB step through
    // inventory -> combo list pages of 10 rows (combos() clamps) -> back to the inventory.
    ui.nav_modal(4000);
    if (const int p = ui.nav_page(); p && m.data().units[std::size_t(u->cls)].hero) {
        const int last = std::max(0, combo_rows_ - 10);
        if (!combo_page) combo_page = true, held_ = {}, combo_scroll_ = p > 0 ? 0 : last;
        else if (p > 0 ? combo_scroll_ < last : combo_scroll_ > 0) combo_scroll_ = std::clamp(combo_scroll_ + 10 * p, 0, last);
        else combo_page = false;
    }
    view_ = hud_minimap_scale(ui, hud);
    const bool dragged = held_.dragging;
    left_panel(ui, m, *u);   // PadWindows: rows, CB, then the page's grid cells / list rows, then the portraits
    if (combo_page) combos(ui, m, *u);
    else inventory(ui, m, *u);
    party(ui, m, hud, dragged);
    draw_tip(ui);
    if (ui.key(SDL_SCANCODE_ESCAPE)) hud.show_character = m.ui().inventory_open = open_ = false, held_ = {};
}

void CharacterWindow::left_panel(Ui& ui, game::Mission& m, Unit& u) {
    World& w = m.world();
    const GameData& gd = m.data();
    const glm::vec2 mouse = ui.mouse();
    const UiSprite* hl = ui.sprite("IUNITATT", "highligh");
    auto row_rect = [&](int k) { return glm::vec4{float(kRows[k].x + 8), float(kRows[k].y + 8), hl->size.x + 1, hl->size.y + 1}; };
    int row = -1;
    for (int k = 0; k < 6 && hl; ++k)
        if (ui.hover(row_rect(k))) row = k;   // pointer, or the pad focus on the row
    if (row >= 0) ui.sprite(hl, {float(kRows[row].x + 8), float(kRows[row].y + 8)});
    ui.sprite("IUNITATT", "bg", {8, 8});
    // EXPBAR (0x4a, 0xa8): width = 211 * (xp - LEVEL[l-1]) / (LEVEL[l] - LEVEL[l-1]); none at level 30.
    const game::LevelRow* next = gd.level_row(u.level);
    if (u.level != 30 && next) {
        const int prev = u.level == 1 ? 0 : gd.level_row(u.level - 1) ? gd.level_row(u.level - 1)->exp : 0;
        if (const UiSprite* bar = ui.sprite("IUNITATT", "expbar"); bar && next->exp > prev) {
            const float bw = std::clamp(float(int(bar->size.x) * (u.xp - prev) / (next->exp - prev)), 0.0f, bar->size.x);
            ui.image(bar->texture, {0x4a, 0xa8, bw, bar->size.y}, bar->uv0,
                     {bar->uv0.x + (bar->uv1.x - bar->uv0.x) * bw / bar->size.x, bar->uv1.y});
        }
    }
    const int gold = m.state().gold;
    ui.sprite("IUNITATT", gold < 2000 ? "coins1" : gold < 4000 ? "coins2" : gold < 6000 ? "coins3" : "coins4", {0x18, 0xcc});
    // ADD button on the hovered row while attribute points are left (0x623580 at (vx + 0xc, ay + 8)).
    if (row >= 0 && u.attr_points != 0) {
        const glm::vec4 r{float(kRows[row].vx + 0xc), float(kRows[row].ay + 8), 19, 19};
        // PadWindows: with the pad the focused row stands for its ADD button (South = ADD).
        const bool pad = ui.pad_focus();
        ui.sprite("COMMELE", pad ? std::string("add_ov") : button(ui, r, "add"), {r.x, r.y});
        if (pad ? ui.clicked(row_rect(row)) : ui.clicked(r)) w.allocate_point(u, kSpend[row], false);
    }
    // Name: up to two lines centred vertically in (0x24,0x1f)-(0x108,0x66) (0x627d80 / 0x626e00).
    if (const UiFont* f = ui.font(kNameFont)) {
        const auto lines = wrap_text(f->metrics, u.name, 0x108 - 0x24);
        if (lines.size() < 3) {
            const int lh = f->metrics.line_height(), h = int(lines.size()) * lh;
            for (std::size_t i = 0; i < lines.size(); ++i)
                ui.text(f, lines[i], {0x24, float(0x1f + (0x66 - 0x1f + 1 - h) / 2 + int(i) * lh)}, kUiWhite);
        }
    }
    text_centre(ui, kBigNumFont, std::to_string(u.level), {0x1a, 0x7b, 0x44, 0xad});
    text_right(ui, kNumFont, std::to_string(u.xp), 0x108, 0x7a);
    text_right(ui, kNumFont, u.level == 30 || !next ? std::string(ui.text("UNITATT", 21)) : std::to_string(next->exp), 0x108, 0x8f);
    text_right(ui, kBigNumFont, std::to_string(gold), 0x108, 0xd2);
    const std::string values[6] = {std::to_string(u.hp) + "/" + std::to_string(w.max_hp(u)),
                                   std::to_string(u.ep) + "/" + std::to_string(w.max_ep(u)), std::to_string(w.stat_str(u)),
                                   std::to_string(w.stat_def(u)), std::to_string(w.stat_dex(u)), std::to_string(w.stat_lead(u))};
    for (int k = 0; k < 6; ++k) text_right(ui, k == row ? kNumOverFont : kNumFont, values[k], float(kRows[k].vx + 8), float(kRows[k].vy + 8));
    const int kills[4] = {u.kills, u.officer_kills, u.crit_count, u.crit_kills};
    for (int k = 0; k < 4; ++k) {
        static constexpr int kY[4][2] = {{0x1a2, 0x1b7}, {0x1be, 0x1d5}, {0x1da, 0x1f1}, {0x1f5, 0x20c}};
        text_centre(ui, kNumFont, std::to_string(kills[k]), {0xd8, kY[k][0], 0x11b, kY[k][1]});
    }
    text_right(ui, kNumFont, std::to_string(u.attr_points), 0x108, 0x21c);
    text_right(ui, kNumFont, std::to_string(u.skill_points), 0x108, 0x230);

    // CB (to the combo page) / WP (back to the inventory) button; CB blinks (CB_*G) every 500 ms while combo points
    // are left (FUN_006f7780).
    if (gd.units[std::size_t(u.cls)].hero) {
        const bool green = !combo_page && u.skill_points != 0 && int(blink_ / 0.5f) % 2 == 1;
        std::string s = button(ui, kCbButton, combo_page ? "wp" : "cb");
        if (green) s += "g";
        ui.sprite("IUNITATT", s, {kCbButton.x, kCbButton.y});
        if (ui.clicked(kCbButton)) combo_page = !combo_page, held_ = {};
    }

    // Label tooltips (UNITATT title / text pairs) after the pointer rested 250 ms.
    if (rest_ > 0.25f && !held_.item) {
        int t = -1;
        if (row >= 0) t = 1 + 2 * row;
        else if (ui.pad_focus()) t = -1;   // the labels below are pointer-only
        else if (inside(mouse, {0x49, 0x7e, 0x60, 0xc})) t = 13;
        else if (inside(mouse, {0x49, 0x96, 0x60, 0xc})) t = 15;
        else if (inside(mouse, {0x18, 0x222, 0xc6, 0xb})) t = 17;
        else if (inside(mouse, {0x18, 0x236, 0xc6, 0xb})) t = 19;
        if (t > 0) tip(std::string(ui.text("UNITATT", t)), std::string(ui.text("UNITATT", t + 1)));
    }
}

// RecruitWorkshop: hud grid (0 personal, 1 enhancers, 2 troop) <-> original bag of the item (+0x15 col / +0x16 row in
// bag 1 personal, bag 0 enhancers, bag 2 troop).
static void store_cell(game::ItemInstance& it, const CharacterWindow::Cell& c) {
    static constexpr int kBag[3] = {1, 0, 2};
    it.bag = c.grid >= 0 && c.grid < 3 ? kBag[c.grid] : -1, it.col = c.col, it.row = c.row;
}

// Each bag item sits at its own cell (ItemInstance bag/col/row, saved with the game); Inventory::layout places the
// unplaced or clashing ones first-fit (the Player.dat export order) and they keep that cell from then on.
void CharacterWindow::layout(const GameData& gd, Unit& u) {
    static constexpr int kGrid[3] = {1, 0, 2};   // original bag -> hud grid
    cells_unit_ = u.id;
    u.inv.layout(gd);
    cells_.assign(u.inv.bag.size(), Cell{});
    for (std::size_t i = 0; i < cells_.size(); ++i)
        if (const auto& it = u.inv.bag[i]; it.bag >= 0) cells_[i] = {it.item, it.level, kGrid[it.bag], it.col, it.row};
}

void CharacterWindow::inventory(Ui& ui, game::Mission& m, Unit& u) {
    World& w = m.world();
    const GameData& gd = m.data();
    glm::vec2 mouse = ui.mouse();
    layout(gd, u);
    ui.sprite("IUNITATT", "panel", {0x144, 8});
    ui.sprite("INVENT", "inventbg", {0x154, 0x20});

    // What is under the pointer: equipment slot (src 0..3) or bag item (src 10).
    struct Spot { int src, bag; glm::vec4 rect; };
    std::vector<Spot> spots;
    const game::ItemInstance* slot_items[4] = {&u.inv.melee[0], &u.inv.melee[1], &u.inv.ranged, &u.inv.ammo};
    const glm::vec2 slot_cell[4] = {{0x15f, 0x3c}, {0x1af, 0x3c}, {0x15f, 0x12a}, {0x1d4, 0x130}};
    for (int s = 0; s < 4; ++s)
        if (slot_items[s]->item) {
            const ItemDef& d = gd.items[std::size_t(slot_items[s]->item)];
            spots.push_back({s, -1, {slot_cell[s].x, slot_cell[s].y, float(d.width * 40), float(d.height * 40)}});
        }
    for (std::size_t i = 0; i < cells_.size(); ++i)
        if (cells_[i].grid >= 0) {
            const ItemDef& d = gd.items[std::size_t(cells_[i].item)];
            const glm::ivec2 o = kGridOrigin[cells_[i].grid];
            spots.push_back({10, int(i), {float(o.x + cells_[i].col * 40), float(o.y + cells_[i].row * 40), float(d.width * 40),
                                          float(d.height * 40)}});
        }
    // PadWindows: the pad focus moves per grid cell and equipment slot (registered before the item rects, so those
    // stay out of the ring); the focused rect is the pointer: its centre, or the held item's top-left on it.
    const bool pad = ui.pad_focus();
    for (const glm::vec4& r : kPadSlots) ui.hover(r);
    for (int g = 0; g < 3; ++g)
        for (int y = 0; y < grid_size(g).y; ++y)
            for (int x = 0; x < grid_size(g).x; ++x) ui.hover({float(kGridOrigin[g].x + x * 40), float(kGridOrigin[g].y + y * 40), 40, 40});
    const glm::vec4 fr = pad ? pad_rect(ui) : glm::vec4(0.0f);
    const glm::vec2 fc{fr.x + fr.z / 2, fr.y + fr.w / 2};
    if (pad && held_.item) {
        const ItemDef& d = gd.items[std::size_t(held_.item)];
        mouse = {fr.x + d.width * 20.0f, fr.y + d.height * 20.0f};
    } else if (pad) {
        mouse = fc;
    }
    const Spot* hot = nullptr;
    for (const Spot& s : spots)
        if (pad ? inside(fc, s.src == 10 ? s.rect : kPadSlots[s.src]) : ui.hover(s.rect)) hot = &s;

    auto instance = [&](const Spot& s) -> const game::ItemInstance& { return s.src == 10 ? u.inv.bag[std::size_t(s.bag)] : *slot_items[s.src]; };
    for (const Spot& s : spots) {
        if (held_.item && held_.owner == u.id && held_.src == s.src && held_.bag == s.bag) continue;   // on the pointer
        const game::ItemInstance& it = instance(s);
        const ItemDef& d = gd.items[std::size_t(it.item)];
        icon(ui, d, {s.rect.x, s.rect.y}, !held_.item && hot == &s);
        if (s.src == 3) ui.text(ui.font(kSmallFont), std::to_string(it.quantity), {s.rect.x, s.rect.y}, kUiWhite);
        else if (s.src == 10) item_count(ui, d, it, {s.rect.x, s.rect.y});
    }
    // Drop-target highlight (0x632a70 colour DAT_00a6a550); PadWindows: also marks a focused empty cell / slot (all
    // targets right of x 0x15f down to the enhancer grid are cells or slots).
    const glm::vec4 hi = ui_rgba(255, 255, 255, 72);
    if (pad && !hot && !held_.item && inside(fc, {0x15f, 0x3c, 0x21a + 6 * 40 - 0x15f, 0x220 + 40 - 0x3c})) ui.fill(fr, hi);

    // Input (FUN_006d8ac0): left press picks an item up, release after a drag (or the next press) drops it; right
    // button = put it back / use the item under the pointer (potions, armour). PadWindows: South picks up / drops,
    // West (X / Square) = the right button. Only while the shown unit is reachable (0x6d8c21: +0x2f == 0, not
    // task-locked, inside the minimap view: World::inventory_reachable); an item held from another commander
    // (portrait switch) only goes to a portrait or back (right button).
    const UiInput& in = ui.input();
    const bool south = pad && in.mouse_released, west = pad && in.pad.button == SDL_GAMEPAD_BUTTON_WEST;
    const bool live = w.inventory_reachable(u, view_.x, view_.y), mine = held_.owner == u.id;
    if (held_.item) {
        const ItemDef& d = gd.items[std::size_t(held_.item)];
        if (inside(mouse, kWeaponArea) && (d.type == 8 || d.type == 9 || d.type == 10)) ui.fill(kWeaponArea, hi);
        if (inside(mouse, kRangedArea) && (d.type == 0xb || d.type == 0xc)) ui.fill(kRangedArea, hi);
        if (inside(mouse, kAmmoArea) && is_ammo(d.type)) ui.fill(kAmmoArea, hi);
        for (int g = 0; g < 3 && mine; ++g) {
            const glm::ivec2 o = kGridOrigin[g], n = grid_size(g);
            if (!inside(mouse, {float(o.x), float(o.y), float(n.x * 40), float(n.y * 40)})) continue;
            Cell c{held_.item, held_.level, g, int(std::lround((mouse.x - o.x - d.width * 20.0f) / 40.0f)),
                   int(std::lround((mouse.y - o.y - d.height * 20.0f) / 40.0f))};
            if ((g != 1 || is_class(d, "ENHANCER")) && fits(gd, cells_, c, held_.src == 10 ? held_.bag : -1))
                ui.fill({float(o.x + c.col * 40), float(o.y + c.row * 40), float(d.width * 40), float(d.height * 40)}, hi);
        }
        const std::string name = item_icon(d);
        const glm::ivec2 sz = ui.texture_size(name);
        if (TextureHandle t = ui.texture(name))
            ui.image(t, {mouse.x - d.width * 20.0f - 8, mouse.y - d.height * 20.0f - 8, float(sz.x), float(sz.y)});
        if (in.mouse_released && held_.dragging) {   // end of a drag
            held_.dragging = false;
            if (live && glm::length(mouse - held_.at) > 4.0f) drop(m, u, mouse);
        } else if (live && (in.right_pressed || west)) held_ = {};
        else if (live && (south || in.mouse_pressed)) drop(m, u, mouse);
    } else if (hot) {
        const game::ItemInstance& it = instance(*hot);
        const ItemDef& d = gd.items[std::size_t(it.item)];
        if (live && (in.mouse_pressed || south)) held_ = {hot->src, hot->bag, u.id, it.item, it.level, mouse, !pad};
        else if (live && (in.right_pressed || west) && hot->src == 10) w.use_item(u, hot->bag);
        else {
            tip(d.name, item_tip(ui, d, it.level));
            if (pad) tip_at_ = {hot->rect.x, hot->rect.y + hot->rect.w};   // PadWindows: below the whole item
        }
    } else if (rest_ > 0.25f && !pad) {   // grid title tooltips (INVENT 2..11)
        static constexpr glm::vec4 kTitles[5] = {{0x156, 0x1a, 0xb2, 0x14}, {0x156, 0x110, 0xb2, 0x14}, {0x156, 0x1ff, 0xb2, 0x14},
                                                 {0x21b, 0x23, 0x104, 0x14}, {0x21b, 0x18b, 0x104, 0x14}};
        for (int k = 0; k < 5; ++k)
            if (inside(mouse, kTitles[k])) tip(std::string(ui.text("INVENT", 2 + 2 * k)), std::string(ui.text("INVENT", 3 + 2 * k)));
    }
}

// Drop the held item at `p` (FUN_006d8ac0 cases 1..11). Returns true when it left the pointer.
bool CharacterWindow::drop(game::Mission& m, Unit& u, glm::vec2 p) {
    World& w = m.world();
    const GameData& gd = m.data();
    const ItemDef& d = gd.items[std::size_t(held_.item)];
    if (held_.owner != u.id) return false;                           // held from another commander
    if (d.usable_unit >= 0 && d.usable_unit != u.cls) return false;   // +0x255 UsableUnit
    const int ranged_type = u.inv.ranged.item ? gd.items[std::size_t(u.inv.ranged.item)].type : 0;
    const int ammo_type = u.inv.ammo.item ? gd.items[std::size_t(u.inv.ammo.item)].type : 0;
    bool ok = false;
    if (inside(p, kWeaponArea) && (d.type == 8 || d.type == 9 || d.type == 10)) ok = held_.src == 10 && w.equip(u, held_.bag);
    else if (inside(p, kRangedArea) && (d.type == 0xb || d.type == 0xc)) ok = held_.src == 10 && ammo_fits(d.type, ammo_type) && w.equip(u, held_.bag);
    else if (inside(p, kAmmoArea) && is_ammo(d.type)) ok = held_.src == 10 && ammo_fits(ranged_type, d.type) && w.equip(u, held_.bag);
    else {
        for (int g = 0; g < 3 && !ok; ++g) {
            const glm::ivec2 o = kGridOrigin[g], n = grid_size(g);
            if (!inside(p, {float(o.x), float(o.y), float(n.x * 40), float(n.y * 40)}) || (g == 1 && !is_class(d, "ENHANCER"))) continue;
            Cell c{held_.item, held_.level, g, int(std::lround((p.x - o.x - d.width * 20.0f) / 40.0f)),
                   int(std::lround((p.y - o.y - d.height * 20.0f) / 40.0f))};
            if (!fits(gd, cells_, c, held_.src == 10 ? held_.bag : -1)) continue;
            if (held_.src == 10) cells_[std::size_t(held_.bag)] = c, store_cell(u.inv.bag[std::size_t(held_.bag)], c), ok = true;
            else if (const std::size_t n = u.inv.bag.size(); w.unequip(u, held_.src)) {   // equipment back into the bag
                if (u.inv.bag.size() > n) store_cell(u.inv.bag.back(), c), cells_.push_back(c);
                ok = true;
            }
        }
    }
    if (ok) held_ = {};
    return ok;
}

// Party portraits while the window is open. FUN_006f7780: a left press on a portrait (FUN_006ffd90: the circle of
// radius 36 around (DAT_009de66c[8 - n + i] - 800 + W, H - 0x3a)) shows that commander (rank > 3: +0x6af; the CSELECT
// blink restarts). FUN_006d8ac0 0x6db54a: a left release there with an item on the pointer hands it to him
// (World::transfer_item, IE_* sound); when that fails a dragged item goes back, a clicked one stays on the pointer.
// A release on no portrait and outside (0, 0)-(800, 600) drops a CanDrop item beside the shown unit
// (World::discard_item = 0x512830; the same keep / go-back rule when it fails). Both need the shown unit reachable
// (0x6d8c21). The HOLD / RESUME button over a portrait keeps its own presses (as in ui_rts). PadWindows: the
// portraits join the focus ring after the page; South on one = press + release; North drops the held item.
void CharacterWindow::party(Ui& ui, game::Mission& m, HudModel& hud, bool dragged) {
    static constexpr int kCentreX[8] = {115, 207, 299, 391, 483, 575, 667, 759};   // DAT_009de66c
    const int n = std::min(int(hud.party.size()), 8);
    const glm::vec2 window_mouse = ui.mouse();
    const Anchor keep = ui.current_frame();
    ui.frame(Anchor::BottomRight);
    const bool pad = ui.pad_focus();
    const glm::vec2 mouse = ui.mouse();
    int at = -1;
    bool hold = false;
    for (int i = 0; i < n; ++i) {
        const glm::vec2 c{float(kCentreX[8 - n + i] + 224), 768.0f - 0x3a};
        const bool focused = ui.hover({c.x - 36, c.y - 36, 72, 72});
        if (pad ? focused : glm::length(mouse - c) <= 36.0f)
            at = i, hold = hud.party[std::size_t(i)].name != hud.player.name && inside(mouse, {c.x - 46, 678, 24, 28});
    }
    ui.frame(keep);
    World& w = m.world();
    const UiInput& in = ui.input();
    Unit* t = at < 0 ? nullptr : w.unit(hud.party[std::size_t(at)].id);
    if (t && t->deleted) t = nullptr;
    if (t && t->rank > 3 && !hold && (in.mouse_pressed || (pad && in.mouse_released))) unit = t->id;
    const bool north = pad && in.pad.button == SDL_GAMEPAD_BUTTON_NORTH;
    if (!held_.item || !(in.mouse_released || north)) return;
    Unit* from = w.unit(held_.owner);
    Unit* shown = w.unit(unit);
    if (!from || !shown || !w.inventory_reachable(*shown, view_.x, view_.y)) return;
    const ItemDef& d = m.data().items[std::size_t(held_.item)];
    bool done = false;
    if (t && !north) done = w.transfer_item(*from, held_.src, held_.bag, *t, view_.x, view_.y);
    else if (north || (!pad && (window_mouse.x < 0 || window_mouse.y < 0 || window_mouse.x > 800 || window_mouse.y > 600)))
        done = w.discard_item(*from, held_.src, held_.bag, *shown);
    else return;
    if (done) item_sound(ui, d), held_ = {};
    else if (dragged) held_ = {};
}

void CharacterWindow::combos(Ui& ui, game::Mission& m, Unit& u) {
    World& w = m.world();
    const GameData& gd = m.data();
    // FUN_006f0fe0: learned skill upgrades (types 9..16, unlocked == applied) -> Attack Combos list; unlocked but not
    // applied ones whose REQUIRE is learned -> New Attacks.
    std::vector<int> learned, fresh;
    auto is_learned = [&](std::string_view code) {
        for (const auto& e : u.upgrades)
            if (e.unlocked == e.applied && formats::iequals(gd.upitems[std::size_t(e.upitem)].code, code)) return true;
        return false;
    };
    for (std::size_t i = 0; i < u.upgrades.size(); ++i) {
        const auto& e = u.upgrades[i];
        const game::UpItem& up = gd.upitems[std::size_t(e.upitem)];
        if (up.type < 9 || up.type > 16) continue;
        if (e.unlocked == e.applied) learned.push_back(int(i));
        else if (e.unlocked > 0 && (up.require.empty() || is_learned(up.require))) fresh.push_back(int(i));
    }
    new_scroll_ = std::clamp(new_scroll_, 0, std::max(0, int(fresh.size()) - 3));
    const glm::vec2 mouse = ui.mouse();
    if (inside(mouse, {0x15d, 0x69, 0x1a8, 0x118}) && ui.input().wheel != 0.0f) combo_scroll_ -= ui.input().wheel > 0 ? 1 : -1;
    combo_scroll_ = std::clamp(combo_scroll_, 0, std::max(0, int(learned.size()) - 10));   // after the wheel / pad page
    combo_rows_ = int(learned.size());   // PadWindows: LB / RB scroll pages of it (draw())

    ui.sprite("IUNITATT", "panel", {0x144, 8});
    ui.sprite("IATTCOMB", "srbar", {0x2e8, 0x69});
    const UiFont* f = ui.font(kListFont);
    const glm::vec4 red = ui_rgba(255, 51, 51);
    auto key = [&](int action) { return m.ui().key_name ? m.ui().key_name(action) : std::string(); };
    for (int r = 0; r < 10; ++r) {
        const int k = combo_scroll_ + r;
        if (k >= int(learned.size())) break;
        const auto& e = u.upgrades[std::size_t(learned[std::size_t(k)])];
        const game::UpItem& up = gd.upitems[std::size_t(e.upitem)];
        const glm::vec4 rr{0x15d, float(0x69 + 0x1c * r), 392, 24};
        const bool hot = ui.hover(rr);
        ui.sprite("IATTCOMB", hot ? "list_ov" : "list_ot", {rr.x, rr.y});
        const float y = float(0x6e + 0x1c * r);
        int x = 0x160 + ui.text(f, up.desc, {0x160, y}, kUiWhite);
        // Key hint " (<key>)" for down / rush / energy / range attacks (types 0xb..0xe), key in red.
        std::string k1;
        if (up.type == 0xb) k1 = key(12);
        else if (up.type == 0xc) k1 = key(4) + "+" + key(4);
        else if (up.type == 0xd) k1 = key(15);
        else if (up.type == 0xe) k1 = key(18);
        if (!k1.empty()) {
            x += ui.text(f, " (", {float(x), y}, kUiWhite);
            x += ui.text(f, k1, {float(x), y}, red, TextAlign::Left, 1.0f, TextOp::Diffuse);
            ui.text(f, ")", {float(x), y}, kUiWhite);
        }
        ui_text_in_rect(ui, f, std::to_string(e.combo_level), {0x269, 0x69 + 0x1c * r, 0x2e4, 0x7f + 0x1c * r}, kUiWhite);
        if (hot && e.combo_level < 10 && u.skill_points != 0) {   // ADD (0x2b7, 0x6c + 0x1c row) -> spend type 8
            const glm::vec4 b{0x2b7, float(0x6c + 0x1c * r), 19, 19};
            const bool pad = ui.pad_focus();   // PadWindows: the focused row stands for its ADD button (South = ADD)
            ui.sprite("COMMELE", pad ? std::string("add_ov") : button(ui, b, "add"), {b.x, b.y});
            if (pad ? ui.clicked(rr) : ui.clicked(b)) w.allocate_point(u, 8, false, learned[std::size_t(k)]);
        }
    }
    for (int r = 0; r < 3; ++r) {
        const int k = new_scroll_ + r;
        if (k >= int(fresh.size())) break;
        const auto& e = u.upgrades[std::size_t(fresh[std::size_t(k)])];
        const game::UpItem& up = gd.upitems[std::size_t(e.upitem)];
        const glm::vec4 rr{0x15c, float(0x1d4 + 0x28 * r), 360, 24};
        ui.sprite("IATTCOMB", ui.hover(rr) ? "att_ov" : "att_ot", {rr.x, rr.y});
        ui.text(f, up.desc, {0x160, float(0x1d6 + 0x28 * r)}, kUiWhite);
        std::string cost(ui.text("ATTCOMB", 1));
        if (auto p = cost.find("%1d"); p != std::string::npos) cost.replace(p, 3, std::to_string(up.cost));
        ui.text(f, cost, {0x2c3, float(0x1d9 + 0x28 * r)}, kUiWhite, TextAlign::Right);
        const glm::vec4 b{0x2d6, float(0x1d7 + 0x28 * r), 56, 20};
        ui.sprite("IATTCOMB", button(ui, b, "learn"), {b.x, b.y});
        if (ui.clicked(b)) w.apply_upgrade(u, fresh[std::size_t(k)]);   // Unit_apply_upgrade 0x4949f0
    }
    if (fresh.size() > 3) {   // SBUP / SBDW (0x1c0 / 0x288, 0x1a4)
        const glm::vec4 up{0x1c0, 0x1a4, 24, 24}, dn{0x288, 0x1a4, 24, 24};
        if (new_scroll_ > 0) {
            ui.sprite("COMMELE", button(ui, up, "sbup", "_out"), {up.x, up.y});
            if (ui.clicked(up)) --new_scroll_;
        }
        if (int(fresh.size()) - new_scroll_ > 3) {
            ui.sprite("COMMELE", button(ui, dn, "sbdw", "_out"), {dn.x, dn.y});
            if (ui.clicked(dn)) ++new_scroll_;
        }
    }
    ui.sprite("IATTCOMB", "bg", {0x158, 0x20});
}

void CharacterWindow::draw_tip(Ui& ui) {
    if (tip_title_.empty() && tip_body_.empty()) return;
    // FUN_006b8db0: HELPBOX 9-slice 0xf8 wide, height = body lines + 0x3c, kept on screen; title in
    // (x+0x14, y+8)-(x+0xe4, y+0x28), body in (x+0xd, y+0x2c)-(x+0xeb, ..).
    const UiFont* tf = ui.font("Goudy_bold_14");
    const UiFont* bf = ui.font("Goudy_12");
    if (!tf || !bf) return;
    std::vector<std::string> body;
    std::size_t a = 0;
    while (a <= tip_body_.size()) {
        const std::size_t e = std::min(tip_body_.find('\n', a), tip_body_.size());
        for (auto& l : wrap_text(bf->metrics, std::string_view(tip_body_).substr(a, e - a), 0xdd)) body.push_back(l);
        if (e == tip_body_.size() && body.empty()) break;
        a = e + 1;
    }
    const int lh = bf->metrics.line_height();
    const float h = float(int(body.size()) * lh + 0x3c);
    // PadWindows: with the pad the box hangs below the focused item (inventory) or rect (the pointer is hidden).
    const glm::vec4 fr = pad_rect(ui);
    const glm::vec2 mp = !ui.pad_focus() ? ui.mouse() : tip_at_.x >= 0 ? tip_at_ : glm::vec2(fr.x, fr.y + fr.w);
    const float x = std::min(mp.x, 1024.0f - 0xf8), y = std::min(mp.y, 768.0f - h);
    auto spr = [&](const char* n) { return ui.sprite("HELPBOX", n); };
    const UiSprite *ul = spr("des_p_ul"), *ur = spr("des_p_ur"), *bl = spr("des_p_bl"), *br = spr("des_p_br"), *uu = spr("des_p_u"),
                   *ll = spr("des_p_l"), *rr = spr("des_p_r"), *bb = spr("des_p_b"), *cc = spr("des_p_c");
    if (!ul || !ur || !bl || !br || !uu || !ll || !rr || !bb || !cc) return;
    const float x1 = x + ll->size.x, x2 = x + 0xf8 - rr->size.x, y1 = y + uu->size.y, y2 = y + h - bb->size.y;
    // en2d stretches 1-texel edge / centre strips with point sampling: sample the texel centre so no neighbour bleeds in.
    auto stretch = [&](const UiSprite* s, glm::vec4 r) {
        const glm::vec2 half = (s->uv1 - s->uv0) / (2.0f * s->size);
        const glm::vec2 a{s->size.x <= 1.0f ? s->uv0.x + half.x : s->uv0.x, s->size.y <= 1.0f ? s->uv0.y + half.y : s->uv0.y};
        const glm::vec2 b{s->size.x <= 1.0f ? a.x : s->uv1.x, s->size.y <= 1.0f ? a.y : s->uv1.y};
        ui.image(s->texture, r, a, b);
    };
    ui.sprite(ul, {x, y});
    stretch(uu, {x1, y, x2 - x1, uu->size.y});
    ui.sprite(ur, {x2, y});
    stretch(ll, {x, y1, ll->size.x, y2 - y1});
    stretch(cc, {x1, y1, x2 - x1, y2 - y1});
    stretch(rr, {x2, y1, rr->size.x, y2 - y1});
    ui.sprite(bl, {x, y2});
    stretch(bb, {x1, y2, x2 - x1, bb->size.y});
    ui.sprite(br, {x2, y2});
    ui.text(tf, tip_title_, {x + 0x14, y + 8 + float(0x20 - tf->metrics.line_height()) / 2}, kUiWhite, TextAlign::Left, 1.0f, TextOp::Diffuse);
    for (std::size_t i = 0; i < body.size(); ++i)
        ui.text(bf, body[i], {x + 0xd, y + 0x2c + float(int(i) * lh)}, kUiWhite, TextAlign::Left, 1.0f, TextOp::Diffuse);
}

// ---------------------------------------------------------------- shop (i_buy.cpp)
// 800x600 BUY "main" centred: seller grid 6x13 at (+0x100, +0x38), the player's personal bag 6x8 at (+0x210, +0x38),
// coins (+0x10, +0x1d0) + gold right-aligned at (+0xc9, +0x1d4), titles BUY 1 / BUY 2 in (+300, +0x10)-(+0x1c4, +0x38) /
// (+0x23c, +0x10)-(+0x2d4, +0x38), close button c_* (+0x67, +0x218). Stock = item/sellitem.res record
// Facility::sell_list ("item : ? : quantity (-1 no limit) : level (-1 = MinLevel)").
void CharacterWindow::shop(Ui& ui, game::Mission& m) {
    game::MissionUi& mu = m.ui();
    World& w = m.world();
    Unit* u = w.unit(w.player_id());
    const game::Facility* f = mu.buy_open ? m.places().facility(mu.buy_shop) : nullptr;
    if (!f || !u || mu.auto_advance) {   // headless runs: nobody shops (MissionUi closes it after its delay)
        shop_facility_ = 0;
        return;
    }
    const GameData& gd = m.data();
    if (shop_facility_ != f->recno) {   // FUN_006c3230: stock copy
        shop_facility_ = f->recno, stock_.clear(), shop_msg_.clear();
        std::vector<Cell> placed;
        for (const game::ShopStock& g : game::shop_stock(m, f->recno, &sell_ratio_)) {
            Stock s{g.item, g.level, g.quantity, Cell{g.item}};
            s.cell.level = s.level;
            if (!place_first_fit(gd, placed, 3, s.cell)) continue;
            placed.push_back(s.cell);
            stock_.push_back(s);
        }
        auto& left = stock_left_[f->recno];
        if (left.size() != stock_.size()) left.clear();
        for (std::size_t i = 0; i < left.size(); ++i) stock_[i].quantity = left[i];
        left.resize(stock_.size());
    }
    layout(gd, *u);
    if (cells_unit_ != u->id) return;
    ui.nav_modal(4100 + !shop_msg_.empty());   // PadWindows: items + close, or the BUY 8 / 9 message box
    const glm::vec2 o{112, 84};
    const glm::vec2 mouse = ui.mouse();
    int& gold = m.state().gold;
    ui.sprite("BUY", "main", o);
    ui.sprite("BUY", gold < 2000 ? "coins1" : gold < 4000 ? "coins2" : gold < 6000 ? "coins3" : "coins4", o + glm::vec2(0x10, 0x1d0));
    text_right(ui, kBigNumFont, std::to_string(gold), o.x + 0xc9, o.y + 0x1d4);
    const UiFont* tf = ui.font("Goudy_bold_14");
    std::string mine(ui.text("BUY", 2));
    if (auto p = mine.find("%1s"); p != std::string::npos) mine.replace(p, 3, u->name);
    ui_text_in_rect(ui, tf, ui.text("BUY", 1), glm::ivec4(o.x + 300, o.y + 0x10, o.x + 0x1c4, o.y + 0x38), kUiWhite);
    ui_text_in_rect(ui, tf, mine, glm::ivec4(o.x + 0x23c, o.y + 0x10, o.x + 0x2d4, o.y + 0x38), kUiWhite);

    auto line = [&](int text_id, int value) {
        std::string s(ui.text("BUY", text_id));
        if (auto p = s.find("%1d"); p != std::string::npos) s.replace(p, 3, std::to_string(value));
        return s;
    };
    const bool modal = !shop_msg_.empty();
    int buy = -1;
    for (std::size_t i = 0; i < stock_.size(); ++i) {
        const Stock& s = stock_[i];
        if (s.quantity == 0) continue;
        const ItemDef& d = gd.items[std::size_t(s.item)];
        const glm::vec4 r{o.x + 0x100 + float(s.cell.col * 40), o.y + 0x38 + float(s.cell.row * 40), float(d.width * 40), float(d.height * 40)};
        const bool hot = !modal && ui.hover(r);
        icon(ui, d, {r.x, r.y}, hot);
        if (s.quantity >= 2 && s.quantity <= 999) ui.text(ui.font(kSmallFont), std::to_string(s.quantity), {r.x, r.y}, kUiWhite);
        else if (is_class(d, "ENHANCER")) item_count(ui, d, {s.item, 1, s.level}, {r.x, r.y});
        if (!hot) continue;
        std::string body = d.description.empty() ? std::string() : item_description(d, s.level) + "\n";
        body += line(3, d.price) + "\n" + (s.quantity < 0 ? std::string(ui.text("BUY", 4)) : line(5, s.quantity));
        tip(d.name, body);
        if (ui.input().mouse_pressed || (ui.pad_focus() && ui.input().mouse_released)) buy = int(i);   // PadWindows: South
    }
    int sell = -1;
    for (std::size_t i = 0; i < cells_.size() && i < u->inv.bag.size(); ++i) {
        if (cells_[i].grid != 0) continue;
        const game::ItemInstance& it = u->inv.bag[i];
        const ItemDef& d = gd.items[std::size_t(it.item)];
        const glm::vec4 r{o.x + 0x210 + float(cells_[i].col * 40), o.y + 0x38 + float(cells_[i].row * 40), float(d.width * 40),
                          float(d.height * 40)};
        const bool hot = !modal && ui.hover(r);
        icon(ui, d, {r.x, r.y}, hot);
        item_count(ui, d, it, {r.x, r.y});
        if (!hot) continue;
        const int value = is_class(d, "MISSION") ? 0 : int(float(d.price) * sell_ratio_);   // SellingRatio
        tip(d.name, (d.description.empty() ? std::string() : item_description(d, it.level) + "\n") +
                        (value > 0 ? line(6, value) : std::string(ui.text("BUY", 10))));
        if (value > 0 && (ui.input().mouse_pressed || (ui.pad_focus() && ui.input().mouse_released))) sell = int(i);
    }
    // Selling (drag into the seller's grid in the original; one click here): gold += Price x SellingRatio, one piece.
    if (sell >= 0) {
        game::ItemInstance& it = u->inv.bag[std::size_t(sell)];
        gold += int(float(gd.items[std::size_t(it.item)].price) * sell_ratio_);
        if (--it.quantity < 1) u->inv.bag.erase(u->inv.bag.begin() + sell), cells_.erase(cells_.begin() + sell);
    }
    // FUN_006c5a00: price check (BUY 8), room check (BUY 9: World::buy_item, personal grid only), then gold drops.
    // Cost = Price (HYPOTHESIS: the exe's FPU expression is not recovered); one purchase = the item's Quantity.
    if (buy >= 0) {
        Stock& s = stock_[std::size_t(buy)];
        const ItemDef& d = gd.items[std::size_t(s.item)];
        if (gold < d.price) shop_msg_ = ui.text("BUY", 8);
        else if (!w.buy_item(*u, s.item, s.level, std::max(1, d.quantity))) shop_msg_ = ui.text("BUY", 9);
        else {
            layout(gd, *u);
            gold -= d.price;
            if (s.quantity > 0) --s.quantity;
            stock_left_[shop_facility_][std::size_t(buy)] = s.quantity;
        }
    }
    const glm::vec4 close{o.x + 0x67, o.y + 0x218, 104, 32};
    if (modal) {
        tip_title_.clear(), tip_body_.clear();
        if (ui_confirm_popup(ui, shop_msg_)) shop_msg_.clear();
        return;
    }
    const bool hot_close = ui.hover(close);
    ui.sprite("BUY", hot_close && ui.input().mouse_down ? "c_down" : hot_close ? "c_over" : "c_out", {close.x, close.y});
    // Close = commit (Esc or the button / Enter, FUN_006c3540 + FUN_006c8d00); the script's WAIT then resumes.
    if (ui.clicked(close) || ui.key(SDL_SCANCODE_ESCAPE) || ui.key(SDL_SCANCODE_RETURN)) mu.close_interface();
}

// ---------------------------------------------------------------- armory (armory_interface, layout FUN_006b9870)
void CharacterWindow::on_world_events(const std::vector<game::GameEvent>& events, game::Mission& m, HudModel& hud) {
    const World& w = m.world();
    for (const game::GameEvent& e : events) {
        if (e.type != game::Ev::Effect || e.name != "PICKUP" || e.unit != w.player_id()) continue;
        if (e.value <= 0 || e.value >= int(w.data().items.size()) || !w.data().items[std::size_t(e.value)].show_armory) continue;
        auto& hints = m.state().item_hints_a;   // 0x472a90 seen? / 0x4729c0 add
        if (std::find(hints.begin(), hints.end(), std::uint16_t(e.value)) != hints.end()) continue;
        hints.push_back(std::uint16_t(e.value));
        hud.armory_item = e.value, hud.armory_level = w.data().items[std::size_t(e.value)].min_level;
    }
}

// Full-screen ARMORY bg1 (armory_b); title bar "ARMORY" (ARMORY 1) at y 7 between l_arrow / r_arrow; "YOU HAVE ACQUIRED
// THE" (ARMORY 2, Goudy_bold_18 = font 0xa48d48) centred at y 0x40, the item name centred at y 100; frame_bg + frame at
// (0x1c, 0xf8) with the fancybar "DESCRIPTION" (ARMORY 4, Goudy12 Bold Black = 0xa5d3c8) at (0x9c, 0xec) and the
// item_des/<DetailDescriptionFilename> text in (0x34, 0x124)-(0x1cb, 0x253); LONGBAR (0, 0x2c8) with the BACK TO MISSION
// (ARMORY 3) button (W - 0xfa) / 2 .. + 0xf9, 0x2c8 .. 0x2e6.
void CharacterWindow::armory(Ui& ui, game::Mission& m, HudModel& hud) {
    const ItemDef& d = m.data().items[std::size_t(hud.armory_item)];
    ui.nav_modal(4200);   // PadWindows: BACK TO MISSION is the only focus target (East = Escape)
    ui.sprite("ARMORY", "bg1", {0, 0});
    const std::string title(ui.text("ARMORY", 1));
    if (const UiSprite* t = ui.sprite("ARMORY", "title")) {
        const float tx = (1024.0f - t->size.x) / 2.0f;
        ui.sprite(t, {tx, 7});
        ui_text_in_rect(ui, ui.font("Goudy_bold_18"), title, glm::ivec4(tx, 7, tx + t->size.x - 1, 7 + t->size.y - 1), kUiWhite);
        if (const UiSprite* l = ui.sprite("ARMORY", "l_arrow")) ui.sprite(l, {tx - 2 - l->size.x, 11});
        ui.sprite("ARMORY", "r_arrow", {tx + t->size.x + 2, 11});
    }
    ui.text(ui.font("Goudy_bold_18"), ui.text("ARMORY", 2), {512, 0x40}, kUiWhite, TextAlign::Center, 1.0f, TextOp::Diffuse);
    ui.text(ui.font("Goudy_bold_24"), d.name, {512, 100}, kUiWhite, TextAlign::Center, 1.0f, TextOp::Diffuse);
    ui.sprite("ARMORY", "frame_bg", {0x1c, 0xf8});
    ui.sprite("ARMORY", "frame", {0x1c, 0xf8});
    ui.sprite("ARMORY", "fancybar", {0x9c, 0xec});
    ui_text_in_rect(ui, ui.font("Goudy12 Bold Black"), ui.text("ARMORY", 4), {0x9c, 0xec, 0x9c + 199, 0xec + 39}, ui_rgba(0, 0, 0));
    std::string text;
    if (!d.detail_file.empty())
        if (auto bytes = m.vfs().read("item_des/" + d.detail_file)) text.assign(bytes->begin(), bytes->end());
    if (text.empty()) text = item_description(d, hud.armory_level);
    std::erase(text, '\r');
    if (const UiFont* f = ui.font("Goudy_12")) {
        int y = 0x124;
        std::size_t a = 0;
        while (a < text.size() && y < 0x253) {
            const std::size_t e = std::min(text.find('\n', a), text.size());
            for (const auto& l : wrap_text(f->metrics, std::string_view(text).substr(a, e - a), 0x1cb - 0x34))
                if (y < 0x253) ui.text(f, l, {0x34, float(y)}, ui_rgba(0, 0, 0), TextAlign::Left, 1.0f, TextOp::Diffuse), y += f->metrics.line_height();
            a = e + 1;
        }
    }
    ui.sprite("ARMORY", "longbar", {0, 0x2c8});
    if (ui_text_button(ui, ui.font("Goudy14(22) white"), ui.text("ARMORY", 3), {(1024 - 0xfa) / 2, 0x2c8, (1024 - 0xfa) / 2 + 0xf9, 0x2e6}) ||
        ui.key(SDL_SCANCODE_ESCAPE) || ui.key(SDL_SCANCODE_RETURN))
        hud.armory_item = 0;
}

}  // namespace joa::engine
