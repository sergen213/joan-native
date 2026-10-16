// Command line and launcher settings.ini handling.
#include "check.hpp"
#include "engine/cli.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace joa::engine;

static std::string parse(std::vector<const char*> args, Options& o) {
    args.insert(args.begin(), "joan-native");
    return parse_options(int(args.size()), args.data(), o);
}

int main() {
    float a = -1;
    CHECK(parse_aspect("16:9", a) && std::fabs(a - 16.0f / 9.0f) < 1e-6f);
    CHECK(parse_aspect("21:9", a) && std::fabs(a - 21.0f / 9.0f) < 1e-6f);
    CHECK(parse_aspect("32:9", a) && std::fabs(a - 32.0f / 9.0f) < 1e-6f);
    CHECK(parse_aspect("2.39", a) && std::fabs(a - 2.39f) < 1e-6f);
    CHECK(parse_aspect("auto", a) && a == 0.0f);
    CHECK(!parse_aspect("16:0", a));
    CHECK(!parse_aspect("wide", a));
    CHECK(!parse_aspect("-4:3", a));

    {
        Options o;
        CHECK(parse({"--renderer", "gl", "--width", "2560", "--height", "1080", "--borderless", "--aspect", "4:3", "--game-dir",
                     "/x", "--screenshot", "out.png"},
                    o)
                  .empty());
        CHECK(o.renderer == RendererKind::OpenGL && o.width == 2560 && o.height == 1080);
        CHECK(o.mode == WindowMode::Borderless && std::fabs(o.aspect - 4.0f / 3.0f) < 1e-6f);
        CHECK(o.game_dir == "/x" && o.screenshot == "out.png" && o.frames == 5);  // screenshot default
    }
    {
        Options o;
        CHECK(parse({"--headless"}, o).empty() && o.headless && o.frames == 300);
        Options p;
        CHECK(parse({"--frames", "3", "--renderer", "vulkan", "--fullscreen"}, p).empty());
        CHECK(p.frames == 3 && p.renderer == RendererKind::Vulkan && p.mode == WindowMode::Fullscreen);
    }
    {
        Options o;
        CHECK(parse({"--renderdoc-capture", "30"}, o).empty() && o.renderdoc && o.renderdoc_capture == 30);
        Options p;
        CHECK(parse({"--renderdoc"}, p).empty() && p.renderdoc && p.renderdoc_capture == -1);
        CHECK(!parse({"--renderdoc-capture", "-1"}, p).empty());
        CHECK(!parse({"--renderdoc-capture"}, p).empty());
    }
    {
        Options o;
        CHECK(!parse({"--width", "0"}, o).empty());
        CHECK(!parse({"--renderer", "dx8"}, o).empty());
        CHECK(!parse({"--frames"}, o).empty());
        CHECK(!parse({"--bogus"}, o).empty());
        CHECK(!parse({"--settings", "/nonexistent/settings.ini"}, o).empty());
    }
    {  // launcher INI is applied first; explicit flags override it regardless of position
        const auto ini = std::filesystem::temp_directory_path() / "joa_cli_test_settings.ini";
        std::ofstream(ini) << "; launcher\n[renderer]\nbackend=opengl\npreset=original\n[display]\nwidth=1920\nheight=0\n"
                              "aspect=21:9\nmode=fullscreen\n[controller]\ndeadzone=0.25\nsensitivity=1.5\n[paths]\ngame_dir=\n";
        Options o;
        CHECK(parse({"--width", "800", "--settings", ini.c_str()}, o).empty());
        CHECK(o.width == 800 && o.height == 720);  // flag wins; height=0 keeps default
        CHECK(o.mode == WindowMode::Fullscreen && std::fabs(o.aspect - 21.0f / 9.0f) < 1e-6f);
        CHECK(std::fabs(o.deadzone - 0.25f) < 1e-6f && std::fabs(o.sensitivity - 1.5f) < 1e-6f);
        CHECK(o.game_dir.empty() && o.settings == ini.string());
        Options bad;
        CHECK(!apply_settings_ini("[display]\nmode=sideways\n", bad).empty());
        std::filesystem::remove(ini);
    }
    return test_result();
}
