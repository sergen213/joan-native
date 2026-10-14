#pragma once
// In-game audio (docs/AUDIO_AND_VIDEO.md "In-game audio"): world and mission events -> AudioSystem
// (3D sound effects, dialog / attention voices), the music + environment half of FUN_004e6fe0 and the
// listener at the camera. Engine-side only: reads the mission, never changes it.
#include "engine/audio_system.hpp"
#include "engine/camera.hpp"
#include "game/mission.hpp"

#include <cstdint>
#include <map>
#include <random>
#include <vector>

namespace joa::engine {

class GameAudio {
public:
    static constexpr float kBattleRadius = 1500.0f;  // GAMEPLAY.RES {MUSIC} [BATTLE_R] (DAT_00a39180)

    explicit GameAudio(AudioSystem& audio) : audio_(audio) {}

    void menu();  // main-menu music (MUSIC type M) until the next on_tick / stop
    void on_tick(const game::Mission&, const std::vector<game::GameEvent>&, const std::vector<game::MissionEvent>&,
                 const game::Snapshot&);
    void frame(const CameraPose&, float dt);  // listener + stream refill (AudioSystem::update)
    bool voice_active() const;                // a dialog/attention voice is still playing
    void stop();                              // leaving the mission: music, environment, voices, effects

    // FUN_004e6fe0 mood of the running mission (menu excluded).
    static MusicMood mood(const game::Mission&, bool* city = nullptr);

    int sounds_3d = 0;  // positional effects that got a mixer voice (stats / tests)

    void world_sound(const game::World&, const game::GameEvent&, const game::Snapshot&, int player);
    void footstep(const game::World&, const game::GameEvent&, const game::Vec3& at, int player);
    void voices(const game::Mission&);
    void projectiles(const game::Snapshot&);
    void ambient(const game::Mission&, const game::Snapshot&, int player);
    void environment(const game::Mission&, bool battle, bool city);
    VoiceId play_at(std::string_view code, const game::Vec3& pos, int seed);

    AudioSystem& audio_;
    MusicDirector music_;
    bool in_menu_ = false;
    std::map<int, VoiceId> dialog_voices_;     // dialog box id -> voice (0 = no entry in the pack)
    std::map<int, VoiceId> attention_voices_;  // attention message recno -> voice
    std::map<int, VoiceId> arrows_;            // projectile id -> "ARROW" voice (follows the projectile)
    std::map<int, game::Vec3> projectile_pos_; // last known projectile positions (hit sounds after removal)
    std::map<int, int> last_step_;             // unit -> game ms of its last footstep (unit +0x1d6)
    std::map<int, VoiceId> ambient_;           // map trigger index -> emitter voice while the player is inside
    struct EnvSlot {
        int state = 0;  // 0 idle, 1 playing, 2 fading (DAT_00a5cd80[slot])
        const MusicTrack* track = nullptr;
        std::uint32_t start = 0;
    } env_[AudioSystem::kEnvSlots];
    std::uint32_t side_interval_ = 0;  // _DAT_00a5cd90
    std::minstd_rand rng_{1};          // cosmetic (variants, side-sound timing): never the sim RNG
};

}  // namespace joa::engine
