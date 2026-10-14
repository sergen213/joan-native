// Gameplay tuning variables: Resource/GAMEPLAY.RES record bodies (ECON, THROWI, UNIT,
// CHARMOVE, WEATHER, GAME). Syntax: "{SECTION}" headers, "[KEY]value" lines, "//" comments.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::formats {

struct GameVar {
    std::string section;  // last "{...}" header seen, empty before the first one
    std::string key;
    std::string value;    // trimmed text after "]"
    std::string comment;  // trailing "//" comment (often the original #define + old default)
};

std::optional<std::vector<GameVar>> parse_game_vars(std::string_view text, std::string* err = nullptr);

// Value of section/key (case-insensitive) parsed as double; nullopt if missing/non-numeric.
std::optional<double> find_game_var(const std::vector<GameVar>& vars, std::string_view section, std::string_view key);

}  // namespace joa::formats
