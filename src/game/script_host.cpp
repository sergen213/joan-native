#include "game/script_host.hpp"

namespace joa::game {

SourceLoader vfs_source_loader(const formats::Vfs& vfs) {
    return [&vfs](std::string_view path) -> std::optional<std::string> {
        auto b = vfs.read(path);
        if (!b) return std::nullopt;
        return std::string(b->begin(), b->end());
    };
}

Value RecordingHost::call(const BuiltinCall& c) {
    switch (c.fn->ret) {
    case ScriptType::String: return Value::string({});
    case ScriptType::Float: return Value::real(0.0f);
    case ScriptType::Int: break;
    }
    if (builtin_category(c.fn->id) == BuiltinCategory::Spawn) return Value::integer(next_handle_++);
    return Value::integer(0);
}

bool RecordingHost::poll(const BuiltinCall& c) {
    int& n = polls_[c.site];
    if (n < poll_delay_) {
        ++n;
        return false;
    }
    n = 0;
    return true;
}

void RecordingHost::trace(const BuiltinCall& c, const Value& result, bool vm_builtin) {
    static constexpr const char* kKw[] = {"FUNC", "DIALOG", "ACTION", "CAMERA"};
    std::string_view file = c.script;
    if (auto p = file.find_last_of("\\/"); p != std::string_view::npos) file.remove_prefix(p + 1);
    std::string line = "t=" + std::to_string(c.tick) + " " + std::string(file) + ":" + std::to_string(c.line) + " " +
                       kKw[static_cast<int>(c.kw)] + " " + std::string(c.fn->name) + "(";
    for (size_t i = 0; i < c.args.size(); ++i) line += (i ? ", " : "") + format_value(c.args[i]);
    line += ") = " + format_value(result) + (vm_builtin ? " [vm]\n" : "\n");
    for (unsigned char ch : line) hash_ = (hash_ ^ ch) * 1099511628211ull;
    if (keep_log_) log_ += line;
    ++calls_;
}

}  // namespace joa::game
