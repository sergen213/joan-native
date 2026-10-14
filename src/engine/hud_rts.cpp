// RTS overlay. Rts_draw 0x4450f0: while the left button drags (+0x1235) a 1 px rectangle (FUN_00633070, colour
// DAT_00a33724 = (255, 255, 255, 0xcc) set by FUN_0043f780) from the press point to the mouse, then the RTSICON
// cursor sprite (+0x1258) drawn with its hotspot at the mouse for POINT* names, else centred. CommanderPanel
// 0x6fd670 (party portraits) clicks select the commander in RTS (Rts_process_input step 4, 0x6ffd90 portrait under
// the cursor); its hold/resume buttons stay hud_game's.
#include "engine/hud_rts.hpp"

#include "engine/game_camera.hpp"

namespace joa::engine {
namespace {

// Party slot geometry of hud_game party() (FUN_006f9b60): frame x = DAT_009de64c[8 - n + i] - 800 + W.
constexpr int kFrameX[8] = {69, 161, 253, 345, 437, 529, 621, 713};

glm::vec4 portrait_rect(Ui& ui, const HudModel& m, std::size_t i) {
    const int n = std::min(int(m.party.size()), 8);
    const UiSprite* s = m.party[i].portrait.empty() ? nullptr : ui.sprite("MAINGAME", m.party[i].portrait);
    const glm::vec2 size = s ? s->size : glm::vec2(80.0f, 90.0f);
    return {float(kFrameX[8 - n + int(i)] + 224), 648.0f, size.x, size.y};
}

}  // namespace

std::vector<glm::vec4> rts_hud_rects(Ui& ui, const HudModel& m) {
    std::vector<glm::vec4> out;
    const Anchor keep = ui.current_frame();
    ui.frame(Anchor::TopRight);
    out.push_back(ui.rect_pixels({1024 - 0xd3, 10, 199, 200}));  // Minimap_is_mouse_over 0x717650
    ui.frame(Anchor::BottomRight);
    for (std::size_t i = 0; i < std::min<std::size_t>(m.party.size(), 8); ++i) out.push_back(ui.rect_pixels(portrait_rect(ui, m, i)));
    ui.frame(keep);
    return out;
}

void ui_rts(Ui& ui, const HudModel& m, GameCamera& cam, std::vector<UiCommand>& out) {
    ui.frame(Anchor::BottomRight);
    for (std::size_t i = 0; i < std::min<std::size_t>(m.party.size(), 8); ++i) {
        const glm::vec4 r = portrait_rect(ui, m, i);
        const bool hold_button = m.party[i].name != m.player.name || m.rts_mode;
        if (ui.clicked(r) && !(hold_button && ui.hover({r.x, 678, 24, 28}))) out.push_back({UiCommand::Type::RtsPortrait, int(i)});
    }
    // TopLeft frame (same scale, no 4:3 clip): on 16:9 the minimap sits right of the 1024x768 canvas and the Center
    // frame clips the cursor / drag box there (RtsFinish: no cursor over the minimap in real play).
    ui.frame(Anchor::TopLeft);
    if (const glm::vec4 b = cam.drag_box(); b.z > b.x || b.w > b.y) {
        const glm::vec2 a = ui.to_design({b.x, b.y}), c = ui.to_design({b.z, b.w});
        const glm::vec4 col = ui_rgba(255, 255, 255, 0xcc);
        ui.fill({a.x, a.y, c.x - a.x, 1}, col);
        ui.fill({a.x, c.y - 1, c.x - a.x, 1}, col);
        ui.fill({a.x, a.y + 1, 1, c.y - a.y - 2}, col);
        ui.fill({c.x - 1, a.y + 1, 1, c.y - a.y - 2}, col);
    }
    const std::string name = cam.rts_cursor(ui.dt());
    if (const UiSprite* s = ui.sprite("RTSICON", name)) ui.sprite(s, name.starts_with("POINT") ? ui.mouse() : ui.mouse() - s->size * 0.5f);
}

}  // namespace joa::engine
