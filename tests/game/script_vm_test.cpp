// Semantic tests of the script VM against behaviour read from joan.exe (VAs cited per case;
// details in docs/reverse-engineering/SCRIPT_VM.md). No game data needed.
#include <cstdio>
#include <map>
#include <string>

#include "game/script_host.hpp"
#include "game/script_vm.hpp"

using namespace joa::game;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

using Files = std::map<std::string, std::string>;

struct Run {
    std::string log;
    std::vector<std::string> errors;
    bool ok = false;
};

static Run run(const std::string& body, int ticks, Files files = {}, int poll_delay = 0) {
    RecordingHost host(poll_delay);
    Vm vm([&](std::string_view p) -> std::optional<std::string> {
        for (auto& [k, v] : files)
            if (joa::formats::iequals(k, p)) return v;
        return std::nullopt;
    }, host);
    Run r;
    r.ok = vm.load_main_text("main.sce", "\"main.sce\"\n{\n" + body + "\n}\n");
    for (int i = 0; i < ticks && r.ok; ++i) vm.tick();
    r.log = host.log();
    r.errors = vm.errors();
    return r;
}

static bool has(const Run& r, const std::string& s) { return r.log.find(s) != std::string::npos; }
static size_t pos(const Run& r, const std::string& s) { return r.log.find(s); }
static std::string dump(const Run& r) {
    std::string s = r.log;
    for (auto& e : r.errors) s += "ERR " + e + "\n";
    return s;
}

int main() {
    {  // Precedence (FUN_006484b0): || 1, && 2, comparisons 3, + - 4, * / 5, ^ 6, left-assoc.
        Run r = run("INT x\n"
                    "x = ( 1 + 2 * 3 )\nFUNC(\"set_fog_distance\", x)\n"
                    "x = ( 2 * 3 ^ 2 )\nFUNC(\"set_fog_distance\", x)\n"
                    "x = ( 10 - 4 - 3 )\nFUNC(\"set_fog_distance\", x)\n"
                    "x = ( 1 || 0 && 0 )\nFUNC(\"set_fog_distance\", x)\n"
                    "x = ( 2 + 3 == 5 )\nFUNC(\"set_fog_distance\", x)\n",
                    1);
        CHECK(r.ok && has(r, "set_fog_distance(7)") && has(r, "set_fog_distance(18)") &&
                  has(r, "set_fog_distance(3)") && pos(r, "set_fog_distance(1)") != std::string::npos,
              "%s", dump(r).c_str());
    }
    {  // Arithmetic (0x646908..0x6469e0): wrapping int + - *, int / int -> FLOAT (fidiv),
       // ^ = ftol(pow()), assignment moves raw bits into an INT variable.
        Run r = run("INT x\n"
                    "FUNC(\"set_fog_distance\", ( 7 / 2 ))\n"
                    "x = ( 7 / 2 )\nFUNC(\"set_fog_distance\", x)\n"
                    "FUNC(\"set_fog_distance\", ( 7 ^ 2 ))\n"
                    "FUNC(\"set_fog_distance\", ( 2147483647 + 1 ))\n"
                    "FUNC(\"set_fog_distance\", ( 7 / 2 == 3.5 ))\n"
                    "FUNC(\"set_fog_distance\", ( 1 + 0.5 ))\n",
                    1);
        CHECK(has(r, "set_fog_distance(3.5f)"), "%s", dump(r).c_str());
        CHECK(has(r, "set_fog_distance(1080033280)"), "raw float bits in INT var\n%s", dump(r).c_str());  // 0x40600000
        CHECK(has(r, "set_fog_distance(49)") && has(r, "set_fog_distance(-2147483648)"), "%s", dump(r).c_str());
        CHECK(has(r, "set_fog_distance(1) = 0") && has(r, "set_fog_distance(1.5f)"), "%s", dump(r).c_str());
    }
    {  // WAIT + pause_script (FUN_00640370, FUN_0065c4a0/0065c4d0): wake = now + n, done when
       // wake < now; the clock advances 33 ms before each tick. exit restarts at the top.
        Run r = run("FUNC(\"set_fog_distance\", 1)\nWAIT { FUNC(\"pause_script\", 50) }\n"
                    "FUNC(\"set_fog_distance\", 2)\nexit\nFUNC(\"set_fog_distance\", 3)\n",
                    4);
        CHECK(has(r, "t=1 main.sce:3 FUNC set_fog_distance(1)") && has(r, "t=3 main.sce:5 FUNC set_fog_distance(2)") &&
                  has(r, "t=4 main.sce:3 FUNC set_fog_distance(1)") && !has(r, "t=2 main.sce:5") &&
                  !has(r, "set_fog_distance(3)"),
              "%s", dump(r).c_str());
    }
    {  // pause_script(33): wake = 33 + 33 = 66 is not < 66 at tick 2 -> done at tick 3.
        Run r = run("WAIT { FUNC(\"pause_script\", 33) }\nFUNC(\"set_fog_distance\", 2)\nexit\n", 3);
        CHECK(has(r, "t=3 main.sce:4") && !has(r, "t=2 main.sce:4"), "%s", dump(r).c_str());
    }
    {  // Blocking host builtin: body runs once, then polls each tick; the WAIT also keeps
       // non-blocking statements from re-running. exit inside a WAIT body is ignored.
        Run r = run("WAIT { DIALOG(\"add_dialog\", 1, \"hi\")\nexit\nFUNC(\"set_fog_distance\", 5) }\n"
                    "FUNC(\"set_fog_distance\", 6)\n",
                    3, {}, 2);
        CHECK(has(r, "t=1 main.sce:3 DIALOG add_dialog(1, \"hi\")") && has(r, "t=1 main.sce:5 FUNC set_fog_distance(5)") &&
                  has(r, "t=3 main.sce:6 FUNC set_fog_distance(6)") && !has(r, "t=2 main.sce:6") &&
                  r.log.find("add_dialog") == r.log.rfind("add_dialog"),
              "%s", dump(r).c_str());
    }
    {  // IF (FUN_006456f0): condition not re-tested when resuming a suspended branch; else-if.
        Run r = run("INT a\n"
                    "if ( a == 0 ) { a = 1\nWAIT { FUNC(\"pause_script\", 1) }\nFUNC(\"set_fog_distance\", 10) }\n"
                    "else if ( a == 1 ) { FUNC(\"set_fog_distance\", 20)\na = 2 }\n"
                    "else { FUNC(\"set_fog_distance\", 30) }\n",
                    4);
        CHECK(has(r, "t=2 main.sce:6 FUNC set_fog_distance(10)") && has(r, "t=3 main.sce:7 FUNC set_fog_distance(20)") &&
                  has(r, "t=4 main.sce:9 FUNC set_fog_distance(30)"),
              "%s", dump(r).c_str());
    }
    {  // init runs once (flag +0x70); exit (-1) abandons the pass; variables persist.
        Run r = run("INT n\ninit { FUNC(\"set_fog_distance\", 100) }\nn = ( n + 1 )\n"
                    "if ( n == 2 ) { exit }\nFUNC(\"set_fog_distance\", n)\n",
                    3);
        CHECK(r.log.find("(100)") == r.log.rfind("(100)") && has(r, "t=1 main.sce:7 FUNC set_fog_distance(1)") &&
                  !has(r, "t=2 main.sce:7") && has(r, "t=3 main.sce:7 FUNC set_fog_distance(3)"),
              "%s", dump(r).c_str());
    }
    {  // SCRIPT (FUN_00645d40, Script_open_compile 0x645de0): own variables, shared cached
       // instance (_stricmp key), WAIT inside suspends the caller, exit inside returns to it.
        Files f{{"sub.sce", "\"sub.sce\"\n{\nINT k\nk = ( k + 1 )\nFUNC(\"set_fog_distance\", ( k + 1000 ))\n"
                            "WAIT { FUNC(\"pause_script\", 1) }\nexit\nFUNC(\"set_fog_distance\", 999)\n}\n"}};
        Run r = run("INT k\nSCRIPT(\"sub.sce\")\nFUNC(\"set_fog_distance\", ( k + 5 ))\nSCRIPT(\"SUB.SCE\")\n", 4, f);
        CHECK(has(r, "t=1 sub.sce:5 FUNC set_fog_distance(1001)") && has(r, "t=2 main.sce:5 FUNC set_fog_distance(5)") &&
                  has(r, "t=2 sub.sce:5 FUNC set_fog_distance(1002)") && !has(r, "t=3 ") &&
                  has(r, "t=4 sub.sce:5 FUNC set_fog_distance(1003)") && !has(r, "999"),
              "%s", dump(r).c_str());
        Run bad = run("INT q\nSCRIPT(\"s2.sce\")\n", 1, {{"s2.sce", "\"s2.sce\"\n{\nq = 1\n}\n"}});
        CHECK(!bad.ok && !bad.errors.empty() && bad.errors[0].find("Variable not found") != std::string::npos,
              "caller variables must not be visible in a sub-script");
    }
    {  // Argument order (0x643b60: last to first) and lazy RPN operands (FUN_006463f0).
        Run r = run("INT x\nFUNC(\"set_unit_pos\", FUNC(\"get_unit_dir\", 1), FUNC(\"get_unit_dir\", 2))\n"
                    "x = ( FUNC(\"get_unit_dir\", 3) + FUNC(\"get_unit_dir\", 4) * FUNC(\"get_unit_dir\", 5) )\n"
                    "if ( 0 && FUNC(\"get_unit_dir\", 6) ) { }\nif ( 1 || FUNC(\"get_unit_dir\", 7) ) { }\n",
                    1);
        CHECK(pos(r, "get_unit_dir(2)") < pos(r, "get_unit_dir(1)"), "%s", dump(r).c_str());
        CHECK(pos(r, "get_unit_dir(4)") < pos(r, "get_unit_dir(5)") && pos(r, "get_unit_dir(5)") < pos(r, "get_unit_dir(3)"),
              "%s", dump(r).c_str());
        CHECK(!has(r, "get_unit_dir(6)") && has(r, "get_unit_dir(7)"), "%s", dump(r).c_str());
    }
    {  // Executors know only their own ids: ACTION("stop_curr_task") is a no-op (FUN_00661060).
        Run r = run("ACTION(\"stop_curr_task\", 1)\nFUNC(\"stop_curr_task\", 2)\nACTION(\"give_task\", 3, \"IDLE\")\n", 1);
        CHECK(!has(r, "stop_curr_task(1)") && has(r, "FUNC stop_curr_task(2)") && has(r, "ACTION give_task(3, \"IDLE\")"),
              "%s", dump(r).c_str());
    }
    {  // random_number = Borland LCG (FUN_00590540), seed 0.
        Run r = run("INT i\ni = 0\n"
                    "FUNC(\"set_fog_distance\", FUNC(\"random_number\", 100))\nFUNC(\"set_fog_distance\", FUNC(\"random_number\", 100))\n"
                    "FUNC(\"set_fog_distance\", FUNC(\"random_number\", 100))\nFUNC(\"set_fog_distance\", FUNC(\"random_number\", 100))\n",
                    1);
        const size_t a = pos(r, "random_number(100) = 0"), b = pos(r, "random_number(100) = 1"),
                     c = pos(r, "random_number(100) = 33");
        CHECK(a < b && b < c && c != std::string::npos, "%s", dump(r).c_str());
    }
    {  // Bit flags (FUN_00472000/004720d0): bit n-1, value & 0xff shifted (3 sets two bits).
        Run r = run("FUNC(\"set_scenario_bit_flag\", 3, 3)\n"
                    "FUNC(\"set_fog_distance\", FUNC(\"get_scenario_bit_flag\", 3), FUNC(\"get_scenario_bit_flag\", 4), "
                    "FUNC(\"get_scenario_bit_flag\", 2))\n"
                    "FUNC(\"backup_bit_flag\")\nFUNC(\"set_scenario_bit_flag\", 3, 0)\nFUNC(\"restore_bit_flag\")\n"
                    "FUNC(\"set_scenario_stage\", ( FUNC(\"get_scenario_stage\") + 4 ))\n"
                    "FUNC(\"set_fog_distance\", FUNC(\"get_scenario_bit_flag\", 3), FUNC(\"get_scenario_stage\"))\n",
                    1);
        CHECK(has(r, "set_fog_distance(1, 1, 0)") && has(r, "set_fog_distance(1, 4)"), "%s", dump(r).c_str());
    }
    {  // format_string with %Ns / %N,Wd (FUN_005a31e0) and a string literal + compare.
        Run r = run("FUNC(\"attention_call_message\", FUNC(\"format_string\", \"a %2s b %1d%% %1,4d\", 1234567, \"x\"))\n"
                    "FUNC(\"set_fog_distance\", ( \"abc\" < \"abd\" ), ( \"ab\" + \"c\" == \"abc\" ))\n",
                    1);
        CHECK(has(r, "= \"a x b 1,234,567% 1234567\"") && has(r, "set_fog_distance(1, 1)"), "%s", dump(r).c_str());
    }
    {  // load_pass (FUN_0041c810): one exec at the load clock; init done there, not again in tick 1.
        RecordingHost host;
        Vm vm([](std::string_view) { return std::optional<std::string>(); }, host);
        CHECK(vm.load_main_text("m.sce", "\"m.sce\"\n{\ninit { FUNC(\"set_fog_distance\", 7) }\n"
                                         "FUNC(\"set_fog_distance\", FUNC(\"get_scenario_count_time\"))\n}\n"),
              "compile");
        vm.load_pass();
        CHECK(vm.game_ms() == 0 && vm.ticks() == 0, "load_pass must not advance the clock");
        vm.tick();
        const std::string& l = host.log();
        CHECK(l.find("t=0 m.sce:3 FUNC set_fog_distance(7)") != std::string::npos && l.find("(7)") == l.rfind("(7)") &&
                  l.find("t=1 m.sce:4 FUNC get_scenario_count_time() = 0") != std::string::npos,
              "%s", l.c_str());
    }
    {  // Runtime errors are logged once per message, not per tick (errors_ is saved and hashed); the
       // dedupe set is rebuilt from the saved list on load.
        RecordingHost host;
        auto none = [](std::string_view) { return std::optional<std::string>(); };
        const char* src = "\"m.sce\"\n{\nINT x\nWAIT { x = 1 }\nFUNC(\"set_fog_distance\", ( 1 - \"a\" ))\n}\n";
        Vm vm(none, host);
        CHECK(vm.load_main_text("m.sce", src), "compile");
        for (int i = 0; i < 5; ++i) vm.tick();
        std::string all;
        for (auto& e : vm.errors()) all += e + "\n";
        CHECK(vm.errors().size() == 2, "%s", all.c_str());
        std::vector<std::uint8_t> save;
        vm.save(save);
        Vm vm2(none, host);
        CHECK(vm2.load_main_text("m.sce", src) && vm2.load(save) && vm2.hash() == vm.hash(), "reload");
        for (int i = 0; i < 5; ++i) vm.tick(), vm2.tick();
        CHECK(vm.errors().size() == 2 && vm2.errors().size() == 2 && vm2.hash() == vm.hash(), "after reload");
    }
    // Compile errors of the original: unknown function, undeclared variable, non-call statement,
    // DIALOG/ACTION inside a math expression (FUN_00647c90 accepts only FUNC).
    for (auto* bad : {"FUNC(\"no_such_function\")\n", "x = 1\n", "5\n", "if ( DIALOG(\"get_choice\", 1) == 1 ) { }\n"}) {
        Run r = run(bad, 1);
        CHECK(!r.ok, "must not compile: %s", bad);
    }
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
