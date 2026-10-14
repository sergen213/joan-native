// GameAudio against a headless Session (mission 1) with an offline mixer: 3D effects from world events,
// a dialog voice gated by the cut-scene letterbox, menu music and the battle/calm music rules of
// FUN_004e6fe0. Exit 77 without the original data.
#include "check.hpp"
#include "engine/assets_cache.hpp"
#include "engine/game_audio.hpp"
#include "engine/session.hpp"

#include <cstdlib>
#include <map>

using namespace joa;
using namespace joa::engine;

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    const formats::Vfs vfs(dir ? dir : ".");
    if (!vfs.exists("Resource/WAVE.RES") || !vfs.exists("Resource/S1L1_tactic.RES")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    AudioSystem audio(vfs, {}, false);
    GameAudio ga(audio);
    std::vector<float> buf;
    const auto mix = [&](std::size_t frames) {
        buf.resize(frames * 2);
        audio.mixer().mix(buf.data(), frames);
    };

    // Main menu: MUSIC type M.
    ga.menu();
    ga.frame({}, 0.016f);
    CHECK(audio.current_music() && audio.current_music()->type == int(MusicMood::Menu));

    Session s(vfs);
    s.seed = 7;
    std::string err;
    if (!s.new_game(1, 1, &err)) {
        std::fprintf(stderr, "new_game: %s\n", err.c_str());
        return 1;
    }
    game::Mission& m = *s.mission();
    m.autosave = false;
    game::World& w = m.world();
    std::map<std::string, int> codes;  // diagnostics: emitted world sound codes
    const auto tick = [&](const game::InputFrame& f) {
        s.tick(f);
        auto we = s.take_world_events();
        for (const auto& e : we)
            if (e.type == game::Ev::Sound) ++codes[e.name];
            else if (e.type == game::Ev::Action && e.unit == w.player_id()) ++codes["action:" + e.name];
        ga.on_tick(m, we, s.take_mission_events(), s.current());
        CameraPose pose;
        if (const game::Unit* p = w.unit(w.player_id())) {
            pose.target = to_engine(p->pos.x, p->pos.y, p->pos.z);
            pose.eye = pose.target + glm::vec3(0, 250, 500);
        }
        ga.frame(pose, float(kGameTickSeconds));
        mix(std::size_t(kOutputRate * kGameTickSeconds));
    };

    // 1. 300 ticks with Joan attacking: animation cues become 3D sounds.
    for (int i = 0; i < 300; ++i) {
        game::InputFrame f;
        if (i % 12 == 0) f.press(game::IN_ATTACK_A);
        tick(f);
    }
    std::printf("3D sounds: %d; music %s; world sound codes:", ga.sounds_3d,
                audio.current_music() ? audio.current_music()->code.c_str() : "-");
    for (const auto& [c, n] : codes) std::printf(" %s%s=%d", c.c_str(), audio.sounds().find(c) < 0 ? "(?)" : "", n);
    std::printf("\n");
    CHECK(ga.sounds_3d >= 1);
    CHECK(audio.current_music() && audio.current_music()->type != int(MusicMood::Menu));  // left the menu theme

    // 2. Dialog voice: starts with the fully shown letterbox, voice_active() until it ends.
    const auto cs = m.ui().cs;
    m.ui().cs.status = 2, m.ui().cs.fade = 255.0f;
    const int id = m.ui().add_dialog(m, w.player_id(), "test", "S1L1_tactic", 14, 0, 0, nullptr);
    CHECK(id != 0);
    ga.on_tick(m, {}, m.take_events(), s.current());
    CHECK(ga.voice_active());
    int ms = 0;
    for (; ga.voice_active() && ms < 60000; ms += 100) mix(kOutputRate / 10);
    std::printf("dialog voice played %d ms\n", ms);
    CHECK(ms > 500 && !ga.voice_active());
    m.ui().del_dialog(m, id);
    m.ui().cs = cs;
    ga.on_tick(m, {}, m.take_events(), s.current());

    // 3. Battle music: clear every hostile, then spawn one beside Joan.
    const game::Unit& joan = *w.unit(w.player_id());
    std::string foe_code;
    int foe_nation = 0;
    for (game::Unit& u : w.units())
        if (!u.deleted && u.nation != 0 && w.hostile(joan.nation, u.nation)) {
            if (foe_code.empty()) foe_code = w.data().units[std::size_t(u.cls)].code, foe_nation = u.nation;
            w.delete_unit(u.id);
        }
    std::printf("mood after clearing hostiles: %d (foe %s nation %d)\n", int(GameAudio::mood(m)), foe_code.c_str(), foe_nation);
    CHECK(!foe_code.empty());
    CHECK(GameAudio::mood(m) == MusicMood::Normal);
    int foe = 0;
    const auto music_type = [&] { return audio.current_music() && audio.music_playing() ? audio.current_music()->type : 0; };
    const auto tick_until = [&](int type, int max_ticks) {
        int n = 0;
        for (; music_type() != type && n < max_ticks; ++n) {
            if (game::Unit* f = w.unit(foe)) f->hp = w.max_hp(*f);  // Joan's escort would kill it within the 8 s fade
            tick({});
        }
        return n;
    };
    tick_until(1, 600);
    CHECK(music_type() == 1);
    foe = w.spawn_unit(foe_code, foe_nation, {joan.pos.x + 300, joan.pos.y, joan.pos.z}, 180.0f);
    CHECK(foe != 0);
    CHECK(GameAudio::mood(m) == MusicMood::Battle);
    const int to_battle = tick_until(2, 600);  // 8 s fade of the normal track, then the battle track
    std::printf("battle music after %d ticks\n", to_battle);
    CHECK(music_type() == 2);
    w.delete_unit(foe);
    for (int i = 0; i < 230; ++i) tick({});  // 7.6 s calm: the battle track holds
    CHECK(music_type() == 2 && !audio.music_fading());
    const int to_normal = tick_until(1, 600);  // > 8 s calm, then an 8 s fade
    std::printf("normal music after %d more ticks\n", to_normal);
    CHECK(music_type() == 1);

    ga.stop();
    CHECK(!audio.current_music() && audio.mixer().active_voices() == 0);
    return test_result();
}
