#include "game/script_vm.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>

#include "formats/script_ast.hpp"
#include "formats/text_strings.hpp"
#include "formats/vfs_fs.hpp"

namespace joa::game {

using formats::ScriptExpr;
using formats::ScriptFunc;
using formats::ScriptStmt;

Value Value::integer(std::int32_t v) { return {ScriptType::Int, static_cast<std::uint32_t>(v), {}}; }
Value Value::real(float v) { return {ScriptType::Float, std::bit_cast<std::uint32_t>(v), {}}; }
Value Value::string(std::string v) { return {ScriptType::String, 0, std::move(v)}; }
float Value::as_float() const { return type == ScriptType::String ? 0.0f : std::bit_cast<float>(bits); }

bool Value::truthy() const {
    switch (type) {
    case ScriptType::Int: return bits != 0;
    case ScriptType::Float: return as_float() != 0.0f;
    case ScriptType::String: return !str.empty();
    }
    return false;
}

std::string format_value(const Value& v) {
    char buf[48];
    switch (v.type) {
    case ScriptType::Int: std::snprintf(buf, sizeof buf, "%d", v.as_int()); return buf;
    case ScriptType::Float: std::snprintf(buf, sizeof buf, "%gf", static_cast<double>(v.as_float())); return buf;
    case ScriptType::String: return "\"" + formats::latin1_to_utf8(v.str) + "\"";
    }
    return {};
}

std::int32_t Rng::next(std::int32_t n) {
    seed = seed * 0x15a4e35u + 1u;
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(n) * ((seed >> 16) & 0x7fffu)) >> 15);
}

bool is_blocking_builtin(Keyword kw, std::uint16_t id) {
    switch (kw) {
    case Keyword::Func:  // FUN_006509e0
        return id == 296 || id == 297 || id == 301 || id == 302 || id == 303 || id == 304 || id == 329 ||
               id == 330 || id == 385 || id == 390 || id == 398;
    case Keyword::Dialog: return id == 316 || id == 319;  // FUN_00660e60 (322 returns 0x101: done)
    case Keyword::Action: return id == 323;               // FUN_006612e0
    case Keyword::Camera: return false;                   // FUN_00661040 returns 1
    }
    return false;
}

bool is_vm_builtin(std::uint16_t id) {
    return id == 1 || id == 204 || id == 206 || id == 207 || id == 208 || (id >= 219 && id <= 226) ||
           (id >= 254 && id <= 259) || (id >= 277 && id <= 288) || id == 291 || id == 292 || id == 330 ||
           id == 338 || id == 363;
}

// EX.res table of one script file: {atoi(tag), text} in file order (FUN_006381a0).
struct ScriptText {
    std::vector<std::pair<std::uint16_t, std::string>> entries;
};

std::string BuiltinCall::localized(size_t n) const {
    std::string_view v = s(n);
    if (text && !v.empty() && v[0] == '[') {
        const std::uint32_t num = static_cast<std::uint32_t>(std::atoi(std::string(v.substr(1)).c_str()));
        for (const auto& [k, t] : text->entries)
            if (k == num) return t;
    }
    return std::string(v);
}

namespace {

// Original string objects are 0x191-byte buffers; copies go through strncpy(..., 400)
// (FUN_005a1440) and concatenation through strncat to 400 (FUN_005a14f0).
constexpr size_t kStrMax = 400;
Value clip(Value v) {
    if (v.type == ScriptType::String && v.str.size() > kStrMax) v.str.resize(kStrMax);
    return v;
}

enum class Op : std::uint8_t { Add, Sub, Mul, Div, Pow, Lt, Gt, Le, Ge, Eq, Ne, And, Or };

struct Var {
    Value v;
};

struct Unit;

struct Expr {
    enum class K : std::uint8_t { Lit, Var, Call, Script, Bin } k = K::Lit;
    Value val;  // last result, persists like node+0xc/+0x10
    int line = 0;
    Var* var = nullptr;
    Op op = Op::Add;
    std::unique_ptr<Expr> l, r;
    const ScriptFunc* fn = nullptr;
    Keyword kw = Keyword::Func;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<Value> argv;  // argument values of the last execution (read again by WAIT polls)
    Unit* unit = nullptr;     // Call: calling script; Script: the sub-script
};

struct Stmt {
    enum class K : std::uint8_t { Block, Init, Wait, If, Cond, Else, Exit, Assign, Call, Script } k = K::Block;
    int line = 0;
    std::vector<std::unique_ptr<Stmt>> kids;  // If: Cond, then..., [Else, else...]
    size_t cur = 0;                           // resume cursor (DynArray current index)
    bool flag = false;                        // Init: done (+0x70); Wait: body executed (+0x20)
    std::unique_ptr<Expr> e;                  // Cond/Call/Script: the expression; Assign: rhs
    Var* var = nullptr;                       // Assign target
};

struct Unit {
    std::string path;
    Stmt root;
    std::deque<Var> vars;
    ScriptText text;
};

struct CompileError {
    std::string msg;
};

bool str_iequal(std::string_view a, std::string_view b) { return formats::iequals(a, b); }

}  // namespace

struct Vm::Impl {
    Vm& vm;
    SourceLoader load;
    IScriptHost& host;
    std::unique_ptr<Unit> main;
    std::vector<std::unique_ptr<Unit>> cache;  // SCRIPT() sub-scripts (FUN_00648c50, _stricmp)
    std::uint32_t pause_wake = 0;              // DAT_00a6a254

    // ---------------------------------------------------------------- compile
    struct Scope {
        std::vector<std::pair<std::string, Var*>> vars;
    };
    struct Ctx {
        Unit* unit;
        std::vector<Scope> scopes;
    };

    [[noreturn]] static void fail(const Ctx& c, int line, const std::string& m) {
        throw CompileError{c.unit->path + ":" + std::to_string(line) + ": " + m};
    }

    // Lookup order of FUN_0063ee70: own list newest first, then the parent node; _stricmp.
    static Var* find_var(const Ctx& c, std::string_view name) {
        for (auto s = c.scopes.rbegin(); s != c.scopes.rend(); ++s)
            for (auto v = s->vars.rbegin(); v != s->vars.rend(); ++v)
                if (str_iequal(v->first, name)) return v->second;
        return nullptr;
    }

    void load_text(Unit& u) {
        // FUN_0063f580: "<path up to the first '.'>EX.res", optional.
        std::string stem = u.path.substr(0, u.path.find('.'));
        if (auto t = load(stem + "EX.res")) {
            if (auto tags = formats::parse_tagged_strings(*t))
                for (auto& ts : *tags)
                    u.text.entries.emplace_back(static_cast<std::uint16_t>(std::atoi(ts.tag.c_str())), std::move(ts.text));
        }
    }

    void compile_unit(Unit& u, std::string_view text) {
        std::string err;
        auto ast = formats::parse_script(text, &err);
        if (!ast) throw CompileError{u.path + ": " + err};
        Ctx c{&u, {Scope{}}};
        u.root.k = Stmt::K::Block;
        compile_body(c, ast->body, u.root);
        load_text(u);
    }

    // Script_open_compile (VA 0x645de0): sub-scripts are compiled at the parent's compile time,
    // inserted into the cache before their own compile (so self-inclusion terminates).
    Unit* sub_script(const Ctx& c, int line, const std::string& path) {
        for (auto& u : cache)
            if (str_iequal(u->path, path)) return u.get();
        auto src = load(path);
        if (!src) fail(c, line, "Cannot open script " + path);
        cache.push_back(std::make_unique<Unit>());
        Unit* u = cache.back().get();
        u->path = path;
        compile_unit(*u, *src);
        return u;
    }

    void compile_body(Ctx& c, const std::vector<ScriptStmt>& body, Stmt& into) {
        for (const ScriptStmt& s : body)
            if (auto n = compile_stmt(c, s)) into.kids.push_back(std::move(n));
    }

    std::unique_ptr<Stmt> scoped(Ctx& c, Stmt::K k, const ScriptStmt& s) {
        auto n = std::make_unique<Stmt>();
        n->k = k;
        n->line = s.line;
        c.scopes.emplace_back();
        compile_body(c, s.body, *n);
        c.scopes.pop_back();
        return n;
    }

    std::unique_ptr<Stmt> compile_stmt(Ctx& c, const ScriptStmt& s) {
        using SK = ScriptStmt::Kind;
        switch (s.kind) {
        case SK::Decl: {
            // Declarations live in the enclosing node's variable list, not its children (0x63ef50).
            Var& v = c.unit->vars.emplace_back();
            if (s.type == "INT") v.v = Value::integer(0);
            else if (s.type == "FLOAT") v.v = Value::real(0.0f);
            else if (s.type == "STRING") v.v = Value::string({});
            else fail(c, s.line, s.type + " variables are not supported (never used by shipped scripts)");
            c.scopes.back().vars.emplace_back(s.name, &v);
            return nullptr;
        }
        case SK::Init: return scoped(c, Stmt::K::Init, s);
        case SK::Wait: return scoped(c, Stmt::K::Wait, s);
        case SK::Exit: {
            auto n = std::make_unique<Stmt>();
            n->k = Stmt::K::Exit;
            n->line = s.line;
            return n;
        }
        case SK::If: {
            // FUN_00645a40: children = cond, then-stmts, ELSE marker, else-stmts; one scope.
            auto n = std::make_unique<Stmt>();
            n->k = Stmt::K::If;
            n->line = s.line;
            c.scopes.emplace_back();
            auto cond = std::make_unique<Stmt>();
            cond->k = Stmt::K::Cond;
            cond->line = s.line;
            cond->e = compile_expr(c, s.expr, true);
            n->kids.push_back(std::move(cond));
            compile_body(c, s.body, *n);
            if (!s.else_body.empty()) {
                auto el = std::make_unique<Stmt>();
                el->k = Stmt::K::Else;
                n->kids.push_back(std::move(el));
                compile_body(c, s.else_body, *n);
            }
            c.scopes.pop_back();
            return n;
        }
        case SK::Assign: {
            auto n = std::make_unique<Stmt>();
            n->k = Stmt::K::Assign;
            n->line = s.line;
            n->var = find_var(c, s.name);
            if (!n->var) fail(c, s.line, "Variable not found ! (" + s.name + ")");
            n->e = compile_expr(c, s.expr);
            return n;
        }
        case SK::Expr: {
            // Statement tokens (FUN_006488d0): only FUNC/DIALOG/CAMERA/ACTION calls and SCRIPT.
            if (s.expr.kind != ScriptExpr::Kind::Call) fail(c, s.line, "expression is not a statement");
            auto n = std::make_unique<Stmt>();
            n->line = s.line;
            n->e = compile_expr(c, s.expr);
            n->k = n->e->k == Expr::K::Script ? Stmt::K::Script : Stmt::K::Call;
            return n;
        }
        }
        fail(c, s.line, "unsupported statement");
    }

    // math: inside an IF condition or operator operand, i.e. compiled by the shunting-yard
    // parser FUN_00647c90, which accepts only FUNC calls (DIALOG/ACTION/CAMERA are errors).
    std::unique_ptr<Expr> compile_expr(Ctx& c, const ScriptExpr& x, bool math = false) {
        using EK = ScriptExpr::Kind;
        auto e = std::make_unique<Expr>();
        e->line = x.line;
        switch (x.kind) {
        case EK::Int:  // atoi (FUN_00661ff0), wraps like the 32-bit CRT
            e->val = Value::integer(static_cast<std::int32_t>(std::strtoll(x.text.c_str(), nullptr, 10)));
            return e;
        case EK::Float:  // atof stored as float (FUN_00662060)
            e->val = Value::real(static_cast<float>(std::atof(x.text.c_str())));
            return e;
        case EK::String: e->val = Value::string(x.text); return e;
        case EK::Var:
            e->k = Expr::K::Var;
            e->var = find_var(c, x.text);
            if (!e->var) fail(c, x.line, "Variable not found ! (" + x.text + ")");
            e->val = e->var->v;
            return e;
        case EK::Vector: fail(c, x.line, "XYZ vectors are not supported (never used by shipped scripts)");
        case EK::Binary: {
            static constexpr std::pair<std::string_view, Op> kOps[] = {
                {"+", Op::Add}, {"-", Op::Sub}, {"*", Op::Mul}, {"/", Op::Div}, {"^", Op::Pow},
                {"<", Op::Lt},  {">", Op::Gt},  {"<=", Op::Le}, {">=", Op::Ge}, {"==", Op::Eq},
                {"!=", Op::Ne}, {"&&", Op::And}, {"||", Op::Or}};
            e->k = Expr::K::Bin;
            for (auto [t, o] : kOps)
                if (t == x.text) e->op = o;
            e->l = compile_expr(c, x.args[0], true);
            e->r = compile_expr(c, x.args[1], true);
            return e;
        }
        case EK::Call: break;
        }
        if (x.text == "SCRIPT") {
            if (x.args.size() != 1 || x.args[0].kind != EK::String) fail(c, x.line, "Expecting script name");
            e->k = Expr::K::Script;
            e->unit = sub_script(c, x.line, x.args[0].text);
            return e;
        }
        e->k = Expr::K::Call;
        e->kw = x.text == "DIALOG" ? Keyword::Dialog : x.text == "ACTION" ? Keyword::Action
              : x.text == "CAMERA" ? Keyword::Camera : Keyword::Func;
        if (math && e->kw != Keyword::Func) fail(c, x.line, "Unrecognized entity in math expression!");
        e->fn = formats::find_script_func(x.args[0].text);
        if (!e->fn) fail(c, x.line, "Wrong function name (" + x.args[0].text + ")!");
        e->val.type = e->fn->ret;  // node+0xc = table return type (FUN_00643c00)
        e->unit = c.unit;
        for (size_t i = 1; i < x.args.size(); ++i) e->args.push_back(compile_expr(c, x.args[i]));
        e->argv.resize(e->args.size());
        return e;
    }

    std::unique_ptr<Unit> compile_main(std::string path, std::string_view text) {
        auto u = std::make_unique<Unit>();
        u->path = std::move(path);
        compile_unit(*u, text);
        return u;
    }

    // ---------------------------------------------------------------- execute
    void error(const Expr& e, const std::string& m) {
        vm.errors_.push_back((e.unit ? e.unit->path : std::string("?")) + ":" + std::to_string(e.line) + ": " + m);
    }

    int run_unit(Unit& u) { return run_block(u.root); }

    // Block executor (root FUN_00649060, ID/INIT/RESET 0x63f790/0x63fb80/0x63fde0).
    int run_block(Stmt& s) {
        auto& k = s.kids;
        if (k.empty()) return 1;
        for (;;) {
            const int r = exec(*k[s.cur]);
            if (r == 0) return 0;
            if (r != 1) {  // -1 (exit); 2/-2 come only from GOTO/vector IF, unsupported
                s.cur = 0;
                return r;
            }
            if (s.cur + 1 >= k.size()) {
                s.cur = 0;
                return 1;
            }
            ++s.cur;
        }
    }

    // FUN_00640370.
    int run_wait(Stmt& s) {
        auto& k = s.kids;
        if (k.empty()) return 1;
        if (!s.flag) {
            for (; s.cur < k.size(); ++s.cur) exec(*k[s.cur]);  // results ignored (exit too)
            s.flag = true;
            s.cur = 0;
        }
        for (; s.cur < k.size(); ++s.cur) {
            Stmt& c = *k[s.cur];
            Expr* call = nullptr;
            if (c.k == Stmt::K::Call) call = c.e.get();
            else if (c.k == Stmt::K::Assign) {
                // Polls assign->rhs->call (0x640433); a non-call rhs dereferences null there.
                if (c.e->k == Expr::K::Call) call = c.e.get();
                else error(*c.e, "WAIT polls an assignment whose value is not a call (crash in original)");
            }
            if (call && !poll(*call)) {
                s.cur = 0;
                return 0;
            }
        }
        s.cur = 0;
        s.flag = false;
        return 1;
    }

    // FUN_006456f0. The condition is evaluated only when the cursor is on it, so a branch
    // that suspended resumes without re-testing.
    int run_if(Stmt& s) {
        auto& k = s.kids;
        if (s.cur == 0) {
            Expr& cond = *k[0]->e;
            eval(cond);
            const bool t = cond.val.truthy();
            if (k.size() == 1) return 1;
            s.cur = 1;
            if (!t) {
                while (k[s.cur]->k != Stmt::K::Else) {
                    if (s.cur + 1 >= k.size()) {
                        s.cur = 0;
                        return 1;
                    }
                    ++s.cur;
                }
                if (s.cur + 1 >= k.size()) {
                    s.cur = 0;
                    return 1;
                }
                ++s.cur;
            } else if (k[s.cur]->k == Stmt::K::Else) {
                s.cur = 0;
                return 1;
            }
        }
        while (k[s.cur]->k != Stmt::K::Else) {
            const int r = exec(*k[s.cur]);
            if (r == 0) return 0;
            if (r != 1) {
                s.cur = 0;
                return r;
            }
            if (s.cur + 1 >= k.size()) break;
            ++s.cur;
        }
        s.cur = 0;
        return 1;
    }

    int exec(Stmt& s) {
        switch (s.k) {
        case Stmt::K::Block: return run_block(s);
        case Stmt::K::Init: {
            if (s.flag) return 1;
            const int r = run_block(s);
            if (r == 1 && !s.kids.empty()) s.flag = true;
            return r;
        }
        case Stmt::K::Wait: return run_wait(s);
        case Stmt::K::If: return run_if(s);
        case Stmt::K::Cond:
        case Stmt::K::Else: return 1;
        case Stmt::K::Exit: return -1;  // FUN_006409d0
        case Stmt::K::Assign:           // FUN_00644530; the argument node always reports 1
            eval(*s.e);
            assign(*s.var, s.e->val, *s.e);
            return 1;
        case Stmt::K::Call: eval(*s.e); return 1;  // call node exec returns 1 (0x643b60)
        case Stmt::K::Script: {                    // FUN_00645d40: exit inside the sub-script -> 1
            const int r = run_unit(*s.e->unit);
            s.e->val = Value::integer(r);
            return r == 0 ? 0 : 1;
        }
        }
        return 1;
    }

    // Raw copy by the value's type; the variable keeps its declared type (FUN_00644530).
    void assign(Var& v, const Value& x, const Expr& at) {
        if ((x.type == ScriptType::String) != (v.v.type == ScriptType::String)) {
            error(at, "string/number assignment mismatch (memory corruption in original)");
            return;
        }
        if (x.type == ScriptType::String) v.v.str = clip(x).str;
        else v.v.bits = x.bits;
    }

    void eval(Expr& e) {
        switch (e.k) {
        case Expr::K::Lit: return;
        case Expr::K::Var: e.val = e.var->v; return;
        case Expr::K::Call: call(e); return;
        case Expr::K::Script: e.val = Value::integer(run_unit(*e.unit)); return;  // never suspends here
        case Expr::K::Bin: eval_bin(e); return;
        }
    }

    // FUN_006463f0 evaluates the shunting-yard RPN with lazy operands: sub-expressions first
    // (in RPN order), then the operator's own leaf operands left then right. `&&` skips its
    // right leaf when the left is a zero int/float; `||` never short-circuits.
    void eval_bin(Expr& e) {
        Expr& L = *e.l;
        Expr& R = *e.r;
        if (L.k == Expr::K::Bin) eval_bin(L);
        if (R.k == Expr::K::Bin) eval_bin(R);
        if (L.k != Expr::K::Bin) eval(L);
        bool right = true;
        if (e.op == Op::And)
            right = (L.val.type == ScriptType::Int && L.val.bits != 0) ||
                    (L.val.type == ScriptType::Float && L.val.as_float() != 0.0f);
        if (right && R.k != Expr::K::Bin) eval(R);
        e.val = apply(e, L.val, R.val);
    }

    static std::int32_t ftol(double d) {  // __ftol: truncate; out of range / NaN -> 0x80000000
        if (!(d > -2147483649.0 && d < 2147483648.0)) return INT32_MIN;
        return static_cast<std::int32_t>(d);
    }

    Value apply(const Expr& e, const Value& a, const Value& b) {
        const ScriptType ta = a.type, tb = b.type;
        if (ta == ScriptType::Int && tb == ScriptType::Int) {
            const std::int32_t x = a.as_int(), y = b.as_int();
            const std::uint32_t ux = a.bits, uy = b.bits;
            switch (e.op) {
            case Op::Add: return Value::integer(static_cast<std::int32_t>(ux + uy));
            case Op::Sub: return Value::integer(static_cast<std::int32_t>(ux - uy));
            case Op::Mul: return Value::integer(static_cast<std::int32_t>(ux * uy));
            case Op::Div:  // fild/fidiv/fstp (0x64697e): int / int is a FLOAT
                return Value::real(static_cast<float>(static_cast<double>(x) / static_cast<double>(y)));
            case Op::Pow: return Value::integer(ftol(std::pow(static_cast<double>(x), static_cast<double>(y))));
            case Op::Lt: return Value::integer(x < y);
            case Op::Gt: return Value::integer(x > y);
            case Op::Le: return Value::integer(x <= y);
            case Op::Ge: return Value::integer(x >= y);
            case Op::Eq: return Value::integer(x == y);
            case Op::Ne: return Value::integer(x != y);
            case Op::And: return Value::integer(x != 0 && y != 0);
            case Op::Or: return Value::integer(x != 0 || y != 0);
            }
        }
        const bool na = ta != ScriptType::String, nb = tb != ScriptType::String;
        if (na && nb) {  // at least one float: both converted to float
            const float x = ta == ScriptType::Float ? a.as_float() : static_cast<float>(a.as_int());
            const float y = tb == ScriptType::Float ? b.as_float() : static_cast<float>(b.as_int());
            switch (e.op) {
            case Op::Add: return Value::real(x + y);
            case Op::Sub: return Value::real(x - y);
            case Op::Mul: return Value::real(x * y);
            case Op::Div: return Value::real(x / y);
            case Op::Pow: return Value::real(static_cast<float>(std::pow(static_cast<double>(x), static_cast<double>(y))));
            case Op::Lt: return Value::integer(x < y);
            case Op::Gt: return Value::integer(x > y);
            case Op::Le: return Value::integer(x <= y);
            case Op::Ge: return Value::integer(x >= y);
            case Op::Eq: return Value::integer(x == y);
            case Op::Ne: return Value::integer(x != y);
            case Op::And: return Value::integer(x != 0.0f && y != 0.0f);
            case Op::Or: return Value::integer(x != 0.0f || y != 0.0f);
            }
        }
        if (!na && !nb) {  // strcmp / strncat
            const int c = a.str.compare(b.str);
            switch (e.op) {
            case Op::Add: return clip(Value::string(a.str + b.str));
            case Op::Lt: return Value::integer(c < 0);
            case Op::Gt: return Value::integer(c > 0);
            case Op::Le: return Value::integer(c <= 0);
            case Op::Ge: return Value::integer(c >= 0);
            case Op::Eq: return Value::integer(c == 0);
            case Op::Ne: return Value::integer(c != 0);
            default: break;
            }
        } else if (e.op == Op::And) {
            return Value::integer(0);  // mixed string/number: only && is defined (always 0)
        }
        error(e, "operator not defined for these operand types (expression error -2 in original)");
        return Value::integer(0);
    }

    BuiltinCall make_call(const Expr& e) const {
        BuiltinCall c;
        c.fn = e.fn;
        c.kw = e.kw;
        c.args = e.argv;
        c.script = e.unit->path;
        c.line = e.line;
        c.site = &e;
        c.tick = vm.ticks_;
        c.text = &e.unit->text;
        return c;
    }

    // Call node exec (0x643b60): arguments are evaluated LAST TO FIRST, then the executor of
    // the keyword runs. Each executor only knows its own ids: ACTION("stop_curr_task") and
    // FUNC("add_dialog") are silent no-ops; the CAMERA executor (0x661020) is empty.
    void call(Expr& e) {
        for (size_t i = e.args.size(); i-- > 0;) {
            eval(*e.args[i]);
            e.argv[i] = clip(e.args[i]->val);
        }
        const formats::ScriptExecutor x = e.fn->executor;
        const bool runs = (e.kw == Keyword::Func && x == formats::ScriptExecutor::Func) ||
                          (e.kw == Keyword::Dialog && x == formats::ScriptExecutor::Dialog) ||
                          (e.kw == Keyword::Action && x == formats::ScriptExecutor::Action);
        if (!runs) return;
        const BuiltinCall c = make_call(e);
        const bool internal = is_vm_builtin(e.fn->id);
        const Value r = internal ? vm_builtin(c) : host.call(c);
        if (e.fn->ret == ScriptType::String) e.val = clip(Value::string(r.str));
        else e.val = Value{e.fn->ret, r.bits, {}};
        host.trace(c, e.val, internal);
    }

    bool poll(Expr& e) {
        if (!is_blocking_builtin(e.kw, e.fn->id)) return true;
        if (e.kw == Keyword::Func && e.fn->id == 330) return pause_wake < vm.game_ms_;  // FUN_0065c4d0
        return host.poll(make_call(e));
    }

    static void set_bit(std::uint32_t& w, std::int32_t n, std::int32_t v) {  // FUN_00472000
        const unsigned s = static_cast<unsigned>(n - 1) & 31u;
        w = (w & ~(1u << s)) | ((static_cast<std::uint32_t>(v) & 0xffu) << s);
    }
    static std::int32_t get_bit(std::uint32_t w, std::int32_t n) {  // FUN_004720d0
        return static_cast<std::int32_t>((w >> (static_cast<unsigned>(n - 1) & 31u)) & 1u);
    }

    static size_t stat(std::string_view s) {  // challenge stat names, _stricmp; others -> 0
        static constexpr std::string_view kStats[] = {"STRENGTH", "DEXTERITY", "LEADERSHIP", "DEFENSE"};
        for (size_t i = 0; i < 4; ++i)
            if (str_iequal(s, kStats[i])) return i;
        return 0;
    }
    size_t slot(std::int32_t s) {  // 1-based, unchecked in the original
        if (s >= 1 && s <= 5) return static_cast<size_t>(s - 1);
        vm.errors_.push_back("challenge slot " + std::to_string(s) + " out of range (out-of-bounds in original)");
        return 0;
    }

    Value vm_builtin(const BuiltinCall& c) {
        ScenarioState& st = vm.state_;
        auto ok = [] { return Value::integer(1); };
        switch (c.fn->id) {
        case 1: return Value::integer(vm.rng_.next(c.i(0)));  // random_number
        case 204:                                             // FUN_006594e0
            if (str_iequal(c.s(0), "WIN")) st.game_finish = 1;
            else if (str_iequal(c.s(0), "LOSE")) st.game_finish = 2;
            return ok();
        case 206: return Value::integer(st.game_finish != 0);
        case 207: st.time_limit = st.time_remaining = c.i(0); return ok();  // FUN_00659610
        case 208: return Value::integer(st.time_remaining);                // remaining, not the limit
        case 254: st.challenge_restart[stat(c.s(0))][slot(c.i(1))] = static_cast<std::uint32_t>(c.i(2)); return ok();
        case 255: return Value::integer(static_cast<std::int32_t>(st.challenge_restart[stat(c.s(0))][slot(c.i(1))]));
        case 256: st.challenge_flag[stat(c.s(0))][slot(c.i(1))] = static_cast<std::int8_t>(c.i(2)); return ok();
        case 257: return Value::integer(st.challenge_flag[stat(c.s(0))][slot(c.i(1))]);
        case 258: st.challenge_duration = static_cast<std::uint32_t>(c.i(0)); return ok();
        case 259: return Value::integer(static_cast<std::int32_t>(st.challenge_duration));
        case 288: {  // FUN_004cd790
            const std::string_view m = c.s(0);
            st.mode = str_iequal(m, "SCENARIO_NORMAL_MODE") ? 0 : str_iequal(m, "SCENARIO_INTERACT_MODE") ? 1
                    : str_iequal(m, "SCENARIO_TRAINING_MODE") ? 2 : 3;
            return ok();
        }
        case 219: st.objective_count_time = c.i(0); return ok();
        case 220: return Value::integer(st.objective_count_time);
        case 221: st.count_time[0] = 0; return ok();
        case 222: return Value::integer(static_cast<std::int32_t>(st.count_time[0]));
        case 223: st.count_time[1] = 0; return ok();
        case 224: return Value::integer(static_cast<std::int32_t>(st.count_time[1]));
        case 225: st.count_time[2] = 0; return ok();
        case 226: return Value::integer(static_cast<std::int32_t>(st.count_time[2]));
        case 277: set_bit(st.bits[0], c.i(0), c.i(1)); return ok();
        case 278: return Value::integer(get_bit(st.bits[0], c.i(0)));
        case 279: set_bit(st.bits[1], c.i(0), c.i(1)); return ok();
        case 280: return Value::integer(get_bit(st.bits[1], c.i(0)));
        case 281: st.bits[1] = 0; return ok();
        case 282: set_bit(st.bits[2], c.i(0), c.i(1)); return ok();
        case 283: return Value::integer(get_bit(st.bits[2], c.i(0)));
        case 284: set_bit(st.bits[3], c.i(0), c.i(1)); return ok();
        case 285: return Value::integer(get_bit(st.bits[3], c.i(0)));
        case 286: st.stage = c.i(0); return ok();
        case 287: return Value::integer(st.stage);
        case 291: std::copy(std::begin(st.bits), std::end(st.bits), st.backup); return ok();
        case 292: std::copy(std::begin(st.backup), std::end(st.backup), st.bits); return ok();
        case 330: pause_wake = vm.game_ms_ + static_cast<std::uint32_t>(c.i(0)); return ok();  // FUN_0065c4a0
        case 338: return Value::integer(static_cast<std::int32_t>(vm.game_ms_ - st.start_ms));  // FUN_0065c9b0
        case 363: return Value::string(format_string(c));
        }
        return Value::integer(0);
    }

    // format_string (case 363 -> FUN_005a31e0). Template = localized arg 1; "%N[,W]c" takes
    // script argument N+1. Implemented letters: s, d, z (the only ones in shipped text: %Ns, %1d).
    std::string format_string(const BuiltinCall& c) {
        const std::string t = c.localized(0);
        std::string out;
        for (size_t p = 0; p < t.size(); ++p) {
            if (t[p] != '%' || p + 1 >= t.size()) {
                out += t[p];
                continue;
            }
            if (t[p + 1] == '%') {
                out += '%';
                ++p;
                continue;
            }
            if (t[p + 1] < '1' || t[p + 1] > '9') {  // '%' + non-digit is literal
                out += '%';
                continue;
            }
            const size_t n = static_cast<size_t>(t[p + 1] - '0');
            size_t q = p + 2;
            int w = 0;
            if (q + 1 < t.size() && t[q] == ',' && t[q + 1] >= '0' && t[q + 1] <= '9') {
                w = t[q + 1] - '0';
                q += 2;
            }
            if (q >= t.size()) break;
            const char letter = t[q];
            p = q;
            if (letter == 's') {
                out += c.s(n);
            } else if (letter == 'd' || letter == 'z') {
                const std::int32_t v = c.i(n);
                if (letter == 'z' && v == 0) continue;
                std::string digits = std::to_string(v < 0 ? -static_cast<std::int64_t>(v) : v);
                if (w != 4)
                    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), ",");
                if (v < 0) digits = "(" + digits + ")";
                if (w == 2) digits = "$" + digits;
                if (w == 3) digits += "%";
                out += digits;
            } else {
                vm.errors_.push_back(std::string(c.script) + ":" + std::to_string(c.line) + ": format_string letter '" +
                                     letter + "' not implemented");
            }
        }
        if (out.size() > kStrMax) out.resize(kStrMax);
        return out;
    }
};

Vm::Vm(SourceLoader load, IScriptHost& host, std::uint32_t seed) : impl_(new Impl{*this, std::move(load), host, {}, {}, 0}) {
    rng_.seed = seed;
}
Vm::~Vm() = default;

bool Vm::load_main_text(std::string_view name, std::string_view text) {
    impl_->main.reset();
    impl_->cache.clear();
    try {
        impl_->main = impl_->compile_main(std::string(name), text);
        return true;
    } catch (const CompileError& e) {
        errors_.push_back(e.msg);
        return false;
    }
}

bool Vm::load_main(std::string_view game_path) {
    auto src = impl_->load(game_path);
    if (!src) {
        errors_.push_back("Cannot open script " + std::string(game_path));
        return false;
    }
    return load_main_text(game_path, *src);
}

std::optional<int> Vm::run_once(std::string_view game_path) {
    auto src = impl_->load(game_path);
    if (!src) {
        errors_.push_back("Cannot open script " + std::string(game_path));
        return std::nullopt;
    }
    try {
        auto u = impl_->compile_main(std::string(game_path), *src);
        return impl_->run_unit(*u);
    } catch (const CompileError& e) {
        errors_.push_back(e.msg);
        return std::nullopt;
    }
}

bool Vm::load_scenario(const formats::ScenarioStep& step) {
    state_.start_ms = game_ms_;
    const std::string dir = "SCRIPT\\SCENARIO\\";
    if (!load_main(dir + step.script_file + ".SCE")) return false;
    if (!step.description_file.empty() && impl_->load(dir + step.description_file + ".SCE"))
        return run_once(dir + step.description_file + ".SCE").has_value();
    return true;
}

int Vm::tick() {
    game_ms_ += kTickMs;
    ++ticks_;
    ScenarioState& st = state_;
    // FUN_00471620: unsigned "x > dt ? x - dt : 0" countdowns; training mode runs the
    // challenge duration, every other mode the 4x5 restart durations.
    auto down = [](std::uint32_t& x) { x = x > kTickMs ? x - kTickMs : 0; };
    if (st.mode == 2) down(st.challenge_duration);
    else
        for (auto& row : st.challenge_restart)
            for (auto& x : row) down(x);
    int r = 1;
    if (impl_->main && st.game_finish != 2) r = impl_->run_unit(*impl_->main);
    if (st.game_finish == 0 && !st.counting_paused) {  // FUN_004cd5d0
        for (auto& t : st.count_time) t += kTickMs;
        if (st.time_limit > 0) {
            st.time_remaining -= static_cast<std::int32_t>(kTickMs);
            if (st.time_remaining < 1) st.game_finish = 2;
        }
    }
    return r;
}

}  // namespace joa::game
