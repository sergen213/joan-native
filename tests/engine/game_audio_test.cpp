// GameAudio against a headless Session (mission 1) with an offline mixer: Joan's sword swings, hits, blocks and
// footsteps against an English infantryman (rendered to $JOA_TEST_OUT/game_audio_fight.wav), a dialog voice gated by
// the cut-scene letterbox, menu music and the battle/calm music rules of FUN_004e6fe0. Exit 77 without the data.
#include "check.hpp"
#include "engine/assets_cache.hpp"
#include "engine/game_audio.hpp"
#include "engine/session.hpp"

#include "wav_write.hpp"

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
    std::vector<float> rec;            // fight render (section 1)
    std::vector<std::pair<std::string, std::size_t>> swings;  // Joan's move-record sounds, frame in `rec`
    const auto tick = [&](const game::InputFrame& f) {
        if (m.ui().cut_scene_status()) m.ui().skip_cut_scene(m);  // cut scenes reset Joan to IDLE every tick
        s.tick(f);
        auto we = s.take_world_events();
        for (const auto& e : we)
            if (e.type == game::Ev::Sound) {
                ++codes[e.name];
                if (e.unit == w.player_id() && !e.name.starts_with("TS_")) swings.emplace_back(e.name, rec.size() / 2);
            } else if (e.type == game::Ev::Action && e.unit == w.player_id()) ++codes["action:" + e.name];
        ga.on_tick(m, we, s.take_mission_events(), s.current());
        CameraPose pose;  // follow camera: pivot = hero + 50, eye 187.5 behind (GameCamera)
        if (const game::Unit* p = w.unit(w.player_id())) {
            pose.target = to_engine(p->pos.x, p->pos.y, p->pos.z + 50.0f);
            pose.eye = pose.target + glm::vec3(0, 0, 187.5f);
        }
        ga.frame(pose, float(kGameTickSeconds));
        mix(std::size_t(kOutputRate * kGameTickSeconds));
        rec.insert(rec.end(), buf.begin(), buf.end());
    };
    const auto peak = [&](std::size_t from, std::size_t frames) {
        float p = 0;
        for (std::size_t i = from * 2; i < std::min(rec.size(), (from + frames) * 2); ++i) p = std::max(p, std::fabs(rec[i]));
        return p;
    };

    // 1. Attack combos against an English heavy infantryman in front of Joan; music and environment muted.
    audio.mixer().set_bus_volume(Bus::Music, 0.0f);
    audio.mixer().set_bus_volume(Bus::Env, 0.0f);
    for (int i = 0; i < 10; ++i) tick({});
    game::Unit& hero = *w.unit(w.player_id());
    const float yaw = glm::radians(hero.yaw_deg);
    const int foe1 = w.spawn_unit("ENGLISH_HEAVY_INFANTRY", 1, {hero.pos.x + 90 * std::cos(yaw), hero.pos.y + 90 * std::sin(yaw), hero.pos.z},
                                  hero.yaw_deg + 180);
    CHECK(foe1 != 0);
    swings.clear(), rec.clear();
    for (int i = 0; i < 300; ++i) {  // A, AA, AAA... chains, B finishers every other second
        game::InputFrame f;
        if (i % 5 == 0) f.press(i % 60 < 40 ? game::IN_ATTACK_A : game::IN_ATTACK_B);
        tick(f);
    }
    const std::size_t combo_sounds = swings.size();
    // a cue survives the action change: AAA (SOUND_TIME 200) cut short by a flinch on the next tick
    hero.combo = "AA", hero.combo_idx = 2;
    CHECK(w.set_action(hero, game::ATTACK, 'A') && w.move(hero)->sound == "AAA");
    tick({});
    CHECK(w.set_action(hero, game::ATTACKED));
    for (int i = 0; i < 10; ++i) tick({});
    bool aaa = false;
    for (std::size_t i = combo_sounds; i < swings.size(); ++i) aaa |= swings[i].first == "AAA";
    CHECK(aaa);
    // a shot sounds the record's SOUND (BOW) with a bow, never its SOUND2 (CROSSBOW)
    for (int i = 0; i < 30; ++i) tick({});
    for (std::size_t it = 1; it < w.data().items.size() && !hero.inv.ranged.item; ++it)
        if (w.data().items[it].type == 0xb) hero.inv.ranged.item = int(it);
    const std::size_t shot_sounds = swings.size();
    CHECK(hero.inv.ranged.item != 0 && w.set_action(hero, game::SHTOUT));
    for (int i = 0; i < 3; ++i) tick({});
    CHECK(swings.size() == shot_sounds + 1 && swings.back().first == "BOW");
    hero.inv.ranged.item = 0;
    // footsteps: running forward repeats TS_JOAN_GROUND every SOUNDTIME (800 ms), not once per RUN start
    w.delete_unit(foe1);
    for (int i = 0; i < 60; ++i) tick({});
    int steps = 0;
    for (int i = 0, last = ga.last_step_[w.player_id()]; i < 90; ++i) {  // 3 s
        game::InputFrame f;
        f.held.set(game::IN_FORWARD);
        tick(f);
        if (ga.last_step_[w.player_id()] != last) ++steps, last = ga.last_step_[w.player_id()];
    }
    std::printf("footsteps in 3 s of running: %d\n", steps);
    CHECK(steps >= 3);

    std::map<std::string, int> kinds;
    float quietest = 1.0f;
    for (std::size_t i = 0; i < combo_sounds; ++i) {
        const auto& [name, at] = swings[i];
        ++kinds[name];
        // the opening swing ("A", SOUND_TIME 0) starts before its hit lands; Joan stands 50 below the listener
        // (camera pivot), inside MIN_DIST 100: full gain, peak 0.97 * 0.707 (centred). At the eye: 0.36.
        if (name == "A") quietest = std::min(quietest, peak(at, 2205));
    }
    std::printf("Joan's combo sounds:");
    for (const auto& [c, n] : kinds) std::printf(" %s=%d", c.c_str(), n);
    std::printf("; quietest opening swing peak %.2f; 3D sounds %d\n", quietest, ga.sounds_3d);
    CHECK(kinds["A"] >= 3 && kinds["AA"] >= 2 && kinds["AAA"] >= 2);  // every combo step sounds
    CHECK(quietest > 0.6f);
    CHECK(codes["HIT_H"] >= 2);  // the foe's hit reactions
    if (const char* o = std::getenv("JOA_TEST_OUT")) {
        write_wav(std::string(o) + "/game_audio_fight.wav", rec);
        std::printf("wrote %s/game_audio_fight.wav (%.1f s)\n", o, double(rec.size() / 2) / kOutputRate);
    }
    audio.mixer().set_bus_volume(Bus::Music, 1.0f);
    audio.mixer().set_bus_volume(Bus::Env, 1.0f);
    std::printf("3D sounds: %d; music %s; world sound codes:", ga.sounds_3d,
                audio.current_music() ? audio.current_music()->code.c_str() : "-");
    for (const auto& [c, n] : codes) std::printf(" %s%s=%d", c.c_str(), audio.sounds().find(c) < 0 ? "(?)" : "", n);
    std::printf("\n");
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
