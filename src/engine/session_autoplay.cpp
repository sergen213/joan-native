#include "engine/session_autoplay.hpp"

#include "game/mission.hpp"
#include "game/terrain_query.hpp"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace joa::engine {
namespace {

int scancode(std::string name) {  // '_' stands for a space ("Left_Ctrl")
    std::replace(name.begin(), name.end(), '_', ' ');
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

// GamepadPlay: positional names (south/east/west/north, also a/b/x/y) or any SDL gamepad button name.
int pad_button(const std::string& name) {
    static constexpr std::pair<std::string_view, SDL_GamepadButton> kNames[] = {
        {"south", SDL_GAMEPAD_BUTTON_SOUTH}, {"east", SDL_GAMEPAD_BUTTON_EAST}, {"west", SDL_GAMEPAD_BUTTON_WEST},
        {"north", SDL_GAMEPAD_BUTTON_NORTH}, {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}};
    for (const auto& [n, b] : kNames)
        if (n == name) return int(b);
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str());
    return b == SDL_GAMEPAD_BUTTON_INVALID ? -1 : int(b);
}
int pad_axis(const std::string& name) {
    const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name.c_str());
    return a == SDL_GAMEPAD_AXIS_INVALID ? -1 : int(a);
}
constexpr int kPadRelease = 100;  // releases_ code -(kPadRelease + button)

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
        if (e.verb == "log" || e.verb == "shot" || e.verb == "type" || e.verb == "warp" || e.verb == "clear" || e.verb == "owner") {  // rest of the line (paths / names may contain spaces)
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
        else if (e.verb == "pad") ok = need(2) && pad_button(e.args[0]) >= 0;
        else if (e.verb == "axis") ok = need(2) && pad_axis(e.args[0]) >= 0;
        else if (e.verb == "cmd") ok = need(1) && ui_command_type(e.args[0]).has_value();
        else if (e.verb == "give" || e.verb == "drop") ok = need(1);  // EquipVisuals: test hooks (item.res code)
        else if (e.verb == "bag" || e.verb == "xp") ok = need(1);     // CharDetails: give without equipping / gain XP
        else if (e.verb == "warp" || e.verb == "gold" || e.verb == "clear" || e.verb == "owner") ok = need(1);  // RecruitWorkshop / RecruitFix test hooks
        else if (e.verb == "attr") ok = need(2);   // RecruitFix: attr <World::allocate_point type> <points>
        else if (e.verb == "cheat") ok = need(1) && e.args[0] == "enable_fog_of_war";  // HudFix: console cheat
        else ok = e.verb == "win" || e.verb == "quit" || e.verb == "log" || e.verb == "type" || e.verb == "crit";
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
        else if (r.second <= -kPadRelease) raw.pad_buttons[std::size_t(-r.second - kPadRelease)] = false;
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
        else if (e.verb == "give" || e.verb == "drop" || e.verb == "bag") out.items.push_back({e.verb, a[0], a.size() > 1 ? num(a[1]) : 0.0f, a.size() > 2 ? num(a[2]) : 0.0f});
        else if (e.verb == "xp") out.items.push_back({e.verb, {}, num(a[0])});
        else if (e.verb == "crit") out.items.push_back({e.verb, {}});
        else if (e.verb == "warp" || e.verb == "clear" || e.verb == "owner") out.items.push_back({e.verb, a[0]});   // RecruitWorkshop / RecruitFix
        else if (e.verb == "attr") out.items.push_back({e.verb, a[0], num(a[1])});
        else if (e.verb == "gold") out.items.push_back({e.verb, {}, num(a[0])});
        else if (e.verb == "cheat") out.items.push_back({e.verb, a[0], a.size() > 1 ? num(a[1]) : 1.0f});
        else if (e.verb == "quit") out.quit = true;
        else if (e.verb == "shot") out.shots.push_back(a[0]);
        else if (e.verb == "log") out.logs.push_back(a.empty() ? std::string() : a[0]);
        else if (e.verb == "type") raw.text += a[0];  // as SDL text input (UTF-8) this frame
        else if (e.verb == "pad") {
            pad_ = true;
            const int b = pad_button(a[0]);
            raw.pad_buttons[std::size_t(b)] = a[1] != "up";
            if (a[1] == "tap") releases_.push_back({frame + (a.size() > 2 ? std::max(1, int(num(a[2]))) : 2), -(kPadRelease + b)});
        } else if (e.verb == "axis") {
            pad_ = true;
            raw.pad_axes[std::size_t(pad_axis(a[0]))] = std::clamp(num(a[1]), -1.0f, 1.0f);
        }
    }
    out.pointer = pointer_;
    raw.pad_connected |= pad_;  // a scripted pad counts as connected (Options -> Input lists Gamepad/Joystick)
    return out;
}

bool Autoplay::apply_item_hook(game::Mission& m, const FrameOut::ItemHook& h) {
    game::World& w = m.world();
    game::Unit* p = w.unit(w.player_id());
    if (!p) return false;
    if (h.verb == "xp") return w.add_xp(*p, int(h.dist), false, false), true;   // CharDetails: level up for the F1 demo
    // HudFinish/HudFix: the state after 600 s of play: S1L1_tactic re-sets set_critical_attack_enabled every pass to
    // get_scenario_count_time > 600000, so push the scenario count time past it, plus an HP trail > CRI_DIFF so the
    // next tick's Player_critCheck starts a sequence.
    if (h.verb == "crit") {
        m.scenario().count_time[0] = std::max(m.scenario().count_time[0], 600001u);
        return w.critical_attack_enabled = true, p->hp_trail = p->hp + w.data().c.cri_diff + 1, true;
    }
    if (h.verb == "bag") return w.give_item(*p, h.code, 0, 1);
    // RecruitFix: `attr <type> <n>` spends n attribute points (types of 0x50d640: 1 str, 2 dex, 3 def, 5 lead, 6 hp,
    // 7 ep) as the character window's ADD does; `owner <nation> <place name>` = set_place_nation_by_code.
    if (h.verb == "attr") {
        bool ok = true;
        for (int i = 0; i < int(h.dist) && ok; ++i) ok = w.allocate_point(*p, std::atoi(h.code.c_str()), false);
        return ok;
    }
    if (h.verb == "owner") {
        const std::size_t sp = h.code.find(' ');
        const int n = sp == std::string::npos ? 0 : w.data().nation_id(h.code.substr(0, sp));
        game::Place* pl = n ? m.places().place(m.places().place_by_name(h.code.substr(sp + 1))) : nullptr;
        return pl && (pl->nation = n, true);
    }
    // RecruitWorkshop: `warp <facility name>` puts the player at the facility point (inside its interact box),
    // `gold <n>` sets the party gold. Test hooks.
    if (h.verb == "gold") return m.state().gold = int(h.dist), true;
    // HudFix: Script_cheat_exec "enable_fog_of_war [n]" (FUN_00456990): n == 0 sets minimap vision area 10 to the
    // world rect (everything visible, as the mask before its first build), else clears it.
    if (h.verb == "cheat") {
        game::MissionUi::VisionArea& v = m.ui().vision[9];
        v.enabled = h.dist == 0.0f;
        v.shape = 0;
        const auto* g = dynamic_cast<const game::GriTerrain*>(w.terrain());
        const int span = g ? int(std::max(g->grid().cells_x, g->grid().cells_y) * formats::TerrainGrid::kCellSize) : 1 << 20;
        v.c[0] = v.c[1] = 0, v.c[2] = v.c[3] = span;
        return true;
    }
    if (h.verb == "warp") {   // RecruitFix: else the centre of the place of that name
        const game::Facility* f = m.places().facility(m.places().facility_by_name(h.code));
        const game::Place* pl = f ? nullptr : m.places().place(m.places().place_by_name(h.code));
        if (!f && !pl) return false;
        const game::Vec3 at = f ? f->pos : pl->pos;
        p->pos = {at.x, at.y, w.terrain() ? w.terrain()->height(at.x, at.y) : at.z};
        return true;
    }
    // RecruitFix: `clear <place name>` kills every unit hostile to the player inside that place (stands in for the
    // fight that captures it).
    if (h.verb == "clear") {
        const int pl = m.places().place_by_name(h.code);
        if (!m.places().place(pl)) return false;
        for (game::Unit& u : w.units())
            if (!u.deleted && !w.dead(u) && w.hostile_to_player(u) && m.places().inside(pl, u.pos)) w.unit_die(u, p->id);
        return true;
    }
    if (h.verb == "drop") {
        const float y = p->yaw_rad() + h.angle * 0.017453292f;
        return w.add_item(h.code, 0, 1, {p->pos.x + std::cos(y) * h.dist, p->pos.y + std::sin(y) * h.dist, p->pos.z}) != 0;
    }
    if (!w.give_item(*p, h.code, 0, 1)) return false;
    for (int i = int(p->inv.bag.size()) - 1; i >= 0; --i)
        if (w.data().items[std::size_t(p->inv.bag[std::size_t(i)].item)].code == h.code)
            return w.equip(*p, i) || w.use_item(*p, i);
    return false;
}

}  // namespace joa::engine
