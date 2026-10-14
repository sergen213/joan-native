// MGEXP HUD: EXP counter, level-up banner, combo HUD. The exe draws them right after the PLVLBAR in FUN_006f9b60
// (FUN_006fe660, FUN_006ffed0, ComboHUD_update) with en2d sprite draw 0x633500 (MGEXP table; trailing right/bottom
// args = stretched rect). Positions: digits/EXPTEXT use W-n / H-n, the level-up and combo banners the literal
// 0x400 (and the EXP fly-out target (0x3fc, 0x212)), all equal to W/H-relative at 1024x768: everything is drawn
// in the bottom-right frame. Integer casts = __ftol truncation.
#include "engine/hud_exp.hpp"

#include "engine/ui_core.hpp"
#include "game/mission.hpp"

#include <algorithm>
#include <string>

namespace joa::engine {
namespace {

constexpr int W = 1024, H = 768;

int digit_count(int v) { return int(std::to_string(v).size()); }  // strlen(itoa) 0x58fca0
int digit(int v, int i) {                                         // (v % 10^(i+1)) / 10^i
    int p = 10;
    for (int k = 0; k < i; ++k) p *= 10;
    return v % p / (p / 10);
}
glm::vec4 alpha(int a) { return {1.0f, 1.0f, 1.0f, float(a) / 255.0f}; }
int ramp(float el, float ms) { return int(std::min(el, ms) / ms * 255.0f); }  // ftol(min(dt, ms) / ms * 255)

void stretched(Ui& ui, const std::string& name, int x, int y, int r, int b, int a) {
    ui.sprite(ui.sprite("MGEXP", name), glm::vec4(x, y, r - x, b - y), alpha(a));
}

// Banner scaled by s about its natural rect (0x400 - w, cy - h/2 .. ) (LEVELUP*, COMBO*).
void banner(Ui& ui, const char* name, int cy, int a) {
    const UiSprite* sp = ui.sprite("MGEXP", name);
    if (!sp) return;
    const float s = float(a) / 255.0f;
    const int w = int(sp->size.x), h = int(sp->size.y), sw = int(float(w) * s), sh = int(float(h) * s);
    const int x = 0x400 - w / 2 - sw / 2, y = cy + h / 2 - sh / 2;
    ui.sprite(sp, glm::vec4(x, y, sw, sh), alpha(a));
}

// ---- EXP counter: FUN_006fe660 ----
void exp_counter(Ui& ui, HudModel::Exp& fx, bool roll_step) {
    if (fx.queue.empty()) return;
    HudModel::Exp::Entry& e = fx.queue.front();
    // EXPTEXT slides in from the right edge (x = W - 125 t) over 200 ms; EXPTEXT2 once settled. The last entry
    // slides back out in state 4 (t = 1 - ...). The exe also computes alpha t*255 here but draws opaque.
    if (e.state == 0 || e.state == 4) {
        float t = std::min(fx.now - fx.slide_t, 200.0f) / 200.0f;
        if (fx.queue.size() == 1 && e.state == 4) t = 1.0f - t;
        ui.sprite("MGEXP", t == 1.0f ? "EXPTEXT2" : "EXPTEXT", {float(int(float(W) - 125.0f * t)), float(H - 0x104)});
    } else {
        ui.sprite("MGEXP", "EXPTEXT2", {float(W - 0x7d), float(H - 0x104)});
    }
    const int n = e.digits;
    if (e.state == 0) {
        // Roll: digit i (ones first) counts up by 1 (by 2 while >= 2 short) per step; higher digits appear only once
        // the lower ones match. The exe steps once per drawn frame of its 30 Hz loop (roll_step: 1000/30 ms real time).
        for (int i = 0; i < n; ++i) {
            const int rd = digit(e.roll, i), td = digit(e.value, i);
            int p = 1;
            for (int k = 0; k < i; ++k) p *= 10;
            ui.sprite("MGEXP", "E" + std::to_string(rd) + "_1", {float(W - 0x50 - 0x50 * i), float(H - 0x140)});
            if (rd != td) {
                if (roll_step) e.roll += td - rd < 2 ? p : 2 * p;
                return;
            }
            if (i + 1 == n) e.state = 1;
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        int a = 0xff;
        std::string name = "E" + std::to_string(digit(e.roll, i)) + "_";
        if (e.state == 1 || e.state == 3) {  // one-frame "_2" flash; state 2 -> 3 -> 4 may pass in one frame
            name += '2';
            if (i + 1 == n) {
                if (fx.queue.size() == 1 && e.state == 3) fx.slide_t = fx.now;
                if (e.state == 3) e.t = fx.now;
                ++e.state;
            }
        } else if (e.state == 2) {
            name += '3';
            if (e.t < 0.0f) e.t = fx.now;
            if (fx.now - e.t >= 1500.0f) ++e.state;  // cmp 0x5dc
        } else {
            name += '1';
            a = 255 - ramp(fx.now - e.t, 300.0f);
        }
        if (a == 0) {  // faded: drop the entry (the next one starts rolling next frame)
            fx.queue.erase(fx.queue.begin());
            return;
        }
        if (e.state == 4) {  // shrink with the alpha and fly from the digits' centre toward (0x3fc, 0x212)
            const float s = float(a) / 255.0f;
            const int left = W - 0x50 - (n - 1) * 0x50, cx0 = 0x400 - (0x400 - left) / 2, cy0 = H - 0x126;
            const int cx = int(float(cx0) + float(0x3fc - cx0) * (1.0f - s));
            const int cy = int(float(cy0) + float(0x212 - cy0) * (1.0f - s));
            const int sw = int(s * 80.0f), sh = int(s * 52.0f);
            const int x = int(float(cx) - (float(n) / 2.0f + float(i) - float(n) + 1.0f) * float(sw)), y = cy - sh / 2;
            stretched(ui, name, x, y, x + sw, y + sh, a);
        } else {
            ui.sprite("MGEXP", name, {float(W - 0x50 - 0x50 * i), float(H - 0x140)});
        }
    }
}

// ---- level-up banner: FUN_006ffed0 ----
// LEVEL<k> + L<d><suffix> digits scaled by s: the word left of the digits, digits right-aligned at 0x400 and
// squeezed to the word's height (the exe passes LEVEL's height as the digit rect bottom).
void level_word(Ui& ui, HudModel::Exp& fx, const char* name, const char* suffix, int a) {
    const UiSprite* sp = ui.sprite("MGEXP", name);
    if (!sp) return;
    const float s = float(a) / 255.0f;
    const int n = digit_count(fx.level), w = int(sp->size.x), h = int(sp->size.y);
    const int sw = int(float(w) * s), sh = int(float(h) * s);
    const int cx = 0x400 - w / 2, cy = H - 400 + h / 2;
    const int x = int(float(cx - sw / 2) - float(n * 0x50) * s), y = cy - sh / 2;
    ui.sprite(sp, glm::vec4(x, y, sw, sh), alpha(a));
    for (int i = 0; i < n; ++i) {
        const int dx = int(1024.0f - float((i + 1) * 0x50) * s), dr = int(float(dx) + 80.0f * s);
        stretched(ui, "L" + std::to_string(digit(fx.level, i)) + suffix, dx, y, dr, y + sh, a);
    }
}

void level_up(Ui& ui, HudModel::Exp& fx) {
    int& st = fx.level_state;
    if (!st) return;
    const float el = fx.now - fx.level_t;
    constexpr int cy = H - 400;
    if (st == 1) {  // LEVELUP0 grows / fades in over 500 ms
        const int a = ramp(el, 500.0f);
        banner(ui, "LEVELUP0", cy, a);
        if (a == 0xff) ++st;
    } else if (st == 2) {
        banner(ui, "LEVELUP1", cy, 0xff), ++st;
    } else if (st == 3) {
        banner(ui, "LEVELUP2", cy, 0xff), fx.level_t = fx.now, ++st;
    } else if (st == 4) {  // hold 2200 ms (cmp 0x898)
        banner(ui, "LEVELUP3", cy, 0xff);
        if (el >= 2200.0f) ++st, fx.level_t = fx.now;
    } else if (st == 5) {  // LEVELUP3 shrinks out while "LEVEL n" grows in, 300 ms
        banner(ui, "LEVELUP3", cy, 255 - ramp(el, 300.0f));
        const int a = ramp(el, 300.0f);
        level_word(ui, fx, "LEVEL1", "_1", a);
        if (a == 0xff) ++st;
    } else if (st == 6) {
        level_word(ui, fx, "LEVEL2", "_2", 0xff), fx.level_t = fx.now, ++st;
    }
    if (st == 7) {  // separate `if` in the exe: drawn in the same frame state 6 ends
        level_word(ui, fx, "LEVEL3", "_3", 0xff);
        if (fx.now - fx.level_t >= 2200.0f) ++st, fx.level_t = fx.now;
    } else if (st == 8) {
        level_word(ui, fx, "LEVEL2", "_2", 0xff), fx.level_t = fx.now, ++st;
    } else if (st == 9) {
        const int a = 255 - ramp(el, 300.0f);
        level_word(ui, fx, "LEVEL1", "_1", a);
        if (a == 0) st = 0;
    }
}

// ---- combo HUD: ComboHUD_update 0x702740 ----
void combo(Ui& ui, HudModel::Exp& fx) {
    int& st = fx.combo_state;
    if (!st) return;
    const float el = fx.now - fx.combo_t;
    constexpr int cy = H - 0x1c7;
    if (st == 1) {  // COMBO0 grows / fades in over 500 ms
        const int a = ramp(el, 500.0f);
        banner(ui, "COMBO0", cy, a);
        if (a == 0xff) ++st;
    } else if (st == 2) {
        banner(ui, "COMBO1", cy, 0xff), ++st;
    } else if (st == 3) {
        banner(ui, "COMBO1", cy, 0xff);
        // count C<d>-1 right-aligned at 864, 80 wide, stretched to 62 high around COMBO1's top edge (cy at s = 1)
        for (int i = 0, n = digit_count(fx.combo); i < n; ++i) {
            const int x = 864 - (i + 1) * 0x50;
            stretched(ui, "C" + std::to_string(digit(fx.combo, i)) + "-1", x, cy - 0x1f, x + 0x50, cy + 0x1f, 0xff);
        }
        if (fx.combo < fx.combo_hits) fx.combo = fx.combo_hits, fx.combo_t = fx.now;
        else if (el >= 2000.0f) ++st, fx.combo_t = fx.now, fx.combo = 0;  // the exe also zeroes DAT_00a5ba62 here
    } else if (st == 4) {
        banner(ui, "COMBO0", cy, 0xff), ++st;
    } else if (st == 5) {  // fade / shrink out 300 ms
        const int a = 255 - ramp(el, 300.0f);
        banner(ui, "COMBO0", cy, a);
        if (a == 0) st = 0;
    }
}

}  // namespace

void hud_exp_add(HudModel::Exp& fx, int xp) {
    if (fx.queue.size() >= 9) return;
    if (fx.queue.empty()) fx.slide_t = fx.now;
    fx.queue.push_back({xp, digit_count(xp)});
}

void hud_exp_level_up(HudModel::Exp& fx, int level) { fx.level_state = 1, fx.level_t = fx.now, fx.level = level; }

void hud_exp_combo(HudModel::Exp& fx, int hits) { fx.combo_state = 1, fx.combo_t = fx.now, fx.combo = hits; }

void hud_exp_events(const std::vector<game::GameEvent>& events, game::Mission& m, HudModel& hud) {
    game::World& w = m.world();
    HudModel::Exp& fx = hud.exp;
    for (const game::GameEvent& e : events) {
        if (e.unit != w.player_id()) continue;
        if (e.type == game::Ev::XpGain) hud_exp_add(fx, e.value);         // Unit_add_exp 0x5229a0, player only
        else if (e.type == game::Ev::LevelUp) hud_exp_level_up(fx, e.value);  // Unit_check_level_up, player only
    }
    // Unit_onMessage 0x75d540: every player melee hit with the count > 1 calls ComboHUD_show (restarting its fade-in).
    if (w.combo_hits > fx.combo_hits && w.combo_hits > 1) hud_exp_combo(fx, w.combo_hits);
    fx.combo_hits = w.combo_hits;
}

void hud_exp(Ui& ui, HudModel& m) {
    HudModel::Exp& fx = m.exp;
    if (!m.sim_paused) fx.now += ui.dt() * 1000.0f;  // DAT_00a500ba: the timers stand while the game is paused
    if (m.rts_mode) {  // DAT_00a5b9f8 == 4: none drawn, the combo HUD resets
        fx.combo_state = 0, fx.combo = 0;
        return;
    }
    // The roll steps once per drawn frame (also while paused); the exe draws at 30 Hz (FrameTimer, Sys_main_loop).
    constexpr float kFrameMs = 1000.0f / 30.0f;
    fx.roll_acc += ui.dt() * 1000.0f;
    const bool roll_step = fx.roll_acc >= kFrameMs;
    if (roll_step) fx.roll_acc = std::min(fx.roll_acc - kFrameMs, kFrameMs);
    ui.frame(Anchor::BottomRight);
    exp_counter(ui, fx, roll_step);
    level_up(ui, fx);
    combo(ui, fx);
}

}  // namespace joa::engine
