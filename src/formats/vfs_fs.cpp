#include "formats/vfs_fs.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace joa::formats {
namespace fs = std::filesystem;

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0) return std::nullopt;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!buf.empty() && !in.read(reinterpret_cast<char*>(buf.data()), size)) return std::nullopt;
    return buf;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

std::optional<fs::path> Vfs::resolve(std::string_view game_path) const {
    if (game_path.size() >= 2 && game_path[1] == ':') return std::nullopt;  // "C:\..."
    if (!game_path.empty() && (game_path[0] == '\\' || game_path[0] == '/')) return std::nullopt;

    fs::path cur = root_;
    std::size_t i = 0;
    while (i <= game_path.size()) {
        std::size_t j = game_path.find_first_of("\\/", i);
        if (j == std::string_view::npos) j = game_path.size();
        const std::string_view part = game_path.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return std::nullopt;

        std::error_code ec;
        fs::path exact = cur / std::string(part);
        if (fs::exists(exact, ec)) {
            cur = std::move(exact);
            continue;
        }
        // ponytail: linear directory scan per miss; add a per-directory cache if lookups get hot.
        bool found = false;
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
            if (iequals(it->path().filename().string(), part)) {
                cur = it->path();
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    return cur;
}

std::optional<std::vector<std::uint8_t>> Vfs::read(std::string_view game_path) const {
    auto p = resolve(game_path);
    if (!p) return std::nullopt;
    std::error_code ec;
    if (!fs::is_regular_file(*p, ec)) return std::nullopt;
    return read_file(*p);
}

std::vector<fs::path> Vfs::find_by_extension(std::string_view ext) const {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (name.starts_with('.') || (it.depth() == 0 && name == "work") || name.starts_with("build"))
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && iequals(it->path().extension().string(), ext)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace joa::formats
