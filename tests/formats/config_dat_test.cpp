// CONFIG.DAT field mapping against the shipped file (values equal FUN_00459e30 defaults).
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "formats/config_dat.hpp"
#include "formats/vfs_fs.hpp"

using namespace joa::formats;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK(%s) failed\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !std::filesystem::exists(std::filesystem::path(dir) / "joan.exe")) return 77;
    Vfs vfs(dir);
    auto bytes = vfs.read("CONFIG.DAT");
    CHECK(bytes);
    auto c = parse_config_dat(*bytes);
    CHECK(c);
    CHECK(c->width == 1024 && c->height == 768 && c->bpp == 32 && c->refresh_hz == 60);
    CHECK(c->player_name == "Player");
    CHECK(c->difficulty == 1);
    // Volumes are user settings (this install was played: 9/15/100); defaults are 77/39/100.
    CHECK(c->music_volume >= 0 && c->music_volume <= 100 && c->sfx_volume >= 0 && c->sfx_volume <= 100 &&
          c->speech_volume >= 0 && c->speech_volume <= 100);
    CHECK(c->control_scheme == 3 && c->camera_turn_deg == 90.0f);
    CHECK(c->view_distance >= 0x401 && c->view_distance <= 0x4001);  // shipped file: 0x4001 (default 0x2001)
    CHECK(std::fabs(c->brightness + 0.2f) < 1e-6f && std::fabs(c->contrast - 1.1f) < 1e-6f && c->gamma == 1.0f);
    CHECK(c->custom_keys == 0);
    CHECK(c->raw[0x17] == 'P');

    std::vector<std::uint8_t> short_file(bytes->begin(), bytes->end() - 1);
    CHECK(!parse_config_dat(short_file));
    std::printf("config ok: %dx%dx%d@%d shadows=%d grass=%d/%d%%\n", c->width, c->height, c->bpp, c->refresh_hz,
                c->shadow_mode, c->grass_distance, c->grass_density);
    return 0;
}
