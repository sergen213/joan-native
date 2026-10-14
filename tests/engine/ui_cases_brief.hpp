#pragma once
// Mission-brief pages (hud_brief.cpp) vs the original (tools/reference/capture_ui.py brief: mission 1, Normal, each
// topic clicked at (512, 234 + 63 i), list row 2 at (200, 280)). The original's mouse pointer stays at the click point
// and is left out of the score; the -2 cases hover that row like the capture did. Scored without the parts this
// reconstruction does not draw: the commander's 3D actor (commanders-2).
#include "ui_shot.hpp"

#include <algorithm>
#include <vector>

namespace briefcase {
// Whole frame minus a 75x60 box around the pointer clicked at `c` (hand sprite left/below of the hotspot).
inline std::vector<uishot::Rect> without_cursor(int cx, int cy, uishot::Rect extra = {0, 0, 0, 0}) {
    std::vector<uishot::Rect> r{{0, 0, 1024, cy - 15}, {0, cy + 45, 1024, 768 - cy - 45}, {0, cy - 15, cx - 30, 60}, {cx + 45, cy - 15, 1024 - cx - 45, 60}};
    if (extra.w == 0) return r;
    // Also drop `extra` (split every rect around it, coarse: rows above / below and the sides).
    std::vector<uishot::Rect> out;
    for (const uishot::Rect& q : r) {
        const int x0 = std::max(q.x, extra.x), x1 = std::min(q.x + q.w, extra.x + extra.w);
        const int y0 = std::max(q.y, extra.y), y1 = std::min(q.y + q.h, extra.y + extra.h);
        if (x0 >= x1 || y0 >= y1) { out.push_back(q); continue; }
        if (y0 > q.y) out.push_back({q.x, q.y, q.w, y0 - q.y});
        if (y1 < q.y + q.h) out.push_back({q.x, y1, q.w, q.y + q.h - y1});
        if (x0 > q.x) out.push_back({q.x, y0, x0 - q.x, y1 - y0});
        if (x1 < q.x + q.w) out.push_back({x1, y0, q.x + q.w - x1, y1 - y0});
    }
    return out;
}
}  // namespace briefcase

inline const std::vector<uishot::ShotCase> kBriefCases = {
    {"brief-background", "ui_brief_background.png", {-1, -1}, briefcase::without_cursor(512, 234), 0.95},
    {"brief-objectives", "ui_brief_objectives.png", {-1, -1}, briefcase::without_cursor(512, 297), 0.90},
    {"brief-townships", "ui_brief_townships.png", {-1, -1}, briefcase::without_cursor(512, 360), 0.90},
    {"brief-townships-2", "ui_brief_townships_2.png", {205, 285}, briefcase::without_cursor(200, 280), 0.85},
    {"brief-commanders", "ui_brief_commanders.png", {-1, -1}, briefcase::without_cursor(512, 423), 0.95},
    {"brief-commanders-2", "ui_brief_commanders_2.png", {205, 285}, briefcase::without_cursor(200, 280, {571, 203, 316, 247}), 0.90},
    {"brief-battleground", "ui_brief_battleground.png", {-1, -1}, briefcase::without_cursor(512, 486), 0.85},
    // No original reference (2D view not captured; strategy / siege disabled in mission 1): drawn and widescreen only.
    {"brief-battleground-2d", "", {-1, -1}, {}, 0.0},
    {"brief-strategy", "", {-1, -1}, {}, 0.0},
    {"brief-siege", "", {-1, -1}, {}, 0.0},
};
