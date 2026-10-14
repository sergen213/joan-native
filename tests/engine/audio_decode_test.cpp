// Decodes every RES sound entry and every music/*.wma; checks the SOUND/MUSIC tables resolve.
// Exit 77 (skip) without the original data.
#include "check.hpp"
#include "engine/audio_system.hpp"

#include <cstdlib>

using namespace joa::engine;
namespace fs = std::filesystem;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    const joa::formats::Vfs vfs(dir ? dir : ".");
    if (!vfs.exists("Resource/WAVE.RES") || !vfs.exists("music")) {
        std::fprintf(stderr, "SKIP: no original data at %s\n", dir ? dir : ".");
        return 77;
    }

    // Every RIFF entry (M7BRIEF/M8BRIEF mission briefings run 120-133 s) of every sound pack (WAVE.RES, A_WAVE*.RES, scenario voice packs S*L1_*.RES).
    std::size_t entries = 0;
    double total = 0;
    for (const auto& de : fs::directory_iterator(*vfs.resolve("Resource"))) {
        const std::string name = de.path().filename().string();
        const bool pack = joa::formats::iequals(name, "WAVE.RES") || joa::formats::iequals(name.substr(0, 6), "A_WAVE") ||
                          (name.size() > 6 && (name[0] == 'S' || name[0] == 's') && name.find("L1_") == 2);
        if (!pack || de.file_size() == 0) continue;
        auto res = joa::formats::ResArchive::open(de.path());
        CHECK(res.has_value());
        if (!res) continue;
        for (const auto& e : res->entries()) {
            std::string err;
            auto pcm = decode_wav(res->data(e), &err);
            if (!pcm) std::fprintf(stderr, "%s/%s: %s\n", name.c_str(), e.name.c_str(), err.c_str());
            CHECK(pcm.has_value());
            if (!pcm) continue;
            if (!(pcm->seconds() > 0.01 && pcm->seconds() < 300.0))
                std::fprintf(stderr, "%s/%s: %.3f s\n", name.c_str(), e.name.c_str(), pcm->seconds());
            CHECK(pcm->seconds() > 0.01 && pcm->seconds() < 300.0);
            CHECK(pcm->rate == 11025 || pcm->rate == 22050 || pcm->rate == 44100);
            float peak = 0;
            for (float s : pcm->samples) peak = std::max(peak, std::fabs(s));
            CHECK(peak <= 1.0f);
            ++entries;
            total += pcm->seconds();
        }
    }
    std::printf("RES sound entries: %zu, %.1f s\n", entries, total);
    CHECK(entries == 503 + 11 + 1 + 5 + 2 + 8 + 33 + 18 + 21 + 39 + 1 + 28 + 47);

    // Every music/environment track.
    std::size_t tracks = 0;
    for (const auto& de : fs::directory_iterator(*vfs.resolve("music"))) {
        if (!joa::formats::iequals(de.path().extension().string(), ".wma")) continue;
        std::string err;
        auto s = AudioFileStream::open(de.path(), &err);
        CHECK(s != nullptr);
        if (!s) continue;
        std::vector<float> buf;
        std::size_t frames = 0, n;
        float peak = 0;
        while ((n = s->read(buf, 1 << 15))) {
            frames += n;
            for (float x : buf) peak = std::max(peak, std::fabs(x));
            buf.clear();
        }
        const double secs = double(frames) / kOutputRate;
        // JOA_Encounter.wma ends in a truncated ASF packet (FFmpeg and WMF stop at 43.7 of 44.9 s).
        const bool truncated = de.path().filename() == "JOA_Encounter.wma";
        if (std::fabs(secs - s->duration()) > 0.5)
            std::fprintf(stderr, "%s: decoded %.2f s, container %.2f s\n", de.path().filename().c_str(), secs, s->duration());
        CHECK(secs <= s->duration() + 0.5 && secs >= s->duration() - (truncated ? 1.5 : 0.5));
        CHECK(secs > 1.0 && peak > 0.01f && peak < 1.5f);
        ++tracks;
    }
    CHECK(tracks == 40);

    // SOUND table (STD.SET): 505 records, every wave (and WF_RANDMAX variant) present in WAVE.RES.
    SoundBank bank;
    std::string err;
    CHECK(bank.load(vfs, &err));
    CHECK(bank.defs().size() == 505);
    for (std::size_t i = 0; i < bank.defs().size(); ++i) {
        if (!bank.pcm(int(i))) std::fprintf(stderr, "SOUND %s: wave %s missing\n", bank.defs()[i].code.c_str(), bank.defs()[i].wave.c_str());
        CHECK(bank.pcm(int(i)) != nullptr);
    }
    const int hit = bank.find("hit_h");
    CHECK(hit >= 0 && bank.defs()[std::size_t(hit)].randmax == 5);
    CHECK(bank.defs()[std::size_t(bank.variant(hit, 7))].code == "HIT_H3");  // 7 % 5 = 2 -> third record
    CHECK(bank.find("NO_SUCH_SOUND") == -1);

    // MUSIC table: 42 records, every file present, menu track as observed in the original.
    MusicTable music;
    CHECK(music.load(vfs, &err));
    CHECK(music.tracks().size() == 42);
    for (const auto& t : music.tracks()) CHECK(vfs.exists("music/" + t.file) && t.type != 0);
    std::minstd_rand rng(1);
    CHECK(music.pick(3, 0, 1, rng)->file == "JOA_In_your_name.wma");
    CHECK(music.pick(1, 0, 8, rng)->start_sce <= 8);
    CHECK(music.by_time(4, 1430.0f)->code == "ENV_NIGHT" && music.by_time(4, 100.0f)->code == "ENV_NIGHT");
    CHECK(music.by_time(4, 400.0f)->code == "ENV_MORNING");
    return test_result();
}
