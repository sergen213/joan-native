// HudGaps crit: critical-attack HUD Crit_drawHUD 0x45fae0 (HUD object 0xa4f820) and the kill interface
// counter panel FUN_006df180 (object 0xa50518). Positions are the exe's constants in 1024x768 units.
#include "engine/hud_crit.hpp"

#include "game/mission.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace joa::engine {
namespace {

glm::vec4 alpha(int a) { return ui_rgba(255, 255, 255, a); }

// Natural-size sprite centred on (cx, cy) as the exe does it: (W/2 - w/2, cy - h/2), integer halves.
void centred(Ui& ui, const UiSprite* s, float cy, int a = 255) {
    if (s) ui.sprite(s, glm::vec2(512 - int(s->size.x) / 2, int(cy) - int(s->size.y) / 2), alpha(a));
}

// One result banner (CA / CK / CF) centred on (W/2, cy) and its three glows (natural size, same centre).
// t = ms since CRITATT. Returns false once GLOW03 has run out (the original clears the flag then).
bool banner(Ui& ui, const HudModel::Crit& c, float t, const char* set, const char* name, const char* glow, float cy) {
    const float f1 = float(c.fade_in1), f2 = float(c.fade_in2);
    if (const UiSprite* s = ui.sprite(set, name)) {
        const float w = s->size.x, h = s->size.y;
        auto scaled = [&](float k, int a) {   // stretched to (x1, y1)-(x2, y2) inclusive, __ftol truncation
            const int x1 = int(512 - k * w / 2), y1 = int(cy - k * h / 2), x2 = int(512 + k * w / 2), y2 = int(cy + k * h / 2);
            ui.sprite(s, glm::vec4(x1, y1, x2 - x1 + 1, y2 - y1 + 1), alpha(a));
        };
        if (t <= f1) {   // FADE_IN1: grow 0 -> 1.1x while fading in
            const float r = std::min(t / f1, 1.0f);
            scaled(r * 1.1f, int(255.0f * r));
        } else if (t <= f1 + f2) {   // FADE_IN2: settle 1.1x -> 1.0x
            scaled(1.1f - std::min((t - f1) / f2, 1.0f) / 10.0f, 255);
        } else {
            centred(ui, s, cy);
        }
    }
    auto glow_n = [&](int n, float k) {   // <glow>0n at alpha 255 k
        centred(ui, ui.sprite(set, std::string(glow) + char('0' + n)), cy, int(k * 255.0f));
    };
    float a = f1 + f2;   // GLOW_IN1: GLOW01 in; GLOW_IN2: GLOW01 -> GLOW02; GLOW_IN3: GLOW02 -> GLOW03
    for (int n = 1; n <= 3; ++n) {
        const float d = float(c.glow_in[n - 1]);
        if (t > a && t <= a + d) {
            const float k = std::min((t - a) / d, 1.0f);
            if (n > 1) glow_n(n - 1, 1.0f - k);
            glow_n(n, k);
        }
        a += d;
    }
    return t <= a;
}

void crit(Ui& ui, HudModel::Crit& c, bool sim_paused) {
    if (!c.enabled) return;
    // Key icons (x 0x12 + 0x2d i, y H-0xa1): Light/Heavy lit for the keys entered, *OFF for the rest; the
    // length counts the 'A'/'B' keys of the combo (Crit_startSequence 0x462970).
    ui.frame(Anchor::BottomLeft);
    const int len = int(std::count_if(c.keys.begin(), c.keys.end(), [](char k) { return k == 'A' || k == 'B'; }));
    for (int i = 0; i < len && i < int(c.keys.size()); ++i) {
        const char k = c.keys[std::size_t(i)];
        if (k != 'A' && k != 'B') continue;
        const bool lit = i < c.progress;
        ui.sprite("CRITICAL", k == 'A' ? (lit ? "LIGHT" : "LIGHTOFF") : (lit ? "HEAVY" : "HEAVYOFF"), {float(i * 45 + 18), 768.0f - 161.0f});
    }
    if (!c.banner) return;
    // Result banner: CF (+0x1a) over CK (+0x1b, centred) over CA (+0x19, H/2 - 100); CK and CF share CRITATT's clock.
    ui.frame(Anchor::Center);
    if (!sim_paused) c.banner_ms += ui.dt() * 1000.0f;   // game ms (DAT_00a500ba - +0x2f): stands while the game is paused
    const bool on = c.banner == 3   ? banner(ui, c, c.banner_ms, "CRITI_F", "CF", "CFGLOW0", 384)
                    : c.banner == 2 ? banner(ui, c, c.banner_ms, "CRITI_K", "CK", "CKGLOW0", 384)
                                    : banner(ui, c, c.banner_ms, "CRITICAL", "CA", "GLOW0", 384 - 100);
    if (!on) c.banner = 0;
}

// Kill interface FUN_006df180: panel + icon at (0x1ac, 8), the second counter at y 0x33 when the first is
// shown; text in (500, y+4)-(0x250, y+28), font obj 0xa48f70 "Celtic 10pt English", texture colours
// (selectors 0,0,0,0 ignore the draw colour).
void kill_panels(Ui& ui, const HudModel& m) {
    static constexpr const char* kIcon[6] = {"icon", "ficon", "hicon", "ticon", "icon", "gold"};
    ui.frame(Anchor::TopLeft);
    const UiFont* f = ui.font("Celtic 10pt English");
    for (int n = 0; n < 2; ++n) {
        const HudModel::KillPanel& k = m.kill_panel[n];
        if (!k.active) continue;
        const int y = n == 1 && m.kill_panel[0].active ? 0x33 : 8;
        ui.sprite("KILL", "panel", {428.0f, float(y)});
        if (k.mode >= 0 && k.mode < 6) ui.sprite("KILL", kIcon[k.mode], {428.0f, float(y)});
        if (!f) continue;
        const int x = std::max(500 + (93 - f->metrics.measure(k.text)) / 2, 500);   // 0x626130 centring
        ui.text(f, k.text, glm::vec2(x, y + 4 + (25 - f->metrics.line_height()) / 2), kUiWhite);
    }
}

}  // namespace

void hud_crit(Ui& ui, HudModel& m) {
    crit(ui, m.crit, m.sim_paused);
    kill_panels(ui, m);
}

void hud_crit_fill(game::Mission& m, HudModel& hud) {
    const game::World& w = m.world();
    const game::Consts& c = m.data().c;
    HudModel::Crit& cr = hud.crit;
    cr.enabled = w.critical_attack_enabled;
    cr.keys = w.crit.active ? w.crit.keys : std::string();
    cr.progress = w.crit.progress;
    cr.fade_in1 = c.crit_fade_in1, cr.fade_in2 = c.crit_fade_in2;
    std::copy(std::begin(c.crit_glow_in), std::end(c.crit_glow_in), cr.glow_in);

    const game::MissionState& s = m.state();
    const int limit[2] = {s.kill_citizen_limit, s.kill_citizen_limit_2};   // obj +10 / +0xe
    for (int n = 0; n < 2; ++n) {
        const auto& k = m.ui().kill[n];
        HudModel::KillPanel& p = hud.kill_panel[n];
        p.active = k.active, p.mode = k.mode;
        if (!p.active) continue;
        if (k.mode == 3) {   // time left = limit - scenario count time 1 (DAT_00a59171) / 1000, "%d:%d"
            const int t = limit[n] - int(m.scenario().count_time[0] / 1000);
            char buf[32];
            std::snprintf(buf, sizeof buf, "%d:%d", t % 3600 / 60, t % 3600 % 60);
            p.text = buf;
            continue;
        }
        // DAT_00a5918d citizens killed, 0xa59199 female citizens, 0xa591a5 houses burnt, 0xa591c8 saved, 0xa591b6 gold crates
        const int count = k.mode == 0 ? s.citizen_killed : k.mode == 1 ? s.fcitizen_killed : k.mode == 2 ? s.house_burnt
                          : k.mode == 4 ? s.citizen_saved : s.gold_crates;
        p.text = std::to_string(count) + "/" + std::to_string(limit[n]);
    }
}

void hud_crit_events(const std::vector<game::GameEvent>& events, HudModel& hud) {
    HudModel::Crit& c = hud.crit;
    for (const game::GameEvent& e : events) {
        if (e.type != game::Ev::Crit) continue;
        if (e.name == "CRITATT") c.banner = 1, c.banner_ms = 0.0f;   // Crit_key 0x462810: +0x19 = 1, +0x2f = now
        else if (e.name == "CRITKILL") c.banner = 2;                 // FUN_00509d70: +0x1b = 1 (start kept)
        else if (e.name.starts_with("CRITSEQ:") && c.banner == 1) c.banner = 0;   // Crit_startSequence: +0x19 = 0
    }
}

}  // namespace joa::engine
