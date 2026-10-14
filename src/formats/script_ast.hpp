// Scenario/unit script language of joan.exe (script/scenario/*.sce, unitAI.res bodies).
// Plain text, interpreted at load time (no bytecode). Grammar recovered from the token
// table at VA 0xa214c8 and the parser around VA 0x640000-0x649000; see
// docs/formats/SCRIPTS_AND_GAME_DATA.md.
//
//   script  := STRING '{' stmt* '}'
//   stmt    := (INT|FLOAT|STRING|VECTOR) ident
//            | INIT block | WAIT block | IF '(' expr ')' block [ELSE (block | if-stmt)]
//            | EXIT | ident '=' expr | expr
//   (GOTO, CHOICE, SPEAKER, RESET, ID, UNIT, OBJECT, EVENTSYSTEM are lexer keywords that no
//    shipped script uses; the parser rejects them.)
//   block   := '{' stmt* '}'
//   expr    := binary expression, precedence (CONFIRMED, VA 0x6484b0):
//              || 1, && 2, == != < > <= >= 3, + - 4, * / 5, ^ 6; all left-associative
//   primary := INT | FLOAT | STRING | ident | '(' expr ')' | XYZ '(' e ',' e ',' e ')'
//            | (FUNC|DIALOG|ACTION|CAMERA|SCRIPT) '(' args ')'
// Keywords and function names are case-insensitive (_stricmp). "//" starts a comment.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::formats {

struct ScriptExpr {
    enum class Kind : std::uint8_t { Int, Float, String, Var, Call, Vector, Binary };
    Kind kind = Kind::Int;
    // Int/Float: literal text; String: contents without quotes; Var: name;
    // Call: keyword upper-cased (FUNC, DIALOG, ACTION, CAMERA, SCRIPT); Binary: operator.
    std::string text;
    double number = 0;  // Int/Float value
    // Call: arguments in order (FUNC/DIALOG/ACTION: args[0] is the function-name String);
    // Vector: x, y, z; Binary: lhs, rhs.
    std::vector<ScriptExpr> args;
    int line = 0;
};

struct ScriptStmt {
    enum class Kind : std::uint8_t { Decl, Assign, Expr, If, Wait, Init, Exit };
    Kind kind = Kind::Expr;
    std::string type;  // Decl: INT/FLOAT/STRING/VECTOR (upper-cased)
    std::string name;  // Decl/Assign: variable
    ScriptExpr expr;   // Assign: value; Expr: expression; If: condition
    std::vector<ScriptStmt> body;       // If then-branch, Wait/Init block
    std::vector<ScriptStmt> else_body;  // If else-branch (an else-if is one nested If)
    int line = 0;
};

struct Script {
    std::string name;  // leading quoted name, e.g. "S1L1.sce"
    std::vector<ScriptStmt> body;
};

// err gets "line N: message" on failure. Trailing NULs (unitAI.res bodies) are ignored.
std::optional<Script> parse_script(std::string_view text, std::string* err = nullptr);

// Canonical pretty-print (tabs, one statement per line); parse(format(s)) == s.
std::string format_script(const Script& s);
std::string format_expr(const ScriptExpr& e);

// Visits every Call expression (pre-order, including nested ones).
void for_each_call(const Script& s, const std::function<void(const ScriptExpr&)>& fn);

}  // namespace joa::formats
