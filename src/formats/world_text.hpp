// Text formats used by map/, Resource/*.pro and map/common.{lgt,sca}.
//
// KvText  ("Key<ws>:<ws>value" per line; '#' lines are comments):
//   Resource/*.pro, map/*.lgt, map/*.sca, map/scenarioN/*.lgt, *.day.
//   Order matters: .lgt/.sca/.day repeat the same keys once per light/stage.
// BlockText ("TypeName" line, "{", "key:value[;]" lines, "}"):
//   map/scenarioN/*.{stg,pla,uni,wor,ter,trk,cm,pth,wav,por}, Resource/*.cm.
//   Lines inside a block without ':' (e.g. .por coordinate tuples) are kept with an empty key.
//   Lines outside any block that are not block headers go to top_level (e.g. "Anti-Portal Number: 9").
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::formats {

struct KvLine {
    std::string key;
    std::string value;
};

std::vector<KvLine> parse_kv_text(std::string_view text);

struct TextBlock {
    std::string type;  // "CreateObject", "CreateSceneSector", ...
    std::vector<KvLine> fields;
    const std::string* get(std::string_view key) const;  // first match, case-insensitive
    double number(std::string_view key, double fallback = 0.0) const;  // accepts "172.2f"
};

struct BlockText {
    std::vector<KvLine> top_level;
    std::vector<TextBlock> blocks;
};

// Fails (nullopt) on unbalanced or nested braces.
std::optional<BlockText> parse_block_text(std::string_view text, std::string* err = nullptr);

}  // namespace joa::formats
