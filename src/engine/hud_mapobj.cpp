// HudGaps mapobj: minimap vision mask (FUN_0070ec50 + FUN_00719900 / FUN_00719b40 / FUN_00719e60, dot test in
// FUN_0070fbe0) and the objective panel (HUD object 0xa4d618, draw FUN_00704810). See hud_mapobj.hpp.
#include "engine/hud_mapobj.hpp"

#include "game/mission.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace joa::engine {
namespace {

using Area = HudModel::VisionArea;
using Line = HudModel::ObjectiveLine;

constexpr float kViewX = 1024 - 0xd3, kViewY = 10;  // fog texture position (W-0xd3, 10), 200x200

const game::UnitClass* class_of(game::Mission& m, const game::Unit& u) {
    return u.cls >= 0 && std::size_t(u.cls) < m.data().units.size() ? &m.data().units[std::size_t(u.cls)] : nullptr;
}

// FUN_00719e60 row span of a quadrilateral (points in a y-down plane): edges going down bound the left side, edges
// going up the right side; an edge crossing out of [lo, hi] drops the row. `cut` truncates like the exe's ftol.
bool quad_span(const glm::vec2 (&q)[4], float row, float lo, float hi, bool cut, float& l, float& r) {
    bool hit = false;
    l = lo, r = hi;
    for (int k = 0; k < 4; ++k) {
        const glm::vec2 a = q[k], b = q[(k + 1) & 3];
        if (a.y == b.y) continue;
        const float t = (row - a.y) / (b.y - a.y);
        if (t < 0.0f || t > 1.0f) continue;
        hit = true;
        float x = a.x + t * (b.x - a.x);
        if (cut) x = std::trunc(x);
        if (b.y <= a.y) {
            if (x < lo) return false;
            r = std::min(r, x);
        } else {
            if (x > hi) return false;
            l = std::max(l, x);
        }
    }
    return hit;
}

glm::vec2 centre(const Area& a) { return a.round ? glm::floor(a.p[0] + 0.5f) : a.p[0]; }  // ftol(x + 0.5), x > 0

// ---- objective panel ----
Line* find(HudModel& hud, int recno) {
    for (Line& l : hud.objective_list)
        if (l.recno == recno) return &l;
    return nullptr;
}
Line line(int recno) {
    Line l;
    l.recno = recno;
    return l;
}
// +0x157 = strlen(MAINGAME n) + 1: the text lives in Ui, so the draw clamps this to the real length.
constexpr int kRevealFull = std::numeric_limits<short>::max();
// The "New!" highlight is skipped in scenario 6 stage 1 (FUN_00707400 / FUN_007076c0: DAT_00a58fa7 != 6 || stage != 1).
bool highlight_new(game::Mission& m) { return !(m.scenario_no() == 6 && m.scenario().stage == 1); }
void highlight(Line& l, int status) { l.status = status, l.reveal = kRevealFull, l.reveal_ms = 0.0f; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Minimap vision
// ---------------------------------------------------------------------------------------------------------------

// FUN_0070ec50: the mask is rebuilt every frame while the minimap is on and the player's world exists. Sources:
// live units (not Unit_is_dead) of the player's nation or allied to it (relation +3 == 4) whose class is not
// citizen (+2 == 0), HORSE (+0x118 == 2) or special 7, circle radius = unit +0x3c1 (VISION_R by default); territory
// areas of owned / allied territories (owner > 0); the ten script areas (set_minimap_*_vision_area).
void hud_mapobj_fill(game::Mission& m, HudModel& hud) {
    game::World& w = m.world();
    hud.minimap_vision.clear();
    const game::Unit* pl = w.unit(w.player_id());
    hud.minimap_fog = pl && !pl->deleted;
    if (hud.minimap_fog) {
        auto friendly = [&](int nation) { return nation == pl->nation || m.places().relation(nation, pl->nation).status == 4; };
        for (const game::Unit& u : w.units()) {
            if (u.deleted || w.dead(u) || !friendly(u.nation)) continue;
            const game::UnitClass* c = class_of(m, u);
            if (!c || c->special == 2 || c->special == 7 || c->unit_class == 0) continue;
            Area a{1, {{u.pos.x, u.pos.y}}, float(u.m.vision), true};
            hud.minimap_vision.push_back(a);
        }
        for (const game::Territory& t : m.places().territories()) {
            if (t.owner <= 0 || !friendly(t.owner)) continue;
            for (const game::Territory::Area& ta : t.areas) {
                Area a{ta.shape, {}, float(ta.r), false};
                for (int k = 0; k < 4; ++k) a.p[k] = {float(ta.x[k]), float(ta.y[k])};
                hud.minimap_vision.push_back(a);
            }
        }
        for (const game::MissionUi::VisionArea& v : m.ui().vision) {
            if (!v.enabled) continue;
            Area a{v.shape, {}, float(v.radius), false};
            for (int k = 0; k < 4; ++k) a.p[k] = {float(v.c[2 * k]), float(v.c[2 * k + 1])};
            hud.minimap_vision.push_back(a);
        }
    }

    // Objective lines: end-condition fields the draw reads each frame (FUN_00704810 reads the record live).
    const auto& ends = m.state().end;
    std::erase_if(hud.objective_list, [&](const Line& l) { return l.recno < 1 || l.recno > int(ends.size()); });
    for (Line& l : hud.objective_list) {
        const game::EndCondition& e = ends[std::size_t(l.recno - 1)];
        l.type = e.type;
        l.enabled = e.enabled != 0;
        l.des = e.has_des ? e.des : std::string();
        l.suffix = e.para1;
        l.a = l.b = 0;
        // ponytail: signed minutes; the exe divides (DAT_00a591cc - DAT_00a59171 / 1000) mixing int and unsigned.
        if (e.para1 == 1) l.a = (m.scenario().objective_count_time - int(m.scenario().count_time[0] / 1000)) / 60;
        if (e.para1 == 2) l.a = m.state().rescue_count, l.b = m.state().rescue_limit;  // DAT_00a591d0 / 0xa591d4
    }
    hud.objective_paused = m.state().cut_scene;  // Game_is_cutscene (Game+0x744)
}

// World-space form of the mask test (FUN_0070fbe0 tests the dot's mask pixel).
// ponytail: no pixel rounding, so dots exactly on a circle edge may differ by one pixel from the exe.
bool hud_vision_visible(const HudModel& hud, glm::vec2 p) {
    if (!hud.minimap_fog) return true;
    constexpr float kInf = std::numeric_limits<float>::infinity();
    for (const Area& a : hud.minimap_vision) {
        if (a.shape == 1) {
            const glm::vec2 d = p - centre(a);
            if (glm::dot(d, d) <= a.r * a.r) return true;
        } else if (a.shape == 0) {
            const glm::vec2 lo = glm::min(a.p[0], a.p[1]), hi = glm::max(a.p[0], a.p[1]);
            if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y) return true;
        } else if (a.shape == 2) {
            const glm::vec2 q[4] = {{a.p[0].x, -a.p[0].y}, {a.p[1].x, -a.p[1].y}, {a.p[2].x, -a.p[2].y}, {a.p[3].x, -a.p[3].y}};
            float l, r;
            if (quad_span(q, -p.y, -kInf, kInf, false, l, r) && l <= p.x && p.x <= r) return true;
        }
    }
    return false;
}

void hud_vision_mask(const HudModel& hud, glm::vec2 origin, glm::vec2 scale, VisionMask& mask) {
    mask.fill(0);
    auto span = [&](int row, int x0, int x1) {
        if (row < 0 || row > 199 || x1 < 0 || x0 > 199) return;
        x0 = std::max(x0, 0), x1 = std::min(x1, 199);
        if (x0 <= x1) std::fill_n(mask.begin() + row * 200 + x0, x1 - x0 + 1, std::uint8_t(1));
    };
    const glm::vec2 s = scale;
    for (const Area& a : hud.minimap_vision) {
        if (a.shape == 1) {  // circle (FUN_0070ec50 unit loop / FUN_00719b40), world radius
            const glm::vec2 c = centre(a);
            const float dx = c.x - origin.x, dy = origin.y - c.y, R = a.r;
            const int r0 = std::max(int(dy * s.y - R * s.y), 0), r1 = std::min(int(dy * s.y + R * s.y), 199);
            for (int row = r0; row <= r1; ++row) {
                const float d = float(row) / s.y - dy, q = R * R - d * d;
                if (q < 0.0f) continue;
                const float h = std::sqrt(q) * (s.x / s.y);
                span(row, int((dx - h) * s.x + 0.5f), int((dx + h) * s.x + 0.5f));
            }
        } else if (a.shape == 0) {  // rectangle (FUN_00719900)
            const glm::vec2 lo = glm::min(a.p[0], a.p[1]), hi = glm::max(a.p[0], a.p[1]);
            const int x0 = int((lo.x - origin.x) * s.x), x1 = int((hi.x - origin.x) * s.x);
            for (int row = std::max(int((origin.y - hi.y) * s.y), 0); row <= std::min(int((origin.y - lo.y) * s.y), 199); ++row)
                span(row, x0, x1);
        } else if (a.shape == 2) {  // quadrilateral (FUN_00719e60)
            glm::vec2 q[4];
            float y0 = 1e30f, y1 = -1e30f;
            for (int k = 0; k < 4; ++k) {
                q[k] = {(a.p[k].x - origin.x) * s.x, (origin.y - a.p[k].y) * s.y};
                y0 = std::min(y0, q[k].y), y1 = std::max(y1, q[k].y);
            }
            for (int row = std::max(int(y0), 0); row <= std::min(int(y1), 199); ++row) {
                float l, r;
                if (quad_span(q, float(row), 0.0f, 199.0f, true, l, r)) span(row, int(l), int(r));
            }
        }
    }
}

glm::vec2 hud_minimap_turn(int rotation, glm::vec2 p) {
    const glm::vec2 c{kViewX + 99.5f, kViewY + 100.0f}, d = p - c;
    switch (rotation & 3) {
        case 1: return c + glm::vec2(d.y, -d.x);
        case 2: return c - d;
        case 3: return c + glm::vec2(-d.y, d.x);
        default: return p;
    }
}

// FUN_0070e500: the mask texture (0 -> 0x80000000 black, 1 -> transparent) drawn at (W-0xd3, 10) with colour
// (0,0,0,0xff) and the minimap rotation, after the unit dots and before FRAME. One fill per run of identical fogged rows.
void hud_minimap_fog(Ui& ui, const HudModel& m, glm::vec2 off, glm::vec2 ppw) {
    if (!m.minimap_fog) return;
    const glm::vec2 origin{(kViewX - off.x) / ppw.x, m.map_size.y + (off.y - kViewY) / ppw.y};
    VisionMask mask;
    hud_vision_mask(m, origin, ppw, mask);
    const glm::vec4 fog = ui_rgba(0, 0, 0, 0x80);
    std::vector<glm::ivec2> runs, prev;  // [x0, x1) fogged runs of a row
    int start = 0;
    auto flush = [&](int row) {
        for (const glm::ivec2 r : prev) {
            const glm::vec2 a = hud_minimap_turn(m.brief_rotation, {kViewX + float(r.x), kViewY + float(start)});
            const glm::vec2 b = hud_minimap_turn(m.brief_rotation, {kViewX + float(r.y), kViewY + float(row)});
            const glm::vec2 lo = glm::min(a, b), hi = glm::max(a, b);
            ui.fill({lo.x, lo.y, hi.x - lo.x, hi.y - lo.y}, fog);
        }
    };
    for (int row = 0; row <= 200; ++row) {
        runs.clear();
        for (int x = 0; row < 200 && x < 200;) {
            if (mask[std::size_t(row * 200 + x)]) { ++x; continue; }
            const int x0 = x;
            while (x < 200 && !mask[std::size_t(row * 200 + x)]) ++x;
            runs.push_back({x0, x});
        }
        if (row < 200 && runs == prev) continue;
        flush(row);
        prev.swap(runs);
        start = row;
    }
}

// FUN_00718540 waypoints (MAPICON wp_<type>), after FRAME: centred, only when fully inside the view.
void hud_minimap_waypoints(Ui& ui, const HudModel& m, glm::vec2 off, glm::vec2 ppw) {
    for (const HudMapIcon& ic : m.minimap_waypoints) {
        const UiSprite* s = ui.sprite("MAPICON", ic.sprite);
        if (!s) continue;
        const glm::vec2 p = glm::round(hud_minimap_turn(m.brief_rotation, glm::vec2(ic.world.x, m.map_size.y - ic.world.y) * ppw + off));
        const glm::vec2 h = s->size / 2.0f;
        if (p.x - h.x >= kViewX && p.y - h.y >= kViewY && p.x + h.x <= kViewX + 199 && p.y + h.y <= kViewY + 200) ui.sprite(s, p - h);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Objective panel
// ---------------------------------------------------------------------------------------------------------------

void hud_mapobj_reset(game::Mission& m, HudModel& hud) {
    // Game_add_end_condition appends every record (FUN_007078c0); seeding covers loaded games and an init script
    // that ran before the reset (its queued ObjectiveAdd events are then duplicates and ignored).
    hud.objective_list.clear();
    for (int i = 1; i <= int(m.state().end.size()); ++i) hud.objective_list.push_back(line(i));
    hud.objective_fade = 0, hud.objective_fade_ms = 0.0f, hud.objective_pinned = false, hud.show_objective = false;
}

void hud_objective_show(HudModel& hud) {
    if (hud.objective_fade == 0) hud.objective_fade = 1, hud.objective_fade_ms = 0.0f, hud.show_objective = true, hud.objective_pinned = false;
    if (hud.objective_fade == 2) {  // resume the fade-in at the current fade-out alpha
        hud.objective_fade = 1;
        hud.objective_fade_ms = (255.0f - hud.objective_fade_ms / 3000.0f * 255.0f) * 2000.0f / 255.0f;
        hud.show_objective = true, hud.objective_pinned = false;
    }
}

void hud_objective_toggle(HudModel& hud) {  // PlayerCtrl_process 0x42e603..0x42e758
    switch (hud.objective_fade) {
        case 0: hud.show_objective = true, hud.objective_fade_ms = 0.0f, hud.objective_fade = 1; break;
        case 3: hud.objective_fade_ms = 0.0f, hud.objective_fade = 2; break;
        case 1: hud.objective_fade = 2, hud.objective_fade_ms = (255.0f - hud.objective_fade_ms / 2000.0f * 255.0f) / 255.0f * 3000.0f; break;
        case 2:
            hud.objective_fade = 1, hud.objective_fade_ms = (255.0f - hud.objective_fade_ms / 3000.0f * 255.0f) * 2000.0f / 255.0f;
            hud.show_objective = true;
            break;
    }
    hud.objective_pinned = true;
}

void hud_mapobj_event(const game::MissionEvent& e, game::Mission& m, HudModel& hud) {
    using K = game::MissionEvent::Kind;
    Line* l = find(hud, e.a);
    switch (e.kind) {
        case K::ObjectiveAdd:  // Game_add_end_condition -> FUN_007078c0: append
            if (!l) hud.objective_list.push_back(line(e.a));
            break;
        case K::ObjectiveHide:  // del_end_condition / enabled 0 -> FUN_007072c0: remove
            if (l) hud.objective_list.erase(hud.objective_list.begin() + (l - hud.objective_list.data()));
            break;
        case K::ObjectiveShow:
            if (e.a <= 0) {  // show_objective (FUN_00707ce0)
                hud_objective_show(hud);
            } else if (!l) {  // enabled 1 / finish NORMAL -> FUN_00707400: insert first with "New!"
                Line n = line(e.a);
                if (highlight_new(m)) highlight(n, 2);
                hud.objective_list.insert(hud.objective_list.begin(), n);
                hud_objective_show(hud);
            }
            break;
        case K::ObjectiveUpdate:  // set_end_condition_des_and_update -> FUN_007076c0
            if (l) {
                if (highlight_new(m)) highlight(*l, 2);
                hud_objective_show(hud);
            }
            break;
        case K::ObjectiveFinish:  // WIN -> FUN_00707960 "COMPLETED!" / LOSE -> FUN_00707b20 "FAILED!"
        case K::ObjectiveFail:
            if (l) highlight(*l, e.kind == K::ObjectiveFinish ? 0 : 1);
            hud.objective_fade_ms = 0.0f;  // +0x153 = now, also while shown (the 12 s hold restarts)
            hud_objective_show(hud);
            break;
        default: break;
    }
}

int hud_objective_alpha(HudModel& hud, float ms) {
    if (!hud.objective_paused) hud.objective_fade_ms += ms;
    switch (hud.objective_fade) {
        case 1: {  // fade in over 2000 ms
            const int a = int(std::min(hud.objective_fade_ms / 2000.0f * 255.0f, 255.0f));
            if (a == 255) hud.objective_fade = 3;
            return a;
        }
        case 2: {  // fade out over 3000 ms, then hidden
            const int a = int(std::max(255.0f - hud.objective_fade_ms / 3000.0f * 255.0f, 0.0f));
            if (a == 0) hud.objective_fade = 0, hud.show_objective = false;
            return a;
        }
        case 3:
            if (!hud.objective_pinned && hud.objective_fade_ms > 11999.0f) hud.objective_fade = 2, hud.objective_fade_ms = 0.0f;
            return 255;
        default:  // shown without the state machine (UI samples): settled
            hud.objective_fade = 3, hud.objective_pinned = true;
            return 255;
    }
}

std::string hud_format_count(std::string_view fmt, int a, int b) {
    std::string out;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        const bool arg = fmt[i] == '%' && i + 2 < fmt.size() && (fmt[i + 1] == '1' || fmt[i + 1] == '2');
        const int v = arg && fmt[i + 1] == '2' ? b : a;
        if (arg && fmt[i + 2] == 'd') {
            out += std::to_string(v);
            i += 2;
        } else if (arg && fmt[i + 2] == 'p' && i + 3 < fmt.size() && fmt[i + 3] == '{') {  // %Np{one|many}
            const std::size_t bar = fmt.find('|', i), end = fmt.find('}', i);
            if (bar == std::string_view::npos || end == std::string_view::npos || bar > end) return out + std::string(fmt.substr(i));
            out += v == 1 ? fmt.substr(i + 4, bar - i - 4) : fmt.substr(bar + 1, end - bar - 1);
            i = end;
        } else {
            out += fmt[i];
        }
    }
    return out;
}

// FUN_00704810: main lines (type 0) then side lines (type 2), at most 10 rows; LIGHTBAR / DARKBAR at (0, 0x94 + 20 n),
// text (5, 0x97 + 20 n) Goudy12 Bold (font obj 0xa5d3c8) white, "LINE" over the first side row after main rows.
// A highlighted line ("COMPLETED!" gold f2d531 / "FAILED!" red / "New!" green, MAINGAME 1..3) types its prefix in
// from the right, one character per 100 ms while the panel is fully shown, its colour going white -> target by
// (target - 255) / len * (len - idx) per channel. At the whole prefix: "New!" stays 2000 ms then clears; COMPLETED! /
// FAILED! fade the row out over 2000 ms and drop the line from the panel. Suffixes MAINGAME 5 / 6 by para1.
void hud_objectives(Ui& ui, HudModel& m) {
    ui.frame(Anchor::TopLeft);
    const float ms = ui.dt() * 1000.0f;  // real ms (DAT_00a500ba)
    for (Line& l : m.objective_list) l.reveal_ms += ms;
    auto drawn = [](const Line& l, int pass) { return l.enabled && l.type == (pass ? 2 : 0); };
    if (std::none_of(m.objective_list.begin(), m.objective_list.end(), [&](const Line& l) { return drawn(l, 0) || drawn(l, 1); }))
        return;  // the exe advances the panel state inside the row loop only
    const int alpha = hud_objective_alpha(m, ms);
    if (!m.show_objective) return;
    static constexpr glm::ivec3 kTarget[3] = {{0xf2, 0xd5, 0x31}, {255, 0, 0}, {0, 255, 0}};
    const UiFont* f = ui.font("Goudy12 Bold Black");
    int row = 0;
    bool main_rows = false;
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < m.objective_list.size() && row < 10; ++i) {
            Line& l = m.objective_list[i];
            if (!drawn(l, pass)) continue;
            int a = alpha;
            glm::ivec3 c{255};
            std::string text;
            if (l.status >= 0 && l.status <= 2 && l.reveal > 0) {
                const std::string word(ui.text("MAINGAME", l.status + 1 == 3 ? 3 : l.status + 1));
                const int len = int(word.size()) + 1;
                l.reveal = std::min(l.reveal, len);
                const glm::ivec3 tint = glm::ivec3(255) + (kTarget[l.status] - 255) / len * (len - l.reveal);
                if (l.reveal == 1) {
                    text = word + " ", c = tint;
                    const int left = int(std::max(255.0f - l.reveal_ms / 2000.0f * 255.0f, 0.0f));
                    if (l.status == 2) {
                        a = 255;
                        if (left == 0) l.status = -1, l.reveal = 0, l.reveal_ms = 0.0f;
                    } else {
                        a = left;
                        if (a == 0) {  // FUN_007072c0 and return: no further rows this frame
                            m.objective_list.erase(m.objective_list.begin() + std::ptrdiff_t(i));
                            return;
                        }
                    }
                } else if (m.objective_fade == 3) {
                    text = (word + " ").substr(std::size_t(l.reveal - 1)), c = tint;
                    if (l.reveal_ms > 100.0f) --l.reveal, l.reveal_ms = 0.0f;
                }
            }
            text += l.des;
            if (l.suffix == 1) text += hud_format_count(ui.text("MAINGAME", 5), l.a, l.b);
            else if (l.suffix == 2) text += hud_format_count(ui.text("MAINGAME", 6), l.a, l.b);
            const glm::vec4 bar = ui_rgba(255, 255, 255, a);
            if (pass == 1 && main_rows) ui.sprite("MAINGAME", "line", {0, float(0x93 + 20 * row)}, bar), main_rows = false;
            const float y = float(0x94 + 20 * row);
            ui.sprite("MAINGAME", row % 2 ? "darkbar" : "lightbar", {0, y}, bar);
            ui.text(f, text, {5, y + 3}, ui_rgba(c.r, c.g, c.b, a), TextAlign::Left, 1.0f, TextOp::Diffuse);
            main_rows |= pass == 0;
            ++row;
        }
}

}  // namespace joa::engine
