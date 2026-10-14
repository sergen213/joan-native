// joa-script: inspect Joan of Arc scripts and gameplay data.
//   joa-script funcs                     function table (id, name, return, arity, signature, executor case VA)
//   joa-script dump <file> [record]      .sce script, pscript .txt, scenario .scr, *EX.res string table,
//                                        or a RES/SET archive (lists records; with [record] dumps one:
//                                        script bodies, GAMEPLAY vars, dBase tables, Key:value text)
// Text is printed as UTF-8 (originals are Latin-1).
#include <cstdio>
#include <filesystem>
#include <string>

#include "formats/res_archive.hpp"
#include "formats/scenario_def.hpp"
#include "formats/script_ast.hpp"
#include "formats/script_funcs.hpp"
#include "formats/script_particle.hpp"
#include "formats/text_strings.hpp"
#include "formats/text_vars.hpp"
#include "formats/vfs_fs.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static void out(const std::string& s) { std::fputs(latin1_to_utf8(s).c_str(), stdout); }

static const char* type_name(ScriptType t) { return t == ScriptType::Int ? "int" : t == ScriptType::Float ? "float" : "string"; }

static int funcs() {
    std::printf("id\tname\treturn\tarity\tsig\texecutor\tcase_va\n");
    for (const auto& f : script_funcs())
        std::printf("%u\t%.*s\t%s\t%u\t%.*s\t%s\t0x%06x\n", f.id, int(f.name.size()), f.name.data(), type_name(f.ret),
                    f.arity, int(f.sig.size()), f.sig.data(),
                    f.executor == ScriptExecutor::Func ? "FUNC" : f.executor == ScriptExecutor::Dialog ? "DIALOG" : "ACTION", f.case_va);
    return 0;
}

static int dump_script(std::string_view text) {
    std::string err;
    auto s = parse_script(text, &err);
    if (!s) return std::fprintf(stderr, "parse error: %s\n", err.c_str()), 1;
    out(format_script(*s));
    std::printf("\n// calls\n");
    for_each_call(*s, [](const ScriptExpr& c) {
        if (c.text == "SCRIPT") return out("// line " + std::to_string(c.line) + ": SCRIPT " + format_expr(c.args[0]) + "\n");
        const ScriptFunc* f = find_script_func(c.args[0].text);
        std::printf("// line %d: %s %s id=%d args=%zu arity=%d%s\n", c.line, c.text.c_str(), c.args[0].text.c_str(), f ? f->id : 0,
                    c.args.size() - 1, f ? f->arity : -1, f ? "" : "  UNKNOWN FUNCTION");
    });
    return 0;
}

static std::string scalar(const PsScalar& s) {
    char b[64];
    if (s.random) std::snprintf(b, sizeof b, "random(%g,%g)", s.lo, s.hi);
    else std::snprintf(b, sizeof b, "%g", s.lo);
    return b;
}

static void dump_props(const std::vector<PsProperty>& props, const char* ind) {
    for (const auto& p : props) {
        std::string v = p.value.kind == PsValue::Kind::String ? "\"" + p.value.str + "\"" : "";
        for (size_t i = 0; i < p.value.c.size(); ++i) v += (i ? ", " : "") + scalar(p.value.c[i]);
        const char* k = p.value.kind == PsValue::Kind::Vector ? "xyz" : p.value.kind == PsValue::Kind::Color ? "rgba" : "";
        if (p.stage == PsProperty::Stage::Fade) std::printf("%sfade@%g %s = %s(%s)\n", ind, p.fade_at, p.name.c_str(), k, v.c_str());
        else std::printf("%s%s%s = %s(%s)\n", ind, p.stage == PsProperty::Stage::Initial ? "initial " : "", p.name.c_str(), k, v.c_str());
    }
}

static int dump_particles(std::string_view text) {
    std::string err;
    auto ps = parse_particle_system(text, &err);
    if (!ps) return std::fprintf(stderr, "parse error: %s\n", err.c_str()), 1;
    std::printf("particle system \"%s\" version %g\n", ps->name.c_str(), ps->version);
    dump_props(ps->props, "  ");
    for (const auto& s : ps->sequences) {
        std::printf("  sequence \"%s\"\n", s.name.c_str());
        dump_props(s.props, "    ");
    }
    return 0;
}

static int dump_text(std::string_view name, std::string_view text) {
    std::string n(name);
    for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](std::string_view s) { return n.size() >= s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0; };
    std::string_view t = trim_ws(text);
    if (t.starts_with("ParticleSystem")) return dump_particles(text);
    if (t.starts_with("\"")) return dump_script(text);
    if (has(".scr")) {
        std::string err;
        auto d = parse_scenario_def(text, &err);
        if (!d) return std::fprintf(stderr, "parse error: %s\n", err.c_str()), 1;
        std::printf("code %s\n", d->code.c_str());
        for (const auto& s : d->steps)
            std::printf("step: world=%s units=%s script=%s des=%s obj=%s objmap=%s\n", s.world_file.c_str(), s.unit_file.c_str(),
                        s.script_file.c_str(), s.description_file.c_str(), s.objective_file.c_str(), s.objective_map_file.c_str());
        return 0;
    }
    if (t.starts_with("[0")) {
        auto tab = parse_tagged_strings(text);
        if (tab) {
            for (const auto& s : *tab) out("[" + s.tag + "] " + s.text + "\n");
            return 0;
        }
    }
    if (auto vars = parse_game_vars(text); vars && t.find('{') != std::string_view::npos && t.find('[') != std::string_view::npos) {
        for (const auto& v : *vars) std::printf("%s.%s = %s\t// %s\n", v.section.c_str(), v.key.c_str(), v.value.c_str(), v.comment.c_str());
        return 0;
    }
    out(std::string(text.substr(0, text.find('\0'))));
    return 0;
}

static int dump_dbf(std::span<const std::uint8_t> bytes) {
    std::string err;
    auto t = DbfTable::parse(bytes, &err);
    if (!t) return std::fprintf(stderr, "dbf: %s\n", err.c_str()), 1;
    for (size_t f = 0; f < t->fields().size(); ++f) std::printf("%s%s", f ? "\t" : "", t->fields()[f].name.c_str());
    std::printf("\n");
    for (size_t r = 0; r < t->record_count(); ++r) {
        std::string row;
        for (size_t f = 0; f < t->fields().size(); ++f) row += (f ? "\t" : "") + t->text(r, f);
        out(row + "\n");
    }
    return 0;
}

static int dump(const fs::path& p, const char* record) {
    auto bytes = read_file(p);
    if (!bytes) return std::fprintf(stderr, "cannot read %s\n", p.c_str()), 1;
    std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    std::string ext = p.extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    bool archive = (ext == ".res" || ext == ".set" || p.filename() == "RESOURCE") && !text.starts_with("[0");
    if (!archive) return dump_text(p.filename().string(), text);

    std::string err;
    auto res = ResArchive::parse(std::move(*bytes), &err);
    if (!res) return std::fprintf(stderr, "not a RES archive: %s\n", err.c_str()), 1;
    if (!record) {
        for (const auto& e : res->entries()) std::printf("%-34s offset=%u size=%u\n", e.name.c_str(), e.offset, e.size);
        return 0;
    }
    const ResEntry* e = res->find(record);
    if (!e) return std::fprintf(stderr, "no record %s\n", record), 1;
    auto data = res->data(*e);
    if (!data.empty() && data[0] == 0x03) return dump_dbf(data);
    return dump_text(e->name, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "funcs") return funcs();
    if (cmd == "dump" && argc >= 3) return dump(argv[2], argc > 3 ? argv[3] : nullptr);
    std::fprintf(stderr, "usage: joa-script funcs | joa-script dump <file> [record]\n");
    return 2;
}
