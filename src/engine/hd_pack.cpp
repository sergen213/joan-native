#include "engine/hd_pack.hpp"

#include "engine/assets_cache.hpp"
#include "engine/png.hpp"
#include "formats/vfs_fs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace joa::engine {
namespace {

// "TEXTURE\\Item\\Church.DDS" -> "texture/item/church".
std::string rel_key(std::string_view rel) {
    std::string k = lower(rel);
    std::replace(k.begin(), k.end(), '\\', '/');
    while (k.starts_with("./")) k.erase(0, 2);
    while (k.starts_with('/')) k.erase(0, 1);
    if (const auto dot = k.rfind('.'); dot != std::string::npos && k.find('/', dot) == std::string::npos) k.resize(dot);
    return k;
}

bool contains_any(std::string_view s, std::initializer_list<std::string_view> words) {
    return std::any_of(words.begin(), words.end(), [&](std::string_view w) { return s.find(w) != std::string_view::npos; });
}

}  // namespace

std::filesystem::path hd_pack_home() {
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x == '/') return std::filesystem::path(x) / "joan-native/hd";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : ".") / ".local/share/joan-native/hd";
}

std::vector<std::filesystem::path> resolve_hd_packs(const std::vector<std::string>& entries) {
    std::vector<std::filesystem::path> out;
    for (const std::string& e : entries) {
        if (e.empty()) continue;
        const std::filesystem::path p = std::filesystem::path(e).is_absolute() ? std::filesystem::path(e) : hd_pack_home() / e;
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) out.push_back(p);
        else std::fprintf(stderr, "joan-native: hd pack %s: no directory %s, skipped\n", e.c_str(), p.c_str());
    }
    return out;
}

HdPacks::HdPacks(std::vector<std::filesystem::path> packs) {
    namespace fs = std::filesystem;
    for (const fs::path& pack : packs) {
        auto& idx = index_.emplace_back();
        std::error_code ec;
        // Only the trees the engine reads textures from; never the whole directory (a pack may be a
        // full mod extraction with executables, models, nested archives). resource/ = UI cursors
        // (resource/i_cursor/<offset>.png).
        for (const auto& top : fs::directory_iterator(pack, ec)) {
            const std::string t = lower(top.path().filename().string());
            if (!top.is_directory(ec) || (t != "texture" && t != "int8x6" && t != "save" && t != "resource")) continue;
            for (auto it = fs::recursive_directory_iterator(top.path(), fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                const std::string ext = lower(it->path().extension().string());
                if ((ext != ".dds" && ext != ".png") || !it->is_regular_file(ec)) continue;
                // .dds beats .png of the same stem (keeps its file mips).
                auto [slot, fresh] = idx.try_emplace(rel_key(it->path().lexically_relative(pack).generic_string()), it->path());
                if (!fresh && ext == ".dds") slot->second = it->path();
            }
        }
        std::fprintf(stderr, "joan-native: hd pack %s: %zu textures\n", pack.c_str(), idx.size());
    }
}

std::size_t HdPacks::files() const {
    std::size_t n = 0;
    for (const auto& i : index_) n += i.size();
    return n;
}

HdOverride HdPacks::find(std::string_view rel) const {
    HdOverride o;
    if (index_.empty()) return o;
    const std::string key = rel_key(rel);
    std::size_t first = 0, last = index_.size();
    for (std::size_t i = 0; i < index_.size(); ++i)
        if (const auto it = index_[i].find(key); it != index_[i].end()) {
            o.albedo = it->second;
            first = i, last = i + 1;
            break;
        }
    const std::pair<const char*, std::filesystem::path*> maps[] = {{"_n", &o.normal}, {"_r", &o.roughness}, {"_m", &o.metal}, {"_ao", &o.ao}};
    for (const auto& [suffix, out] : maps)
        for (std::size_t i = first; i < last; ++i)
            if (const auto it = index_[i].find(key + suffix); it != index_[i].end()) {
                *out = it->second;
                break;
            }
    return o;
}

MaterialClass material_class(std::string_view rel) {
    const std::string k = rel_key(rel);
    const auto slash = k.rfind('/');
    const std::string_view dir = slash == std::string::npos ? std::string_view{} : std::string_view(k).substr(0, slash);
    const std::string_view name = std::string_view(k).substr(slash == std::string::npos ? 0 : slash + 1);
    if (dir == "texture/terrain") return MaterialClass::Terrain;
    if (dir == "texture/unit") return MaterialClass::Character;
    if (dir == "texture/particle") return MaterialClass::Particle;
    if (dir != "texture/item") return MaterialClass::Generic;
    // texture/item holds carried weapons/armour/loot, props, buildings and vegetation side by side.
    // ponytail: name heuristics checked against the shipped names; a per-pack class table if they misfire.
    const bool bow = name.find("bow") != std::string_view::npos && name.find("bowl") == std::string_view::npos;
    if (bow || contains_any(name, {"sword", "sowrd", "axe", "mace", "spear", "lance", "shield", "sheld", "helm", "armor", "armour",
                                   "arrow", "halb", "hammer", "dagger", "flail", "club", "weapon", "itm_", "bmlb", "gold", "key",
                                   "crystal", "quartz"}) ||
        name.starts_with("sw_") || name.starts_with("cb_") || name.starts_with("rg_") || name.starts_with("at_") ||
        name.starts_with("gs_") || name.starts_with("pa_") || name.starts_with("ma_") || name.starts_with("hr_"))
        return MaterialClass::Item;
    if (contains_any(name, {"tree", "grass", "bush", "leaf", "leaves", "plant", "flower", "pine", "corn", "vegetation", "straw", "root"}))
        return MaterialClass::Vegetation;
    return MaterialClass::Building;
}

bool load_image_file(const std::filesystem::path& file, formats::DdsTexture& out, std::string& err, std::uint32_t maxMips) {
    const auto bytes = formats::read_file(file);
    if (!bytes) return err = "unreadable", false;
    if (lower(file.extension().string()) != ".png") return formats::parse_dds(*bytes, out, err, maxMips);
    out = formats::DdsTexture{};
    formats::DdsImage img;
    int w = 0, h = 0;
    if (!decode_png(*bytes, w, h, img.rgba, err)) return false;
    img.width = out.width = std::uint32_t(w);
    img.height = out.height = std::uint32_t(h);
    out.format = "PNG";
    out.images.push_back(std::move(img));
    return true;
}

}  // namespace joa::engine
