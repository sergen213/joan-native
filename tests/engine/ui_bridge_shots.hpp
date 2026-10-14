#pragma once
// GameUiBridge overlays through a real backend (gl_/vk_ui_bridge_test.cpp): loading screen stages, cut-scene
// letterbox + dialog box (mission 1 tutorial), game-over box, win message and game-end menu. Masked SSIM against
// the original's shots where one exists (world-free areas only: the 3D scene is not rendered here).
#include "ui_shot.hpp"

#include "engine/game_ui_bridge.hpp"
#include "engine/session.hpp"

#include <functional>

namespace uibridge {
using namespace joa::engine;
using uishot::Rect;

struct Shot {
    const char* name;
    const char* reference;  // work/reference/shots file, "" = none
    std::vector<Rect> include;
    double min_ssim;
};

inline int run(RendererKind kind) {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    const char* tag = kind == RendererKind::Vulkan ? "vk" : "gl";
    const std::string shots = std::string(game) + "/work/reference/shots/";
    Options o;
    o.renderer = kind;
    o.width = 1024;
    o.height = 768;
    o.game_dir = game;
    o.vk_validation = kind == RendererKind::Vulkan;
    std::unique_ptr<Platform> platform;
    std::unique_ptr<IRenderer> r;
    try {
        platform = std::make_unique<Platform>(o, true);
        r = kind == RendererKind::Vulkan ? create_vk_renderer(platform->window(), o) : create_gl_renderer(platform->window());
    } catch (const std::exception& e) {
        if (kind == RendererKind::Vulkan && uishot::vulkan_missing(e)) {
            std::fprintf(stderr, "SKIP: %s\n", e.what());
            return 77;
        }
        throw;
    }
    {
        joa::formats::Vfs vfs(game);
        Ui ui(vfs, *r);
        GameUiBridge b(vfs);
        HudModel hud;
        Session s(vfs);
        s.seed = 12345;
        std::string err;
        if (!s.new_game(1, 1, &err)) {
            std::fprintf(stderr, "FAIL new_game: %s\n", err.c_str());
            return 1;
        }
        joa::game::Mission& m = *s.mission();
        m.autosave = false;
        b.auto_advance = true;
        b.reset(m, hud);

        // Draws `fn` for each dt in `steps` (the last frame is read back) and scores it.
        auto shot = [&](const Shot& c, const std::vector<float>& steps, const std::function<void(float)>& fn) {
            const glm::ivec4 vp{0, 0, 1024, 768};
            Image img;
            for (std::size_t i = 0; i < steps.size(); ++i) {
                std::vector<UiQuad> quads;
                ui.begin(vp, quads, {}, steps[i]);
                fn(steps[i]);
                FrameData f;
                f.view.viewport = vp;
                f.ui = quads;
                r->begin_frame({1024, 768});
                r->submit(f);
                if (i + 1 == steps.size()) img = r->read_back();
                r->end_frame();
            }
            write_png(uishot::out_path(std::string("ui_bridge_") + tag + "_" + c.name + ".png"), img.width, img.height, img.rgba);
            CHECK(img.width == 1024 && img.height == 768);
            if (!*c.reference || img.width != 1024) return;
            uishot::Gray ref;
            if (!uishot::load_reference(shots + c.reference, ref)) {
                std::fprintf(stderr, "SKIP compare %s: reference %s unavailable\n", c.name, c.reference);
                return;
            }
            const double v = uishot::mean_ssim(uishot::to_gray(img.rgba.data(), 1024, 768, 1024), ref, c.include);
            std::fprintf(stderr, "%s %-10s ssim = %.3f (min %.2f)\n", tag, c.name, v, c.min_ssim);
            CHECK(v >= c.min_ssim);
        };
        auto repeat = [](float dt, int n, float last) {
            std::vector<float> v(std::size_t(n), dt);
            v.push_back(last);
            return v;
        };

        // Loading: stage 2 (title, name fading in) and stage 5 with the bar at 45 %.
        shot({"loading_7s", "s04_04_loading_7s.png", {}, 0.80}, repeat(0.1f, 44, 0.05f),
             [&](float) { b.draw_loading(ui, 1, 0.0f); });
        CHECK(b.loading_stage() == 2);
        GameUiBridge fresh(vfs);
        shot({"loading_bar", "s04_04_mission1_cutscene.png", {}, 0.70}, repeat(0.1f, 82, 0.05f),
             [&](float) { fresh.draw_loading(ui, 1, 0.45f); });
        CHECK(fresh.loading_stage() == 5);

        // Mission 1 tutorial cut scene: Jean de Metz's first line, typed up to the reference's "protective o".
        const std::string line = "We must make haste";
        const joa::game::MissionUi::DialogBox* d = nullptr;
        for (int t = 0; t < 20000 && !d; ++t) {
            s.tick({});
            s.take_world_events();
            b.on_events(s.take_mission_events(), m, hud);
            for (const auto& x : m.ui().dialogs)
                if (!x.deleted && x.text.find(line) != std::string::npos && m.ui().cs.status == 2 && m.ui().cs.fade == 255.0f) d = &x;
        }
        CHECK(d != nullptr);
        if (d) {
            const std::size_t n = d->text.find("protective o");
            const float chars = n == std::string::npos ? float(d->text.size()) : float(n + 12);
            m.ui().auto_advance = false;  // keep the box up while drawing
            shot({"dialog", "s04_04_mission1_tutorial_msg.png", {{0, 0, 1024, 88}, {0, 680, 1024, 88}}, 0.70},
                 {0.0f, chars / 40.0f - 0.001f}, [&](float) {
                     std::vector<UiCommand> out;
                     b.draw(ui, m, hud, out);
                 });
            m.ui().auto_advance = true;
        }

        // Game-over box with the mission 1 text (s08_end): GAME OVER art and the three buttons.
        std::vector<UiCommand> out;
        m.ui().del_all_dialogs(m);
        m.ui().cs.active = false;
        m.ui().cs.status = 0;
        m.ui().title.active = false;
        m.state().game_over = 1;
        m.state().game_over_text = "You have perished in your efforts to reach the city of Orl\xe9" "ans - now all hope is lost!";
        // Scored on the opaque MBUT buttons only (GAME OVER art and the text are see-through over the missing world).
        shot({"gameover", "s08_end.png", {{412, 564, 200, 24}, {412, 604, 200, 24}, {412, 644, 200, 24}}, 0.60},
             repeat(0.05f, 40, 0.05f),
             [&](float) { b.draw(ui, m, hud, out); });
        m.state().game_over = 0;

        // Win message (settled, "Next Mission") and the game-end menu it opens.
        m.scenario().game_finish = 1;
        shot({"win", "", {}, 0}, repeat(0.05f, 40, 0.05f), [&](float) { b.draw(ui, m, hud, out); });
        {
            std::vector<UiQuad> q;
            UiInput in;
            in.mouse = {512, 706};
            in.mouse_released = true;
            ui.begin({0, 0, 1024, 768}, q, in, 0.05f);
            b.draw(ui, m, hud, out);
        }
        shot({"endmenu", "", {}, 0}, {0.05f}, [&](float) { b.draw(ui, m, hud, out); });
        m.scenario().game_finish = 0;
    }
    r.reset();
    platform.reset();
    SDL_Quit();
    return test_result();
}

}  // namespace uibridge
