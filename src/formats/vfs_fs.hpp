// Read-only, case-insensitive view of the original install directory.
// Original game code builds paths like "MAP\\scenario1\\S1L1.GRI" with arbitrary case and
// backslashes; resolve() maps them onto the real (case-sensitive) Linux files. Never writes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace joa::formats {

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& p);

// ASCII case-insensitive equality (game paths are 7-bit ASCII).
bool iequals(std::string_view a, std::string_view b);

class Vfs {
public:
    explicit Vfs(std::filesystem::path root) : root_(std::move(root)) {}
    const std::filesystem::path& root() const { return root_; }

    // Accepts '\\' or '/' separators and any case. Rejects absolute paths, drive letters and "..".
    // Returns the existing on-disk path (file or directory) or nullopt.
    std::optional<std::filesystem::path> resolve(std::string_view game_path) const;
    std::optional<std::vector<std::uint8_t>> read(std::string_view game_path) const;
    bool exists(std::string_view game_path) const { return resolve(game_path).has_value(); }

    // Every regular file under root (recursive, skipping root/work and dot-dirs) whose extension
    // equals ext case-insensitively (ext includes the dot, e.g. ".gri"). Sorted.
    std::vector<std::filesystem::path> find_by_extension(std::string_view ext) const;

private:
    std::filesystem::path root_;
};

}  // namespace joa::formats
