#include "engine/game_audio.hpp"

#include "engine/assets_cache.hpp"

#include <cstdio>
#include <string>

namespace joa::engine {
namespace {

using game::Ev;
using K = game::MissionEvent::Kind;

const game::Vec3* unit_pos(const game::Snapshot& s, int id) {
    for (const game::UnitView& u : s.units)
        if (u.id == id) return &u.pos;
    return nullptr;
}

// Voice pack entry name: "[0014N]" text tag -> "0014" (docs/AUDIO_AND_VIDEO.md §1).
std::string voice_id(int n) {
    char b[16];
    std::snprintf(b, sizeof b, "%04d", n);
    return b;
}

// Unit_search_for_unit("E", BATTLE_R, centre on leader) as used by FUN_004e6fe0, read-only: AiSystem's
// version updates the sticky target and draws from the sim RNG. Deviation: no line-of-sight test.
bool enemy_near(const game::World& w, const game::Unit& p) {
    const game::Unit* l = w.unit(p.leader);
    const game::Vec3 c = l && !l->deleted && l->id != p.id ? l->pos : p.pos;
    const float r2 = GameAudio::kBattleRadius * GameAudio::kBattleRadius;
    for (const game::Unit& o : w.units()) {
        if (o.deleted || o.id == p.id || o.nation == 0 || o.retreat == -1 || w.dead(o)) continue;
        if (!w.hostile(p.nation, o.nation)) continue;
        const auto& cls = w.data().units[std::size_t(o.cls)];
        if (!(cls.trigger_mask & 1) || cls.unit_class == 8) continue;
        const float dx = o.pos.x - c.x, dy = o.pos.y - c.y, dz = o.pos.z - c.z;
        if (dx * dx + dy * dy + dz * dz < r2) return true;
    }
    return false;
}

}  // namespace

MusicMood GameAudio::mood(const game::Mission& cm, bool* city) {
    auto& m = const_cast<game::Mission&>(cm);  // ponytail: Mission has no const accessors; only read here
    const game::World& w = m.world();
    if (city) *city = false;
    MusicMood mood = MusicMood::Normal;
    if (m.scenario().mode == 3) {  // SCENARIO_BATTLE_MODE (step +0x1ae == 3)
        mood = MusicMood::Battle;
    } else if (const game::Unit* p = w.unit(w.player_id()); p && !p->deleted) {
        const game::Territory* t = m.places().territory(m.places().player_territory());
        const bool inside = t && t->player_inside;
        if (inside && t->scale == 4) mood = MusicMood::Church;
        if (enemy_near(w, *p)) mood = MusicMood::Battle;
        if (inside && t->owner != -1) {
            if ((t->owner != 0 && w.hostile(p->nation, t->owner)) || t->under_attack) mood = MusicMood::Battle;  // FUN_00509590
            if (city) *city = t->scale == 3 || t->scale == 4;
        }
    }
    if (m.finish() == 2) mood = MusicMood::GameOver;
    if (m.finish() == 1) mood = MusicMood::Won;
    // HYPOTHESIS: show_ending's id 0 is the good ending (DAT_00a57ecb == 0 -> type '1').
    if (m.state().ending) mood = m.state().ending == 1 ? MusicMood::GoodEnd : MusicMood::BadEnd;
    return mood;
}

void GameAudio::menu() {
    if (!in_menu_)
        for (int s = 0; s < AudioSystem::kEnvSlots; ++s) audio_.stop_env(s), env_[s] = {};
    in_menu_ = true;
    music_.update(audio_, MusicMood::Menu, 0, 0);
}

void GameAudio::on_tick(const game::Mission& cm, const std::vector<game::GameEvent>& we,
                        const std::vector<game::MissionEvent>& me, const game::Snapshot& cur) {
    auto& m = const_cast<game::Mission&>(cm);  // read-only, see mood()
    in_menu_ = false;
    const int player = m.world().player_id();
    for (const game::GameEvent& e : we) world_sound(m.world(), e, cur, player);  // before projectiles(): hit positions
    for (const game::MissionEvent& e : me) {
        // b == 0: UI / script sounds at the listener (FUN_004ceef0). Otherwise set_dialog_voice (voices()),
        // keg_explode (Keg_on_event plays nothing) or a map trigger the player entered (ambient()).
        if (e.kind == K::Sound) {
            // Workshop start 0x46a040: RTS_SIEGE_008 only when no RTS_SIEGE_009 still plays (0x473140).
            if (e.b == 0 && !(e.text == "RTS_SIEGE_008" && siege_built_ && audio_.mixer().playing(siege_built_))) {
                const VoiceId v = audio_.play_sound(e.text);
                if (e.text == "RTS_SIEGE_009") siege_built_ = v;
            }
        } else if (e.kind == K::Dialog || e.kind == K::DialogClosed) {  // ids restart after del_all_dialog
            if (auto it = dialog_voices_.find(e.a); it != dialog_voices_.end()) {
                audio_.mixer().stop(it->second);  // dialog destructor stops its voice (FUN_0063b5e0 tail)
                dialog_voices_.erase(it);
            }
        } else if (e.kind == K::AttentionMessage && e.b == 0) {
            if (auto it = attention_voices_.find(e.a); it != attention_voices_.end()) {
                audio_.mixer().stop(it->second);
                attention_voices_.erase(it);
            }
        }
    }
    voices(m);
    projectiles(cur);
    ambient(m, cur, player);
    bool city = false;
    const MusicMood mm = mood(m, &city);
    music_.update(audio_, mm, m.scenario_no(), m.now());
    environment(m, mm == MusicMood::Battle, city);
}

// SE_play_trigger 0x4c0680: a .stg PLAY_SOUND / PLAY_AMB_SOUND emitter sounds at its position while the player
// unit is inside its radius (0 = everywhere) and is cut when the player leaves.
void GameAudio::ambient(const game::Mission& cm, const game::Snapshot& cur, int player) {
    const game::Vec3* p = unit_pos(cur, player);
    const auto& ts = const_cast<game::Mission&>(cm).objects().triggers();
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const game::MapTrigger& t = ts[i];
        if (t.type != 0x800 && t.type != 0x1000) continue;
        const float dx = p ? t.pos.x - p->x : 0, dy = p ? t.pos.y - p->y : 0, dz = p ? t.pos.z - p->z : 0;
        const bool in = p && (t.radius == 0 || dx * dx + dy * dy + dz * dz <= t.radius * t.radius);
        const auto it = ambient_.find(int(i));
        if (in && it == ambient_.end()) {
            ambient_[int(i)] = play_at(t.code, t.pos, 0);
        } else if (!in && it != ambient_.end()) {
            audio_.mixer().stop(it->second);
            ambient_.erase(it);
        }
    }
}

VoiceId GameAudio::play_at(std::string_view code, const game::Vec3& p, int seed) {
    const VoiceId v = audio_.play_sound(code, to_engine(p.x, p.y, p.z), seed);
    sounds_3d += v != 0;
    return v;
}

void GameAudio::world_sound(const game::World& w, const game::GameEvent& e, const game::Snapshot& cur, int player) {
    const game::Vec3* at = unit_pos(cur, e.unit);
    switch (e.type) {
    case Ev::Sound:
        if (e.other) {  // projectile impact (ThrowItem_on_hit_actor / ThrowItem_update): at the projectile
            const auto it = projectile_pos_.find(e.other);
            const game::Vec3* p = it != projectile_pos_.end() ? &it->second : at;
            // EXPLOSIVEHIT1..3 = Misc_random(3) in the original; cosmetic RNG here.
            const std::string code = e.name == "EXPLOSIVEHIT" ? e.name + char('1' + rng_() % 3) : e.name;
            if (p) play_at(code, *p, 0);
        } else if (at && e.name.starts_with("TS_")) {  // AnimGrp_load: "TS_" sounds are surface footsteps (id -99)
            footstep(w, e, *at, player);
        } else if (at) {  // move-record SOUND/SOUND2 cue (FUN_0050db80): MAIN_CHAR only for the player, variant by recno
            const int i = audio_.sounds().find(e.name);
            if (i >= 0 && (!audio_.sounds().defs()[std::size_t(i)].main_char || e.unit == player))
                play_at(e.name, *at, e.unit);
        }
        break;
    case Ev::Blocked:  // Melee_tryHit "GUARD" / Ranged_tryHit "GUARDARR" at the defender
        if (at) play_at(e.name == "GUARDARR" ? "GUARDARR" : "GUARD", *at, 0);
        break;
    case Ev::LevelUp:  // Unit_check_level_up: player only
        if (at && e.unit == player) play_at("LEVELUP", *at, 0);
        break;
    case Ev::Effect:  // Unit_add_exp: "EXPUP" for the player (World emits the effect for the player only)
        if (at && e.name == "EXPUP") play_at("EXPUP", *at, 0);
        break;
    default: break;
    }
}

// FUN_0050db80 footstep branch: "<SOUND>_<surface>", not in cut scenes; SOUND_TIME -1 marks player-only steps
// (not in RTS); one step per unit until the composite sound's SOUNDTIME has passed.
void GameAudio::footstep(const game::World& w, const game::GameEvent& e, const game::Vec3& at, int player) {
    const game::Unit* u = w.unit(e.unit);
    const game::MoveRecord* r = u ? w.move(*u) : nullptr;
    if (!r || w.cut_scene) return;
    const int timing = r->sound2 == e.name ? r->sound_time2 : r->sound_time;
    if (timing == -1 && (e.unit != player || w.game_mode == 4)) return;
    // ponytail: surface is always GROUND; GRASS/SNOW (terrain texture name), WET (rain), WATER (below the
    // water surface) need terrain material and weather queries the game layer does not have yet.
    const std::string code = e.name + "_GROUND";
    const int i = audio_.sounds().find(code);
    if (i < 0) return;
    int& last = last_step_[e.unit];
    if (last && w.now() - last <= audio_.sounds().defs()[std::size_t(i)].sound_time) return;
    last = w.now();
    play_at(code, at, e.unit);
}

void GameAudio::voices(const game::Mission& cm) {
    const game::MissionUi& ui = const_cast<game::Mission&>(cm).ui();
    // FUN_0063b5e0: a dialog voice starts once, when the letterbox is fully shown (the typing gate).
    if (ui.cs.status == 2 && ui.cs.fade == 255.0f)
        for (const auto& d : ui.dialogs)
            if (!d.deleted && d.voice && !dialog_voices_.count(d.id))
                dialog_voices_[d.id] = audio_.play_voice(d.voice_stem, voice_id(d.voice));
    // FUN_004ace90 state 4: the attention voice starts when the text starts typing.
    for (const auto& a : ui.messages)
        if (a.state == 4 && a.voice && !attention_voices_.count(a.recno))
            attention_voices_[a.recno] = audio_.play_voice(a.voice_stem, voice_id(a.voice));
}

void GameAudio::projectiles(const game::Snapshot& cur) {
    projectile_pos_.clear();
    for (const game::ProjectileView& p : cur.projectiles) projectile_pos_[p.id] = p.pos;
    for (auto it = arrows_.begin(); it != arrows_.end();) {
        const auto p = projectile_pos_.find(it->first);
        if (p == projectile_pos_.end()) {
            audio_.mixer().stop(it->second);
            it = arrows_.erase(it);
        } else {
            audio_.mixer().set_position(it->second, to_engine(p->second.x, p->second.y, p->second.z));
            ++it;
        }
    }
    // ThrowItem_launch / ThrowItem_update: arrows, projectiles and explosive arrows carry an "ARROW" sound.
    for (const game::ProjectileView& p : cur.projectiles)
        if (p.type <= 2 && !arrows_.count(p.id)) arrows_[p.id] = play_at("ARROW", p.pos, 0);
}

// Environment half of FUN_004e6fe0: slot 0 time-of-day loop, slot 1 city / war side sounds, slot 2 bells
// or side sounds of the time-of-day kind, every 30-60 s (60-120 s for slot 2 outside towns).
void GameAudio::environment(const game::Mission& cm, bool battle, bool city) {
    auto& m = const_cast<game::Mission&>(cm);
    const std::uint32_t now = m.now();
    const MusicTable& tbl = audio_.music_table();
    const bool off = m.finish() == 2 || m.state().ending;
    // ponytail: the rain variant (FUN_0041bf10(1) -> pick(4, R)) is not ported; weather has no rain flag yet.
    const MusicTrack* time = off ? nullptr : tbl.by_time(4, m.state().minute);
    for (int slot = 0; slot < AudioSystem::kEnvSlots; ++slot) {
        EnvSlot& s = env_[slot];
        if (off) {  // FUN_005861a0
            audio_.stop_env(slot);
            s = {};
            continue;
        }
        const MusicTrack* t = time;
        const bool due = s.start + side_interval_ < now;
        if (slot == 1 && t) {
            t = nullptr;
            if (due && (city || battle)) {
                t = tbl.pick(5, city ? (battle ? 7 : 6) : 9, 0, rng_);
                side_interval_ = 30000 + rng_() % 30000;
            }
        } else if (slot == 2 && t) {
            const int kind = t->env_type;
            t = nullptr;
            if (city && due) {
                t = tbl.pick(5, 8, 0, rng_);
                side_interval_ = 30000 + rng_() % 30000;
            } else if (!city && m.ui().music_side_env && due) {
                t = tbl.pick(5, kind, 0, rng_);
                side_interval_ = 60000 + 2 * (rng_() % 30000);
            }
        }
        if (!t) {
            if (slot == 0) audio_.stop_env(0), s = {};
            continue;
        }
        const bool playing = audio_.env_playing(slot);
        if (s.state == 1 && playing && audio_.current_env(slot) == t) continue;
        if (s.state == 1 && playing) {  // FUN_005868f0(2): 5 s fade, the new track waits for it
            audio_.fade_out_env(slot);
            s.state = 2;
            continue;
        }
        if (s.state == 2 && playing) continue;
        if (audio_.play_env(slot, t->code, slot == 0)) s = {1, t, now};
    }
}

void GameAudio::frame(const CameraPose& pose, [[maybe_unused]] float dt) {
    Listener l;
    l.position = pose.eye;
    if (const glm::vec3 f = pose.target - pose.eye; glm::dot(f, f) > 0.0f) l.forward = glm::normalize(f);
    audio_.set_listener(l);
    if (in_menu_) music_.update(audio_, MusicMood::Menu, 0, 0);
    audio_.update();
}

bool GameAudio::voice_active() const {
    for (const auto& [id, v] : dialog_voices_)
        if (v && audio_.mixer().playing(v)) return true;
    for (const auto& [id, v] : attention_voices_)
        if (v && audio_.mixer().playing(v)) return true;
    return false;
}

void GameAudio::stop() {
    audio_.stop_music();
    for (int s = 0; s < AudioSystem::kEnvSlots; ++s) audio_.stop_env(s), env_[s] = {};
    audio_.mixer().stop_all();
    dialog_voices_.clear();
    attention_voices_.clear();
    arrows_.clear();
    projectile_pos_.clear();
    last_step_.clear();
    ambient_.clear();
    in_menu_ = false;
}

}  // namespace joa::engine
