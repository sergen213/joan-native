// RecruitWorkshop: siege workshop screen (interfac i_work.cpp). See hud_recruit.hpp.
#include "engine/hud_recruit.hpp"

#include "engine/hud_effects.hpp"
#include "engine/menu_saveload.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>

namespace joa::engine {
namespace {

constexpr const char* kWhite = "Goudy14(22) white";
constexpr glm::vec2 kFrame{0x27b, 0x5b};

// FUN_006ea310: CN / FC / BR / TB sprite suffix by class code (L_ row icon, N_ built, R_ new; BARRACK set). The code
// lookup (0x5270b0) ignores case: unit.res spells them "fauconneau" / "Batterram".
const char* suffix(const game::UnitClass& c) {
    using formats::iequals;
    return iequals(c.code, "CANNON") ? "cn" : iequals(c.code, "FAUCONNEAU") ? "fc" : iequals(c.code, "BATTERRAM") ? "br"
         : iequals(c.code, "TREBUCHET") ? "tb" : nullptr;
}
int price(const game::UnitClass& c, int type) { return type == 0 ? c.gold : c.resource; }   // +0x161 gold, +0x165 resource

}  // namespace

// FUN_006ea020: built counts (+0xdc) per product, money = gold (NORMAL) or the facility's resource (IRON / WOOD).
void FacilityWindow::open_workshop(game::Mission& m, int fac) {
    const game::Facility* f = m.places().facility(fac);
    if (!f || f->kind != game::Facility::Workshop) return;
    *this = FacilityWindow{};
    facility = fac, workshop = true, row_count_ = 4, state_ = Opening;
    for (size_t i = 0; i < f->products.size() && i < 4; ++i) have[i] = f->products[i].built, total += have[i];
    money = f->workshop_type == 0 ? m.state().gold : f->resource;
}

// FUN_006ea1a0: one queue entry per new weapon (0x46ab40, team 9999), then the money copy is written back.
void FacilityWindow::commit_workshop(game::Mission& m) {
    game::Facility* f = m.places().facility(facility);
    if (!f) return;
    const std::vector<game::Facility::Product> products = f->products;
    for (size_t i = 0; i < products.size() && i < 4; ++i)
        for (int k = 0; k < add[i]; ++k) m.places().build_siege(m, facility, products[i].type);
    money -= cost;
    if (f->workshop_type == 0) m.state().gold = money;
    else f->resource = money;
    cost = 0, add = {};
}

void FacilityWindow::workshop_screen(Ui& ui, game::Mission& m, HudModel&) {
    game::Places& pl = m.places();
    const game::Facility* f = pl.facility(facility);
    if (!f) return close();
    const game::GameData& gd = m.data();
    const int type = f->workshop_type;
    const std::string unit_word(ui.text("WORKSHOP", type == 0 ? 3 : type == 1 ? 4 : 5));
    const bool live = state_ == Open && !help_;

    ui.sprite("BARRACK", "bg", {0, 0}, {1, 1, 1, alpha()});
    title(ui, ui.text("WORKSHOP", 16));

    // Rows: "%s (%d %s)" (cost Gold or Resource), Attack = Dexterity / 5 (melee siege, SRange < 1) or / 20 (ranged), at
    // most 5 att_r; Defense / 5 att_y (max 5); Mobility = WalkSpeed att_b (max 5); MELEE / RANGE (WORKSHOP 6 / 7).
    const int rows = std::min(4, int(f->products.size()));
    for (int i = 0; i < rows; ++i) {
        const int t = f->products[size_t(i)].type;
        if (t < 1 || t > int(gd.units.size())) continue;
        const game::UnitClass& uc = gd.units[size_t(t - 1)];
        const int c = price(uc, type);
        const glm::vec2 p = row_pos(i);
        const bool hot = live && ui.hover({p.x, p.y, 0x200 + 0x4c, 100});
        const float off = panel(ui, i, p, hot);
        const glm::vec4 plus{0x1ec + off, p.y + 0x14, 36, 28}, minus{0x1ec + off, p.y + 0x34, 36, 28};
        ui.sprite("COMMELE2", std::string("aw_r") + (live && ui.hover(plus) ? ui.input().mouse_down ? "_dw" : "_ov" : "_ot"), {plus.x, plus.y});
        ui.sprite("COMMELE2", std::string("aw_l") + (live && ui.hover(minus) ? ui.input().mouse_down ? "_dw" : "_ov" : "_ot"), {minus.x, minus.y});
        row_frame(ui, p, hot, "WORKSHOP", 8);
        ui.text(ui.font(kWhite), ui_format("%1s (%2d %3s)", {uc.name, std::to_string(c), unit_word}), p + glm::vec2(0x60, 0x10));
        ui.sprite("MESSAGE", "icon_1", p + glm::vec2(0x10, 0x10));
        if (const char* s = suffix(uc)) ui.sprite("BARRACK", std::string("l_") + s, p + glm::vec2(0x10, 0x10));
        ui.sprite("MESSAGE", "icon_2", p + glm::vec2(0x10, 0x10));
        const int step = uc.srange < 1 ? 5 : 20;
        for (int v = 1, x = 0x60, n = 0; v <= uc.dex && n < 5; v += step, x += 0x12, ++n) ui.sprite("BARRACK", "att_r", p + glm::vec2(x, 0x40));
        for (int v = 1, x = 200, n = 0; v <= uc.def && n < 5; v += 5, x += 0x12, ++n) ui.sprite("BARRACK", "att_y", p + glm::vec2(x, 0x40));
        for (int v = 1, x = 0x130; float(v) <= uc.walk && v <= 5; ++v, x += 0x12) ui.sprite("BARRACK", "att_b", p + glm::vec2(x, 0x40));
        ui.text(ui.font(kWhite), ui.text("WORKSHOP", uc.srange < 1 ? 6 : 7), p + glm::vec2(0x198, 0x40), ui_rgba(0, 0, 0),
                TextAlign::Left, 1.0f, TextOp::Diffuse);

        // FUN_006ec480: aw_r adds while built + new < capacity (+0x7d) and money covers it, aw_l removes.
        if (!live) continue;
        const int dx = hot ? ui.nav_adjust() : 0;
        if (ui.clicked(plus) || dx > 0) {
            if (f->capacity <= total || money < cost + c) continue;
            cost += c, ++add[size_t(i)], ++total;
        } else if (ui.clicked(minus) || dx < 0) {
            if (add[size_t(i)] < 1) continue;
            cost -= c, --add[size_t(i)], --total;
        }
    }

    // Info frame (FUN_006ee790 + FUN_006ea310 tail): LOCATION, TOTAL SIEGE WEAPONS grid (built N_*, new R_*, empty
    // ICON frames up to the capacity; 4 per line at x +0x1e + 0x4b k, y +0x9d / +0xed / +0x13d), COST, GOLD / RESOURCE.
    const glm::vec2 o{frame_x(), kFrame.y};
    const int fx = int(o.x), fy = int(o.y);
    ui.sprite("BARRACK", "frame", o);
    auto bar = [&](const char* s, int y) { ui.sprite("BARRACK", s, {o.x + 0x13, o.y + float(y)}); };
    auto label = [&](std::string_view s, int y, glm::vec4 col = kUiWhite) { ui.text(ui.font(kWhite), s, {o.x + 0x17, o.y + float(y)}, col); };
    bar("titlebar", 0xf), label(ui.text("WORKSHOP", 12), 0x11);
    if (const game::Places::RecruitRec* rec = pl.recruit_rec(f->recruit_list))
        if (const UiFont* bf = ui.font("Goudy12 Bold Black"))
            ui.text_block(bf, rec->location, {o.x + 0x16, o.y + 0x2a, 0x152 - 0x16, 0x5c - 0x2a}, kUiWhite, 0, TextAlign::Center, TextOp::Diffuse);
    bar("goldbar", 0x73), bar("titlebar", 0x7f), label(ui.text("WORKSHOP", 13), 0x81);
    int n = 0;
    auto cell = [&](const char* icon) {
        const glm::vec2 at{o.x + 0x1e + float(0x4b * (n % 4)), o.y + float(n < 4 ? 0x9d : n < 8 ? 0xed : 0x13d)};
        ui.sprite("MESSAGE", "icon_1", at);
        if (icon) ui.sprite("BARRACK", icon, at);
        ui.sprite("MESSAGE", "icon_2", at);
        ++n;
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < rows; ++i) {
            const int t = f->products[size_t(i)].type;
            const char* s = t >= 1 && t <= int(gd.units.size()) ? suffix(gd.units[size_t(t - 1)]) : nullptr;
            const std::string icon = s ? std::string(pass ? "r_" : "n_") + s : std::string();
            for (int k = 0; k < (pass ? add : have)[size_t(i)]; ++k) cell(s ? icon.c_str() : nullptr);
        }
    while (n < f->capacity && n < 12) cell(nullptr);
    const char* money_icon = type == 0 ? "gold" : type == 1 ? "iron" : "wood";
    bar("goldbar", 0x182), bar("titlebar", 0x18e), label(ui.text("WORKSHOP", 14), 400);
    ui_text_in_rect(ui, ui.font(kWhite), std::to_string(cost) + " " + unit_word, {fx + 0x16, fy + 0x1b4, fx + 0x152, fy + 0x1d0}, kUiWhite);
    ui.sprite("BARRACK", money_icon, {o.x + 0x100, o.y + 0x184});
    bar("goldbar", 0x1d2), bar("titlebar", 0x1de);
    label(ui.text("WORKSHOP", type == 0 ? 3 : 15), 0x1e0, ui_rgba(0x20, 0x20, 0x20));
    ui_text_in_rect(ui, ui.font(kWhite), std::to_string(money) + " " + unit_word, {fx + 0x16, fy + 0x204, fx + 0x152, fy + 0x220}, kUiWhite);
    ui.sprite("BARRACK", money_icon, {o.x + 0x100, o.y + 0x1d4});

    // HELP (0x26, 0x2be: H_OT / H_OV / H_DW, WORKSHOP 18) opens des\workshop_normal_help.txt (NORMAL) or
    // workshop_resource_help.txt in the G_POPUP text box (0x6d5230 body, title WORKSHOP 19, width 0x262, flags 4: CLOSE
    // only); the screen stays drawn and its input waits (+0x51). BUILD / CANCEL as the barracks.
    const glm::vec4 help{0x26, 0x2be, 100, 44};
    const bool hhot = live && ui.hover(help);
    ui.sprite("BARRACK", hhot && ui.input().mouse_down ? "h_dw" : hhot ? "h_ov" : "h_ot", {help.x, help.y}, {1, 1, 1, alpha()});
    ui_text_in_rect(ui, ui.font(kWhite), ui.text("WORKSHOP", 18), {0x26, 0x2be, 0x26 + 99, 0x2be + 43}, {1, 1, 1, alpha()});
    const bool ok = button(ui, {0x28b, 0x2be, 156, 44}, ui.text("WORKSHOP", 1), "check");
    const bool cancel = button(ui, {0x32e, 0x2be, 156, 44}, ui.text("WORKSHOP", 2), "cross");
    if (help_) {
        if (ui_text_popup(ui, ui.text("WORKSHOP", 19), help_text_, 0x262)) help_ = false;
        return;
    }
    if (state_ != Open) return;
    if (live && ui.clicked(help)) {
        help_text_.clear();
        if (auto b = m.vfs().read(std::string("DES\\") + (type == 0 ? "workshop_normal_help.txt" : "workshop_resource_help.txt")))
            help_text_.assign(b->begin(), b->end());
        std::erase(help_text_, '\r');
        help_ = true;
    } else if (ok || ui.key(SDL_SCANCODE_RETURN)) {
        commit_workshop(m), close();
    } else if (cancel || ui.key(SDL_SCANCODE_ESCAPE)) {
        close();
    }
}

}  // namespace joa::engine
