#include "engine/session_client.hpp"

#include "engine/assets_cache.hpp"
#include "engine/audio_system.hpp"
#include "engine/camera.hpp"
#include "engine/game_audio.hpp"
#include "engine/game_camera.hpp"
#include "engine/game_input.hpp"
#include "engine/game_ui_bridge.hpp"
#include "engine/hud_rts.hpp"
#include "engine/hd_pack.hpp"
#include "engine/hud_character.hpp"
#include "engine/hud_effects.hpp"  // ui_format
#include "engine/hud_recruit.hpp"
#include "engine/menu_saveload.hpp"  // ui_confirm_popup
#include "engine/menu_options_store.hpp"
#include "engine/platform.hpp"
#include "engine/png.hpp"
#include "engine/profile.hpp"
#include "engine/renderer_gl.hpp"
#include "engine/renderer_null.hpp"
#include "engine/renderer_vk.hpp"
#include "engine/session_autoplay.hpp"
#include "engine/session_flow.hpp"
#include "engine/sim.hpp"
#include "engine/ui_runner.hpp"
#include "engine/ui_screens.hpp"
#include "engine/video_player.hpp"
#include "engine/world_view.hpp"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace joa::engine {
namespace {

// DEFAULTK actions the flow handles (menus, quick save/load, RTS toggle, HUD toggles).
constexpr int kFlowActions[] = {23, 34, 35, 36, 37, 38, 39, 40, 41, 45, 46, 53, 54};

AudioSettings audio_settings(const GameOptions& g, const Options& o) {
    AudioSettings a;
    a.master = o.master_volume >= 0 ? float(o.master_volume) / 100.0f : 1.0f;
    a.music = float(g.music_volume) / 100.0f;
    a.env = float(g.environment_volume) / 100.0f;
    a.sfx = float(g.sound_volume) / 100.0f;
    a.three_d = g.sound_3d;
    return a;
}

int mission_of_map(Session& s, std::string_view stem) {
    for (const auto& m : s.campaign().missions) {
        const std::string a = m.stem();
        if (a.size() == stem.size() && std::equal(a.begin(), a.end(), stem.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); }))
            return m.no;
    }
    return 0;
}

class Client {
public:
    explicit Client(const Options& o)
        : o_(o), vfs_(o.game_dir.empty() ? std::filesystem::path(".") : std::filesystem::path(o.game_dir)), session_(vfs_),
          flow_(session_, menu_, hud_), bridge_(vfs_) {}

    int run();

private:
    void configure_options();
    void ensure_view();
    void tick_world(double dt);
    void render_frame(double dt, int frame, const Autoplay::FrameOut& script);
    bool mission_visible() const;

    const Options& o_;
    formats::Vfs vfs_;
    Session session_;
    MenuModel menu_;
    HudModel hud_;
    GameFlow flow_;
    GameInput input_;
    GameCamera camera_;
    GameUiBridge bridge_;
    CharacterWindow chars_;  // CharDetails: F1 character window + shop
    FacilityWindow facility_;  // RecruitWorkshop: barracks / siege workshop screens
    std::unique_ptr<Platform> platform_;
    std::unique_ptr<IRenderer> renderer_;
    std::unique_ptr<AudioSystem> audio_;
    std::unique_ptr<GameAudio> game_audio_;
    std::unique_ptr<Ui> ui_;
    std::unique_ptr<WorldView> view_;
    int view_generation_ = -1;
    FixedStep clock_;
    RawInput raw_;
    UiInputTracker ui_tracker_;
    Autoplay script_;
    bool fixed_dt_ = false, was_paused_ = true, menu_music_ = false;
    bool loading_ = false;      // the loading-screen sequence runs (UiBridge stages 0..7); the world stays hidden
    float load_progress_ = 0;   // 1 once the pending start/load ran
    int click_release_ = -1;  // frame of a scripted click's button release
    bool capture_screen_ = false;  // SaveCapture: F10 (DEFAULTK 55) pressed: write this frame, UI included
    // GamepadPlay: menu navigation, the RTS virtual cursor (ui_raw_ = raw_ + its synthesized buttons) and settings.
    PadUiTracker pad_ui_;
    PadPointer pad_pointer_;
    PadSettings pad_settings_;
    RawInput ui_raw_;
    std::vector<DrawItem> draws_;
    std::vector<UiQuad> quads_;
    std::vector<UiCommand> commands_;
    CameraPose pose_;
    FrameView frame_view_;
    glm::ivec4 viewport_{0};
};

void Client::configure_options() {
    // settings.ini (launcher contract) volumes override the in-game option values at startup.
    PadSettings pad;
    if (o_.deadzone >= 0) pad.deadzone = std::clamp(o_.deadzone, 0.0f, 0.9f);
    session_.control_mode = key_scheme(menu_.options.input_method);
    pad.sensitivity = o_.sensitivity;
    input_.configure(menu_.bindings, menu_.options, pad, o_.camera_fix);
    pad_settings_ = pad;
    if (audio_) audio_->apply(audio_settings(menu_.options, o_));
    if (view_) view_->shadow_mode = menu_.options.shadow;
}

bool Client::mission_visible() const {
    const FlowState s = flow_.state();
    return !loading_ && session_.mission() && view_ && (s == FlowState::Playing || s == FlowState::Won || s == FlowState::Lost);
}

void Client::ensure_view() {
    game::Mission* m = session_.mission();
    if (!m || view_generation_ == session_.generation()) return;
    view_generation_ = session_.generation();
    view_.reset();
    view_ = std::make_unique<WorldView>(*renderer_, vfs_, *m, resolve_hd_packs(o_.graphics.hd_packs));
    camera_.assets = &view_->assets();
    view_->shadow_mode = menu_.options.shadow;
    m->ui().key_name = [this](int action) { return action_key_name(vfs_, menu_, action); };  // get_key_string: user bindings
    view_->sync(m->world(), session_.previous(), session_.current());
    camera_.reset(*m);
    camera_.load_view(session_.view_section);  // RtsHero: groups / commanders / RTS camera of a native save
    hud_.rts_join_prompt.clear();
    if (o_.free_camera || !o_.camera_pose.empty()) {
        camera_.free = true;
        const auto& p = o_.camera_pose;
        const game::Unit* joan = m->world().unit(m->world().player_id());
        const glm::vec3 eye = p.size() >= 3 ? glm::vec3(p[0], p[1], p[2])  // original units (set_free_pose)
                              : joan        ? glm::vec3(joan->pos.x, joan->pos.y, joan->pos.z + 250.0f)
                                            : glm::vec3(0.0f);
        camera_.set_free_pose(eye, p.size() >= 4 ? p[3] : (joan ? joan->yaw_deg : 0.0f), p.size() >= 5 ? p[4] : -20.0f);
    }
    bridge_.reset(*m, hud_);
    chars_.reset(), hud_.show_character = false, hud_.armory_item = 0;
    facility_.reset(), hud_.recruit_screen = hud_.workshop_screen = 0;   // RecruitWorkshop
    session_.rts_unlocked = camera_.rts_unlocked = o_.rts_unlocked;
    input_.reset();
    clock_.accumulator = 0;
    if (game_audio_) game_audio_->stop();
    menu_music_ = false;
}

void Client::tick_world(double dt) {
    game::Mission* m = session_.mission();
    const bool paused = flow_.paused() || loading_;
    if (!m || paused) {
        clock_.accumulator = clock_.tick;  // render the last tick (alpha 1) while paused
        was_paused_ = true;
        return;
    }
    if (was_paused_) input_.reset(), was_paused_ = false;
    for (int n = clock_.advance(dt); n > 0 && !flow_.paused(); --n) {
        JOA_ZONE("sim.tick");
        game::InputFrame in = input_.take();
        camera_.before_tick(*m, in);
        session_.tick(in);
        camera_.after_tick(*m, input_);
        view_->sync(m->world(), session_.previous(), session_.current());
        const auto we = session_.take_world_events();
        const auto me = session_.take_mission_events();
        view_->on_events(we, session_.current());
        chars_.on_world_events(we, *m, hud_);  // CharDetails: ARMORY window on show_armory pickups
        if (game_audio_) game_audio_->on_tick(*m, we, me, session_.current());
        bridge_.on_events(me, *m, hud_);
        facility_.on_mission_events(me, *m, hud_);  // RecruitWorkshop: Interact "recruit_screen" / "workshop_screen"
        bridge_.on_world_events(we, *m, hud_);
        if (platform_ && o_.rumble)  // GamepadPlay: rumble on the player's hits / damage
            if (const PadRumble r = pad_rumble(we, m->world().player_id()); r.ms > 0) platform_->rumble(r.low, r.high, r.ms);
        flow_.update();
    }
}

void Client::render_frame(double dt, int frame, const Autoplay::FrameOut& script) {
    const glm::ivec2 size = platform_ ? platform_->pixel_size() : glm::ivec2(o_.width, o_.height);
    viewport_ = fit_viewport(size, o_.aspect);
    const float aspect = float(viewport_.z) / float(std::max(viewport_.w, 1));
    FrameData f;
    draws_.clear();
    quads_.clear();
    game::Mission* m = session_.mission();
    const float alpha = flow_.paused() ? 1.0f : clock_.alpha();
    if (mission_visible()) {
        pose_ = camera_.pose(alpha);
        f.view.projection = perspective_hor_plus(std::atan(pose_.fov_k), aspect, kDefaultNear, kDefaultFar);
        f.view.viewport = viewport_;
        view_->selection = session_.in_rts() ? &camera_.rts_selection() : nullptr;  // RTS selection rings
        view_->frame(alpha, pose_, float(dt), f.view, draws_);
        frame_view_ = f.view;
        f.lighting = view_->lighting();
        bridge_.voice_active = game_audio_ && game_audio_->voice_active();
        bridge_.rts_commander = camera_.rts_commander(), bridge_.rts_siege = camera_.rts_hover_siege();  // HudFinish
        camera_.minimap_scale = hud_minimap_scale(*ui_, hud_);  // RtsHero: Ctrl_switch_commander view test
        bridge_.char_unit = hud_.show_character ? chars_.unit : 0;
        hud_.sim_paused = flow_.paused();
        bridge_.menu = &menu_;
        bridge_.fill(*m, session_.current(), f.view.projection * f.view.view, viewport_, hud_);
        hud_.rts_mode = session_.in_rts();
        if (hud_.show_character) {   // CharDetails: name bars are world billboards in the original, i.e. under the window
            const UiLayout l = ui_layout(viewport_);
            std::erase_if(hud_.name_bars, [&](const HudUnit& b) {
                const glm::vec2 d = (b.screen - l.safe_origin) / l.scale;
                return d.x < 808.0f && d.y < 600.0f && d.y > 0.0f;
            });
        }
    } else {
        f.view.viewport = viewport_;
        f.lighting = Lighting{};  // menus / loading: black behind the 4:3 canvas
    }

    // UI input: the window pointer, or the autoplay pointer (design units of the 4:3 canvas), or the pad's RTS
    // cursor (ui_raw_ carries its buttons); the pad's menu navigation (GamepadPlay).
    UiInput ui_in = ui_tracker_.update(platform_ ? platform_->window() : nullptr, ui_raw_);
    if (script.pointer) {
        const UiLayout l = ui_layout(viewport_);
        ui_in.mouse = l.safe_origin + *script.pointer * l.scale;
        if (script.click) ui_in.mouse_down = ui_in.mouse_pressed = true, click_release_ = frame + 1;
        else if (frame == click_release_) ui_in.mouse_down = false, ui_in.mouse_released = true;
    }
    if (pad_pointer_.active) ui_in.mouse = pad_pointer_.pos;
    ui_in.pad = pad_ui_.update(raw_, float(dt), pad_settings_.deadzone);
    commands_.clear();
    ui_->begin(viewport_, quads_, ui_in, float(dt));
    if (mission_visible() && !camera_.free) {  // the fly camera is a map viewer: no HUD / cut-scene overlays
        // Full-screen pages (mission brief, message log) sit above the cut-scene overlays: scripts open the brief
        // while their opening cut scene holds a black screen (S3L1), and the sim waits for the brief to close.
        if (hud_.show_mission_brief || hud_.show_message_log) {
            bridge_.draw(*ui_, *m, hud_, commands_);
            ui_hud(*ui_, hud_, commands_);
        } else {
            // CharDetails: the character window sits below the HUD (the attention message overlaps it in
            // s04_06_character_details); the 800x600 shop covers it.
            const bool shop = m->ui().buy_open || hud_.armory_item;  // full-screen shop / armory cover the HUD
            const bool facility = hud_.recruit_screen || hud_.workshop_screen || facility_.open();  // RecruitWorkshop
            if (!shop && !facility) chars_.draw(*ui_, *m, hud_);
            if (hud_.show_character) ui_->nav_seal();  // the HUD drawn over it stays out of its pad focus ring
            if (!bridge_.blocking(*m) && !facility) ui_hud(*ui_, hud_, commands_);
            bridge_.draw(*ui_, *m, hud_, commands_);
            if (shop) chars_.draw(*ui_, *m, hud_);
            if (facility) facility_.draw(*ui_, *m, hud_);   // RecruitWorkshop: barracks / siege workshop screens
        }
    }
    if (mission_visible() && !hud_.rts_join_prompt.empty())  // RtsHero: CHARMOVE|1 over-capacity confirmation
        if (const int r = ui_confirm_popup(*ui_, hud_.rts_join_prompt)) hud_.rts_join_prompt.clear(), camera_.rts_join_answer(*m, r == 1);
    ui_menu(*ui_, menu_, commands_);
    if (loading_) bridge_.draw_loading(*ui_, flow_.loading_mission(), load_progress_);
    ui_->frame(Anchor::Center);
    const bool pointer = flow_.paused() || session_.in_rts() || flow_.state() != FlowState::Playing;
    // RtsComplete: RTS overlay (portrait selection, drag box, RTSICON cursor) instead of the arrow while playing.
    if (mission_visible() && !camera_.free && session_.in_rts() && !flow_.paused() && menu_.screen == MenuScreen::None &&
        !hud_.show_mission_brief && !hud_.show_message_log && !bridge_.blocking(*m)) {
        camera_.rts_blocked = rts_hud_rects(*ui_, hud_);
        ui_rts(*ui_, hud_, camera_, commands_);
    } else if (pointer) {
        ui_->cursor();
    }
    if (platform_) {
        platform_->set_mouse_look(flow_.state() == FlowState::Playing && (camera_.free ? !flow_.paused() : !pointer));
        platform_->set_text_input(menu_.screen == MenuScreen::Save && menu_.save_state == 2);  // description entry
    }

    f.draws = draws_;
    f.ui = quads_;
    renderer_->begin_frame(size);
    {
        JOA_ZONE("render.submit");
        renderer_->submit(f);
    }
    std::vector<std::string> shots = script.shots;
    if (!o_.screenshot.empty() && frame + 1 == o_.frames) shots.push_back(o_.screenshot);
    if (capture_screen_) shots.push_back(next_screenshot_path().string()), capture_screen_ = false;
    for (const std::string& path : shots) {
        const Image img = renderer_->read_back();
        if (!write_png(path, img.width, img.height, img.rgba)) std::fprintf(stderr, "joan-native: cannot write %s\n", path.c_str());
        else std::fprintf(stderr, "joan-native: wrote %s (%dx%d) frame %d state %s\n", path.c_str(), img.width, img.height, frame,
                          flow_state_name(flow_.state()));
    }
    renderer_->end_frame();
}

int Client::run() {
    const bool headless = o_.headless;
    const bool capture = !o_.screenshot.empty() || !o_.autoplay.empty();
    fixed_dt_ = headless || capture;
    clock_.tick = kGameTickSeconds;
    clock_.max_ticks_per_frame = 8;
    if (!o_.autoplay.empty()) {
        std::string err;
        if (!script_.load(o_.autoplay, &err)) return std::fprintf(stderr, "joan-native: autoplay: %s\n", err.c_str()), 1;
    }
    if (headless) {
        renderer_ = std::make_unique<NullRenderer>();
    } else {
        platform_ = std::make_unique<Platform>(o_, capture);
        renderer_ = o_.renderer == RendererKind::Vulkan ? create_vk_renderer(platform_->window(), o_) : create_gl_renderer(platform_->window());
        SDL_HideCursor();  // CURSOR.RES pointer drawn by the UI
    }
    renderer_->set_graphics(o_.graphics.resolve());
    ui_ = std::make_unique<Ui>(vfs_, *renderer_);
    load_options(vfs_, options_ini_path(), menu_.options, menu_.bindings);
    if (o_.music_volume >= 0) menu_.options.music_volume = o_.music_volume;
    if (o_.sound_volume >= 0) menu_.options.sound_volume = o_.sound_volume;
    if (o_.environment_volume >= 0) menu_.options.environment_volume = o_.environment_volume;
    audio_ = std::make_unique<AudioSystem>(vfs_, audio_settings(menu_.options, o_), !fixed_dt_);
    game_audio_ = std::make_unique<GameAudio>(*audio_);
    configure_options();
    flow_.options_changed = [this] { configure_options(); };
    flow_.list_saves = [this] { return list_saves(vfs_); };
    // Last world frame re-rendered without UI, off-screen: begin_frame -> submit -> read_back and no end_frame, so
    // nothing is presented (GL never swaps; VK finish_scene ends recording and the next begin_frame starts a new slot).
    // The frame's normal begin/submit/end follows, so the HUD never blinks.
    flow_.capture_world = [this] {
        if (!mission_visible()) return Image{};
        FrameData f;
        f.view = frame_view_;
        f.lighting = view_->lighting();
        f.draws = draws_;
        renderer_->begin_frame(platform_ ? platform_->pixel_size() : glm::ivec2(o_.width, o_.height));
        renderer_->submit(f);
        const Image img = renderer_->read_back();
        return save_thumbnail(img, viewport_);
    };
    session_.seed = o_.seed;
    bridge_.auto_advance = headless;  // nobody answers dialogs / waits for camera scripts without a window
    session_.rts_unlocked = camera_.rts_unlocked = o_.rts_unlocked;
    session_.on_rts_exit = [this](game::Mission& m) { camera_.rts_deinit(m); };  // RtsHero: Rts_deinit hero switch
    session_.save_view = [this] { return camera_.save_view(); };               // RtsHero: "VIEW" save section

    // Entry: CLI shortcut (no movies/menus), else the startup movies (Esc skips one) and the main menu.
    const int difficulty = o_.difficulty >= 0 ? o_.difficulty : menu_.options.difficulty;
    if (!o_.scenario.empty()) {
        const int no = mission_of_map(session_, o_.scenario);
        if (!no) return std::fprintf(stderr, "joan-native: --scenario %s is not a campaign mission map\n", o_.scenario.c_str()), 1;
        flow_.new_game(no, difficulty);
    } else if (o_.new_game) {
        flow_.new_game(o_.mission, difficulty);
    } else if (!o_.load.empty()) {
        flow_.load(o_.load);
    } else if (platform_ && !capture && !o_.no_movies) {
        for (const char* movie : {"movie/enlight.wmv", "movie/intro.wmv"})
            if (const auto path = vfs_.resolve(movie)) play_movie(*platform_, *renderer_, &audio_->mixer(), *path);
    }
    if (flow_.state() == FlowState::Loading && !flow_.run_pending()) return 1;
    if (flow_.state() == FlowState::Menu && !flow_.error.empty()) return 1;  // a CLI shortcut failed

    std::uint64_t last_ns = SDL_GetTicksNS();
    int frame = 0;
    const int last = o_.frames > 0 ? o_.frames : (!script_.empty() ? script_.last_frame() + 2 : 0);
    for (; last == 0 || frame < last; ++frame) {
        if (platform_ && !platform_->pump(raw_)) break;
        const std::uint64_t now_ns = SDL_GetTicksNS();
        const double dt = fixed_dt_ ? kGameTickSeconds : std::min(double(now_ns - last_ns) * 1e-9, 0.25);
        last_ns = now_ns;
        const Autoplay::FrameOut script = script_.apply(frame, raw_);
        for (const std::string& l : script.logs) {
            std::fprintf(stderr, "autoplay %d: state %s%s%s mission %d tick %llu menu %d brief %d %s\n", frame, flow_state_name(flow_.state()),
                         flow_.paused() ? " (paused)" : "", loading_ ? " (loading screen)" : "",
                         session_.mission() ? session_.mission()->scenario_no() : 0, static_cast<unsigned long long>(session_.ticks()),
                         int(menu_.screen), int(hud_.show_mission_brief), l.c_str());
            if (game::Mission* m = session_.mission()) {  // player + nearest hostile: helps writing scripts
                game::World& w = m->world();
                const game::Unit* p = w.unit(w.player_id());
                const game::Unit* foe = nullptr;
                float best = 1e30f;
                for (const game::Unit& u : w.units())
                    if (p && !u.deleted && !w.dead(u) && w.hostile_to_player(u)) {
                        const float d = std::hypot(u.pos.x - p->pos.x, u.pos.y - p->pos.y);
                        if (d < best) best = d, foe = &u;
                    }
                if (p)
                    std::fprintf(stderr, "  player %d (%s) at %.0f,%.0f yaw %.0f hp %d action %d mode %d; nearest hostile %s\n", p->id,
                                 w.data().units[std::size_t(p->cls)].code.c_str(), p->pos.x, p->pos.y, p->yaw_deg, p->hp, p->action, w.game_mode,
                                 foe ? (std::to_string(foe->id) + " at " + std::to_string(int(foe->pos.x)) + "," + std::to_string(int(foe->pos.y)) +
                                        " dist " + std::to_string(int(best)) + " bearing " +
                                        std::to_string(int(std::atan2(foe->pos.y - p->pos.y, foe->pos.x - p->pos.x) * 57.29578f)))
                                           .c_str()
                                     : "none");
                if (w.game_mode == 4) {  // RtsFinish: RTS camera pivot + selection (recno@x,y task/action) for scripts
                    std::fprintf(stderr, "  rts camera %.0f,%.0f dist %.0f; voice %s; selection:", camera_.state().pivot.x,
                                 camera_.state().pivot.y, camera_.state().dist, camera_.rts_voice().c_str());
                    for (int id : camera_.rts_selection())
                        if (const game::Unit* u = w.unit(id))
                            std::fprintf(stderr, " %d@%.0f,%.0f/t%d/a%d", id, u->pos.x, u->pos.y, m->ai().st(*u).task.id, u->action);
                    std::fprintf(stderr, "\n");
                }
            }
        }
        if (script.win && session_.mission()) session_.mission()->finish_objectives();
        for (const auto& h : script.items)  // EquipVisuals test hooks
            if (!session_.mission() || !Autoplay::apply_item_hook(*session_.mission(), h))
                std::fprintf(stderr, "joan-native: autoplay: %s %s failed\n", h.verb.c_str(), h.code.c_str());
        if (script.quit) break;
        input_.feed(raw_, float(dt));

        for (const UiCommand& c : script.commands) flow_.command(c);
        const auto zoom = hud_.minimap_zoom;
        // PadWindows: while the pad drives the character window, D-pad up (J_JOYPAD 7 = action 35) moves its focus
        // instead of toggling it shut (East / Start / Esc close it); the keyboard F1 still closes it.
        const bool pad_window = hud_.show_character && ui_->pad_focus() && raw_.pad_buttons[SDL_GAMEPAD_BUTTON_DPAD_UP];
        for (int id : kFlowActions)
            if (!loading_ && input_.pressed(id) && !(pad_window && id == 35)) flow_.action(id);
        if (hud_.minimap_zoom != zoom) audio_->play_sound("MAP_BUTTON");  // minimap toggle key (0x70eb20)
        if (input_.pressed(44)) bridge_.show_names = !bridge_.show_names;  // Show Hit Point Bar (F11)
        if (input_.pressed(55)) capture_screen_ = true;                       // Capture Screen (F10)

        ensure_view();
        if (flow_.state() == FlowState::Menu && !menu_music_) game_audio_->menu(), menu_music_ = true;
        tick_world(dt);
        // GamepadPlay: RTS virtual cursor (right stick; South / West = left / right button) while RTS is live.
        if (mission_visible() && !flow_.paused() && !camera_.free && session_.in_rts() && !bridge_.blocking(*session_.mission()))
            pad_pointer_.update(raw_, float(dt), viewport_, pad_settings_);
        else
            pad_pointer_.active = false;
        ui_raw_ = raw_;
        pad_pointer_.apply(ui_raw_);
        if (mission_visible() && !flow_.paused()) {
            if (camera_.free) camera_.free_move(input_, float(dt), {raw_.mouse_dx, raw_.mouse_dy}, menu_.options);
            else if (session_.in_rts()) {
                glm::vec2 mouse{-1.0f};
                if (script.pointer) {
                    const UiLayout l = ui_layout(viewport_);
                    mouse = l.safe_origin + *script.pointer * l.scale;
                } else if (platform_) {
                    SDL_GetMouseState(&mouse.x, &mouse.y);
                    mouse *= SDL_GetWindowPixelDensity(platform_->window());
                }
                if (pad_pointer_.active) mouse = pad_pointer_.pos;
                camera_.rts_input(*session_.mission(), ui_raw_, mouse, viewport_, frame_view_.projection * frame_view_.view);
                if (camera_.rts_join_target() && hud_.rts_join_prompt.empty())  // RtsHero: the popup opens, the game pauses
                    hud_.rts_join_prompt = ui_format(ui_->text("CHARMOVE", 1), camera_.rts_join_args());
            }
        }
        render_frame(dt, frame, script);
        for (const std::string& s : ui_->sounds) audio_->play_sound(s);  // UI SE, 2D at the listener
        raw_.end_frame();
        game_audio_->frame(pose_, float(dt));
        if (fixed_dt_) {  // no device pulls the mixer: advance it in step so voices end and dialogs proceed
            static std::vector<float> scratch;
            const std::size_t n = std::size_t(kGameTickSeconds * 44100.0);
            scratch.resize(n * 2);
            audio_->mixer().mix(scratch.data(), n);
        }

        for (const UiCommand& c : commands_) {
            game::Mission* cm = session_.mission();
            if (cm && c.type == UiCommand::Type::MinimapClick) camera_.rts_minimap(*cm, c.value, c.world);
            else if (cm && c.type == UiCommand::Type::RtsPortrait) camera_.rts_portrait(*cm, bridge_.party_unit(c.value));
            else if (!cm || !bridge_.handle(c, *cm)) flow_.command(c);
        }
        flow_.update();
        if (flow_.state() == FlowState::Loading && !loading_) loading_ = true, load_progress_ = 0, bridge_.restart_loading();
        if (loading_) {
            // LoadingScreen 0x755e30: the load runs at stage 5 (bar shown), then the bar fades and the screen goes black.
            if (flow_.state() == FlowState::Loading && bridge_.loading_stage() >= 5) flow_.run_pending(), load_progress_ = 1, ensure_view();
            if (bridge_.loading_stage() >= 8 || (flow_.state() != FlowState::Loading && flow_.state() != FlowState::Playing))
                loading_ = false;
        }
        if (flow_.state() == FlowState::Quit) break;
        JOA_FRAME_MARK();
    }
    if (headless || !o_.autoplay.empty())
        std::printf("frames=%d state=%s mission=%d ticks=%llu hash=%016llx\n", frame, flow_state_name(flow_.state()),
                    session_.mission() ? session_.mission()->scenario_no() : 0, static_cast<unsigned long long>(session_.ticks()),
                    static_cast<unsigned long long>(session_.mission() ? session_.mission()->hash() : 0));
    return 0;
}

}  // namespace

int run_game(const Options& o) { return Client(o).run(); }

}  // namespace joa::engine
