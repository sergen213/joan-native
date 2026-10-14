#include "engine/session.hpp"

#include "game/savegame_import.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

// 512x256 24-bit bottom-up BMP (40-byte DIB, 393270 bytes as the original's thumbnails), `t` at the top left.
std::vector<std::uint8_t> thumbnail_bmp(const Image& t) {
    constexpr int W = 512, H = 256, row = W * 3;
    std::vector<std::uint8_t> b;
    game::ByteWriter w(b);
    w.u8('B'), w.u8('M'), w.u32(54 + row * H), w.u32(0), w.u32(54);
    w.u32(40), w.i32(W), w.i32(H), w.u16(1), w.u16(24), w.u32(0), w.u32(row * H), w.i32(2835), w.i32(2835), w.u32(0), w.u32(0);
    b.resize(54 + std::size_t(row) * H);
    for (int y = 0; y < std::min(t.height, H); ++y)
        for (int x = 0; x < std::min(t.width, W); ++x) {
            const std::uint8_t* s = &t.rgba[(std::size_t(y) * t.width + x) * 4];
            std::uint8_t* d = &b[54 + std::size_t(H - 1 - y) * row + std::size_t(x) * 3];
            d[0] = s[2], d[1] = s[1], d[2] = s[0];
        }
    return b;
}

std::filesystem::path home() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : ".";
}

// xdg-user-dir PICTURES: environment, then XDG_PICTURES_DIR="..." in user-dirs.dirs ($HOME expanded).
std::filesystem::path pictures_dir() {
    if (const char* e = std::getenv("XDG_PICTURES_DIR"); e && *e) return e;
    const char* cfg = std::getenv("XDG_CONFIG_HOME");
    std::ifstream f((cfg && *cfg ? std::filesystem::path(cfg) : home() / ".config") / "user-dirs.dirs");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("XDG_PICTURES_DIR=", 0) != 0) continue;
        std::string v = line.substr(17);
        std::erase(v, '"');
        if (v.rfind("$HOME", 0) == 0) v = home().string() + v.substr(5);
        if (!v.empty() && v != home().string() && v != home().string() + "/") return v;  // "$HOME/" = disabled
    }
    const char* state = std::getenv("XDG_STATE_HOME");
    return (state && *state ? std::filesystem::path(state) : home() / ".local/state") / "joan-native" / "screenshots";
}

}  // namespace

Image save_thumbnail(const Image& frame, glm::ivec4 vp) {
    if (frame.width <= 0 || frame.height <= 0) return {};
    if (vp.z <= 0 || vp.w <= 0 || vp.x + vp.z > frame.width || vp.y + vp.w > frame.height) vp = {0, 0, frame.width, frame.height};
    constexpr int W = 320, H = 240;
    const int cw = std::min(vp.z, vp.w * 4 / 3), ch = std::min(vp.w, vp.z * 3 / 4);
    const int x0 = vp.x + (vp.z - cw) / 2, y0 = vp.y + (vp.w - ch) / 2;
    Image t{W, H, std::vector<std::uint8_t>(std::size_t(W) * H * 4)};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int ax = x0 + x * cw / W, bx = std::max(ax + 1, x0 + (x + 1) * cw / W);
            const int ay = y0 + y * ch / H, by = std::max(ay + 1, y0 + (y + 1) * ch / H);
            unsigned sum[4] = {};
            for (int j = ay; j < by; ++j)
                for (int i = ax; i < bx; ++i)
                    for (int c = 0; c < 4; ++c) sum[c] += frame.rgba[(std::size_t(j) * frame.width + i) * 4 + c];
            const unsigned n = unsigned((bx - ax) * (by - ay));
            for (int c = 0; c < 4; ++c) t.rgba[(std::size_t(y) * W + x) * 4 + c] = std::uint8_t(sum[c] / n);
        }
    return t;
}

std::filesystem::path next_screenshot_path() {
    const std::filesystem::path dir = pictures_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (int i = 0;; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "JOAN%03d.png", i);
        if (!std::filesystem::exists(dir / name, ec)) return dir / name;
    }
}

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
    view_section.clear();
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
        started(false);
        if (const game::SaveSection* v = f->find("VIEW")) view_section = v->data;
        return true;
    }
    started(true);
    return true;
}

bool Session::save(const std::filesystem::path& path, const std::string& description, std::string* err, const Image* thumbnail) {
    if (!mission()) return fail(err, "no mission");
    game::SaveFile f = mission()->save();
    std::vector<std::uint8_t> meta;
    game::ByteWriter w(meta);
    w.str(description);
    w.i32(mission()->scenario_no());
    w.u64(std::uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    f.add("META", std::move(meta));
    if (save_view) f.add("VIEW", save_view());
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (!game::write_save_atomic(path, game::encode_save(f), 3, err)) return false;
    std::filesystem::path bmp = path;
    bmp.replace_extension(".bmp");
    if (thumbnail && thumbnail->width > 0) return game::write_save_atomic(bmp, thumbnail_bmp(*thumbnail), 0, err);
    std::filesystem::remove(bmp, ec);
    return true;
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

void Session::set_control_mode(int mode) {
    control_mode = mode;
    game::Mission* m = mission();
    if (!m) return;
    game::World& w = m->world();
    game::PlayerController& pc = w.controller();
    if (w.game_mode == 4) {
        pc.prev_mode = mode;
    } else if (w.game_mode != mode) {
        if (const game::Unit* p = w.unit(pc.unit); p && p->m.climb) return;
        pc.prev_mode = w.game_mode;
        w.game_mode = mode;
    }
}

bool Session::toggle_rts() {
    game::Mission* m = mission();
    if (!m || (m->scenario_no() < 5 && !rts_unlocked) || m->ui().game_cut_scene) return false;
    game::World& w = m->world();
    game::PlayerController& pc = w.controller();
    game::Unit* player = w.unit(w.player_id());
    if (!player || w.dead(*player)) return false;
    if (w.game_mode == 4) {  // set_mode(+0x14): Rts_deinit (view part first: it may switch the player), player's task parked
        if (on_rts_exit) on_rts_exit(*m);
        player = w.unit(w.player_id());
        if (!player) return false;
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
