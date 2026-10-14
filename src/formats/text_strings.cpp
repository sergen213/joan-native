#include "formats/text_strings.hpp"

#include <cctype>

namespace joa::formats {

std::string_view trim_ws(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}

std::string latin1_to_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += char(c);
        } else {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::vector<std::string> split_tilde_blocks(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        std::string_view t = cur;
        while (!t.empty() && (t.front() == '\n' || t.front() == '\0')) t.remove_prefix(1);
        while (!t.empty() && (t.back() == '\n' || t.back() == '\0')) t.remove_suffix(1);
        if (!t.empty()) out.emplace_back(t);
        cur.clear();
    };
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (trim_ws(line) == "~") {
            flush();
            continue;
        }
        cur.append(line);
        cur += '\n';
    }
    flush();
    return out;
}

std::optional<TaggedString> split_string_tag(std::string_view s) {
    if (s.size() < 7 || s[0] != '[' || s[6] != ']') return std::nullopt;
    for (int i = 1; i <= 4; ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt;
    if (!std::isalpha(static_cast<unsigned char>(s[5]))) return std::nullopt;
    return TaggedString{std::string(s.substr(1, 5)), std::string(s.substr(7))};
}

std::optional<std::vector<TaggedString>> parse_tagged_strings(std::string_view text, std::string* err) {
    std::vector<TaggedString> out;
    for (auto& b : split_tilde_blocks(text)) {
        auto t = split_string_tag(b);
        if (!t) {
            if (err) *err = "block without [NNNNX] tag: " + b.substr(0, 40);
            return std::nullopt;
        }
        std::string_view body = t->text;
        while (!body.empty() && body.front() == '\n') body.remove_prefix(1);
        t->text = std::string(body);
        out.push_back(std::move(*t));
    }
    return out;
}

}  // namespace joa::formats
