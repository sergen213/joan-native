#pragma once
// HD texture packs: read-only directories mirroring the original relative texture paths
// (texture/**, int8x6/**, save/**, resource/** for UI cursors), matched case-insensitively. docs/HD_ASSETS.md.
#include "engine/renderer.hpp"
#include "formats/dds.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace joa::engine {

// $XDG_DATA_HOME/joan-native/hd (else ~/.local/share/joan-native/hd).
std::filesystem::path hd_pack_home();
// Settings `hd_packs` entries -> existing directories, order kept: absolute paths as-is, names under
// hd_pack_home(). Missing entries are skipped with a log line.
std::vector<std::filesystem::path> resolve_hd_packs(const std::vector<std::string>& entries);

// Files of one override; empty path = not provided.
struct HdOverride {
    std::filesystem::path albedo, normal, roughness, metal, ao;
};

class HdPacks {
public:
    HdPacks() = default;
    explicit HdPacks(std::vector<std::filesystem::path> packs);  // indexes every pack once
    bool empty() const { return index_.empty(); }
    std::size_t files() const;  // indexed image files over all packs
    // rel = original texture path relative to the game root ("TEXTURE\\ITEM\\Church.dds", any case,
    // either slash, any extension). Albedo: first pack holding <stem>.dds|.png. Companions
    // <stem>_n/_r/_m/_ao come from that same pack; without an albedo override, each from the first
    // pack that has it.
    HdOverride find(std::string_view rel) const;

private:
    std::vector<std::unordered_map<std::string, std::filesystem::path>> index_;  // lowercase rel stem -> file
};

// Remaster material class from the original relative path (directory + name heuristics).
MaterialClass material_class(std::string_view rel);

// Decodes a .dds (whole mip chain) or .png (one level) file to RGBA8; maxMips as in parse_dds.
bool load_image_file(const std::filesystem::path& file, formats::DdsTexture& out, std::string& err, std::uint32_t maxMips = 0);

}  // namespace joa::engine
