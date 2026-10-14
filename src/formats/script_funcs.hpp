// Built-in function table of the scenario script interpreter in joan.exe (407 entries).
// Scripts call these as FUNC("name", ...), DIALOG("name", ...) or ACTION("name", ...);
// lookup in the original is _stricmp (case-insensitive) over the table at VA 0xa1a254.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace joa::formats {

enum class ScriptType : std::uint8_t { Int = 1, Float = 2, String = 3 };
// Which executor in joan.exe implements the call: FUNC switch at VA 0x649920, DIALOG
// executor (VA 0x65fd70/0x660e60) or ACTION executor (VA 0x661060).
enum class ScriptExecutor : std::uint8_t { Func, Dialog, Action };

struct ScriptFunc {
    std::uint16_t id;          // original enum value (1..407), stored in the call node
    std::string_view name;
    ScriptType ret;            // table return type
    std::uint8_t arity;        // highest argument index read by the executor (0 = none)
    std::string_view sig;      // per argument: i int, s string, f float, ? int-or-string unknown
    std::uint32_t case_va;     // executor switch case address (0 for DIALOG/ACTION calls)
    ScriptExecutor executor;
};

std::span<const ScriptFunc> script_funcs();
const ScriptFunc* find_script_func(std::string_view name);

}  // namespace joa::formats
