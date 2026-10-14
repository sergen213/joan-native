// CONFIG.DAT : raw 0xB05-byte dump of joan.exe's global config object at 0x00A57FB8.
// Load FUN_0045aaf0 (requires file size == 0xB05), save FUN_0045ad80, defaults FUN_00459e30.
// Offsets/labels and confidence: docs/formats/CONTAINERS_WORLD_SAVES.md ("CONFIG.DAT").
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace joa::formats {

struct KeyBinding {               // 0x30-byte record; table copied from the controls page (FUN_004cc380)
    std::array<std::uint8_t, 0x30> raw{};
};

struct GameConfig {
    static constexpr std::size_t kSize = 0xB05;
    static constexpr std::size_t kKeyBindings = 55;  // (0xB05 - 0xB5) / 0x30

    std::int32_t difficulty_value = 0;   // 0x00 table[difficulty] (DAT_009fb1ca)
    std::uint8_t difficulty_param = 0;   // 0x04 table[difficulty] (DAT_009fb1db)
    std::uint8_t difficulty = 0;         // 0x0A 0..3
    std::string player_name;             // 0x17 char[21], default "Player"
    std::int16_t music_volume = 0;       // 0x3A 0..100, default 77
    std::int16_t sfx_volume = 0;         // 0x3C 0..100, default 39: "Environmental" (environment streams)
    std::int16_t volume_3e = 0;          // 0x3E default 100, UNKNOWN channel
    std::uint8_t flag_40 = 0;            // 0x40 default 1, sound enabled
    std::int16_t speech_volume = 0;      // 0x41 0..100, default 100: "Sound Volume" (3D effects + UI, not speech)
    std::uint8_t sound_3d = 0;           // 0x43 1 = AL_INVERSE_DISTANCE_CLAMPED, 0 = AL_NONE (docs/AUDIO_AND_VIDEO.md)
    std::int32_t control_scheme = 0;     // 0x6B 0..3 key preset, default 3
    float mouse_sensitivity = 0;         // 0x6F -1..1
    std::uint8_t control_73 = 0;         // 0x73 combo index, UNKNOWN (camera related)
    std::uint8_t control_74 = 0;         // 0x74 combo index, UNKNOWN (camera related)
    std::uint8_t control_75 = 0;         // 0x75 combo index, UNKNOWN
    float camera_turn_deg = 0;           // 0x76 default 90.0
    std::int32_t display_7e = 0;         // 0x7E compared during display-mode matching, UNKNOWN
    std::int32_t width = 0;              // 0x82 default 1024
    std::int32_t height = 0;             // 0x86 default 768
    std::int32_t bpp = 0;                // 0x8A default 32
    std::int32_t refresh_hz = 0;         // 0x8E default 60
    std::int32_t view_distance = 0;      // 0x98 0x401..0x4001 world units (far plane), default 0x2001
    std::uint8_t shadow_mode = 0;        // 0x9C 0..3 (2/3 need >= 4 texture stages)
    std::uint8_t shader_quality = 0;     // 0x9D 0..2 (2 needs render targets)
    std::uint8_t effect_9e = 0;          // 0x9E capability-gated effect toggle, UNKNOWN
    std::uint8_t dynamic_grass = 0;      // 0x9F
    std::int32_t grass_distance = 0;     // 0xA0 multiple of 20
    std::int32_t grass_density = 0;      // 0xA4 percent
    float brightness = 0;                // 0xA8 -1..1, default -0.2
    float contrast = 0;                  // 0xAC default 1.1
    float gamma = 0;                     // 0xB0 default 1.0
    std::uint8_t custom_keys = 0;        // 0xB4 1 = key table below is valid
    std::array<KeyBinding, kKeyBindings> keys{};  // 0xB5

    std::array<std::uint8_t, kSize> raw{};  // whole file, for fields not named above
};

std::optional<GameConfig> parse_config_dat(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

}  // namespace joa::formats
