// Parses all 266 particle-system scripts in script/pscript.
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "formats/script_particle.hpp"
#include "formats/vfs_fs.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

int main() {
    {
        auto ps = parse_particle_system("ParticleSystem \" X \" 1.00\n{\n Position = XYZ(0,0,0)\n EventSequence \" a \"\n {\n"
                                        "  LifeTime = random(1,2)\n  Texture = \" Snow \"\n  Initial Color = RGBA(255, random(1,2), 3, 4)\n"
                                        "  Fade So At 0.15 Size = 1.5\n }\n}\n");
        CHECK(ps && ps->name == "X" && ps->sequences.size() == 1, "synthetic");
        if (ps && ps->sequences.size() == 1) {
            const auto& p = ps->sequences[0].props;
            CHECK(p.size() == 4 && p[0].value.c[0].random && p[0].value.c[0].hi == 2, "random");
            CHECK(p[1].value.kind == PsValue::Kind::String && p[1].value.str == "Snow", "string");
            CHECK(p[2].stage == PsProperty::Stage::Initial && p[2].value.c.size() == 4, "initial rgba");
            CHECK(p[3].stage == PsProperty::Stage::Fade && p[3].fade_at == 0.15f && p[3].name == "Size", "fade");
        }
    }

    const char* env = std::getenv("JOA_GAME_DIR");
    if (!env || !fs::exists(fs::path(env) / "script" / "pscript")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }

    size_t files = 0, seqs = 0, props = 0;
    for (const auto& e : fs::directory_iterator(fs::path(env) / "script" / "pscript")) {
        auto bytes = read_file(e.path());
        std::string err;
        auto ps = parse_particle_system(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err);
        CHECK(ps, "%s: %s", e.path().filename().c_str(), err.c_str());
        if (!ps) continue;
        CHECK(!ps->sequences.empty(), "%s: no EventSequence", e.path().filename().c_str());
        ++files;
        seqs += ps->sequences.size();
        props += ps->props.size();
        for (const auto& s : ps->sequences) props += s.props.size();
    }
    CHECK(files == 266, "expected 266 particle systems, got %zu", files);
    std::printf("pscript: %zu systems, %zu sequences, %zu properties\n", files, seqs, props);
    return failures ? 1 : 0;
}
