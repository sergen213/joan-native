// Main menu, difficulty picker and their shared backdrop (MENU1024 + MENU_B, 1024x768 design units).
// RE: init FUN_00709090, tick/animation FUN_0070acb0, draw FUN_0070a8b0, input FUN_0070a370, button draw
// callbacks FUN_00707e30 (L_*) / FUN_00708760 (M_*). Evidence and timings: work/ui/notes_MenuFrontEnd.md.
#include "engine/ui_screens.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>

namespace joa::engine {
namespace {

constexpr float kDone = 1e9f;  // sequence time far past every animation

float fade(float t, float start, float rate) { return std::clamp((t - start) * rate, 0.0f, 1.0f); }

// Constant-speed move (FUN_0061e020, accel 0): integer pixel steps, clamped at the target.
float slide(float t, float start, float from, float to, float speed) {
    const float d = std::floor(std::max(t - start, 0.0f) * speed);
    return from < to ? std::min(from + d, to) : std::max(from - d, to);
}

// Opening sequence (FUN_0070acb0 after its 1000 ms wait at +0x342a); each step starts when the previous ends.
constexpr float kMain = 1.0f;                     // "main" alpha 0->1 at 3/s
constexpr float kAlab = kMain + 1.0f / 3.0f;      // alab_l -188->0, alab_r 1024->836 at 1200 px/s
constexpr float kLea = kAlab + 188.0f / 1200.0f;  // lea_l -264->0, lea_r 1024->760 at 1600 px/s; alab_s 4/s
constexpr float kGold = kLea + 0.25f;             // gold_l/gold_r 4/s once alab_s = 1 (lea is in by then)
constexpr float kLogoBar = kGold + 0.25f;         // logo_bar 1024->0 (y 16) at 7680 px/s
constexpr float kLogo = kLogoBar + 1024.0f / 7680.0f;  // logo 4/s
constexpr float kSphere = kLogo + 0.25f;          // sphere 4/s and joan 1/s
constexpr float kStripe = kSphere + 0.7f;         // stripe 2/s once joan > 0.7 (copyright uses its alpha)
constexpr float kButtons = kStripe + 0.5f;        // first button once stripe = 1

// Button slide-in: FUN_0061e020(0, y, 3500 px/s, -1500 px/s^2), accel -4000 once x >= -409.6; the next button
// starts when this one passes x > -301. Closed form of that motion (python in the notes).
constexpr float kStagger = 0.061461f, kSlideIn = 0.157450f;
float button_x(float t) {
    constexpr float t1 = 0.029443f, v1 = 3455.836f;
    if (t <= 0.0f) return -512.0f;
    if (t >= kSlideIn) return 0.0f;
    if (t < t1) return std::floor(-512.0f + 3500.0f * t - 750.0f * t * t);
    t -= t1;
    return std::min(0.0f, std::floor(-409.6f + v1 * t - 2000.0f * t * t));
}

struct ButtonArt {
    const char *ot, *ov, *dw, *mark_l, *mark_r, *font;
    int h, text_dy, mark_dl, mark_dr, mark_dy;
};
// FUN_00707e30: 512x72 L_* art, Goudy_bold_24, text at y+11, L_MARK at centre -w/2-40 and +w/2, y+12.
constexpr ButtonArt kLarge{"l_ot", "l_ov", "l_dw", "l_mark", "l_mark", "Goudy_bold_24", 72, 11, -40, 0, 12};
// FUN_00708760: 512x48 M_* art, Goudy_bold_20, text at y+5, M_LA at centre -w/2-73, M_RA at +w/2+5, y+14.
constexpr ButtonArt kMedium{"m_ot", "m_ov", "m_dw", "m_la", "m_ra", "Goudy_bold_20", 48, 5, -73, 5, 14};

// Text-box button (rect x..x+512, y..y+h inclusive). Normal: *_ot, black text. Hover: *_ov + marks, white text.
// Held: *_dw + marks, white text moved 1 px right/down. Fires on release when `active`.
bool button(Ui& ui, const ButtonArt& a, glm::vec2 p, std::string_view label, bool active) {
    const glm::vec4 r{p.x, p.y, 513.0f, float(a.h + 1)};
    const bool over = ui.hover(r), down = over && ui.input().mouse_down;
    ui.sprite("MENU_B", down ? a.dw : over ? a.ov : a.ot, p);
    const UiFont* f = ui.font(a.font);
    const int w = f ? f->metrics.measure(label) : 0;
    const float c = p.x + 256.0f, push = down ? 1.0f : 0.0f;
    if (over) {
        ui.sprite("MENU_B", a.mark_l, {c - float(w / 2) + float(a.mark_dl), p.y + float(a.mark_dy)});
        ui.sprite("MENU_B", a.mark_r, {c + float(w / 2) + float(a.mark_dr), p.y + float(a.mark_dy)});
    }
    // 0x6255d0 selectors (0,1,2,0): RGB from the colour, alpha = glyph alpha (TextOp::Diffuse).
    ui.text(f, label, {c - float(w / 2) + push, p.y + float(a.text_dy) + push}, over ? kUiWhite : ui_rgba(0, 0, 0),
            TextAlign::Left, 1.0f, TextOp::Diffuse);
    return active && ui.clicked(r);
}

glm::vec4 alpha(float a) { return {1.0f, 1.0f, 1.0f, a}; }

// Button slide-out: FUN_0061e020(-512, y, 10 px/s, accel 12000); the next button starts once x <= -51.2.
constexpr float kOutStagger = 0.091546f, kOutTime = 0.291287f;
constexpr float kMainOut = 4.0f * kOutStagger + kOutTime;  // fifth button at -512
float out_x(float e) {
    e = std::max(e, 0.0f);
    return std::max(-512.0f, -std::floor(10.0f * e + 6000.0f * e * e));
}

// Reverse sequence (before Credits / starting a campaign), measured from the sphere's fade-out; each step starts
// when the previous one finished, fades at 4/s, slides back to the start position at the opening speed.
constexpr float kRevLogo = 0.25f, kRevLogoBar = 0.5f, kRevGold = kRevLogoBar + 1024.0f / 7680.0f;
constexpr float kRevLea = kRevGold + 0.25f;               // lea out + alab_s fade
constexpr float kRevAlab = kRevLea + 0.25f;               // once alab_s = 0 (lea is out by then)
constexpr float kRevMain = kRevAlab + 188.0f / 1200.0f;   // "main" fade
constexpr float kRevDone = kRevMain + 0.25f;

// Backdrop state: opening-sequence time, reverse-sequence time, Joan and stripe alpha.
struct Stage {
    float seq = kDone, rev = -kDone, joan = 1.0f, stripe = 1.0f;
};
float in_out(const Stage& s, float in, float rate, float out) { return fade(s.seq, in, rate) * (1.0f - fade(s.rev, out, 4.0f)); }
float pos(const Stage& s, float in, float from, float to, float speed, float out) {
    return s.rev >= out ? slide(s.rev, out, to, from, speed) : slide(s.seq, in, from, to, speed);
}

// FUN_0070a8b0 layers below the buttons: black, main, sphere, joan, lea_l, lea_r, gold_l, gold_r, (Load/Options
// pages), logo_bar, logo. Final positions are the slide targets of FUN_0070acb0.
void backdrop_lower(Ui& ui, const Stage& s) {
    ui.fill({0, 0, 1024, 768}, {0, 0, 0, 1});
    ui.sprite("MENU1024", "main", {0, 0}, alpha(in_out(s, kMain, 3.0f, kRevMain)));
    ui.sprite("MENU1024", "sphere", {32, 0}, alpha(in_out(s, kSphere, 4.0f, 0.0f)));
    ui.sprite("MENU1024", "joan", {-45, 150}, alpha(s.joan));
    ui.sprite("MENU1024", "lea_l", {pos(s, kLea, -264, 0, 1600, kRevLea), 0});
    ui.sprite("MENU1024", "lea_r", {pos(s, kLea, 1024, 760, 1600, kRevLea), 0});
    ui.sprite("MENU1024", "gold_l", {45, 0}, alpha(in_out(s, kGold, 4.0f, kRevGold)));
    ui.sprite("MENU1024", "gold_r", {755, 0}, alpha(in_out(s, kGold, 4.0f, kRevGold)));
    ui.sprite("MENU1024", "logo_bar", {pos(s, kLogoBar, 1024, 0, 7680, kRevLogoBar), 16});
    ui.sprite("MENU1024", "logo", {288, 26}, alpha(in_out(s, kLogo, 4.0f, kRevLogo)));
}

// Layers above the buttons: alab_s, alab_l, alab_r, stripe, then MAINMENU 10 in "Goudy12 Bold Black" centred in
// (0, stripe.y2-8)-(1024, stripe.y2+12) with the stripe's alpha (FUN_00626130, stripe y2 = 744+8-1).
void backdrop_upper(Ui& ui, const Stage& s) {
    ui.sprite("MENU1024", "alab_s", {0, 0}, alpha(in_out(s, kLea, 4.0f, kRevLea)));
    ui.sprite("MENU1024", "alab_l", {pos(s, kAlab, -188, 0, 1200, kRevAlab), 0});
    ui.sprite("MENU1024", "alab_r", {pos(s, kAlab, 1024, 836, 1200, kRevAlab), 0});
    ui.sprite("MENU1024", "stripe", {240, 744}, alpha(s.stripe));
    if (const UiFont* f = ui.font("Goudy12 Bold Black"); f && s.stripe > 0.0f) {
        const std::string_view str = ui.text("MAINMENU", 10);
        const int x = (1025 - f->metrics.measure(str)) / 2, y = 743 + (21 - f->metrics.line_height()) / 2;
        ui.text(f, str, {float(x), float(y)}, {0, 0, 0, s.stripe}, TextAlign::Left, 1.0f, TextOp::Diffuse);
    }
}

}  // namespace

// Load/Options opened from the main menu: everything settled, Joan and the stripe faded out (FUN_0070acb0 fades
// both at 4/s before opening those pages); Main/Difficulty keep them.
void menu_frontend_backdrop(Ui& ui, const MenuModel& m) {
    const float keep = m.screen == MenuScreen::Main || m.screen == MenuScreen::Difficulty ? 1.0f : 0.0f;
    const Stage s{kDone, -kDone, keep, keep};
    backdrop_lower(ui, s);
    backdrop_upper(ui, s);
}

void menu_main(Ui& ui, MenuModel& m, std::vector<UiCommand>& out) {
    const float t = m.time;
    Stage s{t, -kDone, fade(t, kSphere, 1.0f), fade(t, kStripe, 2.0f)};
    float start = kButtons;
    if (m.main_entry == 1) s = {kDone, -kDone, fade(t, 0.0f, 1.0f), fade(t, 0.7f, 2.0f)}, start = 1.2f;
    if (m.main_entry == 2) s = {}, start = 0.0f;
    // SE (FUN_0070acb0): MENU_SLIDE_IN as "main" starts fading in (full opening only), BUTTON_SLIDE_MANY as the
    // first button starts sliding in; the exit click plays BUTTON_SLIDE_MANY again (slide-out start).
    if (m.frontend_exit < 0) {
        if (m.main_entry == 0) ui.sound_at("MENU_SLIDE_IN", t, kMain);
        ui.sound_at("BUTTON_SLIDE_MANY", t, start);
    }

    // Exit (FUN_0070acb0 with +0x3412 set): the buttons slide out; then New Campaign -> Difficulty and Quit at
    // once, Load/Options after Joan + stripe fade out at 4/s, Credits after that plus the reverse sequence.
    const int exit = m.frontend_exit;
    const float e = exit >= 0 ? t - m.frontend_exit_time : -1.0f;
    constexpr float kDoneAt[5] = {kMainOut, kMainOut + 0.25f, kMainOut + 0.25f, kMainOut + 0.25f + kRevDone, kMainOut};
    if (exit >= 1 && exit <= 3) s.joan = s.stripe = 1.0f - fade(e, kMainOut, 4.0f);
    if (exit == 3) s.rev = e - kMainOut - 0.25f;

    backdrop_lower(ui, s);
    // Input only once every button is in and no exit runs (FUN_0070a370 ignores input while +0x341e = animating).
    const bool ready = exit < 0 && t >= start + 4.0f * kStagger + kSlideIn;
    static constexpr int kLabel[5] = {1, 2, 3, 11, 4};  // New Campaign, Load a Campaign, Options, Credits, Quit
    for (int i = 0; i < 5; ++i) {
        const float x = exit >= 0 ? out_x(e - float(i) * kOutStagger) : button_x(t - start - float(i) * kStagger);
        if (button(ui, kLarge, {x, 224.0f + 76.0f * float(i)}, ui.text("MAINMENU", kLabel[i]), ready))
            m.frontend_exit = i, m.frontend_exit_time = t, ui.sound("BUTTON_SLIDE_MANY");
    }
    backdrop_upper(ui, s);

    if (exit < 0 || e < kDoneAt[exit]) return;
    if (exit == 4) {  // stays slid out; the command fires once
        if (e - ui.dt() < kDoneAt[4]) out.push_back({UiCommand::Type::Quit, 0, {}, {}});
        return;
    }
    m.frontend_exit = -1;
    switch (exit) {
        case 0: m.screen = MenuScreen::Difficulty, m.main_entry = 2; break;
        case 1: m.screen = MenuScreen::Load, m.previous = MenuScreen::Main, m.main_entry = 1; break;
        case 2: m.screen = MenuScreen::Options, m.options_tab = OptionsTab::Root, m.previous = MenuScreen::Main, m.main_entry = 1; break;
        default: m.screen = MenuScreen::Credits, m.main_entry = 0; break;
    }
}

// Difficulty picker (MAINMENU 5..9) on the settled backdrop; buttons at y = 224 + 52i slide in like the main ones.
void menu_difficulty(Ui& ui, MenuModel& m, std::vector<UiCommand>& out) {
    const float t = m.time;
    const int exit = m.frontend_exit;
    const float e = exit >= 0 ? t - m.frontend_exit_time : -1.0f;
    // EASY..VERY HARD: buttons slide out; once the 4th (+0x20fe) is out the stripe fades at 4/s and Joan at 1/s,
    // the sphere follows when stripe = 0 and Joan < 0.3 (+0.7 s), then the reverse sequence; NewGame at its end.
    // CANCEL / Escape: buttons slide out, then Main (its buttons slide back in).
    constexpr float kFade = 3.0f * kOutStagger + kOutTime, kStart = kFade + 0.7f + kRevDone;
    Stage s;
    if (exit >= 0 && exit < 4) s = {kDone, e - kFade - 0.7f, 1.0f - fade(e, kFade, 1.0f), 1.0f - fade(e, kFade, 4.0f)};

    if (exit < 0) ui.sound_at("BUTTON_SLIDE_MANY", t, 0.0f);       // buttons start sliding in (+0x3414 branch)
    if (exit >= 0 && exit < 4) ui.sound_at("MENU_SLIDE_OUT", e, kFade);  // stripe/Joan fade out starts
    backdrop_lower(ui, s);
    const bool ready = exit < 0 && t >= 4.0f * kStagger + kSlideIn;
    for (int i = 0; i < 5; ++i) {
        const float x = exit >= 0 ? out_x(e - float(i) * kOutStagger) : button_x(t - float(i) * kStagger);
        if (button(ui, kMedium, {x, 224.0f + 52.0f * float(i)}, ui.text("MAINMENU", 5 + i), ready))
            m.frontend_exit = i, m.frontend_exit_time = t, ui.sound("BUTTON_SLIDE_MANY");
    }
    backdrop_upper(ui, s);

    if (ready && m.frontend_exit < 0 && ui.shortcut(SDL_SCANCODE_ESCAPE))  // CANCEL hotkey
        m.frontend_exit = 4, m.frontend_exit_time = t, ui.sound("BUTTON_SLIDE_MANY");
    if (exit == 4 && e >= kMainOut) m.screen = MenuScreen::Main, m.frontend_exit = -1;
    if (exit >= 0 && exit < 4 && e >= kStart && e - ui.dt() < kStart) out.push_back({UiCommand::Type::NewGame, exit, {}, {}});
}

}  // namespace joa::engine
