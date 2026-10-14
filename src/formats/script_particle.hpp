// Particle-system scripts: script/pscript/*.txt (266 files, plain text). Keywords match the
// joan.exe particle lexer table at VA 0xa21680 (PARTICLESYSTEM, EVENTSEQUENCE, RANDOM, XYZ,
// RGBA, FADE SO AT, INITIAL, FINAL + property names). Case-insensitive, "//" comments.
//
//   ParticleSystem " name " version { prop* EventSequence " name " { prop* }* }
//   prop := Name = value | Initial Name = value | Fade So At t Name = value
//   value := scalar | XYZ(s,s,s) | RGBA(s,s,s,s) | " string "      scalar := number | random(a,b)
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace joa::formats {

struct PsScalar {
    float lo = 0, hi = 0;  // fixed value: lo == hi and random == false (order kept as written)
    bool random = false;
};

struct PsValue {
    enum class Kind { Scalar, Vector, Color, String };
    Kind kind = Kind::Scalar;
    std::vector<PsScalar> c;  // 1 (Scalar), 3 (XYZ) or 4 (RGBA) components
    std::string str;          // String: trimmed contents (texture/model name)
};

struct PsProperty {
    enum class Stage { Base, Initial, Fade };
    Stage stage = Stage::Base;
    float fade_at = 0;  // Fade: normalized particle age 0..1 ("Fade So At t")
    std::string name;   // as written, e.g. "LifeTime", "Velocity"
    PsValue value;
};

struct PsSequence {
    std::string name;
    std::vector<PsProperty> props;
};

struct ParticleSystemDef {
    std::string name;
    float version = 0;
    std::vector<PsProperty> props;
    std::vector<PsSequence> sequences;
};

std::optional<ParticleSystemDef> parse_particle_system(std::string_view text, std::string* err = nullptr);

}  // namespace joa::formats
