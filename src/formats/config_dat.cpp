#include "formats/config_dat.hpp"

#include <algorithm>

#include "formats/vfs_bytes.hpp"

namespace joa::formats {

std::optional<GameConfig> parse_config_dat(std::span<const std::uint8_t> bytes, std::string* err) {
    if (bytes.size() != GameConfig::kSize) {  // FUN_0045aaf0 ignores any other size
        if (err) *err = "CONFIG.DAT must be exactly 0xB05 bytes";
        return std::nullopt;
    }
    GameConfig c;
    std::copy(bytes.begin(), bytes.end(), c.raw.begin());
    auto at = [&](std::size_t off) { return ByteReader(bytes, off); };
    c.difficulty_value = at(0x00).i32();
    c.difficulty_param = at(0x04).u8();
    c.difficulty = at(0x0A).u8();
    c.player_name = at(0x17).fixed_string(21);
    c.music_volume = at(0x3A).i16();
    c.sfx_volume = at(0x3C).i16();
    c.volume_3e = at(0x3E).i16();
    c.flag_40 = at(0x40).u8();
    c.speech_volume = at(0x41).i16();
    c.sound_3d = at(0x43).u8();
    c.control_scheme = at(0x6B).i32();
    c.mouse_sensitivity = at(0x6F).f32();
    c.control_73 = at(0x73).u8();
    c.control_74 = at(0x74).u8();
    c.control_75 = at(0x75).u8();
    c.camera_turn_deg = at(0x76).f32();
    c.display_7e = at(0x7E).i32();
    c.width = at(0x82).i32();
    c.height = at(0x86).i32();
    c.bpp = at(0x8A).i32();
    c.refresh_hz = at(0x8E).i32();
    c.view_distance = at(0x98).i32();
    c.shadow_mode = at(0x9C).u8();
    c.shader_quality = at(0x9D).u8();
    c.effect_9e = at(0x9E).u8();
    c.dynamic_grass = at(0x9F).u8();
    c.grass_distance = at(0xA0).i32();
    c.grass_density = at(0xA4).i32();
    c.brightness = at(0xA8).f32();
    c.contrast = at(0xAC).f32();
    c.gamma = at(0xB0).f32();
    c.custom_keys = at(0xB4).u8();
    for (std::size_t i = 0; i < GameConfig::kKeyBindings; ++i)
        std::copy_n(bytes.begin() + 0xB5 + i * 0x30, 0x30, c.keys[i].raw.begin());
    return c;
}

}  // namespace joa::formats
