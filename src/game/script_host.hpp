// Script hosts that do not need a world: RecordingHost logs every builtin call and returns
// neutral values, so mission scripts can run headless (tests, tools/joa-script-run).
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "game/script_vm.hpp"
#include "formats/vfs_fs.hpp"

namespace joa::game {

// Reads script sources from the original install (case-insensitive game paths).
SourceLoader vfs_source_loader(const formats::Vfs& vfs);

class RecordingHost : public IScriptHost {
public:
    // Blocking builtins report "finished" on their (poll_delay + 1)-th poll. keep_log = false
    // only hashes the trace (long corpus runs).
    explicit RecordingHost(int poll_delay = 0, bool keep_log = true) : poll_delay_(poll_delay), keep_log_(keep_log) {}

    // int/float 0 and "" — except spawn-category builtins, which return fresh handles 1, 2, ...
    // so scripts see distinct recnos. Strings for String builtins are "".
    Value call(const BuiltinCall& c) override;
    bool poll(const BuiltinCall& c) override;
    // Appends "t=<tick> <file>:<line> FUNC name(args) = result" to log().
    void trace(const BuiltinCall& c, const Value& result, bool vm_builtin) override;

    const std::string& log() const { return log_; }
    std::uint64_t calls() const { return calls_; }
    std::uint64_t hash() const { return hash_; }  // FNV-1a of log()

private:
    int poll_delay_;
    bool keep_log_;
    std::int32_t next_handle_ = 1;
    std::map<const void*, int> polls_;
    std::string log_;
    std::uint64_t calls_ = 0;
    std::uint64_t hash_ = 1469598103934665603ull;
};

}  // namespace joa::game
