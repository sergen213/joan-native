#include "formats/world_text.hpp"

#include <charconv>

#include "formats/vfs_fs.hpp"

namespace joa::formats {

static std::string_view trim(std::string_view s) {
    const auto ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

static KvLine split_kv(std::string_view line) {
    const auto c = line.find(':');
    if (c == std::string_view::npos) return {"", std::string(line)};
    return {std::string(trim(line.substr(0, c))), std::string(trim(line.substr(c + 1)))};
}

template <class F>
static void for_each_line(std::string_view text, F&& f) {
    while (!text.empty()) {
        const auto nl = text.find('\n');
        f(trim(text.substr(0, nl)));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
}

std::vector<KvLine> parse_kv_text(std::string_view text) {
    std::vector<KvLine> out;
    for_each_line(text, [&](std::string_view l) {
        if (!l.empty() && l[0] != '#') out.push_back(split_kv(l));
    });
    return out;
}

const std::string* TextBlock::get(std::string_view key) const {
    for (const auto& f : fields)
        if (iequals(f.key, key)) return &f.value;
    return nullptr;
}

double TextBlock::number(std::string_view key, double fallback) const {
    const std::string* v = get(key);
    if (!v) return fallback;
    double d = fallback;
    const char* b = v->data();
    if (*b == '+') ++b;
    std::from_chars(b, v->data() + v->size(), d);
    return d;
}

std::optional<BlockText> parse_block_text(std::string_view text, std::string* err) {
    BlockText doc;
    std::optional<std::string> pending;  // last top-level line; becomes a block type if "{" follows
    bool in_block = false;
    bool bad = false;
    for_each_line(text, [&](std::string_view l) {
        if (bad || l.empty()) return;
        if (l == "{") {
            if (in_block || !pending) { bad = true; return; }
            doc.blocks.push_back(TextBlock{std::move(*pending), {}});
            pending.reset();
            in_block = true;
        } else if (l == "}") {
            if (!in_block) { bad = true; return; }
            in_block = false;
        } else if (in_block) {
            if (l.back() == ';') l.remove_suffix(1);
            doc.blocks.back().fields.push_back(split_kv(l));
        } else {
            if (pending) doc.top_level.push_back(split_kv(*pending));
            pending = std::string(l);
        }
    });
    if (bad || in_block) {
        if (err) *err = "unbalanced or nested braces";
        return std::nullopt;
    }
    if (pending) doc.top_level.push_back(split_kv(*pending));
    return doc;
}

}  // namespace joa::formats
