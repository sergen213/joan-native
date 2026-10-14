#include "game/mission_ui.hpp"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <type_traits>

#include "game/mission.hpp"
#include "formats/res_archive.hpp"
#include "formats/world_text.hpp"

namespace joa::game {

namespace {

using K = MissionEvent::Kind;

constexpr int kScreenW = 1024;  // DAT_00a4885c (1024x768 hard-coded, RENDERING_PIPELINE.md)
constexpr int kScreenH = 768;   // DAT_00a48860
constexpr int kWipeTexW = 512;  // int8x6/cut_scene_texture.dds width (+0x3a -> +0x3c)
constexpr int kDt = static_cast<int>(Vm::kTickMs);  // DAT_00a500be at the fixed 30 Hz step

std::string cam_text(const float* c) {
    char b[160];
    std::snprintf(b, sizeof b, "%g %g %g %g %g %g", c[0], c[1], c[2], c[3], c[4], c[5]);
    return b;
}

// Ctrl_range_exit 0x42b6e0 as called by the cut-scene start: input schemes 0/1/3 leave range mode.
void range_exit(World& w) {
    if (w.game_mode != 0 && w.game_mode != 1 && w.game_mode != 3) return;
    PlayerController& pc = w.controller();
    Unit* u = w.unit(pc.unit);
    if (!u || u->deleted || !pc.range_mode) return;
    pc.range_mode = false;
    u->range_target = 0;
    w.stop(*u);
}

struct Saver {
    ByteWriter& w;
    template <class T> void operator()(T& v) {
        if constexpr (std::is_same_v<T, std::string>) w.str(v);
        else if constexpr (std::is_array_v<T>) for (auto& e : v) (*this)(e);
        else w.put(v);
    }
    template <class T, class G> void vec(std::vector<T>& v, G g) {
        w.u32(static_cast<std::uint32_t>(v.size()));
        for (T& e : v) g(e);
    }
    template <class A, class B> void map(std::map<A, B>& m) {
        w.u32(static_cast<std::uint32_t>(m.size()));
        for (auto& [k, v] : m) {
            A kk = k;
            (*this)(kk), (*this)(v);
        }
    }
};

struct Loader {
    formats::ByteReader& r;
    template <class T> void operator()(T& v) {
        if constexpr (std::is_same_v<T, std::string>) {
            const std::uint32_t n = r.u32();
            if (n > r.remaining()) return r.fail();
            const auto b = r.bytes(n);
            v.assign(reinterpret_cast<const char*>(b.data()), b.size());
        } else if constexpr (std::is_array_v<T>) {
            for (auto& e : v) (*this)(e);
        } else {
            v = r.get<T>();
        }
    }
    template <class T, class G> void vec(std::vector<T>& v, G g) {
        const std::uint32_t n = r.u32();
        if (n > r.remaining()) return r.fail();
        v.assign(n, T{});
        for (T& e : v) g(e);
    }
    template <class A, class B> void map(std::map<A, B>& m) {
        const std::uint32_t n = r.u32();
        if (n > r.remaining()) return r.fail();
        m.clear();
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            A k{};
            B v{};
            (*this)(k), (*this)(v);
            m.emplace(std::move(k), std::move(v));
        }
    }
};

}  // namespace

template <class F> void MissionUi::visit(F& f) {
    f(cs.start_ms), f(cs.active), f(cs.black_start), f(cs.black_end), f(cs.speed), f(cs.fade), f(cs.status);
    f(cs.camera_move), f(cs.cam), f(cs.saved_camera), f(cs.wipe), f(cs.hold_ms), f(cs.wipe_flip), f(cs.starting);
    f(cs.ending), f(game_cut_scene);
    f(camera.playing), f(camera.name), f(camera.start_ms), f(camera.speed), f(camera.flag5), f(camera.flag6);
    f.vec(dialogs, [&](DialogBox& d) {
        f(d.id), f(d.deleted), f(d.speaker), f(d.text), f(d.voice_stem), f(d.voice), f(d.anim), f(d.anim2), f(d.rect);
        f(d.done), f(d.shown_ms), f(d.has_choices), f(d.choice);
        f.vec(d.choices, [&](DialogChoice& c) { f(c.text), f(c.enabled); });
    });
    f(dialog_show_disabled);
    f.vec(messages, [&](AttentionMessage& a) {
        f(a.recno), f(a.state), f(a.fast), f(a.y), f(a.slot), f(a.len), f(a.typed), f(a.t), f(a.f47), f(a.f4b);
        f(a.f4f), f(a.f53), f(a.f57), f(a.text), f(a.icon), f(a.speaker), f(a.voice_stem), f(a.voice);
    });
    f(next_message), f(messages_paused), f(messages_pause_ms);
    f(title.active), f(title.start_ms), f(title.duration), f(title.fade_in), f(title.fade_out), f(title.text);
    f(buy_open), f(buy_shop), f(buy_seller), f(brief_open), f(letter_open), f(letter_id), f(interface_ms);
    f(clock_paused), f(inventory_open), f(scenario_part), f(music_side_env);
    for (KillInterface& k : kill) f(k.active), f(k.mode);
    for (Waypoint& p : waypoints) f(p.type), f(p.pos), f(p.blink), f(p.blink_ms), f(p.flag), f(p.follow);
    f.map(minimap_icons);
    f(minimap_rotation), f(minimap_gold);
    for (VisionArea& v : vision) f(v.enabled), f(v.shape), f(v.c), f(v.radius);
    f.map(site_text), f.map(site_dialog);
}

void MissionUi::save(ByteWriter& w) const {
    Saver s{w};
    w.u32(1);  // layout version
    const_cast<MissionUi*>(this)->visit(s);
}

bool MissionUi::load_state(formats::ByteReader& r) {
    if (r.u32() != 1) return false;
    Loader l{r};
    visit(l);
    return r.ok();
}

// ---------------------------------------------------------------------------------------------------
// Cut-scene controller 0xa5d380

// Game_set_cutscene 0x4725f0 + FUN_0049b5e0 (attention messages freeze while a cut scene runs; on
// resume their +0x43 timers are shifted by start - now, as the original does).
void MissionUi::set_game_cut_scene(Mission& m, bool on) {
    if (on) {
        game_cut_scene = m.state().cut_scene = m.world().cut_scene = true;
    }
    if (messages_paused != on) {
        if (!on) {
            const std::uint32_t delta = messages_pause_ms - m.now();
            for (AttentionMessage& a : messages) a.t += delta;
        } else {
            messages_pause_ms = m.now();
        }
        messages_paused = on;
    }
    if (!on) game_cut_scene = m.state().cut_scene = m.world().cut_scene = false;
    m.emit(K::CutScene, cs.status, on, on ? "start" : "end");
}

void MissionUi::play_camera_script(Mission& m, std::string name, std::uint32_t start_ms) {
    camera.playing = true;
    camera.name = std::move(name);
    camera.start_ms = start_ms;
    m.emit(K::Camera, 0, 1, "play " + camera.name);
}

int MissionUi::cut_scene_start(Mission& m, bool no_camera, bool move, int unit, const float* pos, float a, float b,
                               float c, std::uint8_t black) {
    if (cs.active) return 0;
    // Camera-input flags (+0x41..+0x43), HUD state (+0x44, FUN_005ef400) and the per-unit cues of
    // units with +0x31d (FUN_0043b890) are engine/render state; the controller leaves range mode.
    World& w = m.world();
    range_exit(w);
    cs.saved_camera = 0;  // CameraArray_current_index: the engine restores its own current camera
    if (!no_camera) {
        if (!pos) {
            const Unit* u = w.unit(unit);
            if (!u || u->deleted) return 0;
            const float v[6] = {u->pos.x, u->pos.y, u->pos.z + 50.0f, a, b, c};
            std::copy(v, v + 6, cs.cam);
        } else {
            const float v[6] = {pos[0], pos[1], pos[2], a, b, c};
            std::copy(v, v + 6, cs.cam);
        }
        if (!move) {
            cs.camera_move = false;
            cs.status = 2;
            m.emit(K::Camera, unit, 0, "cut " + cam_text(cs.cam));
        } else {
            play_camera_script(m, "cut_scene_play", m.now());
            cs.camera_move = true;
            cs.status = 1;
        }
    } else {
        cs.camera_move = false;
        cs.status = 2;
    }
    cs.black_start = cs.black_end = black;
    if (black == 1) {
        cs.status = 6;
        cs.wipe = static_cast<std::int16_t>(kScreenW);
        cs.wipe_flip = false;
        if (pos) cs.fade = 0;  // FUN_006cad50 only
    }
    cs.starting = true;
    cs.ending = false;
    cs.active = true;  // FUN_006cab90
    cs.start_ms = m.now();
    cs.speed = 128.0f;
    set_game_cut_scene(m, true);
    // RTS-mode unit/item flags (FUN_0042cf20, FUN_00452be0) and ThrowItems_remove_foreign: render/RTS UI.
    return 1;
}

int MissionUi::cut_scene_end(Mission& m, bool move_back, std::uint8_t black) {
    if (!cs.active) return 0;
    if (cs.status != 2 && cs.ending) return 1;
    if (move_back) {
        play_camera_script(m, "cut_scene_play", m.now());
        cs.camera_move = true;
        cs.status = 4;
    }
    cs.black_end = black;
    if (black == 1) {
        cs.status = 8;
        cs.wipe = static_cast<std::int16_t>(-kWipeTexW);
        cs.wipe_flip = true;
    }
    cs.starting = false;
    cs.ending = true;
    cs.active = true;
    cs.start_ms = m.now();
    cs.speed = -128.0f;
    del_all_dialogs(m);  // FUN_0063d150
    m.emit(K::CutScene, cs.status, 1, "ending");
    return 1;
}

int MissionUi::cut_scene_camera(Mission& m, bool move, int unit, const float* pos, float a, float b, float c) {
    if (cs.status != 2 && cs.status != 7) return 0;
    if (!(0.0f <= cs.speed) || !cs.active) return 0;
    if (!pos) {
        const Unit* u = m.world().unit(unit);
        if (!u || u->deleted) return 0;
        const float v[6] = {u->pos.x, u->pos.y, u->pos.z + 50.0f, a, b, c};
        std::copy(v, v + 6, cs.cam);
    } else {
        const float v[6] = {pos[0], pos[1], pos[2], a, b, c};
        std::copy(v, v + 6, cs.cam);
    }
    if (!move) {
        cs.camera_move = false;
        if (cs.status != 7) cs.status = 2;
        m.emit(K::Camera, unit, 0, "cut " + cam_text(cs.cam));
    } else {
        play_camera_script(m, "cut_scene_play", m.now());
        cs.camera_move = true;
        if (cs.status != 7) cs.status = 3;
    }
    return 1;
}

bool MissionUi::cut_scene_wait_done() const { return cs.status == 0 || cs.status == 2 || cs.ending; }

// FUN_006ca660, once per tick while in game.
void MissionUi::tick_cut_scene(Mission& m) {
    if (!cs.active) return;
    const std::uint32_t now = m.now();
    const auto step = static_cast<std::int16_t>(
        ((static_cast<std::uint64_t>(kWipeTexW + kScreenW) * static_cast<std::uint32_t>(kDt)) & 0xffffffffu) / 0x5dc);
    auto restore_camera = [&] { m.emit(K::Camera, cs.saved_camera, 0, "restore"); };  // FUN_00429860(+0x30)
    auto finish = [&] {
        cs.active = false;
        set_game_cut_scene(m, false);
        cs.status = 0;
    };
    if (cs.status == 6) {
        if (!cs.starting) {
            if (cs.wipe < kScreenW) {
                cs.wipe = static_cast<std::int16_t>(cs.wipe + step);
            } else {
                if (!cs.camera_move) finish();
                else cs.status = 1;
                cs.ending = false;
            }
        } else if (-kWipeTexW < cs.wipe) {
            cs.wipe = static_cast<std::int16_t>(cs.wipe - step);
        } else {
            cs.status = 7;
            cs.hold_ms = now;
            cs.fade = 255.0f;
        }
    } else if (cs.status == 7) {
        if (0x2edu < now - cs.hold_ms) {
            if (!cs.starting) {
                cs.status = 6;
                cs.wipe = -1;
                restore_camera();
            } else {
                cs.status = 8;
                cs.wipe = static_cast<std::int16_t>(kScreenW);
            }
            cs.wipe_flip = !cs.wipe_flip;
        }
    } else if (cs.status == 8) {
        if (!cs.starting) {
            if (cs.wipe < kScreenW) {
                cs.wipe = static_cast<std::int16_t>(cs.wipe + step);
            } else {
                cs.status = 7;
                cs.hold_ms = now;
                cs.fade = 0;
            }
        } else if (-kWipeTexW < cs.wipe) {
            cs.wipe = static_cast<std::int16_t>(cs.wipe - step);
        } else {
            cs.status = cs.camera_move ? 1 : 2;
            cs.starting = false;
        }
    } else {
        cs.fade = static_cast<float>(static_cast<double>(static_cast<float>(kDt) * cs.speed) / 1000.0 + cs.fade);
        if (cs.fade < 0.0f) cs.fade = 0;
        if (255.0f < cs.fade) cs.fade = 255.0f;
        if (cs.fade == 255.0f && !camera.playing) {
            cs.status = 2;
            cs.starting = false;
        }
        if (cs.fade == 0.0f && !camera.playing) {
            finish();
            restore_camera();
            cs.ending = false;
        }
    }
}

// FUN_006ca5c0: ESC during a cut scene.
void MissionUi::skip_cut_scene(Mission& m) {
    if (!cs.active) return;
    if ((cs.black_start == 1 || cs.black_end == 1) && cs.status != 2) return;
    cut_scene_end(m, false, cs.black_end);
}

// ---------------------------------------------------------------------------------------------------
// Dialog boxes

MissionUi::DialogBox* MissionUi::dialog(int id) {
    for (DialogBox& d : dialogs)
        if (d.id == id && !d.deleted) return &d;
    return nullptr;
}

// FUN_0063cdc0: box text "_<speaker name>:_ <text>", default rect when rect == nullptr.
int MissionUi::add_dialog(Mission& m, int speaker, std::string text, std::string stem, int voice, std::int32_t anim,
                          std::int16_t anim2, const std::int16_t* rect) {
    const Unit* u = m.world().unit(speaker);
    if (!u || u->deleted) return 0;
    DialogBox d;
    d.id = static_cast<int>(dialogs.size()) + 1;
    d.speaker = speaker;
    d.text = ("_" + u->name + ":_ " + text).substr(0, 400);
    d.voice_stem = std::move(stem);
    d.voice = voice;
    d.anim = anim;
    d.anim2 = anim2;
    if (rect) std::copy(rect, rect + 4, d.rect);
    m.emit(K::Dialog, d.id, speaker, d.text);
    dialogs.push_back(std::move(d));
    return dialogs.back().id;
}

void MissionUi::del_dialog(Mission& m, int id) {
    if (DialogBox* d = dialog(id)) {
        d->deleted = true;
        m.emit(K::DialogClosed, id);
    }
}

void MissionUi::del_all_dialogs(Mission& m) {
    for (const DialogBox& d : dialogs)
        if (!d.deleted) m.emit(K::DialogClosed, d.id);
    dialogs.clear();  // DynArrayB_clean: ids restart at 1
}

bool MissionUi::dialogs_done() const {
    for (const DialogBox& d : dialogs)
        if (!d.deleted && !d.done) return false;
    return true;
}

void MissionUi::answer_dialog(int id, int choice) {
    if (DialogBox* d = dialog(id)) d->choice = choice;
}

void MissionUi::dialog_done(int id) {
    if (DialogBox* d = dialog(id)) d->done = true;
}

void MissionUi::close_interface() {
    buy_open = brief_open = false;
    if (letter_open) letter_open = clock_paused = false;
}

// ---------------------------------------------------------------------------------------------------
// Attention messages

MissionUi::AttentionMessage* MissionUi::attention_message(int recno) {
    for (AttentionMessage& a : messages)
        if (a.recno == recno) return &a;
    return nullptr;
}

// FUN_004ad710 / FUN_004ad9a0 (+ FUN_004aa3c0 init). Returns the recno, 0 when the same text is live.
int MissionUi::add_attention_message(Mission& m, std::string text, std::string icon, int speaker, std::string stem,
                                     int voice) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
        if (it->text == text) return 0;
    AttentionMessage a;
    a.fast = speaker == 0 && (icon == "M0000" || icon == "TMESS");
    a.len = static_cast<std::int16_t>(text.size());
    a.text = std::move(text);
    a.icon = std::move(icon);
    a.speaker = speaker;
    a.voice_stem = std::move(stem);
    a.voice = voice;
    int queued = 0;  // slow messages still sliding in
    for (const AttentionMessage& o : messages)
        if (!o.fast && (o.state == 0 || o.state == 1) && o.y <= static_cast<float>(kScreenH - 0x112)) ++queued;
    a.slot = static_cast<std::int16_t>(kScreenH - 0x112 - queued * 0x28);
    a.recno = next_message++;
    m.emit(K::AttentionMessage, a.recno, 1, a.text);
    messages.push_back(std::move(a));
    return messages.back().recno;
}

void MissionUi::clear_attention_messages() {
    messages.clear();
    next_message = 1;  // DynArrayB_clean
}

// FUN_004ace90 per message (array process FUN_004ae390: highest recno first, skipped while paused).
// HYPOTHESIS: a message leaves its queue slot only when it is the oldest live one (FUN_004ae710 /
// FUN_004af2b0 ordering), and voices never hold the message (voice length is engine data).
void MissionUi::tick_messages(Mission& m) {
    if (messages_paused) return;
    const std::uint32_t now = m.now();
    const float top = static_cast<float>(kScreenH - 0xc2);
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        AttentionMessage& a = *it;
        switch (a.state) {
        case 0:
            a.state = 1;
            a.t = now;
            break;
        case 1:
            if (a.y < top) {
                if (static_cast<float>(a.slot) <= a.y) {
                    if (a.recno != messages.front().recno) {
                        a.t = now;
                        break;
                    }
                    a.slot = static_cast<std::int16_t>(kScreenH - 0xc2);
                }
                const double v = static_cast<double>(static_cast<float>(kDt)) * 0.3;
                a.y = static_cast<float>((a.fast ? v * 10.0 : v) + a.y);
                if (static_cast<float>(a.slot) < a.y) a.y = static_cast<float>(a.slot);
                if (top < a.y) a.y = top;
                a.t = now;
            } else {
                a.state = 2;
                a.t = now;
            }
            break;
        case 2:
            a.state = 3;
            m.emit(K::Sound, a.recno, 0, "ATTENTION_CALL");
            break;
        case 3: {
            auto grow = [](float& f, double rate) {
                f = static_cast<float>(static_cast<double>(static_cast<float>(kDt)) * rate + f);
                if (255.0f < f) f = 255.0f;
            };
            if (255.0f <= a.f4b) {
                a.state = 4;
            } else {
                grow(a.f4f, 0.5);
                if (200.0f <= a.f4f) grow(a.f53, 0.7);
                if (230.0f <= a.f53) grow(a.f47, 1.3);
                if (255.0f <= a.f47) grow(a.f4b, 1.3);
            }
            break;
        }
        case 4:
            if (static_cast<float>(a.len) <= a.typed) {
                if (a.t + static_cast<std::uint32_t>(a.len * 0x21) < now) a.state = 5;
            } else {
                a.t = now;
                a.typed = static_cast<float>(static_cast<double>(static_cast<float>(kDt)) * 0.04 + a.typed);
                if (static_cast<float>(a.len) <= a.typed) a.typed = static_cast<float>(a.len);
            }
            break;
        case 5:
            if (a.f57 <= 0.0f) a.state = 6;  // FUN_00499e40: message log
            a.f57 = static_cast<float>(a.f57 - static_cast<float>(kDt) * 1.0);
            if (a.f57 < 0.0f) a.f57 = 0;
            break;
        }
    }
    for (auto it = messages.begin(); it != messages.end();) {
        if (it->state == 6) {
            m.emit(K::AttentionMessage, it->recno, 0, it->text);
            it = messages.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------------------------------

void MissionUi::tick(Mission& m) {
    const std::uint32_t now = m.now();
    const int status0 = cs.status;
    m.world().cut_scene = game_cut_scene;

    // Headless camera scripts (FUN_00428aa0 playing) end auto_ms after their start.
    if (auto_advance && camera.playing && now >= camera.start_ms && now - camera.start_ms >= static_cast<std::uint32_t>(auto_ms))
        camera.playing = false;
    tick_cut_scene(m);
    if (cs.status != status0) m.emit(K::CutScene, cs.status, cs.active);

    // Dialog boxes type only while the letterbox is fully shown (FUN_0063b830 gate).
    if (cs.status == 2 && cs.fade == 255.0f)
        for (DialogBox& d : dialogs) {
            if (d.deleted || d.done) continue;
            d.shown_ms += static_cast<std::uint32_t>(kDt);
            if (auto_advance && d.shown_ms >= static_cast<std::uint32_t>(auto_ms)) d.done = true;
        }
    if (auto_advance)
        for (DialogBox& d : dialogs) {
            if (d.deleted || !d.has_choices || !d.done || d.choice) continue;
            const int n = static_cast<int>(d.choices.size());
            auto ok = [&](int i) { return i >= 1 && i <= n && (d.choices[i - 1].enabled || dialog_show_disabled); };
            int pick = ok(auto_choice) ? auto_choice : 0;
            for (int i = 1; !pick && i <= n; ++i)
                if (ok(i)) pick = i;
            if (pick) {
                d.choice = pick;
                m.emit(K::ChoiceDialog, d.id, pick);
            }
        }

    // Title screen FUN_00761630.
    if (title.active && title.start_ms + static_cast<std::uint32_t>(title.duration) < now) {
        title.active = false;
        m.emit(K::TitleScreen, 0, 0, title.text);
    }
    if (auto_advance && (buy_open || brief_open || letter_open) && now - interface_ms >= static_cast<std::uint32_t>(auto_ms)) {
        m.emit(buy_open ? K::BuyInterface : brief_open ? K::Brief : K::Letter, 0, 0, "close");
        close_interface();
    }
    tick_messages(m);
}

std::vector<ShopStock> shop_stock(Mission& m, int facility, float* selling_ratio) {
    std::vector<ShopStock> out;
    if (selling_ratio) *selling_ratio = 0.5f;
    const Facility* f = m.places().facility(facility);
    const auto path = f ? m.vfs().resolve("item/sellitem.res") : std::nullopt;
    auto a = path ? formats::ResArchive::open(*path) : std::nullopt;
    if (!a || f->sell_list < 1 || f->sell_list > int(a->entries().size())) return out;
    const GameData& gd = m.data();
    for (const auto& l : formats::parse_kv_text(a->text(a->entries()[std::size_t(f->sell_list - 1)].name).value_or(""))) {
        std::string key = l.key;
        std::erase_if(key, [](unsigned char c) { return std::isspace(c); });
        if (selling_ratio && formats::iequals(key, "SellingRatio")) *selling_ratio = float(std::atof(l.value.c_str()));
        const int id = gd.item_index(key);
        if (!id) continue;
        int col[3] = {1, -1, -1}, n = 0;
        for (std::size_t a0 = 0; a0 <= l.value.size() && n < 3; ++n) {
            const std::size_t e = std::min(l.value.find(':', a0), l.value.size());
            col[n] = std::atoi(l.value.substr(a0, e - a0).c_str());
            a0 = e + 1;
        }
        out.push_back({id, col[2] < 0 ? gd.items[std::size_t(id)].min_level : col[2], col[1]});
    }
    return out;
}

}  // namespace joa::game
