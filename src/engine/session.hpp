#pragma once
// One play session: game data, the campaign (mission order, profile) and the running mission with its
// World, AiSystem and script VM (joa_game). No rendering: the client reads snapshots and events after
// each 33 ms tick (World::kTickMs, the original's fixed step) and renders interpolated between the last
// two snapshots. docs/PLAYING.md.
#include "engine/renderer.hpp"  // Image
#include "formats/vfs_fs.hpp"
#include "game/campaign_runner.hpp"

#include <cstdint>
#include <optional>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace joa::engine {

inline constexpr double kGameTickSeconds = game::World::kTickMs / 1000.0;

// Native save metadata (section "META", ignored by Mission::load / Mission::hash).
struct SaveMeta {
    std::string description;  // player text (raw cp1252)
    int mission = 0;          // campaign number 1..8
    std::int64_t unix_time = 0;
};

// Save thumbnail as the original renders it (FUN_004731e0 into the 512x256 D3D_create_render_texture, written as
// Save/<name>.BMP): the 4:3 centre of the frame's viewport, box-filtered to 320x240. Empty if `frame` is empty.
Image save_thumbnail(const Image& frame, glm::ivec4 viewport);
// F10 Capture Screen (DEFAULTK 55, FUN_00603b20: ScreenShot\JOAN%03d.JPG, first free index): JOAN%03d.png in
// $XDG_PICTURES_DIR (environment, else ~/.config/user-dirs.dirs as xdg-user-dir reads it, else
// $XDG_STATE_HOME/joan-native/screenshots), first name that does not exist yet (creates the directory).
std::filesystem::path next_screenshot_path();

class Session {
public:
    // Loads GameData + the campaign tables of `vfs`. Throws std::runtime_error on missing data.
    explicit Session(formats::Vfs vfs);
    ~Session();

    const formats::Vfs& vfs() const { return vfs_; }
    game::GameData& data() { return *gd_; }
    game::CampaignRunner& campaign() { return *campaign_; }
    game::Mission* mission() { return campaign_->mission(); }
    const game::Mission* mission() const { return campaign_->mission(); }

    // New campaign (main menu NEW CAMPAIGN + difficulty 0..3) at campaign mission `no` (1..8).
    // Seed of the run's World/VM RNG: `seed`, or a random one when 0 (saves store it).
    bool new_game(int no, int difficulty, std::string* err = nullptr);
    std::uint32_t seed = 0;
    // ViewCtrl_reset 0x42de70 at every mission start: view/controller mode = config+0x6b input scheme (0 keyboard,
    // 1 joypad, 3 keyboard+mouse = key_scheme(GameOptions::input_method)); scripts read it (get_charmove_mode).
    int control_mode = 3;
    // Options -> Input Method (0x73ca80): sets control_mode and, in a running mission, outside RTS switches the
    // controller at once (PlayerCtrl_set_mode 0x42e040: +0x14 = old mode, +0x10 = new; refused while the player is
    // on a ladder/vehicle, +0xcd), in RTS only the mode remembered for leaving RTS (+0x14).
    void set_control_mode(int mode);
    // Game-end CONTINUE after a win: next campaign mission with the carried profile. False after mission 8
    // (*err = "campaign complete") or on error.
    bool continue_after_win(std::string* err = nullptr);
    bool retry(std::string* err = nullptr);  // RESTART MISSION / game-over RETRY

    // Native .jsav (any CampaignRunner save) or an original Save/*.SAV (imported, never written).
    bool load(const std::filesystem::path& path, std::string* err = nullptr);
    // Native save to `path` (atomic, rotated backups); refuses original install directories. `thumbnail`
    // (save_thumbnail, 320x240) is written as <stem>.bmp next to it: 512x256 24-bit, picture top-left, rest black,
    // like the original's Save/<name>.BMP; without one a stale <stem>.bmp is removed.
    bool save(const std::filesystem::path& path, const std::string& description, std::string* err = nullptr,
              const Image* thumbnail = nullptr);
    static std::filesystem::path quick_save_path();  // <native_save_dir>/QuickSave.jsav
    static std::optional<SaveMeta> read_meta(const std::filesystem::path& path);  // native saves only

    // One 33 ms step of the running mission (no-op without one). Collects the step's events.
    void tick(const game::InputFrame& in);
    std::uint64_t ticks() const;  // mission ticks (0 without a mission)

    // Snapshots of the last two ticks (equal right after a start/load) for render interpolation.
    const game::Snapshot& previous() const { return prev_; }
    const game::Snapshot& current() const { return cur_; }
    // Events since the last take_*() (world combat/sound events and mission UI/camera/sound events).
    std::vector<game::GameEvent> take_world_events() { return std::exchange(world_events_, {}); }
    std::vector<game::MissionEvent> take_mission_events() { return std::exchange(mission_events_, {}); }
    // Bumped whenever a different World instance becomes current (start, retry, continue, load): views
    // holding per-unit state rebuild.
    int generation() const { return generation_; }

    // Action 23 (Action/RTS toggle, F2) as ViewCtrl_process_key 0x42e1a0: allowed when the campaign
    // scenario is >= 5 (or rts_unlocked, test hook). Enters/leaves World::game_mode 4 through the
    // AiSystem RTS API. Returns true if the mode changed.
    bool toggle_rts();
    bool rts_unlocked = false;
    bool in_rts() const;
    // Rts_deinit's view part, run before the gameplay part on leaving RTS (GameCamera::rts_deinit: hero switch to
    // the last chosen commander).
    std::function<void(game::Mission&)> on_rts_exit;
    // View-control block of the client (GameCamera::save_view): written as the optional native save section "VIEW"
    // (not part of Mission::hash); a native load leaves the loaded section (empty if none) in `view_section`.
    std::function<std::vector<std::uint8_t>()> save_view;
    std::vector<std::uint8_t> view_section;

private:
    void started(bool fresh);  // after any mission start (fresh) or native load: snapshots, generation

    formats::Vfs vfs_;
    std::unique_ptr<game::GameData> gd_;
    std::unique_ptr<game::CampaignRunner> campaign_;
    game::Snapshot prev_, cur_;
    std::vector<game::GameEvent> world_events_;
    std::vector<game::MissionEvent> mission_events_;
    int generation_ = 0;
};

}  // namespace joa::engine
