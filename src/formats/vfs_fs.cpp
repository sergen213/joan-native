#include "formats/vfs_fs.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>

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

static std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}

// Per directory: lowercased name -> real names in directory_iterator order.
struct Vfs::DirCache {
    using Listing = std::unordered_map<std::string, std::vector<std::string>>;
    std::mutex mu;
    std::unordered_map<std::string, Listing> dirs;   // key: real directory path

    // An exact-case entry wins, else the first case-insensitive match in directory order (what the
    // uncached exists() + directory scan returned). Rescans when uncached, when fresh, or on a miss.
    // dir_ok = false when dir cannot be (fully) listed: gone, not a directory, or search-only.
    std::optional<std::string> lookup(const fs::path& dir, std::string_view part, bool fresh, bool& dir_ok) {
        const std::string key = lowered(part);
        std::lock_guard lock(mu);
        dir_ok = true;
        auto it = dirs.find(dir.native());
        if (it != dirs.end() && !fresh)
            if (const std::string* n = pick(it->second, part, key)) return *n;
        Listing l;
        std::error_code ec;
        for (fs::directory_iterator d(dir, ec), end; !ec && d != end; d.increment(ec)) {
            std::string name = d->path().filename().string();
            l[lowered(name)].push_back(std::move(name));
        }
        if (ec) {   // unreadable or failed mid-scan: do not cache a partial listing
            if (it != dirs.end()) dirs.erase(it);
            dir_ok = false;
            const std::string* n = pick(l, part, key);
            return n ? std::optional(*n) : std::nullopt;
        }
        const std::string* n = pick(l, part, key);
        std::optional<std::string> out = n ? std::optional(*n) : std::nullopt;
        dirs.insert_or_assign(dir.native(), std::move(l));
        return out;
    }

    static const std::string* pick(const Listing& l, std::string_view part, const std::string& key) {
        const auto it = l.find(key);
        if (it == l.end()) return nullptr;
        for (const std::string& n : it->second)
            if (n == part) return &n;
        return &it->second.front();
    }
};

Vfs::Vfs(fs::path root) : root_(std::move(root)), cache_(std::make_shared<DirCache>()) {}

// stale = a directory reached through cached listings could not be listed (renamed/removed since).
std::optional<fs::path> Vfs::walk(std::string_view game_path, bool fresh, bool& stale) const {
    stale = false;
    fs::path cur = root_;
    std::size_t i = 0;
    while (i <= game_path.size()) {
        std::size_t j = game_path.find_first_of("\\/", i);
        if (j == std::string_view::npos) j = game_path.size();
        const std::string_view part = game_path.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return std::nullopt;

        bool dir_ok = true;
        auto name = cache_->lookup(cur, part, fresh, dir_ok);
        if (!name && !dir_ok) {   // unlistable (search-only, or gone): the exact name only
            std::error_code ec;
            if (fs::exists(cur / std::string(part), ec)) name = std::string(part);
            else stale = cur != root_;
        }
        if (!name) return std::nullopt;
        cur /= *name;
    }
    return cur;
}

std::optional<fs::path> Vfs::resolve(std::string_view game_path) const {
    if (game_path.size() >= 2 && game_path[1] == ':') return std::nullopt;  // "C:\..."
    if (!game_path.empty() && (game_path[0] == '\\' || game_path[0] == '/')) return std::nullopt;

    bool stale = false;
    auto p = walk(game_path, false, stale);
    if (p) {
        // One stat instead of one per component: a cached hit may name an entry deleted since.
        std::error_code ec;
        if (*p == root_ || fs::exists(fs::symlink_status(*p, ec))) return p;
        stale = true;
    }
    if (!stale) return std::nullopt;
    return walk(game_path, true, stale);
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
