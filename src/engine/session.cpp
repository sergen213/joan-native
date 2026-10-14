#include "engine/session.hpp"

#include "game/savegame_import.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <stdexcept>

namespace joa::engine {
namespace {

bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}

std::string lower_ext(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

}  // namespace

Session::Session(formats::Vfs vfs) : vfs_(std::move(vfs)) {
    std::string err;
    gd_ = game::GameData::load(vfs_.root(), &err);
    if (!gd_) throw std::runtime_error("game data: " + err);
    campaign_ = std::make_unique<game::CampaignRunner>(*gd_, vfs_);
    if (!campaign_->init(&err)) throw std::runtime_error("campaign: " + err);
}

Session::~Session() = default;

void Session::started(bool fresh) {
    if (fresh) mission()->world().game_mode = control_mode;  // ViewCtrl_reset (a native load restores the saved mode)
    prev_ = cur_ = mission()->world().snapshot();
    world_events_ = mission()->world().take_events();
    for (auto& e : mission()->take_events()) mission_events_.push_back(std::move(e));
    ++generation_;
}

bool Session::new_game(int no, int difficulty, std::string* err) {
    campaign_->profile = game::CampaignProfile{};
    campaign_->profile.difficulty = std::clamp(difficulty, 0, 3);
    if (seed) campaign_->seed = seed;
    else campaign_->seed = std::random_device{}() | 1u;
    world_events_.clear(), mission_events_.clear();
    if (!campaign_->start(no, err)) return false;
    started(true);
    return true;
}

bool Session::continue_after_win(std::string* err) {
    if (!mission()) return fail(err, "no mission");
    const bool ok = campaign_->continue_after_win(err);
    if (ok) started(true);
    else for (auto& e : mission()->take_events()) mission_events_.push_back(std::move(e));  // Ending
    return ok;
}

bool Session::retry(std::string* err) {
    if (!campaign_->retry(err)) return false;
    started(true);
    return true;
}

bool Session::load(const std::filesystem::path& path, std::string* err) {
    const auto bytes = game::read_file(path);
    if (!bytes) return fail(err, "cannot read " + path.string());
    world_events_.clear(), mission_events_.clear();
    if (lower_ext(path) == ".sav") {  // original joan.exe save: import (the file is only read)
        auto o = game::import_original_save(*bytes, *gd_, err);
        if (!o) return false;
        if (!campaign_->start(o->scenario_no, err)) return false;
        game::apply_original_save(*mission(), *o);
    } else {
        auto f = game::decode_save(*bytes, err);
        if (!f || !game::migrate_save(*f, err) || !campaign_->load(*f, err)) return false;
    }
    started(lower_ext(path) == ".sav");
    return true;
}

bool Session::save(const std::filesystem::path& path, const std::string& description, std::string* err) {
    if (!mission()) return fail(err, "no mission");
    game::SaveFile f = mission()->save();
    std::vector<std::uint8_t> meta;
    game::ByteWriter w(meta);
    w.str(description);
    w.i32(mission()->scenario_no());
    w.u64(std::uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    f.add("META", std::move(meta));
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return game::write_save_atomic(path, game::encode_save(f), 3, err);
}

std::filesystem::path Session::quick_save_path() { return game::native_save_dir() / "QuickSave.jsav"; }

std::optional<SaveMeta> Session::read_meta(const std::filesystem::path& path) {
    const auto bytes = game::read_file(path);
    const auto f = bytes ? game::decode_save(*bytes) : std::nullopt;
    if (!f) return std::nullopt;
    SaveMeta m;
    if (const game::SaveSection* s = f->find("META")) {
        formats::ByteReader r(s->data);
        const std::uint32_t n = r.u32();
        const auto text = r.bytes(n);
        m.description.assign(reinterpret_cast<const char*>(text.data()), text.size());
        m.mission = r.i32();
        m.unix_time = r.get<std::int64_t>();
    } else if (const game::SaveSection* s = f->find("MISN")) {  // autosaves written by the mission itself
        formats::ByteReader r(s->data);
        m.mission = r.i32();
    }
    return m;
}

void Session::tick(const game::InputFrame& in) {
    game::Mission* m = mission();
    if (!m) return;
    m->tick(in);
    prev_ = std::move(cur_);
    cur_ = m->world().snapshot();
    for (auto& e : m->world().take_events()) world_events_.push_back(std::move(e));
    for (auto& e : m->take_events()) mission_events_.push_back(std::move(e));
}

std::uint64_t Session::ticks() const { return mission() ? mission()->ticks() : 0; }

bool Session::in_rts() const { return mission() && campaign_->mission()->world().game_mode == 4; }

bool Session::toggle_rts() {
    game::Mission* m = mission();
    if (!m || (m->scenario_no() < 5 && !rts_unlocked) || m->ui().game_cut_scene) return false;
    game::World& w = m->world();
    game::PlayerController& pc = w.controller();
    game::Unit* player = w.unit(w.player_id());
    if (!player || w.dead(*player)) return false;
    if (w.game_mode == 4) {  // set_mode(+0x14): Rts_deinit, player's task parked
        m->ai().rts_exit(*player);
        m->ai().clear_task(*player);
        const int to = pc.prev_mode;
        pc.prev_mode = w.game_mode;
        w.game_mode = to;
    } else {  // set_mode(4): Rts_init hands the player to the AI
        pc.prev_mode = w.game_mode;
        w.game_mode = 4;
        m->ai().rts_enter(*player);
    }
    return true;
}

}  // namespace joa::engine
