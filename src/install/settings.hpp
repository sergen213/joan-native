#pragma once
// settings.ini shared by joa-launcher (writer) and joan-native (reader). Schema: docs/LAUNCHER.md.
#include <filesystem>
#include <string>

namespace joa::install {

struct Settings {
    std::string backend = "opengl";       // [renderer] opengl|vulkan (engine Vulkan backend not ready yet)
    std::string preset = "enhanced";      // [renderer] original|enhanced|ultra|custom
    bool rt_reflections = false;          // [raytracing]
    bool rt_shadows = false;
    bool rt_gi = false;
    int width = 0, height = 0;            // [display] 0 = desktop resolution
    std::string aspect = "auto";          // [display] auto|4:3|16:9|16:10|21:9
    std::string mode = "fullscreen";      // [display] windowed|fullscreen|borderless
    double deadzone = 0.15;               // [controller] 0.0..0.9
    double sensitivity = 1.0;             // [controller] 0.1..5.0
    std::string channel = "stable";       // [update] stable|experimental
    std::string game_dir;                 // [paths] empty = $XDG_DATA_HOME/joan-native/game/current

    // Missing file/keys and invalid values fall back to the defaults above; unknown keys are ignored.
    static Settings load(const std::filesystem::path& file);
    static Settings parse(const std::string& text);
    std::string serialize() const;
    void save(const std::filesystem::path& file) const; // atomic (tmp + rename)
};
} // namespace joa::install
