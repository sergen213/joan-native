// Text tables: "~"-separated blocks (script/scenario/*EX.res, des/strategic_*.txt,
// Resource/help.res) and "[0001N]" string tags used for localizable script strings.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::formats {

// Splits on lines that consist only of "~". Blocks keep their inner newlines (CR removed),
// are trimmed of leading/trailing blank lines; empty blocks are dropped.
std::vector<std::string> split_tilde_blocks(std::string_view text);

struct TaggedString {
    std::string tag;   // "0001N" (four digits + one letter; only 'N' occurs in shipped data)
    std::string text;  // raw Latin-1
};

// Splits "[0001N]text" into {"0001N", "text"}; nullopt if `s` has no leading tag.
std::optional<TaggedString> split_string_tag(std::string_view s);

// Parses an EX.res table: blocks "[0001N]\r\ntext\r\n~". Fails on a block without a tag.
std::optional<std::vector<TaggedString>> parse_tagged_strings(std::string_view text, std::string* err = nullptr);

// Strips spaces, tabs, CR/LF and NULs from both ends.
std::string_view trim_ws(std::string_view s);
// Original text is Windows-1252/ISO-8859-1 ("Orléans" is byte 0xE9); maps Latin-1 to UTF-8.
std::string latin1_to_utf8(std::string_view s);

}  // namespace joa::formats
