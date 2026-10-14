#include "formats/script_particle.hpp"

#include <cctype>
#include <cstdlib>

#include "formats/text_strings.hpp"
#include "formats/vfs_fs.hpp"

namespace joa::formats {
namespace {

struct Tok {
    enum Kind { End, Num, Str, Word, Punct } kind = End;
    std::string text;
    int line = 0;
};

std::vector<Tok> tokenize(std::string_view s) {
    std::vector<Tok> out;
    int line = 1;
    for (size_t p = 0; p < s.size();) {
        char c = s[p];
        if (c == '\n') { ++line; ++p; continue; }
        if (std::isspace(static_cast<unsigned char>(c)) || c == '\0') { ++p; continue; }
        if (s.substr(p, 2) == "//") { while (p < s.size() && s[p] != '\n') ++p; continue; }
        Tok t;
        t.line = line;
        if (c == '"') {
            size_t e = s.find('"', p + 1);
            if (e == std::string_view::npos) e = s.size();
            t.kind = Tok::Str;
            t.text = std::string(trim_ws(s.substr(p + 1, e - p - 1)));
            p = e + 1;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.') {
            size_t b = p++;
            while (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.')) ++p;
            t.kind = Tok::Num;
            t.text = std::string(s.substr(b, p - b));
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t b = p;
            while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_')) ++p;
            t.kind = Tok::Word;
            t.text = std::string(s.substr(b, p - b));
        } else {
            t.kind = Tok::Punct;
            t.text = std::string(1, c);
            ++p;
        }
        out.push_back(std::move(t));
    }
    out.push_back(Tok{Tok::End, "<end>", line});
    return out;
}

struct Fail {
    std::string msg;
};

class Parser {
public:
    explicit Parser(std::vector<Tok> t) : t_(std::move(t)) {}

    ParticleSystemDef system() {
        ParticleSystemDef ps;
        word("ParticleSystem");
        ps.name = str();
        ps.version = num();
        punct("{");
        while (!at_punct("}")) {
            if (at_word("EventSequence")) {
                ++i_;
                PsSequence seq;
                seq.name = str();
                punct("{");
                while (!at_punct("}")) seq.props.push_back(prop());
                ++i_;
                ps.sequences.push_back(std::move(seq));
            } else {
                ps.props.push_back(prop());
            }
        }
        ++i_;
        if (t_[i_].kind != Tok::End) fail("text after closing '}'");
        return ps;
    }

private:
    [[noreturn]] void fail(const std::string& m) const {
        throw Fail{"line " + std::to_string(t_[i_].line) + ": " + m + " (got '" + t_[i_].text + "')"};
    }
    bool at_word(std::string_view w) const { return t_[i_].kind == Tok::Word && iequals(t_[i_].text, w); }
    bool at_punct(std::string_view p) const {
        if (t_[i_].kind == Tok::End) fail("missing '}'");
        return t_[i_].kind == Tok::Punct && t_[i_].text == p;
    }
    void word(std::string_view w) {
        if (!at_word(w)) fail("expected " + std::string(w));
        ++i_;
    }
    void punct(std::string_view p) {
        if (!at_punct(p)) fail("expected '" + std::string(p) + "'");
        ++i_;
    }
    std::string str() {
        if (t_[i_].kind != Tok::Str) fail("expected string");
        return t_[i_++].text;
    }
    float num() {
        if (t_[i_].kind != Tok::Num) fail("expected number");
        return std::strtof(t_[i_++].text.c_str(), nullptr);
    }

    PsScalar scalar() {
        PsScalar s;
        if (at_word("random")) {
            ++i_;
            punct("(");
            s.lo = num();
            punct(",");
            s.hi = num();
            punct(")");
            s.random = true;
        } else {
            s.lo = s.hi = num();
        }
        return s;
    }

    PsValue value() {
        PsValue v;
        if (t_[i_].kind == Tok::Str) {
            v.kind = PsValue::Kind::String;
            v.str = str();
            return v;
        }
        size_t n = 1;
        if (at_word("XYZ")) { v.kind = PsValue::Kind::Vector; n = 3; }
        else if (at_word("RGBA")) { v.kind = PsValue::Kind::Color; n = 4; }
        if (n == 1) {
            v.c.push_back(scalar());
            return v;
        }
        ++i_;
        punct("(");
        for (size_t k = 0; k < n; ++k) {
            if (k) punct(",");
            v.c.push_back(scalar());
        }
        punct(")");
        return v;
    }

    PsProperty prop() {
        PsProperty p;
        if (at_word("Initial")) {
            ++i_;
            p.stage = PsProperty::Stage::Initial;
        } else if (at_word("Fade")) {
            ++i_;
            word("So");
            word("At");
            p.stage = PsProperty::Stage::Fade;
            p.fade_at = num();
        }
        if (t_[i_].kind != Tok::Word) fail("expected property name");
        p.name = t_[i_++].text;
        punct("=");
        p.value = value();
        return p;
    }

    std::vector<Tok> t_;
    size_t i_ = 0;
};

}  // namespace

std::optional<ParticleSystemDef> parse_particle_system(std::string_view text, std::string* err) {
    try {
        return Parser(tokenize(text)).system();
    } catch (const Fail& f) {
        if (err) *err = f.msg;
        return std::nullopt;
    }
}

}  // namespace joa::formats
