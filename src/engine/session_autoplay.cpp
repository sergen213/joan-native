#include "engine/session_autoplay.hpp"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace joa::engine {
namespace {

int scancode(const std::string& name) {
    const SDL_Scancode s = SDL_GetScancodeFromName(name.c_str());
    return s == SDL_SCANCODE_UNKNOWN ? -1 : int(s);
}

int button(const std::string& name) {
    if (name == "left") return SDL_BUTTON_LEFT;
    if (name == "right") return SDL_BUTTON_RIGHT;
    if (name == "middle") return SDL_BUTTON_MIDDLE;
    return -1;
}

float num(const std::string& s) { return std::strtof(s.c_str(), nullptr); }

}  // namespace

std::optional<UiCommand::Type> ui_command_type(std::string_view n) {
    using T = UiCommand::Type;
    static constexpr std::pair<std::string_view, T> names[] = {
        {"NewGame", T::NewGame}, {"Load", T::Load}, {"Save", T::Save}, {"DeleteSave", T::DeleteSave},
        {"OptionsChanged", T::OptionsChanged}, {"Resume", T::Resume}, {"RestartMission", T::RestartMission},
        {"QuitToMain", T::QuitToMain}, {"Quit", T::Quit}, {"MissionBrief", T::MissionBrief}, {"MessageLog", T::MessageLog},
        {"HoldResume", T::HoldResume}, {"MinimapClick", T::MinimapClick}, {"QuickLoad", T::QuickLoad},
        {"QuickLoadCancel", T::QuickLoadCancel}, {"ContinueCampaign", T::ContinueCampaign}, {"LoadMenu", T::LoadMenu},
    };
    for (const auto& [name, t] : names)
        if (name == n) return t;
    return std::nullopt;
}

bool Autoplay::parse(std::string_view text, std::string* err) {
    std::istringstream in{std::string(text)};
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (const auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ls(line);
        Event e;
        if (!(ls >> e.frame)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            if (err) *err = "line " + std::to_string(n) + ": expected <frame> <verb>";
            return false;
        }
        ls >> e.verb;
        if (e.verb == "log" || e.verb == "shot") {  // rest of the line (paths may contain spaces)
            std::string rest;
            std::getline(ls, rest);
            rest.erase(0, rest.find_first_not_of(" \t"));
            rest.erase(rest.find_last_not_of(" \t\r") + 1);
            e.args.push_back(rest);
        } else {
            for (std::string a; ls >> a;) e.args.push_back(a);
        }
        const auto need = [&](std::size_t k) { return e.args.size() >= k; };
        bool ok = true;
        if (e.verb == "key") ok = need(2) && scancode(e.args[0]) >= 0;
        else if (e.verb == "tap") ok = need(1) && scancode(e.args[0]) >= 0;
        else if (e.verb == "button") ok = need(2) && button(e.args[0]) >= 0;
        else if (e.verb == "clickbtn") ok = need(1) && button(e.args[0]) >= 0;
        else if (e.verb == "mouse" || e.verb == "pointer" || e.verb == "click") ok = need(2);
        else if (e.verb == "wheel" || e.verb == "shot") ok = need(1);
        else if (e.verb == "cmd") ok = need(1) && ui_command_type(e.args[0]).has_value();
        else ok = e.verb == "win" || e.verb == "quit" || e.verb == "log";
        if (!ok) {
            if (err) *err = "line " + std::to_string(n) + ": bad event '" + line + "'";
            return false;
        }
        events_.push_back(std::move(e));
    }
    std::stable_sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });
    return true;
}

bool Autoplay::load(const std::string& path, std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), err);
}

Autoplay::FrameOut Autoplay::apply(int frame, RawInput& raw) {
    FrameOut out;
    std::erase_if(releases_, [&](const std::pair<int, int>& r) {
        if (r.first > frame) return false;
        if (r.second >= 0) raw.keys[std::size_t(r.second)] = false;
        else raw.mouse_buttons[std::size_t(-r.second)] = false;
        return true;
    });
    for (; next_ < events_.size() && events_[next_].frame <= frame; ++next_) {
        const Event& e = events_[next_];
        const auto& a = e.args;
        if (e.verb == "key") raw.keys[std::size_t(scancode(a[0]))] = a[1] == "down";
        else if (e.verb == "tap") {
            raw.keys[std::size_t(scancode(a[0]))] = true;
            releases_.push_back({frame + (a.size() > 1 ? std::max(1, int(num(a[1]))) : 2), scancode(a[0])});
        } else if (e.verb == "button") raw.mouse_buttons[std::size_t(button(a[0]))] = a[1] == "down";
        else if (e.verb == "clickbtn") {
            raw.mouse_buttons[std::size_t(button(a[0]))] = true;
            releases_.push_back({frame + (a.size() > 1 ? std::max(1, int(num(a[1]))) : 2), -button(a[0])});
        } else if (e.verb == "mouse") raw.mouse_dx += num(a[0]), raw.mouse_dy += num(a[1]);
        else if (e.verb == "wheel") raw.wheel += num(a[0]);
        else if (e.verb == "pointer") pointer_ = glm::vec2(num(a[0]), num(a[1]));
        else if (e.verb == "click") pointer_ = glm::vec2(num(a[0]), num(a[1])), out.click = true;
        else if (e.verb == "cmd") {
            UiCommand c{*ui_command_type(a[0]), 0, {}, {}};
            if (a.size() > 1) c.value = int(num(a[1]));
            if (a.size() > 2) c.text = a[2];
            if (a.size() > 3) c.text2 = a[3];
            out.commands.push_back(std::move(c));
        } else if (e.verb == "win") out.win = true;
        else if (e.verb == "quit") out.quit = true;
        else if (e.verb == "shot") out.shots.push_back(a[0]);
        else if (e.verb == "log") out.logs.push_back(a.empty() ? std::string() : a[0]);
    }
    out.pointer = pointer_;
    return out;
}

}  // namespace joa::engine
