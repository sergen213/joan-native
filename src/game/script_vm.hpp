// Mission-script interpreter, a faithful re-implementation of joan.exe's script runtime
// (esstate2.cpp: compile VA 0x649280, node factory 0x640db0, executors 0x63f790..0x6463f0).
// Semantics and evidence: docs/reverse-engineering/SCRIPT_VM.md.
//
// Model (all CONFIRMED from code, see the doc):
// - A script compiles to a tree of nodes that keep their own resume cursors. The scenario
//   driver (FUN_00471620, once per 30 Hz logic tick) calls exec on the main script root.
//   Exec returns 1 = finished, 0 = suspended (resume at the same node next tick),
//   -1 = `exit` (abandon this pass; next tick starts at the top again).
// - `init {}` runs once (done flag, set only when its body completes).
// - `WAIT {}` executes its body once, then each tick polls the "is finished" predicate of
//   every FUNC/DIALOG/ACTION call statement in it (only blocking builtins can be unfinished).
// - `SCRIPT("x.sce")` runs a sub-script compiled once and shared through a by-name cache;
//   variables are per declaring scope, persistent, initialised to 0/"" at compile time.
// - Values are {type, raw 32-bit payload}: assignment and builtin argument reads move the raw
//   bits without conversion, exactly as the original does (int / int yields a FLOAT).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "formats/scenario_def.hpp"
#include "formats/script_funcs.hpp"

namespace joa::game {

using formats::ScriptType;

// A script value: type tag (node+0xc) and payload (node+0x10).
struct Value {
    ScriptType type = ScriptType::Int;
    std::uint32_t bits = 0;  // Int value, or IEEE-754 binary32 bits when type == Float
    std::string str;         // String payload

    static Value integer(std::int32_t v);
    static Value real(float v);
    static Value string(std::string v);
    // Builtin argument reads are raw: the original loads node+0x10 as an int (`mov`) or a
    // float (`fld`) whatever the type tag says. A String read as int has no defined value in
    // the original (it is a char* there); 0 here.
    std::int32_t as_int() const { return type == ScriptType::String ? 0 : static_cast<std::int32_t>(bits); }
    float as_float() const;
    // IF truthiness (FUN_006456f0): int != 0, float != 0.0f, string non-empty.
    bool truthy() const;
    bool operator==(const Value&) const = default;
};
std::string format_value(const Value& v);  // 12, 3.5f, "text"

// Shared engine RNG (object 0xa45fa8, seed at +0x7a; FUN_00590540). The original seeds it
// with time(NULL) in WinMain (FUN_005904e0) and 79 engine functions draw from the same stream.
struct Rng {
    std::uint32_t seed = 0;
    // Borland LCG: seed = seed * 0x15a4e35 + 1; returns (u32)(n * ((seed >> 16) & 0x7fff)) >> 15,
    // i.e. [0, n) for 0 <= n <= 0x10000.
    std::int32_t next(std::int32_t n);
};

// Per-scenario script state that the original keeps in the campaign step record
// (DAT_00a58fb7 + step*0x1b2 + 0x18a..0x1ae) and the scenario object (0xa58f90..); save-game
// state. The world layer reads/writes it too (end conditions set game_finish, cut scenes pause).
struct ScenarioState {
    std::uint32_t bits[4] = {};    // scenario / event / event_2 / event_3 bit flags (+0x18a..+0x196)
    std::uint32_t backup[4] = {};  // backup_bit_flag copy (DAT_00a58f97..)
    std::int32_t stage = 0;        // get/set_scenario_stage (+0x19a)
    std::int32_t mode = 0;         // set_scenario_mode: NORMAL 0, INTERACT 1, TRAINING 2, BATTLE/other 3 (+0x1ae)
    std::uint8_t game_finish = 0;  // DAT_00a59298: 0 running, 1 won, 2 lost (scripts stop), 3 other
    std::int32_t time_limit = -1, time_remaining = -1;  // set/get_game_time_limit (+0x1a6/+0x1aa), ms
    std::int32_t objective_count_time = 0;   // set/get_objective_count_time (DAT_00a591cc), seconds
    std::uint32_t count_time[3] = {};        // scenario_count_time 1..3 (DAT_00a59171/75/79), ms
    std::uint32_t challenge_restart[4][5] = {};  // [STRENGTH,DEXTERITY,LEADERSHIP,DEFENSE][slot-1] (DAT_00a59244), ms
    std::int8_t challenge_flag[4][5] = {};       // DAT_00a59230
    std::uint32_t challenge_duration = 0;        // DAT_00a59294, ms
    std::uint32_t start_ms = 0;              // game clock at scenario load (DAT_00a5916d)
    // Host-owned gate of FUN_004cd5d0: true while a cut scene runs or the game clock is stopped;
    // count times and the time limit do not advance then.
    bool counting_paused = false;
};

enum class Keyword : std::uint8_t { Func, Dialog, Action, Camera };

// Builtin classification (docs/reverse-engineering/SCRIPT_VM.md has the per-id table).
enum class BuiltinCategory : std::uint8_t {
    Math, Flags, Timer, Query, UnitOrder, UnitState, Spawn, Item, Dialog, Camera, Objective, Sound, Ui, World
};
BuiltinCategory builtin_category(std::uint16_t id);  // id 1..407
std::string_view category_name(BuiltinCategory c);
// True when a WAIT polls this call (FUN_006509e0 / 0x660e60 / 0x6612e0).
bool is_blocking_builtin(Keyword kw, std::uint16_t id);
// True for builtins the VM implements itself (world-independent); the host never sees them.
bool is_vm_builtin(std::uint16_t id);

struct ScriptText;  // EX.res localisation table of one script file

// One executed builtin call, as handed to the host.
struct BuiltinCall {
    const formats::ScriptFunc* fn = nullptr;
    Keyword kw = Keyword::Func;
    std::span<const Value> args;  // script arguments after the function name, in order
    std::string_view script;      // game path of the calling script file
    int line = 0;
    const void* site = nullptr;   // identity of the call node (valid while the Vm lives)
    std::uint32_t site_id = 0;    // compile-order ordinal of the call node: stable across a
                                  // recompile of the same sources (save games)
    std::uint64_t tick = 0;       // Vm::ticks() of the executing tick (0 during load)
    const ScriptText* text = nullptr;

    // Raw argument reads; missing trailing arguments read as 0 / "".
    std::int32_t i(size_t n) const { return n < args.size() ? args[n].as_int() : 0; }
    float f(size_t n) const { return n < args.size() ? args[n].as_float() : 0.0f; }
    std::string_view s(size_t n) const { return n < args.size() ? std::string_view(args[n].str) : std::string_view(); }
    // FUN_006380b0 localisation: "[NNNNx]..." -> text of the calling script's EX.res entry whose
    // number equals atoi(NNNN); unchanged (tag included) when there is no table or match.
    std::string localized(size_t n) const;
};

// Binds builtins to the world. Called synchronously from Vm::tick().
class IScriptHost {
public:
    virtual ~IScriptHost() = default;
    // Every builtin except is_vm_builtin() ones. Result type follows fn->ret.
    virtual Value call(const BuiltinCall& c) = 0;
    // WAIT completion predicate for is_blocking_builtin() calls (args are those of the call).
    virtual bool poll(const BuiltinCall& c) = 0;
    // Observer for every executed builtin (VM ones too), after it ran.
    virtual void trace(const BuiltinCall& c, const Value& result, bool vm_builtin) { (void)c, (void)result, (void)vm_builtin; }
};

// Supplies script source text by game path (e.g. "script\\scenario\\S1L1_tactic.sce").
using SourceLoader = std::function<std::optional<std::string>(std::string_view game_path)>;

class Vm {
public:
    static constexpr std::uint32_t kTickMs = 33;  // 30 Hz; 0x21 is the original's fixed-step ms

    Vm(SourceLoader load, IScriptHost& host, std::uint32_t seed = 0);
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    // Original scenario load (FUN_0041cc80): compile SCRIPT\SCENARIO\<ScriptFileName>.SCE as the
    // main script, then compile <DescriptionFileName>.SCE, exec it once and discard it.
    bool load_scenario(const formats::ScenarioStep& step);
    // Compile `text` (or the file at game_path) as the main script.
    bool load_main(std::string_view game_path);
    bool load_main_text(std::string_view name, std::string_view text);
    // Compile, exec once, discard (how _des scripts run). Returns the exec code.
    std::optional<int> run_once(std::string_view game_path);

    // One logic tick in the order of the scenario tick FUN_00471620: game clock += kTickMs;
    // challenge timers count down; the main script root is executed once (not after a loss);
    // then count times / time limit advance (FUN_004cd5d0).
    // Returns the root's exec code (1, 0 or -1); 1 when nothing ran.
    int tick();
    // Extra exec of the main script root that Battle::load_world (FUN_0041c810, also used on a
    // scenario-step change) does after the world and units are loaded, before the first tick:
    // `vtbl[0x24]` returns the script itself and FUN_0063ed40 "resets the cursor" of that node,
    // which only rewrites its type field with 1 (no-op), then `exec`. No clock, timer or
    // game-finish handling. Returns the exec code; 1 when no script is loaded.
    int load_pass();

    std::uint32_t game_ms() const { return game_ms_; }
    std::uint64_t ticks() const { return ticks_; }
    ScenarioState& state() { return state_; }
    Rng& rng() { return rng_; }
    // Compile and runtime diagnostics ("file:line: message"). Runtime errors are conditions
    // that are undefined or a crash in the original (they never occur in shipped scripts).
    const std::vector<std::string>& errors() const { return errors_; }

    // Complete runtime state (node cursors, init/WAIT flags, every node value and variable of
    // the main script and the SCRIPT cache, pause wake time, clock, ScenarioState, RNG,
    // errors), little-endian, walked in compile order. load() only accepts data saved by a Vm
    // that compiled the same sources the same way (structure hash), else returns false and
    // leaves the Vm unchanged.
    void save(std::vector<std::uint8_t>& out) const;
    bool load(std::span<const std::uint8_t> in);
    std::uint64_t hash() const;  // FNV-1a of save()

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::uint32_t game_ms_ = 0;
    std::uint64_t ticks_ = 0;
    ScenarioState state_;
    Rng rng_;
    std::vector<std::string> errors_;
};

}  // namespace joa::game
