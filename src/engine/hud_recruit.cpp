// RecruitWorkshop: barracks recruit screen + the chrome shared with the siege workshop screen. See hud_recruit.hpp.
#include "engine/hud_recruit.hpp"

#include "engine/hud_effects.hpp"
#include "engine/menu_saveload.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>

namespace joa::engine {
namespace {

constexpr const char* kWhite = "Goudy14(22) white";      // font object 0xa51608 (buttons, titles, names, values)
constexpr const char* kBlack = "Goudy12 Bold Black";     // 0xa5d3c8 (row labels, location)
constexpr float kRowStagger = 0.03f;                     // 0x1e ms between the rows' move-to delays
constexpr glm::vec2 kFrame{0x27b, 0x5b};

// BarrUI_handle_input / BarrUI_confirm: the six recruitable French classes, by code.
constexpr const char* kCodes[6] = {"FRENCH_LIGHT_INFANTRY", "FRENCH_HEAVY_INFANTRY", "FRENCH_MACEMAN",
                                   "FRENCH_POLEMAN", "FRENCH_ARCHER", "FRENCH_CROSSBOW_MAN"};
constexpr const char* kRowIcon[6] = {"FLINF", "FHINF", "FMACE", "FPOLE", "F_ARCH", "FCROSS"};   // MESSAGE set
constexpr const char* kTroop[6] = {"linf", "hinf", "mace", "pole", "arch", "cross"};            // c_* / n_* BARRACK

int slot_of(const game::GameData& gd, int type) {
    if (type < 1 || type > int(gd.units.size())) return -1;
    for (int s = 0; s < 6; ++s)
        if (formats::iequals(gd.units[size_t(type - 1)].code, kCodes[s])) return s;   // 0x5270b0: case-insensitive
    return -1;   // ponytail: the original keeps the previous row's slot for other classes (none in shipped lists)
}
// BarrUI_count_troop 0x6bd4b0: UnitClass unit_class 1..6 -> slot (pole 3 -> +0x4e, mace 4 -> +0x4a).
int slot_of_class(int unit_class) {
    static constexpr int k[7] = {-1, 0, 1, 3, 2, 4, 5};
    return unit_class >= 0 && unit_class <= 6 ? k[unit_class] : -1;
}

void text_rect(Ui& ui, const char* font, std::string_view s, glm::ivec4 r) { ui_text_in_rect(ui, ui.font(font), s, r, kUiWhite); }

}  // namespace

// ---------------------------------------------------------------- shared chrome

void FacilityWindow::on_mission_events(const std::vector<game::MissionEvent>& events, game::Mission& m, HudModel& hud) {
    for (const game::MissionEvent& e : events) {
        // e.a = the recruiting unit (DAT_00a48959): the player on R, the first selected RTS leader on an RTS click.
        if (e.kind != game::MissionEvent::Kind::Interact) continue;
        if (e.text == "recruit_screen") hud.recruit_screen = e.b, recruiter = e.a;   // DAT_00a48955 facility, 0x6c0310(1)
        else if (e.text == "workshop_screen") hud.workshop_screen = e.b;   // DAT_00a48569, 0x6ed3e0(1)
    }
}

glm::vec2 FacilityWindow::row_pos(int i) const {
    const float target = float(0x68 + 100 * i), dist = target + 100.0f;
    if (state_ == Opening) return {0x24, -100.0f + ui_slide(t_, kRowStagger * float(row_count_ - 1 - i), dist)};
    if (state_ == Closing) return {0x24, target - ui_slide(t_, kRowStagger * float(i), dist)};
    return {0x24, target};
}

float FacilityWindow::frame_x() const {
    const float d = 0x400 - kFrame.x;
    if (state_ == Opening) return 0x400 - ui_slide(t_, 0.0f, d);
    if (state_ == Closing) return kFrame.x + ui_slide(t_, 0.0f, d);
    return kFrame.x;
}

float FacilityWindow::alpha() const {   // 0x61f1b0(1 / 0, 6.0): fade at 6 per second
    return state_ == Opening ? ui_fade(t_, 0.0f, 6.0f) : state_ == Closing ? 1.0f - ui_fade(t_, 0.0f, 6.0f) : 1.0f;
}

// ia_ov (0x1d8 -> 0x224) with aw_r (0x1ec -> 0x238, +0x14) / aw_l (+0x34) slide out from behind the hovered row
// (move-to 300 px/s, -200 px/s^2), drawn before the row.
float FacilityWindow::panel(Ui& ui, int i, glm::vec2 p, bool hot) {
    float& s = slide_[size_t(i)];
    s = std::clamp(s + (hot ? 300.0f : -300.0f) * ui.dt(), 0.0f, float(0x4c));
    ui.sprite("BARRACK", "ia_ov", {0x1d8 + s, p.y});
    return s;
}

void FacilityWindow::title(Ui& ui, std::string_view text) {   // FUN_006c1c70: TITLE + centred text + arrows
    const glm::vec4 a{1, 1, 1, alpha()};
    ui.sprite("BARRACK", "title", {0x138, 7}, a);
    const UiFont* f = ui.font(kWhite);
    const int w = f ? f->metrics.measure(text) : 0;
    ui_text_in_rect(ui, f, text, {0x138, 7, 0x2c4, 0x23}, a);
    ui.sprite("BARRACK", "l_arrow", {float((0x138 + 0x2c4 - w) / 2 - 0x48), 11}, a);
    ui.sprite("BARRACK", "r_arrow", {float((0x2c4 + w + 0x138) / 2 + 4), 11}, a);
}

// FUN_006c0570: B_OT / B_OV / B_DW, CHECK (label = text 1) or CROSS (text 2) at (+5, +8), label at (+0x28, +10).
bool FacilityWindow::button(Ui& ui, glm::vec4 r, std::string_view label, std::string_view mark) {
    const bool hot = state_ == Open && ui.hover(r);
    const glm::vec4 a{1, 1, 1, alpha()};
    ui.sprite("BARRACK", hot && ui.input().mouse_down ? "b_dw" : hot ? "b_ov" : "b_ot", {r.x, r.y}, a);
    ui.sprite("BARRACK", mark, {r.x + 5, r.y + 8}, a);
    ui.text(ui.font(kWhite), label, {r.x + 0x28, r.y + 10}, a);
    return state_ == Open && ui.clicked(r);
}

// FUN_006c0c00 / FUN_006ee210: I_OT (I_OV hovered), graybar (+0x5c, +0x28), the four labels at y +0x30.
void FacilityWindow::row_frame(Ui& ui, glm::vec2 p, bool hot, const char* section, int first_label) {
    ui.sprite("BARRACK", hot ? "i_ov" : "i_ot", p);
    ui.sprite("BARRACK", "graybar", p + glm::vec2(0x5c, 0x28));
    static constexpr int kX[4] = {0x60, 200, 0x130, 0x198};
    for (int k = 0; k < 4; ++k) ui.text(ui.font(kBlack), ui.text(section, first_label + k), p + glm::vec2(kX[k], 0x30));
}

void FacilityWindow::draw(Ui& ui, game::Mission& m, HudModel& hud) {
    const int want = hud.recruit_screen ? hud.recruit_screen : hud.workshop_screen;
    if (state_ == Closed) {
        if (!want) return;
        if (hud.recruit_screen) open_recruit(m, hud.recruit_screen);
        else open_workshop(m, hud.workshop_screen);
        if (state_ == Closed) {   // invalid facility: nothing opens
            hud.recruit_screen = hud.workshop_screen = 0;
            return;
        }
    }
    t_ += ui.dt();
    ui.nav_modal(workshop ? 4200 : 4100);
    if (workshop) workshop_screen(ui, m, hud);
    else recruit_screen(ui, m, hud);
    if (state_ == Opening && t_ >= kRowStagger * float(row_count_ - 1) + ui_slide_time(0x68 + 100.0f)) state_ = Open, t_ = 0.0f;
    if (state_ == Closing && t_ >= kRowStagger * float(row_count_ - 1) + ui_slide_time(float(0x68 + 100 * row_count_))) {
        state_ = Closed;   // state 3: interface off, game speed restored
        hud.recruit_screen = hud.workshop_screen = 0;
        help_ = false;
    }
}

// ---------------------------------------------------------------- barracks

// BarrUI_open 0x6bda00: gold, able citizens = clamp(pop - 0.95 pop_max, 0, 20) of the owner place, the recruiting
// unit's troop by class (BarrUI_count_troop: members, the spawn barrack's pending groups and this facility's queue for
// its troop id).
void FacilityWindow::open_recruit(game::Mission& m, int fac) {
    game::Places& pl = m.places();
    const game::Facility* f = pl.facility(fac);
    if (!f || f->kind != game::Facility::Recruit) return;
    const int who = recruiter;
    *this = FacilityWindow{};
    recruiter = who;
    facility = fac, workshop = false, row_count_ = 6, state_ = Opening;
    money = m.state().gold;
    if (const game::Place* o = pl.place(f->place)) citizens = std::clamp(int(double(o->pop) - 0.95 * double(o->pop_max)), 0, 20);
    game::World& w = m.world();
    const game::Unit* p = w.unit(recruiter);
    if (!p || p->deleted) return;
    const game::GameData& gd = m.data();
    for (int id : p->members)
        if (const game::Unit* u = w.unit(id); u && !u->deleted)
            if (const int s = slot_of_class(gd.units[size_t(u->cls)].unit_class); s >= 0) ++have[size_t(s)];
    if (const game::Facility* b = pl.facility(f->spawn_barrack); b && b->kind == game::Facility::Barrack)
        for (int g = 0; g < b->barrack.groups && b->barrack.team == p->team; ++g)
            if (const int t = b->barrack.type[size_t(g)]; t >= 1 && t <= int(gd.units.size()))
                if (const int s = slot_of_class(gd.units[size_t(t - 1)].unit_class); s >= 0) have[size_t(s)] += b->barrack.count[size_t(g)];
    for (const auto& q : f->queue) {
        if (q.team != p->team) break;
        if (q.type >= 1 && q.type <= int(gd.units.size()))
            if (const int s = slot_of_class(gd.units[size_t(q.type - 1)].unit_class); s >= 0) have[size_t(s)] += q.count;
    }
    for (int n : have) total += n;
}

// BarrUI_confirm 0x6bdc00: one queue order per class with new recruits (the recruiting unit's troop id, 9999 without
// one), gold -= cost, owner population -= recruited.
void FacilityWindow::commit_recruit(game::Mission& m) {
    game::World& w = m.world();
    const game::Unit* p = w.unit(recruiter);
    const bool player = p && !p->deleted;
    game::Places& pl = m.places();
    const game::Facility* f = pl.facility(facility);
    if (!f) return;
    for (int s = 0; s < 6; ++s)
        if (add[size_t(s)] != 0) pl.recruit(facility, m.data().unit_index(kCodes[s]) + 1, player ? p->team : 9999, add[size_t(s)]);
    money -= cost;
    m.state().gold = money;
    if (game::Place* o = pl.place(f->place)) o->pop -= recruited;
    recruited = cost = 0, add = {};
}

void FacilityWindow::recruit_screen(Ui& ui, game::Mission& m, HudModel&) {
    game::Places& pl = m.places();
    const game::Facility* f = pl.facility(facility);
    game::World& w = m.world();
    game::Unit* player = w.unit(recruiter);   // recruiting unit (DAT_00a48959)
    if (player && player->deleted) player = nullptr;
    if (!f) return close();
    const game::GameData& gd = m.data();
    const game::Places::RecruitRec* rec = pl.recruit_rec(f->list1);

    ui.sprite("BARRACK", "bg", {0, 0}, {1, 1, 1, alpha()});
    title(ui, ui.text("BARRACK", 15));
    const int max_troop = player ? m.ai().max_troop_size(*player) : 0;
    const std::string gold_word(ui.text("BARRACK", 3));

    // Rows (FUN_006bdea0): "%s (%d %s)" name / class Gold / GOLD at (+0x60, +0x10); Attack (Strength, att_r), Defense
    // (att_y, x +200), Dexterity (att_b, x +0x130) one icon per 2 points at y +0x40, step 0x12; MELEE / RANGE
    // (SRangeWeaponId +0x177 < 1) in black at (+0x198, +0x40); ICON_1 + class icon + ICON_2 at (+0x10, +0x10).
    const int rows = rec ? std::min(6, int(rec->entries.size())) : 0;
    for (int i = 0; i < rows; ++i) {
        const int type = rec->entries[size_t(i)].type;
        const game::UnitClass& uc = gd.units[size_t(type - 1)];
        const int s = slot_of(gd, type);
        const glm::vec2 p = row_pos(i);
        const bool hot = state_ == Open && ui.hover({p.x, p.y, 0x200 + 0x4c, 100});   // 0x593980(x1, y1, x2 + 0x4c, y2)
        const float off = panel(ui, i, p, hot);
        const glm::vec4 plus{0x1ec + off, p.y + 0x14, 36, 28}, minus{0x1ec + off, p.y + 0x34, 36, 28};
        ui.sprite("COMMELE2", std::string("aw_r") + (state_ == Open && ui.hover(plus) ? ui.input().mouse_down ? "_dw" : "_ov" : "_ot"),
                  {plus.x, plus.y});
        ui.sprite("COMMELE2", std::string("aw_l") + (state_ == Open && ui.hover(minus) ? ui.input().mouse_down ? "_dw" : "_ov" : "_ot"),
                  {minus.x, minus.y});
        row_frame(ui, p, hot, "BARRACK", 7);
        ui.text(ui.font(kWhite), ui_format("%1s (%2d %3s)", {uc.name, std::to_string(uc.gold), gold_word}), p + glm::vec2(0x60, 0x10));
        const int vals[3] = {uc.str, uc.def, uc.dex};
        static constexpr const char* kBar[3] = {"att_r", "att_y", "att_b"};
        static constexpr int kBx[3] = {0x60, 200, 0x130};
        for (int k = 0; k < 3; ++k)
            for (int v = 1, x = kBx[k]; v <= vals[k]; v += 2, x += 0x12) ui.sprite("BARRACK", kBar[k], p + glm::vec2(x, 0x40));
        ui.text(ui.font(kWhite), ui.text("BARRACK", uc.srange < 1 ? 4 : 5), p + glm::vec2(0x198, 0x40), ui_rgba(0, 0, 0),
                TextAlign::Left, 1.0f, TextOp::Diffuse);
        ui.sprite("MESSAGE", "icon_1", p + glm::vec2(0x10, 0x10));
        if (s >= 0) ui.sprite("MESSAGE", kRowIcon[s], p + glm::vec2(0x10, 0x10));
        ui.sprite("MESSAGE", "icon_2", p + glm::vec2(0x10, 0x10));

        // BarrUI_handle_input: aw_r adds (troop max, citizens, gold), aw_l removes; pad: left / right on the row.
        if (state_ != Open || s < 0) continue;
        const int dx = hot ? ui.nav_adjust() : 0;
        if (ui.clicked(plus) || dx > 0) {
            if ((player && max_troop <= total) || citizens < 1 || money < cost + uc.gold) continue;
            cost += uc.gold, --citizens, ++recruited, ++total, ++add[size_t(s)];
        } else if (ui.clicked(minus) || dx < 0) {
            if (add[size_t(s)] < 1) continue;
            cost -= uc.gold, ++citizens, --recruited, --total, --add[size_t(s)];
        }
    }

    // Info frame (FUN_006c1180 + FUN_006bdea0 tail).
    const glm::vec2 o{frame_x(), kFrame.y};
    const int fx = int(o.x), fy = int(o.y);
    ui.sprite("BARRACK", "frame", o);
    auto bar = [&](const char* s, int y) { ui.sprite("BARRACK", s, {o.x + 0x13, o.y + float(y)}); };
    auto label = [&](std::string_view s, int y) { ui.text(ui.font(kWhite), s, {o.x + 0x17, o.y + float(y)}); };
    bar("titlebar", 0xf), label(ui.text("BARRACK", 11), 0x11);
    if (rec)
        if (const UiFont* bf = ui.font(kBlack))   // on the black frame: glyph alpha in white (0x626320 colour 0xffffff)
            ui.text_block(bf, rec->location, {o.x + 0x16, o.y + 0x2a, 0x152 - 0x16, 0x5c - 0x2a}, kUiWhite, 0, TextAlign::Center, TextOp::Diffuse);
    if (const game::Place* own = pl.place(f->place))
        text_rect(ui, kWhite, ui_format(ui.text("BARRACK", 6), {std::to_string(own->pop)}), {fx + 0x16, fy + 0x5e, fx + 0x152, fy + 0x72});
    bar("goldbar", 0x73), bar("titlebar", 0x7f), label(ui.text("BARRACK", 12), 0x81);
    for (int i = 0; i < citizens; ++i)
        ui.sprite("BARRACK", "citizen", {o.x + 0x16 + float(0x1e * (i % 10)), o.y + (i < 10 ? 0x96 : 0xbe)});
    bar("goldbar", 0xf7);
    if (player) bar("titlebar", 0x103), label(player->name, 0x105);
    bar("titlebar", 0x11f), label(ui_format(ui.text("BARRACK", 13), {std::to_string(max_troop)}), 0x121);
    int n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int s = 0; s < 6; ++s)
            for (int k = 0; k < (pass ? add : have)[size_t(s)]; ++k, ++n)   // wraps once, after the tenth icon
                ui.sprite("BARRACK", std::string(pass ? "n_" : "c_") + kTroop[s],
                          {o.x + 0x16 + float(0x1e * (n < 10 ? n : n - 10)), o.y + (n < 10 ? 0x13b : 0x163)});
    bar("goldbar", 0x19a), bar("titlebar", 0x1a6), label(ui.text("BARRACK", 14), 0x1a8);
    text_rect(ui, kWhite, std::to_string(cost) + " " + gold_word, {fx + 0x16, fy + 0x1c2, fx + 0x152, fy + 0x1de});
    ui.sprite("BARRACK", "gold", {o.x + 0x100, o.y + 0x19c});
    bar("goldbar", 0x1d6), bar("titlebar", 0x1e2), label(gold_word, 0x1e4);
    text_rect(ui, kWhite, std::to_string(money) + " " + gold_word, {fx + 0x16, fy + 0x1fe, fx + 0x152, fy + 0x21a});
    ui.sprite("BARRACK", "gold", {o.x + 0x100, o.y + 0x1d8});

    // RECRUIT (0x28b, 0x2be) / CANCEL (0x32e, 0x2be); Enter = confirm, Esc = leave (0x6c0310(0)).
    const bool ok = button(ui, {0x28b, 0x2be, 156, 44}, ui.text("BARRACK", 1), "check");
    const bool cancel = button(ui, {0x32e, 0x2be, 156, 44}, ui.text("BARRACK", 2), "cross");
    if (state_ != Open) return;
    if (ok || ui.key(SDL_SCANCODE_RETURN)) commit_recruit(m), close();
    else if (cancel || ui.key(SDL_SCANCODE_ESCAPE)) close();
}

}  // namespace joa::engine
