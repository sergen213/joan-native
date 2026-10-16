#pragma once
// Shared by gl_equip_visuals / vk_equip_visuals: mission 1 rendered through WorldView with a real backend
// (hidden window, 640x360), equipment changed through the game layer between captures:
//   armour LV1 (joan_armorC) vs LV3 (joan_armorB) and Warbrand vs Flamberge differ in Joan's region,
//   an arrow in flight shows against the same view without it, a floor item shows and is gone after pickup,
//   a broken box shows its "break" clip instead of the intact crate and is gone afterwards.
// Captures land in $JOA_TEST_OUT (equip_<tag>_*.png). Exit 77 without the original data / driver.
#include "check.hpp"
#include "engine/camera.hpp"
#include "engine/platform.hpp"
#include "engine/png.hpp"
#include "engine/renderer_gl.hpp"
#include "engine/renderer_vk.hpp"
#include "engine/session.hpp"
#include "engine/world_view.hpp"

#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

namespace equipshot {
using namespace joa;
using namespace joa::engine;

// Pixels in [x0,x1)x[y0,y1) whose summed RGB difference exceeds 60.
inline int diff_px(const Image& a, const Image& b, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(a.width) + std::size_t(x)) * 4;
            n += std::abs(a.rgba[i] - b.rgba[i]) + std::abs(a.rgba[i + 1] - b.rgba[i + 1]) + std::abs(a.rgba[i + 2] - b.rgba[i + 2]) > 60;
        }
    return n;
}
// Mean position of the pixels diff_px counts over the whole image (x, y, count).
inline glm::vec3 diff_centroid(const Image& a, const Image& b) {
    glm::vec3 s{0.0f};
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x)
            if (diff_px(a, b, x, y, x + 1, y + 1)) s += glm::vec3(float(x), float(y), 1.0f);
    return s.z > 0 ? glm::vec3(s.x / s.z, s.y / s.z, s.z) : s;
}

inline int run(RendererKind kind, const char* tag) {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !formats::Vfs(game).exists("map/scenario1/S1L1.gri")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: no video driver (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();
    const char* out_dir = std::getenv("JOA_TEST_OUT");
    const formats::Vfs vfs(game);
    Options o;
    o.width = 640;
    o.height = 360;
    o.game_dir = game;
    o.renderer = kind;
    o.vk_validation = kind == RendererKind::Vulkan;
    Platform platform(o, true);
    std::unique_ptr<IRenderer> r;
    try {
        r = kind == RendererKind::Vulkan ? create_vk_renderer(platform.window(), o) : create_gl_renderer(platform.window());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "SKIP: renderer unavailable (%s)\n", e.what());
        return 77;
    }
    {
        Session s(vfs);
        s.seed = 7;
        std::string err;
        CHECK(s.new_game(1, 1, &err));
        game::Mission& m = *s.mission();
        m.autosave = false;
        game::World& w = m.world();
        WorldView view(*r, vfs, m);
        const auto tick = [&](int n) {
            for (int i = 0; i < n; ++i) {
                if (m.ui().cut_scene_status()) m.ui().skip_cut_scene(m);
                s.tick({});
                view.sync(w, s.previous(), s.current());
                view.on_events(s.take_world_events(), s.current());
                s.take_mission_events();
            }
        };
        const auto render = [&](const CameraPose& pose, const char* name) {
            Image img;
            for (int i = 0; i < 3; ++i) {  // a few frames: uploads settle, the last one is read back
                const glm::ivec2 size = platform.pixel_size();
                FrameData f;
                f.view.viewport = fit_viewport(size, o.aspect);
                f.view.projection = perspective_hor_plus(glm::radians(kOriginalVerticalFovDeg), float(f.view.viewport.z) / float(f.view.viewport.w),
                                                         kDefaultNear, kDefaultFar);
                std::vector<DrawItem> draws;
                view.frame(1.0f, pose, 0.0f, f.view, draws);
                f.lighting = view.lighting();
                f.draws = draws;
                r->begin_frame(size);
                r->submit(f);
                if (i == 2) img = r->read_back();
                r->end_frame();
            }
            const std::string path = std::string(out_dir ? out_dir : ".") + "/equip_" + tag + "_" + name + ".png";
            if (write_png(path, img.width, img.height, img.rgba)) std::fprintf(stderr, "wrote %s\n", path.c_str());
            return img;
        };
        tick(30);
        game::Unit& joan = *w.unit(w.player_id());
        // Joan from the front-right, 130 units away, framed at mid-body.
        const auto at_joan = [&] {
            CameraPose p;
            const float y = joan.yaw_rad() + 0.6f;
            p.target = to_engine(joan.pos.x, joan.pos.y, joan.pos.z + 40.0f);
            p.eye = to_engine(joan.pos.x + std::cos(y) * 130.0f, joan.pos.y + std::sin(y) * 130.0f, joan.pos.z + 60.0f);
            return p;
        };
        const auto bag = [&](const char* code) {
            for (std::size_t i = 0; i < joan.inv.bag.size(); ++i)
                if (w.data().items[std::size_t(joan.inv.bag[i].item)].code == code) return int(i);
            return -1;
        };
        const CameraPose front = at_joan();
        const Image lv1 = render(front, "armour_lv1");
        CHECK(w.give_item(joan, "JOAN_ARMOR_LV3", 0, 1) && w.use_item(joan, bag("JOAN_ARMOR_LV3")));
        tick(1);
        const Image lv3 = render(front, "armour_lv3");
        CHECK(w.give_item(joan, "JOAN_SWORD_LV2", 0, 1) && w.equip(joan, bag("JOAN_SWORD_LV2")));
        tick(1);
        const Image sword = render(front, "flamberge");
        const int armour_d = diff_px(lv1, lv3, 220, 40, 420, 360), sword_d = diff_px(lv3, sword, 160, 40, 480, 360);
        std::fprintf(stderr, "joan region: armour LV1 vs LV3 %d px, Warbrand vs Flamberge %d px\n", armour_d, sword_d);
        CHECK(armour_d > 2000);
        CHECK(sword_d > 100);

        // Arrow in flight: seen from the side 150 units off its path, against the same view once it is gone.
        joan.inv.ammo = {w.data().item_index("ARROW"), 10, 1};
        joan.aim_dir = {std::cos(joan.yaw_rad()), std::sin(joan.yaw_rad()), 0.0f};
        CHECK(w.spawn_projectile(joan) > 0);
        tick(3);
        CHECK(!s.current().projectiles.empty());
        if (g_failures) return test_result();
        const game::Vec3 a = s.current().projectiles.front().pos;
        CameraPose side;
        const float sy = joan.yaw_rad() + 1.5707964f;
        side.target = to_engine(a.x, a.y, a.z);
        side.eye = to_engine(a.x + std::cos(sy) * 150.0f, a.y + std::sin(sy) * 150.0f, a.z + 20.0f);
        const Image arrow = render(side, "arrow");
        CHECK(view.item_stats().shots == 1);
        // The same arrow from behind (110 units back along its heading, 12 up): fletching near, head ahead.
        const game::Vec3 v = s.current().projectiles.front().vel;
        const float vh = std::max(std::hypot(v.x, v.y), 1e-3f);
        CameraPose behind;
        behind.target = to_engine(a.x + v.x / vh * 80.0f, a.y + v.y / vh * 80.0f, a.z);
        behind.eye = to_engine(a.x - v.x / vh * 110.0f, a.y - v.y / vh * 110.0f, a.z + 12.0f);
        render(behind, "arrow_behind");
        tick(90);
        const Image no_arrow = render(side, "arrow_gone");
        const int arrow_d = diff_px(arrow, no_arrow, 220, 130, 420, 230);
        std::fprintf(stderr, "arrow: %d px differ around the view centre\n", arrow_d);
        CHECK(arrow_d > 30);

        // Floor item 120 units ahead, then walked over (moved under Joan): picked up, gone from the view.
        const game::Vec3 ahead{joan.pos.x + std::cos(joan.yaw_rad()) * 120.0f, joan.pos.y + std::sin(joan.yaw_rad()) * 120.0f, joan.pos.z};
        const int drop = w.add_item("SMALLRED", 0, 1, ahead);
        tick(1);
        CameraPose look;
        look.target = to_engine(ahead.x, ahead.y, ahead.z + 8.0f);
        look.eye = to_engine(ahead.x + std::cos(sy) * 70.0f, ahead.y + std::sin(sy) * 70.0f, ahead.z + 35.0f);
        const Image item = render(look, "floor_item");
        CHECK(view.item_stats().drops == 1);
        w.item(drop)->pos = joan.pos;
        tick(1);
        CHECK(!w.item(drop));
        const Image picked = render(look, "floor_item_picked");
        CHECK(view.item_stats().drops == 0);
        const int item_d = diff_px(item, picked, 260, 120, 380, 260);
        std::fprintf(stderr, "floor item: %d px differ around the view centre\n", item_d);
        CHECK(item_d > 100);

        // A breakable box (MapObjects box, Treasure set) broken by an ally's arrow: intact, mid "break" clip, gone
        // (its bread lying where it stood).
        game::MapObjects& o = m.objects();
        int box = 0;
        for (int r2 = 1; r2 <= o.size() && !box; ++r2)
            if (const game::MapItem* it = o.item(r2); it && it->cls == 10 && it->content && it->modify && !o.res(it->res)->open_with_interact)
                box = r2;
        CHECK(box > 0);
        if (g_failures) return test_result();
        const game::Vec3 bp = o.item(box)->pos;
        CameraPose at_box, above;
        at_box.target = above.target = to_engine(bp.x, bp.y, bp.z + 15.0f);
        at_box.eye = to_engine(bp.x + 90.0f, bp.y + 40.0f, bp.z + 55.0f);
        above.eye = to_engine(bp.x + 10.0f, bp.y, bp.z + 260.0f);  // (near) straight down: height hides no x/y offset
        const Image intact = render(at_box, "box");
        const Image intact_top = render(above, "box_top");
        game::Trigger t;
        t.type = 2, t.target_kind = 2, t.target_id = o.item(box)->modify, t.src_kind = 1, t.sender = joan.id;
        o.on_trigger(m, t);
        tick(5);
        CHECK(o.item(box) && o.item(box)->stage == 0);
        const Image breaking = render(at_box, "box_breaking");
        for (int i = 0; i < 100 && o.item(box) && o.item(box)->cls == 10; ++i) tick(1);
        const Image gone = render(at_box, "box_gone");
        const int break_d = diff_px(intact, breaking, 200, 100, 440, 300), gone_d = diff_px(intact, gone, 200, 100, 440, 300);
        std::fprintf(stderr, "box: intact vs breaking %d px, intact vs gone %d px\n", break_d, gone_d);
        CHECK(break_d > 2000 && gone_d > 2000);  // 640x360 GL / RADV: 15100 / 13418
        // The spilled content is drawn where the box stood (0x48a1f0: ItemArray_create + FUN_0041ff50(box +0x56..+0x5e)),
        // seen from above: its pixels centre on the crate's. Picked up, both views return to the bare ground.
        int spill = 0;
        for (int r2 = 1; r2 <= o.size() && !spill; ++r2)
            if (const game::MapItem* it = o.item(r2); it && it->on_floor && it->pos.x == bp.x && it->pos.y == bp.y && it->pos.z == bp.z) spill = r2;
        CHECK(spill > 0);
        if (g_failures) return test_result();
        const Image spill_top = render(above, "box_spill_top");
        o.item(spill)->pos = joan.pos;
        tick(1);
        CHECK(!o.item(spill));
        const Image empty_top = render(above, "box_empty_top");
        const glm::vec3 crate = diff_centroid(intact_top, empty_top), food = diff_centroid(spill_top, empty_top);
        std::fprintf(stderr, "box from above: crate centre (%.1f, %.1f) %d px, spilled item centre (%.1f, %.1f) %d px\n", crate.x, crate.y,
                     int(crate.z), food.x, food.y, int(food.z));
        CHECK(crate.z > 500 && food.z > 50 && glm::distance(glm::vec2(crate), glm::vec2(food)) < 8.0f);
    }
    return test_result();
}

}  // namespace equipshot
