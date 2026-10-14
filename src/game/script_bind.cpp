#include "game/script_bind.hpp"

#include <cctype>

#include "game/mission.hpp"

namespace joa::game {

float deg_to_rad(float d) { return d * 0.017453292f; }

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

const BuiltinTable& builtin_table() {
    static const BuiltinTable t = [] {
        BuiltinTable b;
        bind_objects(b);
        bind_places(b);
        bind_units(b);
        bind_unit_state(b);
        bind_troops(b);
        bind_mission(b);
        bind_ui(b);
        return b;
    }();
    return t;
}

Value MissionHost::call(const BuiltinCall& c) {
    if (BuiltinFn fn = builtin_table().call[c.fn->id]) return fn(m_, c);
    ++missing_[c.fn->id];
    switch (c.fn->ret) {
    case ScriptType::String: return Value::string({});
    case ScriptType::Float: return Value::real(0.0f);
    case ScriptType::Int: break;
    }
    return Value::integer(0);
}

bool MissionHost::poll(const BuiltinCall& c) {
    if (PollFn fn = builtin_table().poll[c.fn->id]) return fn(m_, c);
    return true;
}

void MissionHost::trace(const BuiltinCall& c, const Value& result, bool vm_builtin) {
    ++calls;
    if (!keep_log) return;
    static constexpr const char* kKw[] = {"FUNC", "DIALOG", "ACTION", "CAMERA"};
    std::string_view file = c.script;
    if (auto p = file.find_last_of("\\/"); p != std::string_view::npos) file.remove_prefix(p + 1);
    log += "t=" + std::to_string(c.tick) + " " + std::string(file) + ":" + std::to_string(c.line) + " " +
           kKw[static_cast<int>(c.kw)] + " " + std::string(c.fn->name) + "(";
    for (size_t i = 0; i < c.args.size(); ++i) log += (i ? ", " : "") + format_value(c.args[i]);
    log += ") = " + format_value(result) + (vm_builtin ? " [vm]\n" : "\n");
}

}  // namespace joa::game
