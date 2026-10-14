#pragma once
// Barracks recruit and siege workshop screens through a real backend (gl_/vk_recruit_workshop_test.cpp): mission 7
// (S7L1) "french recruit" and "workshop1" facilities, after the slide-in, with a few units / one cannon added (the
// screen copies, not committed). No reference shots exist for these windows: images only (viewed by hand).
#include "ui_shot.hpp"

#include "engine/hud_recruit.hpp"
#include "engine/session.hpp"

namespace recruitworkshop {
using namespace joa::engine;

inline int run(RendererKind kind) {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("scenario/scenario_07.scr")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: SDL video unavailable: %s\n", SDL_GetError());
        return 77;
    }
    const char* tag = kind == RendererKind::Vulkan ? "vk" : "gl";
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
        Session s(vfs);
        s.seed = 1;
        std::string err;
        if (!s.new_game(7, 1, &err)) {
            std::fprintf(stderr, "FAIL new_game: %s\n", err.c_str());
            return 1;
        }
        joa::game::Mission& m = *s.mission();
        m.autosave = false;
        for (int t = 0; t < 30; ++t) s.tick({}), s.take_world_events(), s.take_mission_events();   // stage 0: recruit lists
        m.state().gold = 5000;
        HudModel hud;
        FacilityWindow win;
        auto frames = [&](int n, glm::vec2 mouse, bool keep) {
            Image img;
            for (int i = 0; i < n; ++i) {
                std::vector<UiQuad> quads;
                UiInput in;
                in.mouse = mouse;
                ui.begin({0, 0, 1024, 768}, quads, in, 0.05f);
                win.draw(ui, m, hud);
                FrameData f;
                f.view.viewport = {0, 0, 1024, 768};
                f.ui = quads;
                r->begin_frame({1024, 768});
                r->submit(f);
                if (keep && i == n - 1) img = r->read_back();
                r->end_frame();
            }
            return img;
        };
        auto shot = [&](const char* name, glm::vec2 mouse) {
            const Image img = frames(3, mouse, true);
            write_png(uishot::out_path(std::string("recruitworkshop_") + tag + "_" + name + ".png"), img.width, img.height, img.rgba);
            CHECK(img.width == 1024 && img.height == 768);
        };
        hud.recruit_screen = m.places().facility_by_name("french recruit"), win.recruiter = m.world().player_id();
        CHECK(hud.recruit_screen > 0);
        frames(20, {1000, 760}, false);
        CHECK(win.open());
        const auto& u = m.data().units;
        win.add[0] = 2, win.add[4] = 1, win.citizens -= 3, win.recruited = 3, win.total += 3;
        win.cost = 2 * u[std::size_t(m.data().unit_index("FRENCH_LIGHT_INFANTRY"))].gold + u[std::size_t(m.data().unit_index("FRENCH_ARCHER"))].gold;
        shot("barracks", {300, 0x68 + 100 + 50});   // row 1 hovered: I_OV + the ia_ov panel out
        hud.recruit_screen = 0;
        win.reset();
        hud.workshop_screen = m.places().facility_by_name("workshop1");
        CHECK(hud.workshop_screen > 0);
        frames(20, {1000, 760}, false);
        CHECK(win.open() && win.workshop);
        win.add[0] = 1, win.cost = u[std::size_t(m.data().unit_index("CANNON"))].gold, win.total += 1;
        shot("workshop", {300, 0x68 + 50});
    }
    r.reset();
    platform.reset();
    SDL_Quit();
    return test_result();
}

}  // namespace recruitworkshop
