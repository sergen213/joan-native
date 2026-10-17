#pragma once
// --autoplay-script: timed synthetic input for unattended play-throughs and captures. One event per line,
// `<frame> <verb> [args]`, '#' comments; frames count rendered frames (captures run one 33 ms tick per frame).
//   key <SDL scancode name> down|up       hold / release a key (e.g. "key W down", "key F5 down")
//   tap <scancode name> [frames]          down now, up after `frames` (default 2)
//   button left|right|middle down|up      mouse button
//   clickbtn left|right [frames]          button tap
//   mouse <dx> <dy>                       relative mouse motion this frame
//   wheel <steps>
//   pointer <x> <y>                       cursor in 1024x768 design units of the 4:3 canvas
//   click <x> <y>                         pointer + left click (menus)
//   cmd <UiCommand> [value] [text] [text2] inject a command (NewGame 1, ContinueCampaign, QuickLoad, Resume, ...);
//                                         text2 = rest of the line: `cmd Save 0 Test.jsav TEST S1L1 start` writes
//                                         <native save dir>/Test.jsav with that description
//   win                                   test hook: finish every WIN objective (normal win-delay path)
//   shot <file.png>                       capture after this frame is drawn
//   log <text>                            print flow state + text to stderr
//   type <text>                           text input this frame (rest of the line, UTF-8; e.g. the save description)
//   give <item code>                      test hook: item into the player's bag, then used/equipped (armour, sword)
//   drop <item code> [dist] [angle]       test hook: floor item `dist` units from the player, `angle` deg CCW of its heading
//   pad <button> down|up|tap [frames]     gamepad button: south/east/west/north (= a/b/x/y), lb/rb, or an SDL name
//                                         (start, back, dpup, dpdown, dpleft, dpright, leftstick, rightstick, ...)
//   axis <axis> <value>                   gamepad axis (leftx, lefty, rightx, righty, lefttrigger, righttrigger),
//                                         -1..1, held until changed. Any pad/axis event marks a pad as connected.
//   quit
#include "engine/input.hpp"
#include "engine/ui_model.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace joa::game { class Mission; }

namespace joa::engine {

class Autoplay {
public:
    // Returns false (with *err) on a malformed line.
    bool parse(std::string_view text, std::string* err = nullptr);
    bool load(const std::string& path, std::string* err = nullptr);
    bool empty() const { return events_.empty(); }

    struct FrameOut {
        std::vector<UiCommand> commands;
        std::vector<std::string> shots, logs;
        std::optional<glm::vec2> pointer;  // design units, sticky until the next pointer event
        bool click = false;                // left press this frame (UI input)
        bool win = false, quit = false;
        // EquipVisuals: `give <code>` (bag + equip/use on the player), `drop <code> [dist]` (floor item `dist`
        // units ahead of the player, default 0 = at the feet). Test hooks.
        struct ItemHook { std::string verb, code; float dist = 0, angle = 0; };  // angle: degrees CCW from the player heading (drop)
        std::vector<ItemHook> items;
    };
    // Applies a give/drop hook to the running mission's player (false: no player / unknown item).
    static bool apply_item_hook(game::Mission& m, const FrameOut::ItemHook& h);
    // Applies frame `frame`'s events to `raw` (scripted keys/buttons stay held across frames).
    FrameOut apply(int frame, RawInput& raw);
    int last_frame() const { return events_.empty() ? 0 : events_.back().frame; }

private:
    struct Event {
        int frame = 0;
        std::string verb;
        std::vector<std::string> args;
    };
    std::vector<Event> events_;  // sorted by frame (stable)
    std::size_t next_ = 0;
    std::optional<glm::vec2> pointer_;
    std::vector<std::pair<int, int>> releases_;  // (frame, scancode) / (frame, -button) / (frame, -(100 + pad button))
    bool pad_ = false;                           // a pad/axis verb ran: RawInput::pad_connected
};

std::optional<UiCommand::Type> ui_command_type(std::string_view name);

}  // namespace joa::engine
