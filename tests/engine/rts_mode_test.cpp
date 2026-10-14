// RTS mode (GameCamera RTS input, Rts_process_input 0x440b30): box select, Ctrl+1 store / Alt+1 recall, click
// deselect, hover pick types and their commands (enemy -> RTS_ATTACK_UNIT, siege weapon -> LARGE_WEAPON_PUSH crew,
// floor item -> RTS_MOVE_TO its position, hostile wall -> RTS_ATTACK_ITEM), minimap right / left click, RTS cursor.
// RtsHero: acknowledgement voices (SOUND codes), F3 on the selection, second commander press / leaving RTS switch
// the hero, the "team over capacity" CHARMOVE|1 confirmation, groups in the native save ("VIEW", hash unchanged),
// and (mission 7) RMB on an own barracks / IRON-WOOD workshop opens the recruit / workshop screen for the leader.
#include "check.hpp"
#include "engine/assets_cache.hpp"
#include "engine/audio_system.hpp"
#include "engine/game_camera.hpp"
#include "engine/game_input.hpp"
#include "engine/hud_recruit.hpp"
#include "engine/session.hpp"
#include "game/mission.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace joa;
using namespace joa::engine;

namespace {

struct Rig {
    Session& s;
    GameCamera cam;
    GameInput in;
    glm::mat4 vp{1.0f};
    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) {
            game::InputFrame f;
            cam.before_tick(*s.mission(), f);
            s.tick(f);
            cam.after_tick(*s.mission(), in);
        }
    }
    void key(int action) {  // one tick with `action` pressed (commander keys, F3 / F4)
        game::InputFrame f;
        f.press(action);
        cam.before_tick(*s.mission(), f);
        s.tick(f);
        cam.after_tick(*s.mission(), in);
    }
    void look() {  // view-projection of the current camera pose
        vp = perspective_hor_plus(glm::radians(45.0f), 4.0f / 3.0f, 1.0f, 8193.0f) *
             glm::lookAtRH(cam.pose(1.0f).eye, cam.pose(1.0f).target, glm::vec3(0, 1, 0));
    }
    void frame(glm::vec2 at, bool left, bool right, std::initializer_list<SDL_Scancode> keys = {}) {
        RawInput raw;
        raw.mouse_buttons[SDL_BUTTON_LEFT] = left;
        raw.mouse_buttons[SDL_BUTTON_RIGHT] = right;
        for (SDL_Scancode k : keys) raw.keys[std::size_t(k)] = true;
        cam.rts_input(*s.mission(), raw, at, {0, 0, 1024, 768}, vp);
    }
    void click(glm::vec2 at) { frame(at, true, false), frame(at, false, false), tick(20); }  // 20 ticks: no double-click
    void rclick(glm::vec2 at) { frame(at, false, false), frame(at, false, true), frame(at, false, false); }
};

glm::vec2 to_px(const glm::mat4& vp, glm::vec3 o) {
    const glm::vec4 c = vp * glm::vec4(to_engine(o.x, o.y, o.z), 1.0f);
    return {(c.x / c.w * 0.5f + 0.5f) * 1024.0f, (0.5f - c.y / c.w * 0.5f) * 768.0f};
}

bool has(const std::vector<int>& v, int id) { return std::find(v.begin(), v.end(), id) != v.end(); }
game::Vec3 gv(glm::vec3 p) { return {p.x, p.y, p.z}; }

}  // namespace

int main() {
    const char* game_dir = std::getenv("JOA_GAME_DIR");
    if (!game_dir || !formats::Vfs(game_dir).exists("Resource/rts.cm")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    Session s{formats::Vfs(game_dir)};
    s.seed = 1;
    std::string err;
    if (!s.new_game(1, 1, &err)) return std::fprintf(stderr, "new_game: %s\n", err.c_str()), 1;
    game::Mission& m = *s.mission();
    m.autosave = false;
    game::World& w = m.world();
    game::AiSystem& ai = m.ai();
    Rig r{s};
    {  // face the map centre: toward the near map edge no RTS distance gives a valid view (camera stays at 187.5)
        game::Unit& j = *w.unit(w.player_id());
        w.set_yaw_now(j, std::atan2(8192.0f - j.pos.y, 8192.0f - j.pos.x) * 57.29578f);
    }
    r.cam.reset(m);
    s.rts_unlocked = true;
    CHECK(s.toggle_rts());
    r.tick(150);  // rts_play track done (rts_input ignores the mouse while a camera track plays)
    r.vp = perspective_hor_plus(glm::radians(45.0f), 4.0f / 3.0f, 1.0f, 8193.0f) *
           glm::lookAtRH(r.cam.pose(1.0f).eye, r.cam.pose(1.0f).target, glm::vec3(0, 1, 0));
    const game::Unit& joan = *w.unit(w.player_id());
    const game::Unit* jean = nullptr;
    for (const game::Unit& u : w.units())
        if (!u.deleted && w.data().units[std::size_t(u.cls)].code == "JEAN_DE_METZ") jean = &u;
    CHECK(jean != nullptr);
    if (!jean) return test_result();
    // Terrain point under a screen pixel: test units are placed where the RTS camera sees them (near the map edge the
    // camera stays at the follow distance with the pivot still easing down, so the heroes sit low on the screen).
    const auto ground_px = [&](glm::vec2 px) {
        const glm::vec2 ndc{px.x / 512.0f - 1.0f, 1.0f - px.y / 384.0f};
        const glm::mat4 inv = glm::inverse(r.vp);
        glm::vec4 a = inv * glm::vec4(ndc, -1.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
        a /= a.w, b /= b.w;
        const glm::vec3 o{a.x, -a.z, a.y}, d = glm::normalize(glm::vec3(b.x, -b.z, b.y) - o);
        glm::vec3 p = o;
        for (float t = 0.0f; t < 8000.0f && p.z > w.terrain()->height(p.x, p.y); t += 2.0f) p = o + d * t;
        return glm::vec3(p.x, p.y, w.terrain()->height(p.x, p.y));
    };
    const auto mid_px = [&](glm::vec3 feet) { return to_px(r.vp, feet + glm::vec3(0, 0, 40)); };
    const glm::vec3 base{joan.pos.x, joan.pos.y, joan.pos.z};

    // 1. Box select everything on screen, Ctrl+1 stores it, a click on empty ground deselects, Alt+1 recalls it.
    r.frame({12, 12}, true, false);
    r.frame({1012, 756}, true, false);
    CHECK(r.cam.drag_box().z > 0.0f);
    CHECK(has(r.cam.rts_selection(), jean->id));  // live box selection while dragging
    r.frame({1012, 756}, true, false);
    r.frame({1012, 756}, false, false);
    const std::vector<int> all = r.cam.rts_selection();
    std::printf("box selection %zu units\n", all.size());
    CHECK(all.size() >= 2 && has(all, jean->id));
    r.frame({600, 600}, false, false, {SDL_SCANCODE_LCTRL, SDL_SCANCODE_1});
    r.frame({600, 600}, false, false);
    CHECK(r.cam.rts_group(1) == all);
    r.tick(20);
    r.click({150, 150});  // empty ground
    CHECK(r.cam.rts_selection().empty());
    r.frame({600, 600}, false, false, {SDL_SCANCODE_LALT, SDL_SCANCODE_1});
    r.frame({600, 600}, false, false);
    CHECK(r.cam.rts_selection() == all);
    // Leaving RTS clears the working selection, the Ctrl groups stay for the next session (Rts_init resets group 0
    // only). The mission script's opening cut scene (game_cut_scene) blocks the F2 toggle, as in the original: the
    // test ends it first.
    std::printf("cut scene %d\n", int(m.ui().game_cut_scene));
    m.ui().game_cut_scene = false;
    CHECK(s.toggle_rts());
    r.tick(150);
    CHECK(r.cam.rts_selection().empty() && r.cam.rts_group(1) == all);
    m.ui().game_cut_scene = false;
    CHECK(s.toggle_rts());
    r.tick(150);
    r.vp = perspective_hor_plus(glm::radians(45.0f), 4.0f / 3.0f, 1.0f, 8193.0f) *
           glm::lookAtRH(r.cam.pose(1.0f).eye, r.cam.pose(1.0f).target, glm::vec3(0, 1, 0));
    CHECK(has(r.cam.rts_selection(), joan.id) && r.cam.rts_group(1) == all);
    const glm::vec2 joan_px = mid_px({joan.pos.x, joan.pos.y, joan.pos.z}), jean_px = mid_px({jean->pos.x, jean->pos.y, jean->pos.z});
    std::printf("joan at %.0f,%.0f  jean at %.0f,%.0f\n", joan_px.x, joan_px.y, jean_px.x, jean_px.y);

    // 2. RMB on an enemy: hover type 1 (attack cursor), RTS_ATTACK_UNIT (13) on the target for the selection.
    const int foe_nation = joan.nation == 1 ? 2 : 1;
    w.set_hostile(joan.nation, foe_nation, true), w.set_hostile(foe_nation, joan.nation, true);
    const glm::vec3 foe_at = ground_px({300, 600});  // away from Joan / Jean (printed above)
    const int foe = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", foe_nation, gv(foe_at), 180);
    CHECK(foe > 0);
    const glm::vec2 foe_px = mid_px(foe_at);
    r.frame(foe_px, false, false);
    std::printf("hover over enemy: type %d target %d cursor %s\n", r.cam.rts_hover_type(), r.cam.rts_hover_target(), r.cam.rts_cursor(0.0f).c_str());
    CHECK(r.cam.rts_hover_type() == 1 && r.cam.rts_hover_target() == foe);
    for (int i = 0; i < 30; ++i) r.cam.rts_cursor(0.06f);  // finish the click's POINT animation (50 ms frames)
    CHECK(r.cam.rts_cursor(0.0f).find("ATT00") != std::string::npos);
    r.rclick(foe_px);
    CHECK(ai.st(joan).task.id == game::task::RTS_ATTACK_UNIT && ai.st(joan).task.p1 == foe);  // Joan's team selected
    CHECK(r.cam.rts_cursor(0.0f).find("ATT") != std::string::npos);  // command animation family
    w.delete_unit(foe);

    // 3. Siege weapon: a selected French light infantry + RMB on an own cannon = crew (LARGE_WEAPON_PUSH 15), the
    //    crewman leaves the selection (Rts_cmd_assign 0x44f750 A).
    const glm::vec3 cannon_at = ground_px({220, 330}), inf_at = ground_px({520, 230});
    const int cannon = w.spawn_unit("CANNON", joan.nation, gv(cannon_at), 90);
    const int inf = w.spawn_unit("FRENCH_LIGHT_INFANTRY", joan.nation, gv(inf_at), 0);
    CHECK(cannon > 0 && inf > 0);
    r.click(mid_px(inf_at));
    std::printf("click selection: %zu units (first %d, infantry %d)\n", r.cam.rts_selection().size(),
                r.cam.rts_selection().empty() ? 0 : r.cam.rts_selection()[0], inf);
    CHECK(r.cam.rts_selection() == std::vector<int>{inf});
    const glm::vec2 cannon_px = to_px(r.vp, cannon_at + glm::vec3(0, 0, 30));
    r.frame(cannon_px, false, false);
    std::printf("hover over cannon: type %d target %d\n", r.cam.rts_hover_type(), r.cam.rts_hover_target());
    CHECK(r.cam.rts_hover_type() == 6 && r.cam.rts_hover_target() == cannon);
    r.rclick(cannon_px);
    CHECK(ai.st(*w.unit(inf)).task.id == game::task::LARGE_WEAPON_PUSH && ai.st(*w.unit(inf)).task.p1 == cannon);
    CHECK(r.cam.rts_selection().empty());

    // 4. Floor item (unit drop): RMB = RTS_MOVE_TO (12) its position for a single selected unit.
    const glm::vec3 inf2_at = ground_px({820, 230});
    const int inf2 = w.spawn_unit("FRENCH_LIGHT_INFANTRY", joan.nation, gv(inf2_at), 0);
    r.click(mid_px(inf2_at));
    CHECK(r.cam.rts_selection() == std::vector<int>{inf2});
    const glm::vec3 drop_at = ground_px({470, 480});
    const int drop = w.add_item(w.data().items[1].code, 1, 1, {drop_at.x, drop_at.y, drop_at.z});
    CHECK(drop > 0);
    const glm::vec2 drop_px = to_px(r.vp, drop_at + glm::vec3(0, 0, 10));
    r.frame(drop_px, false, false);
    CHECK(r.cam.rts_hover_type() == 4);
    r.rclick(drop_px);
    const game::Task& t4 = ai.st(*w.unit(inf2)).task;
    std::printf("drop order: task %d to %d,%d (item at %.0f,%.0f)\n", t4.id, t4.p1, t4.p2, drop_at.x, drop_at.y);
    CHECK(t4.id == game::task::RTS_MOVE_TO && std::abs(t4.p1 - int(drop_at.x)) <= 1 && std::abs(t4.p2 - int(drop_at.y)) <= 1);
    w.del_item(drop);

    // 5. Hostile wall (map item class 4, hp > 0): RMB = RTS_ATTACK_ITEM (17) on its recno. Kegs (0x12) are not in the
    //    Rts_pick_target item filter (classes 1 / 4 / 10 / 0x27 or a pickable floor marker), so they are not targets.
    int wall_res = 0;
    for (int id = 1; m.objects().res(id); ++id)
        if (m.objects().res(id)->cls == 4 && m.objects().res(id)->hp > 0) wall_res = id;
    CHECK(wall_res > 0);
    const int wall = m.objects().create(m, wall_res);
    CHECK(wall > 0);
    if (game::MapItem* it = m.objects().item(wall)) {
        const glm::vec3 wall_at = ground_px({700, 560});
        it->pos = {wall_at.x, wall_at.y, wall_at.z};
        it->nation = foe_nation;
        const glm::vec2 wall_px = to_px(r.vp, wall_at + glm::vec3(0, 0, 20));
        r.frame(wall_px, false, false);
        std::printf("hover over wall: type %d target %d (recno %d hp %d)\n", r.cam.rts_hover_type(), r.cam.rts_hover_target(), wall, it->hp);
        CHECK(r.cam.rts_hover_type() == 2 && r.cam.rts_hover_target() == wall);
        r.rclick(wall_px);
        CHECK(ai.st(*w.unit(inf2)).task.id == game::task::RTS_ATTACK_ITEM && ai.st(*w.unit(inf2)).task.p1 == wall);
        m.objects().del_item(m, wall);
    }

    // 6. Minimap: right click = move order (Rts_cmd_move), left click = camera jump (distance stepped -200 from the
    //    current one until the view is valid; at the minimum distance FUN_00717240 does nothing).
    const glm::vec2 to_centre = glm::normalize(glm::vec2(8192.0f) - glm::vec2(base));
    glm::vec3 mm = base + glm::vec3(to_centre * 600.0f, 0.0f);
    mm.z = w.terrain()->height(mm.x, mm.y);
    r.cam.rts_minimap(m, 1, {drop_at.x, drop_at.y});  // a destination on the path mesh (reached by the drop order)
    CHECK(ai.st(*w.unit(inf2)).task.id == game::task::RTS_MOVE_TO);
    const float dist0 = r.cam.state().dist;
    r.cam.rts_minimap(m, 0, {mm.x, mm.y});
    r.tick(2);
    std::printf("minimap jump: dist %.0f pivot %.0f,%.0f -> target %.0f,%.0f\n", dist0, r.cam.state().pivot.x, r.cam.state().pivot.y, mm.x, mm.y);
    CHECK(dist0 > GameCamera::kFollowDistance && glm::distance(glm::vec2(r.cam.state().pivot), glm::vec2(mm)) < 20.0f);

    // 7. Order execution (RtsFinish): the hero is AI-driven in RTS and the player controller no longer runs (left
    //    clicks used to start light attacks and froze Joan in RUN). Joan + Jean walk to a move order's slots.
    r.cam.rts_minimap(m, 0, {base.x, base.y});
    r.tick(2);
    r.vp = perspective_hor_plus(glm::radians(45.0f), 4.0f / 3.0f, 1.0f, 8193.0f) *
           glm::lookAtRH(r.cam.pose(1.0f).eye, r.cam.pose(1.0f).target, glm::vec3(0, 1, 0));
    r.frame({600, 600}, false, false, {SDL_SCANCODE_LALT, SDL_SCANCODE_1});
    r.frame({600, 600}, false, false);
    const glm::vec3 j0{joan.pos.x, joan.pos.y, joan.pos.z}, n0{jean->pos.x, jean->pos.y, jean->pos.z};
    glm::vec3 dest = drop_at;  // on the path mesh (reached by the drop order); farther points tried first
    bool moved = false;
    for (float k : {3.0f, 2.0f, 1.0f})
        if (!moved) {
            dest = j0 + (drop_at - j0) * k;
            dest.z = w.terrain()->height(dest.x, dest.y);
            moved = ai.rts_move(r.cam.rts_selection(), gv(dest), false);
        }
    r.tick(150);
    const float dj = glm::distance(glm::vec2(joan.pos.x, joan.pos.y), glm::vec2(j0)), dn = glm::distance(glm::vec2(jean->pos.x, jean->pos.y), glm::vec2(n0));
    std::printf("move order %d: joan walked %.0f (action %d), jean walked %.0f\n", int(moved), dj, joan.action, dn);
    CHECK(moved && dj > 50.0f && dn > 50.0f);
    CHECK(joan.action != 28);  // no ATTACK from the RTS left clicks

    // 8. RtsHero: voices (every emitted code exists in SOUND), F3 on the selection, second commander press switches
    //    the hero, leaving RTS switches to the last chosen commander, over-capacity popup, groups in the save.
    SoundBank bank;
    CHECK(bank.load(s.vfs()));
    const auto said = [&] {
        std::vector<std::string> v;
        for (const auto& e : m.take_events()) if (e.kind == game::MissionEvent::Kind::Sound) v.push_back(e.text);
        for (const auto& e : s.take_mission_events()) if (e.kind == game::MissionEvent::Kind::Sound) v.push_back(e.text);
        for (const std::string& c : v) {
            std::printf("voice %s\n", c.c_str());
            CHECK(c.rfind("RTS", 0) != 0 || bank.find(c) >= 0);
        }
        return v;
    };
    said();
    r.key(25);  // Joan: select (002)
    CHECK(r.cam.rts_voice() == "RTS_JOAN_002" && has(r.cam.rts_selection(), joan.id));
    r.frame({600, 600}, false, false, {SDL_SCANCODE_LCTRL, SDL_SCANCODE_2}), r.frame({600, 600}, false, false);
    CHECK(r.cam.rts_voice() == "RTS_NARRATOR_001" && !r.cam.rts_group(2).empty());
    r.frame({600, 600}, false, false, {SDL_SCANCODE_LALT, SDL_SCANCODE_2}), r.frame({600, 600}, false, false);
    CHECK(r.cam.rts_voice().rfind("RTS_LTINF_00", 0) == 0);
    r.key(25), r.key(25);  // second press (001; Joan already plays)
    CHECK(r.cam.rts_voice() == "RTS_JOAN_001" && w.player_id() == joan.id);
    r.rclick(mid_px(ground_px({512, 500})));
    std::printf("move voice %s\n", r.cam.rts_voice().c_str());
    said();
    {  // F3: Ctrl_use_hp_item on every selected rank > 3 unit
        game::Unit& jm = *w.unit(joan.id);
        int potion = 0;
        for (std::size_t i = 1; i < w.data().items.size() && !potion; ++i)
            if (w.data().items[i].type == 0x15) potion = int(i);
        CHECK(potion > 0 && w.give_item(jm, w.data().items[std::size_t(potion)].code, 0, 1));
        jm.hp = std::max(1, w.max_hp(jm) / 3);
        const int hp0 = jm.hp;
        r.key(19);
        std::printf("F3: joan hp %d -> %d\n", hp0, jm.hp);
        CHECK(jm.hp > hp0);
    }
    r.key(26), r.key(26);  // Jean twice: switch (mode 4: player swap)
    std::printf("second press: player %d voice %s\n", w.player_id(), r.cam.rts_voice().c_str());
    CHECK(w.player_id() == jean->id && r.cam.rts_voice() == "RTS_METZ_001");
    r.key(25), r.key(25);
    CHECK(w.player_id() == joan.id);
    r.key(26);  // Jean chosen once, then F2: Rts_deinit switches to him
    s.on_rts_exit = [&](game::Mission& mm) { r.cam.rts_deinit(mm); };
    m.ui().game_cut_scene = false;
    CHECK(s.toggle_rts());
    r.tick(5);
    std::printf("leave RTS: player %d jean %d ai %d\n", w.player_id(), jean->id, int(jean->ai_controlled));
    CHECK(w.player_id() == jean->id && !jean->ai_controlled && joan.ai_controlled);
    r.cam.rts_unlocked = true;
    r.key(25);  // action mode: Switch to Joan
    CHECK(w.player_id() == joan.id);
    CHECK(s.toggle_rts());
    r.tick(150);
    {  // over capacity: Jean's full troop + one more infantry -> popup; OK joins it (one member leaves)
        game::Unit& jn = *w.unit(jean->id);
        r.cam.rts_minimap(m, 0, {jn.pos.x, jn.pos.y});
        r.tick(3), r.look();
        if (!ai.is_leader(jn)) ai.create_team(jn);
        const int cap = ai.max_troop_size(jn);
        while (int(jn.members.size()) < cap) {
            const int id = w.spawn_unit("FRENCH_LIGHT_INFANTRY", jn.nation, {jn.pos.x + 900.0f, jn.pos.y, jn.pos.z}, 0);
            if (!id) break;
            ai.join_team(*w.unit(id), jn.id);
        }
        const glm::vec3 ex_at = ground_px({300, 600});
        const int extra = w.spawn_unit("FRENCH_LIGHT_INFANTRY", jn.nation, gv(ex_at), 0);
        r.click(mid_px(ex_at));
        CHECK(r.cam.rts_selection() == std::vector<int>{extra});
        const glm::vec2 jpx = mid_px({jn.pos.x, jn.pos.y, jn.pos.z});
        r.frame(jpx, false, false);
        std::printf("hover jean: type %d target %d (cap %d, members %zu)\n", r.cam.rts_hover_type(), r.cam.rts_hover_target(), cap, jn.members.size());
        r.rclick(jpx);
        CHECK(r.cam.rts_join_target() == jn.id && r.cam.rts_join_args().size() == 5);
        if (r.cam.rts_join_args().size() == 5)
            std::printf("popup args %s %s %s %s %s\n", r.cam.rts_join_args()[0].c_str(), r.cam.rts_join_args()[1].c_str(),
                        r.cam.rts_join_args()[2].c_str(), r.cam.rts_join_args()[3].c_str(), r.cam.rts_join_args()[4].c_str());
        CHECK(w.unit(extra)->leader != jn.id);  // nothing joined before the answer
        r.cam.rts_join_answer(m, true);
        std::printf("joined: leader %d members %zu voice %s\n", w.unit(extra)->leader, jn.members.size(), r.cam.rts_voice().c_str());
        CHECK(w.unit(extra)->leader == jn.id && int(jn.members.size()) == cap && r.cam.rts_voice() == "RTS_METZ_012");
    }
    said();
    {  // groups in the native save (section VIEW, Mission::hash unchanged)
        r.frame({600, 600}, false, false, {SDL_SCANCODE_LCTRL, SDL_SCANCODE_3}), r.frame({600, 600}, false, false);
        const std::vector<int> g3 = r.cam.rts_group(3), g1 = r.cam.rts_group(1);
        const char* out = std::getenv("JOA_TEST_OUT");
        const std::filesystem::path path = std::filesystem::path(out ? out : ".") / "rtshero_view.jsav";
        s.save_view = [&] { return r.cam.save_view(); };
        const std::uint64_t h0 = m.hash();
        CHECK(s.save(path, "rts groups", &err));
        CHECK(s.load(path, &err));
        GameCamera c2;
        c2.reset(*s.mission());
        c2.load_view(s.view_section);
        std::printf("save: view %zu bytes, group 3 %zu units, hash %d\n", s.view_section.size(), c2.rts_group(3).size(), int(s.mission()->hash() == h0));
        CHECK(!g3.empty() && c2.rts_group(3) == g3 && c2.rts_group(1) == g1 && s.mission()->hash() == h0);
    }

    // 9. Mission 7: RMB on the own barracks / Alencon's IRON-WOOD workshop -> Interact screens (FacilityWindow).
    if (!s.new_game(7, 1, &err)) return std::fprintf(stderr, "new_game 7: %s\n", err.c_str()), 1;
    game::Mission& m7 = *s.mission();
    m7.autosave = false;
    Rig r7{s};
    for (int t = 0; t < 30; ++t) r7.tick();
    m7.ui().cs.active = false, m7.ui().game_cut_scene = false;
    r7.cam.reset(m7);
    game::World& w7 = m7.world();
    CHECK(s.toggle_rts());
    r7.tick(150);
    const auto building = [&](const char* hero, int key, int fac, const char* screen) {
        game::Unit* h = nullptr;
        for (game::Unit& u : w7.units())
            if (!u.deleted && w7.data().units[std::size_t(u.cls)].code == hero) h = &u;
        const game::Facility* fc = m7.places().facility(fac);
        const game::Place* pl = fc ? m7.places().place(fc->place) : nullptr;
        CHECK(h && pl);
        if (!h || !pl) return;
        if (!m7.ai().is_leader(*h)) m7.ai().create_team(*h);
        h->pos = {pl->pos.x, pl->pos.y, w7.terrain()->height(pl->pos.x, pl->pos.y)};
        r7.key(key);
        const glm::vec3 at{(fc->box_min.x + fc->box_max.x) * 0.5f, (fc->box_min.y + fc->box_max.y) * 0.5f, 0.0f};
        r7.cam.rts_minimap(m7, 0, {at.x + 300.0f, at.y});
        r7.tick(3), r7.look();
        const glm::vec2 px = to_px(r7.vp, {at.x, at.y, w7.terrain()->height(at.x, at.y)});
        r7.frame(px, false, false);
        const std::string cur = r7.cam.rts_cursor(0.0f);
        std::printf("%s: facility %d box z %.0f..%.0f, hover type %d target %d cursor %s\n", screen, fac, fc->box_min.z, fc->box_max.z,
                    r7.cam.rts_hover_type(), r7.cam.rts_hover_target(), cur.c_str());
        CHECK(r7.cam.rts_hover_type() == 10 && r7.cam.rts_hover_target() == fac);
        m7.take_events(), s.take_mission_events();
        r7.rclick(px);
        FacilityWindow win;
        HudModel hud;
        win.on_mission_events(m7.take_events(), m7, hud);
        std::printf("  -> recruit %d workshop %d recruiter %d (%s %d)\n", hud.recruit_screen, hud.workshop_screen, win.recruiter, hero, h->id);
        CHECK((std::string(screen) == "recruit_screen" ? hud.recruit_screen : hud.workshop_screen) == fac);
    };
    int iron = 0;
    for (int i = 1; m7.places().facility(i); ++i)
        if (m7.places().facility(i)->kind == game::Facility::Workshop && m7.places().facility(i)->workshop_type != 0 && !iron) iron = i;
    building("JOAN_OF_ARC", 25, m7.places().facility_by_name("french recruit"), "recruit_screen");
    building("ALENCON", 28, iron, "workshop_screen");

    return test_result();
}
