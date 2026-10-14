#pragma once
// RecruitWorkshop: barracks recruit screen (interfac i_barr.cpp: open FUN_006c0310, copy BarrUI_open 0x6bda00, draw
// FUN_006bdea0, input BarrUI_handle_input 0x6bf630, slides FUN_006bfe00, commit BarrUI_confirm 0x6bdc00; hud_recruit.cpp)
// and siege workshop screen (i_work.cpp: open FUN_006ed3e0, copy FUN_006ea020, draw FUN_006ea310, input FUN_006ec480,
// slides FUN_006ece80, commit FUN_006ea1a0; hud_workshop.cpp). Both are full-screen (BARRACK set bg, six / four
// sliding unit-type rows i_ot at (0x24, 0x68 + 100 i), info FRAME (0x27b, 0x5b)) and pause the game (speed 0).
// Opened by MissionEvent::Interact "recruit_screen" / "workshop_screen" (b = facility recno). RE notes:
// docs/reverse-engineering/UI_SYSTEM.md "Recruit and siege workshop screens".
#include "engine/ui_core.hpp"
#include "engine/ui_model.hpp"
#include "game/mission.hpp"

#include <array>
#include <string>
#include <vector>

namespace joa::engine {

class FacilityWindow {
public:
    void on_mission_events(const std::vector<game::MissionEvent>&, game::Mission&, HudModel&);
    // Draws the open screen (HudModel::recruit_screen / workshop_screen) over everything but the menus.
    void draw(Ui&, game::Mission&, HudModel&);
    void reset() { *this = {}; }
    bool open() const { return state_ != Closed; }

    // Screen copies (committed only by RECRUIT / BUILD / Enter). Barracks: per unit slot 0 light inf, 1 heavy inf,
    // 2 maceman, 3 poleman, 4 archer, 5 crossbowman (+0x42 troop, +0x5a new); workshop: per product (+0x52 built,
    // +0x62 new).
    std::array<int, 6> have{}, add{};
    int money = 0;      // +0x3e gold / workshop +0x41 gold or the facility's iron / wood
    int cost = 0;       // +0x7e / +0x45
    int citizens = 0;   // +0x72 able citizens (clamp(pop - 0.95 pop_max, 0, 20)), barracks
    int recruited = 0;  // +0x76
    int total = 0;      // +0x7a troop size incl. pending / workshop +0x49 weapons (built + new)
    int recruiter = 0;  // DAT_00a48959 recruiting unit (Interact event a: player on R, selected RTS leader)
    int facility = 0;
    bool workshop = false;

private:
    enum State { Closed, Opening, Open, Closing };
    void open_recruit(game::Mission&, int fac);
    void open_workshop(game::Mission&, int fac);
    void recruit_screen(Ui&, game::Mission&, HudModel&);
    void workshop_screen(Ui&, game::Mission&, HudModel&);
    void commit_recruit(game::Mission&);
    void commit_workshop(game::Mission&);
    // Shared chrome (identical in both interfaces).
    void close() { state_ = Closing, t_ = 0.0f; }
    glm::vec2 row_pos(int i) const;    // sliding row origin
    float frame_x() const;             // sliding info frame x
    float alpha() const;               // bg / title / buttons fade
    float panel(Ui&, int i, glm::vec2 p, bool hot);   // ia_ov + arrows slide-out, returns its offset
    void title(Ui&, std::string_view text);
    bool button(Ui&, glm::vec4 r, std::string_view label, std::string_view mark);
    void row_frame(Ui&, glm::vec2 p, bool hot, const char* section, int first_label);
    int row_count_ = 6;                // row objects: 6 barracks / 4 workshop (slide timing)
    State state_ = Closed;
    float t_ = 0.0f;                   // seconds in the current state
    std::array<float, 6> slide_{};     // ia_ov panel offset per row (0..0x4c)
    bool help_ = false;                // workshop HELP box (+0x51)
    std::string help_text_;
};

}  // namespace joa::engine
