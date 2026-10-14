#include "formats/text_vars.hpp"

#include <cstdlib>

#include "formats/text_strings.hpp"
#include "formats/vfs_fs.hpp"

namespace joa::formats {

std::optional<std::vector<GameVar>> parse_game_vars(std::string_view text, std::string* err) {
    std::vector<GameVar> out;
    std::string section;
    int line_no = 0;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;
        std::string_view comment;
        if (auto c = line.find("//"); c != std::string_view::npos) {
            comment = trim_ws(line.substr(c + 2));
            line = line.substr(0, c);
        }
        line = trim_ws(line);
        if (line.empty()) continue;
        size_t close = line.find(line.front() == '{' ? '}' : ']');
        if ((line.front() != '{' && line.front() != '[') || close == std::string_view::npos) {
            if (err) *err = "line " + std::to_string(line_no) + ": expected {SECTION} or [KEY]value";
            return std::nullopt;
        }
        if (line.front() == '{') {
            section = std::string(line.substr(1, close - 1));
            continue;
        }
        out.push_back({section, std::string(line.substr(1, close - 1)), std::string(trim_ws(line.substr(close + 1))),
                       std::string(comment)});
    }
    return out;
}

std::optional<double> find_game_var(const std::vector<GameVar>& vars, std::string_view section, std::string_view key) {
    for (const auto& v : vars) {
        if (!iequals(v.section, section) || !iequals(v.key, key)) continue;
        char* end = nullptr;
        double d = std::strtod(v.value.c_str(), &end);
        if (end == v.value.c_str()) return std::nullopt;
        return d;
    }
    return std::nullopt;
}

}  // namespace joa::formats
