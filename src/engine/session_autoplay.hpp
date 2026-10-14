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
//   cmd <UiCommand> [value] [text]        inject a command (NewGame 1, ContinueCampaign, QuickLoad, Resume, ...)
//   win                                   test hook: finish every WIN objective (normal win-delay path)
//   shot <file.png>                       capture after this frame is drawn
//   log <text>                            print flow state + text to stderr
//   quit
#include "engine/input.hpp"
#include "engine/ui_model.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>

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
    };
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
    std::vector<std::pair<int, int>> releases_;  // (frame, scancode) / (frame, -button)
};

std::optional<UiCommand::Type> ui_command_type(std::string_view name);

}  // namespace joa::engine
