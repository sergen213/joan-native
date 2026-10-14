// Native save container + full mission save/load: save at tick N, load into a fresh process
// state, continue -> identical state hash to the uninterrupted run, at three stages (mission 1
// start, mid-mission, after the transition to mission 2).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++fails; \
    } while (0)

static void container_tests() {
    SaveFile f;
    f.add("ABCD", {1, 2, 3});
    f.add("EFGH", {});
    auto bytes = encode_save(f);
    std::string err;
    auto g = decode_save(bytes, &err);
    CHECK(g && g->version == SaveFile::kVersion && g->sections.size() == 2 && g->find("ABCD")->data.size() == 3);
    auto bad = bytes;
    bad[30] ^= 1;
    CHECK(!decode_save(bad, &err));
    bad = bytes;
    bad[8] = 99;  // version from the future
    CHECK(!decode_save(bad, &err));
    CHECK(!decode_save(std::span(bytes).first(bytes.size() - 1), &err));
    SaveFile old = f;
    old.version = SaveFile::kVersion;
    CHECK(migrate_save(old, &err));

    const fs::path dir = fs::temp_directory_path() / ("joa-save-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const fs::path p = dir / "slot.jsav";
    for (std::uint8_t i = 0; i < 5; ++i) {
        SaveFile s;
        s.add("SEQ ", {i});
        CHECK(write_save_atomic(p, encode_save(s), 3, &err));
    }
    auto cur = read_file(p);
    auto cur_f = cur ? decode_save(*cur) : std::nullopt;
    CHECK(cur_f && cur_f->find("SEQ")->data[0] == 4);
    for (int i = 1; i <= 3; ++i) {
        auto b = read_file(p.string() + "." + std::to_string(i));
        auto bf = b ? decode_save(*b) : std::nullopt;
        CHECK(bf && bf->find("SEQ")->data[0] == 4 - i);
    }
    CHECK(!fs::exists(p.string() + ".4") && !fs::exists(p.string() + ".tmp"));
    fs::remove_all(dir);
    const char* xdg = std::getenv("XDG_DATA_HOME");
    ::setenv("XDG_DATA_HOME", "/tmp/xdg-test", 1);
    CHECK(native_save_dir() == fs::path("/tmp/xdg-test/joan-native/saves"));
    if (xdg) ::setenv("XDG_DATA_HOME", xdg, 1);
    else ::unsetenv("XDG_DATA_HOME");
}

// Runs `a` for `more` ticks and a reloaded copy of its save in parallel; hashes must agree.
static void check_resume(GameData& gd, const formats::Vfs& vfs, CampaignRunner& a, int more, const char* stage) {
    std::string err;
    const auto bytes = encode_save(a.save());
    auto f = decode_save(bytes, &err);
    CampaignRunner b(gd, vfs);
    CHECK(f && b.init(&err) && b.load(*f, &err));
    if (!b.mission()) return void(std::printf("FAIL %s: load: %s\n", stage, err.c_str()));
    b.mission()->autosave = false;
    CHECK(b.mission()->ticks() == a.mission()->ticks());
    CHECK(b.mission()->hash() == a.mission()->hash());
    for (int i = 0; i < more; ++i) a.mission()->tick(), b.mission()->tick();
    const bool same = a.mission()->hash() == b.mission()->hash();
    std::printf("%s: saved at tick %llu (%zu bytes), +%d ticks: %016llx %s %016llx\n", stage,
                static_cast<unsigned long long>(a.mission()->ticks() - more), bytes.size(), more,
                static_cast<unsigned long long>(a.mission()->hash()), same ? "==" : "!=",
                static_cast<unsigned long long>(b.mission()->hash()));
    CHECK(same);
}

int main() {
    container_tests();
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return fails ? 1 : 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    CampaignRunner a(*gd, vfs);
    if (!a.init(&err) || !a.start(1, &err)) return std::printf("start: %s\n", err.c_str()), 1;
    a.mission()->autosave = false;

    check_resume(*gd, vfs, a, 300, "mission 1 start");
    for (int t = a.mission()->ticks(); t < 3000; ++t) a.mission()->tick();
    check_resume(*gd, vfs, a, 600, "mission 1 mid");

    a.mission()->finish_objectives();
    for (int t = 0; t < 3000 && a.mission()->finish() != 1; ++t) a.mission()->tick();
    CHECK(a.mission()->finish() == 1 && a.continue_after_win(&err));
    if (a.mission()->scenario_no() == 2) {
        a.mission()->autosave = false;
        for (int t = 0; t < 100; ++t) a.mission()->tick();
        check_resume(*gd, vfs, a, 300, "mission 2 after transition");
    }
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
