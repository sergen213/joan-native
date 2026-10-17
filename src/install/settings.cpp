#include "install/settings.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace joa::install {
namespace {
std::string trim(std::string s) {
    const auto ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}
void pick(std::string& out, const std::string& v, std::initializer_list<const char*> allowed) {
    for (auto a : allowed) if (v == a) { out = v; return; }
}
void boolean(bool& out, const std::string& v) {
    if (v == "true" || v == "1") out = true;
    else if (v == "false" || v == "0") out = false;
}
template <class T> bool number(T& out, const std::string& v, T lo, T hi) {
    T x{};
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
    if (ec == std::errc{} && end == v.data() + v.size() && x >= lo && x <= hi) return out = x, true;
    return false;
}
// Optional key kept as text: accepted when it parses into [lo, hi].
template <class T> void ranged(std::string& out, const std::string& v, T lo, T hi) {
    if (T x{}; number(x, v, lo, hi)) out = v;
}
} // namespace

Settings Settings::parse(const std::string& text) {
    Settings s;
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = trim(line.substr(1, line.size() - 2)); continue; }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = section + "." + trim(line.substr(0, eq));
        const auto v = trim(line.substr(eq + 1));
        if (key == "renderer.backend") pick(s.backend, v, {"vulkan", "opengl"});
        else if (key == "renderer.preset") pick(s.preset, v, {"original", "enhanced", "ultra", "custom"});
        else if (key == "raytracing.reflections") boolean(s.rt_reflections, v);
        else if (key == "raytracing.shadows") boolean(s.rt_shadows, v);
        else if (key == "raytracing.gi") boolean(s.rt_gi, v);
        else if (key == "graphics.aa") pick(s.aa, v, {"none", "fxaa", "smaa", "taa"});
        else if (key == "graphics.msaa") pick(s.msaa, v, {"1", "2", "4", "8"});
        else if (key == "graphics.anisotropy") ranged(s.anisotropy, v, 1, 16);
        else if (key == "graphics.render_scale") ranged(s.render_scale, v, 0.5, 2.0);
        else if (key == "graphics.sharpness") ranged(s.sharpness, v, 0.0, 1.0);
        else if (section == "graphics") s.graphics_extra.emplace_back(trim(line.substr(0, eq)), v);
        else if (key == "display.width") number(s.width, v, 0, 16384);
        else if (key == "display.height") number(s.height, v, 0, 16384);
        else if (key == "display.aspect") pick(s.aspect, v, {"auto", "4:3", "16:9", "16:10", "21:9"});
        else if (key == "display.mode") pick(s.mode, v, {"windowed", "fullscreen", "borderless"});
        else if (key == "controller.deadzone") number(s.deadzone, v, 0.0, 0.9);
        else if (key == "controller.sensitivity") number(s.sensitivity, v, 0.1, 5.0);
        else if (key == "update.channel") pick(s.channel, v, {"stable", "experimental"});
        else if (key == "debug.overlay") boolean(s.debug_overlay, v);
        else if (key == "paths.game_dir") s.game_dir = v;
    }
    if ((s.width == 0) != (s.height == 0)) s.width = s.height = 0; // half a resolution is no resolution
    return s;
}

Settings Settings::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return parse(text.str());
}

std::string Settings::serialize() const {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::ostringstream o;
    o << "# joan-native settings (schema: docs/LAUNCHER.md)\n"
      << "[renderer]\nbackend=" << backend << "\npreset=" << preset << "\n\n"
      << "[raytracing]\nreflections=" << b(rt_reflections) << "\nshadows=" << b(rt_shadows) << "\ngi=" << b(rt_gi) << "\n\n"
      << "[graphics]\n";
    for (const auto& [k, v] : {std::pair<const char*, const std::string&>{"aa", aa}, {"msaa", msaa}, {"anisotropy", anisotropy},
                               {"render_scale", render_scale}, {"sharpness", sharpness}})
        if (!v.empty()) o << k << "=" << v << "\n";
    for (const auto& [k, v] : graphics_extra) o << k << "=" << v << "\n";
    o << "\n"
      << "[display]\nwidth=" << width << "\nheight=" << height << "\naspect=" << aspect << "\nmode=" << mode << "\n\n"
      << "[controller]\ndeadzone=" << deadzone << "\nsensitivity=" << sensitivity << "\n\n"
      << "[update]\nchannel=" << channel << "\n\n"
      << "[debug]\noverlay=" << b(debug_overlay) << "\n\n"
      << "[paths]\ngame_dir=" << game_dir << "\n";
    return o.str();
}

void Settings::save(const std::filesystem::path& file) const {
    std::filesystem::create_directories(file.parent_path());
    auto tmp = file;
    tmp += ".tmp-" + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << serialize();
        out.flush();
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    std::filesystem::rename(tmp, file);
}
} // namespace joa::install
