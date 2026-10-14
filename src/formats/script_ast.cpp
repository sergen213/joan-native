#include "formats/script_ast.hpp"

#include <array>
#include <cctype>
#include <cstdlib>

#include "formats/vfs_fs.hpp"

namespace joa::formats {
namespace {

enum class Tok : std::uint8_t { End, Int, Float, String, Ident, Keyword, Punct };

struct Token {
    Tok kind = Tok::End;
    std::string text;  // keyword upper-cased; punct/operator literal
    int line = 0;
};

// Keyword tokens of the original lexer table (VA 0xa21570..0xa21620).
constexpr std::array<std::string_view, 23> kKeywords = {
    "EVENTSYSTEM", "ID",   "INIT",   "RESET",  "WAIT",   "CHOICE", "IF",  "ELSE",
    "SPEAKER",     "UNIT", "OBJECT", "GOTO",   "EXIT",   "FUNC",   "DIALOG", "CAMERA",
    "ACTION",      "XYZ",  "SCRIPT", "INT",    "FLOAT",  "STRING", "VECTOR"};

struct ParseFail {
    std::string msg;
};

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    Token next() {
        skip();
        Token t;
        t.line = line_;
        if (p_ >= s_.size()) return t;
        char c = s_[p_];
        auto digit = [&](size_t i) { return i < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i])); };
        if (c == '"') {
            size_t e = s_.find_first_of("\"\n", p_ + 1);
            if (e == std::string_view::npos || s_[e] != '"') throw ParseFail{"line " + std::to_string(line_) + ": unterminated string"};
            t.kind = Tok::String;
            t.text = std::string(s_.substr(p_ + 1, e - p_ - 1));
            p_ = e + 1;
            return t;
        }
        // The original classifies a token starting with a digit or '-' as a number; binary
        // minus is always written space-separated in shipped scripts.
        if (digit(p_) || ((c == '-' || c == '.') && (digit(p_ + 1) || (c == '-' && s_.substr(p_ + 1, 1) == "." && digit(p_ + 2))))) {
            size_t b = p_++;
            while (p_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.')) ++p_;
            t.text = std::string(s_.substr(b, p_ - b));
            t.kind = t.text.find('.') == std::string::npos ? Tok::Int : Tok::Float;
            return t;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t b = p_;
            while (p_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[p_])) || s_[p_] == '_')) ++p_;
            t.text = std::string(s_.substr(b, p_ - b));
            t.kind = Tok::Ident;
            for (auto k : kKeywords)
                if (iequals(k, t.text)) {
                    t.kind = Tok::Keyword;
                    t.text = std::string(k);
                }
            return t;
        }
        static constexpr std::array<std::string_view, 6> two = {"==", "!=", "<=", ">=", "&&", "||"};
        for (auto op : two)
            if (s_.substr(p_, 2) == op) {
                t.kind = Tok::Punct;
                t.text = std::string(op);
                p_ += 2;
                return t;
            }
        if (std::string_view("+-*/^<>=(){},:").find(c) == std::string_view::npos)
            throw ParseFail{"line " + std::to_string(line_) + ": unexpected character '" + std::string(1, c) + "'"};
        t.kind = Tok::Punct;
        t.text = std::string(1, c);
        ++p_;
        return t;
    }

private:
    void skip() {
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (c == '\0') {
                p_ = s_.size();
            } else if (c == '\n') {
                ++line_;
                ++p_;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == ';') {
                // ';' is not a token in the original; a stray "0;" lexes as the number 0
                // there (S2L1_tactic.sce), so it is skipped like whitespace.
                ++p_;
            } else if (s_.substr(p_, 2) == "//") {
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
            } else {
                break;
            }
        }
    }

    std::string_view s_;
    size_t p_ = 0;
    int line_ = 1;
};

int precedence(std::string_view op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") return 3;
    if (op == "+" || op == "-") return 4;
    if (op == "*" || op == "/") return 5;
    if (op == "^") return 6;
    return 0;
}

class Parser {
public:
    explicit Parser(std::string_view s) : lex_(s) { tok_ = lex_.next(); }

    Script script() {
        Script s;
        if (tok_.kind != Tok::String) fail("expected script name string");
        s.name = take().text;
        s.body = block();
        if (tok_.kind != Tok::End) fail("text after closing '}'");
        return s;
    }

private:
    [[noreturn]] void fail(const std::string& m) const { throw ParseFail{"line " + std::to_string(tok_.line) + ": " + m}; }
    Token take() {
        Token t = std::move(tok_);
        tok_ = lex_.next();
        return t;
    }
    bool is(Tok k, std::string_view text) const { return tok_.kind == k && tok_.text == text; }
    void expect(std::string_view p) {
        if (!is(Tok::Punct, p)) fail("expected '" + std::string(p) + "', got '" + tok_.text + "'");
        take();
    }

    std::vector<ScriptStmt> block() {
        expect("{");
        std::vector<ScriptStmt> out;
        while (!is(Tok::Punct, "}")) {
            if (tok_.kind == Tok::End) fail("missing '}'");
            out.push_back(stmt());
        }
        take();
        return out;
    }

    ScriptStmt stmt() {
        ScriptStmt s;
        s.line = tok_.line;
        if (tok_.kind == Tok::Keyword) {
            const std::string kw = tok_.text;
            if (kw == "INT" || kw == "FLOAT" || kw == "STRING" || kw == "VECTOR") {
                take();
                if (tok_.kind != Tok::Ident) fail("expected variable name after " + kw);
                s.kind = ScriptStmt::Kind::Decl;
                s.type = kw;
                s.name = take().text;
                return s;
            }
            if (kw == "INIT" || kw == "WAIT") {
                take();
                s.kind = kw == "INIT" ? ScriptStmt::Kind::Init : ScriptStmt::Kind::Wait;
                s.body = block();
                return s;
            }
            if (kw == "IF") {
                take();
                s.kind = ScriptStmt::Kind::If;
                expect("(");
                s.expr = expr(1);
                expect(")");
                s.body = block();
                if (is(Tok::Keyword, "ELSE")) {
                    take();
                    if (is(Tok::Keyword, "IF"))
                        s.else_body.push_back(stmt());
                    else
                        s.else_body = block();
                }
                return s;
            }
            if (kw == "EXIT") {
                take();
                s.kind = ScriptStmt::Kind::Exit;
                return s;
            }
            if (kw != "FUNC" && kw != "DIALOG" && kw != "ACTION" && kw != "CAMERA" && kw != "SCRIPT" && kw != "XYZ")
                fail("unsupported keyword " + kw + " (not used by shipped scripts)");
        }
        if (tok_.kind == Tok::Ident) {
            Token id = take();
            if (is(Tok::Punct, "=")) {
                take();
                s.kind = ScriptStmt::Kind::Assign;
                s.name = id.text;
                s.expr = expr(1);
                return s;
            }
            fail("expected '=' after " + id.text);
        }
        s.kind = ScriptStmt::Kind::Expr;
        s.expr = expr(1);
        return s;
    }

    ScriptExpr expr(int min_prec) {
        ScriptExpr lhs = primary();
        while (tok_.kind == Tok::Punct) {
            int p = precedence(tok_.text);
            if (p == 0 || p < min_prec) break;
            ScriptExpr b;
            b.kind = ScriptExpr::Kind::Binary;
            b.line = tok_.line;
            b.text = take().text;
            b.args.push_back(std::move(lhs));
            b.args.push_back(expr(p + 1));
            lhs = std::move(b);
        }
        return lhs;
    }

    ScriptExpr primary() {
        ScriptExpr e;
        e.line = tok_.line;
        switch (tok_.kind) {
        case Tok::Int:
        case Tok::Float:
            e.kind = tok_.kind == Tok::Int ? ScriptExpr::Kind::Int : ScriptExpr::Kind::Float;
            e.number = std::strtod(tok_.text.c_str(), nullptr);
            e.text = take().text;
            return e;
        case Tok::String:
            e.kind = ScriptExpr::Kind::String;
            e.text = take().text;
            return e;
        case Tok::Ident:
            e.kind = ScriptExpr::Kind::Var;
            e.text = take().text;
            return e;
        case Tok::Punct:
            if (tok_.text == "(") {
                take();
                e = expr(1);
                expect(")");
                return e;
            }
            break;
        case Tok::Keyword:
            if (tok_.text == "XYZ" || tok_.text == "FUNC" || tok_.text == "DIALOG" || tok_.text == "ACTION" ||
                tok_.text == "CAMERA" || tok_.text == "SCRIPT") {
                e.kind = tok_.text == "XYZ" ? ScriptExpr::Kind::Vector : ScriptExpr::Kind::Call;
                e.text = take().text;
                expect("(");
                if (!is(Tok::Punct, ")")) {
                    e.args.push_back(expr(1));
                    while (is(Tok::Punct, ",")) {
                        take();
                        e.args.push_back(expr(1));
                    }
                }
                expect(")");
                if (e.kind == ScriptExpr::Kind::Vector && e.args.size() != 3) fail("XYZ needs 3 components");
                if (e.kind == ScriptExpr::Kind::Call && e.text != "SCRIPT" &&
                    (e.args.empty() || e.args[0].kind != ScriptExpr::Kind::String))
                    fail(e.text + " needs a function-name string");
                return e;
            }
            break;
        case Tok::End:
            break;
        }
        fail("unexpected '" + tok_.text + "' in expression");
    }

    Lexer lex_;
    Token tok_;
};

void indent(std::string& out, int depth) { out.append(size_t(depth), '\t'); }

void format_block(std::string& out, const std::vector<ScriptStmt>& body, int depth);

void format_stmt(std::string& out, const ScriptStmt& s, int depth, bool chained_if = false) {
    if (!chained_if) indent(out, depth);
    switch (s.kind) {
    case ScriptStmt::Kind::Decl: out += s.type + " " + s.name + "\n"; break;
    case ScriptStmt::Kind::Assign: out += s.name + " = " + format_expr(s.expr) + "\n"; break;
    case ScriptStmt::Kind::Expr: out += format_expr(s.expr) + "\n"; break;
    case ScriptStmt::Kind::Exit: out += "exit\n"; break;
    case ScriptStmt::Kind::Init:
    case ScriptStmt::Kind::Wait:
        out += s.kind == ScriptStmt::Kind::Init ? "init\n" : "WAIT\n";
        format_block(out, s.body, depth);
        break;
    case ScriptStmt::Kind::If:
        out += "if( " + format_expr(s.expr) + " )\n";
        format_block(out, s.body, depth);
        if (s.else_body.size() == 1 && s.else_body[0].kind == ScriptStmt::Kind::If) {
            indent(out, depth);
            out += "else ";
            format_stmt(out, s.else_body[0], depth, true);
        } else if (!s.else_body.empty()) {
            indent(out, depth);
            out += "else\n";
            format_block(out, s.else_body, depth);
        }
        break;
    }
}

void format_block(std::string& out, const std::vector<ScriptStmt>& body, int depth) {
    indent(out, depth);
    out += "{\n";
    for (const auto& s : body) format_stmt(out, s, depth + 1);
    indent(out, depth);
    out += "}\n";
}

void visit(const ScriptExpr& e, const std::function<void(const ScriptExpr&)>& fn) {
    if (e.kind == ScriptExpr::Kind::Call) fn(e);
    for (const auto& a : e.args) visit(a, fn);
}

void visit(const std::vector<ScriptStmt>& body, const std::function<void(const ScriptExpr&)>& fn) {
    for (const auto& s : body) {
        visit(s.expr, fn);
        visit(s.body, fn);
        visit(s.else_body, fn);
    }
}

}  // namespace

std::optional<Script> parse_script(std::string_view text, std::string* err) {
    try {
        return Parser(text).script();
    } catch (const ParseFail& f) {
        if (err) *err = f.msg;
        return std::nullopt;
    }
}

std::string format_expr(const ScriptExpr& e) {
    switch (e.kind) {
    case ScriptExpr::Kind::Int:
    case ScriptExpr::Kind::Float:
    case ScriptExpr::Kind::Var: return e.text;
    case ScriptExpr::Kind::String: return "\"" + e.text + "\"";
    case ScriptExpr::Kind::Binary: {
        auto sub = [](const ScriptExpr& a) {
            return a.kind == ScriptExpr::Kind::Binary ? "( " + format_expr(a) + " )" : format_expr(a);
        };
        return sub(e.args[0]) + " " + e.text + " " + sub(e.args[1]);
    }
    case ScriptExpr::Kind::Call:
    case ScriptExpr::Kind::Vector: {
        std::string out = e.text + "(";
        for (size_t i = 0; i < e.args.size(); ++i) out += (i ? ", " : "") + format_expr(e.args[i]);
        return out + ")";
    }
    }
    return {};
}

std::string format_script(const Script& s) {
    std::string out = "\"" + s.name + "\"\n";
    format_block(out, s.body, 0);
    return out;
}

void for_each_call(const Script& s, const std::function<void(const ScriptExpr&)>& fn) { visit(s.body, fn); }

}  // namespace joa::formats
