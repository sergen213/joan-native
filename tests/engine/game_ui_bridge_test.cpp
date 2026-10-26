// GameUiBridge headless (NullRenderer + Session mission 1): HUD model from the live mission, tutorial attention
// messages, party hold toggle, the forced-win screen emitting ContinueCampaign on click, and list_saves.
#include "check.hpp"
#include "engine/assets_cache.hpp"
#include "engine/game_ui_bridge.hpp"
#include "engine/renderer_null.hpp"
#include "engine/session.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace joa::engine;

namespace {
constexpr glm::ivec4 kViewport{0, 0, 1024, 768};

// Camera 400 units behind the player, looking at the head (enough for the name-bar projection).
glm::mat4 view_proj(const joa::game::Mission& m) {
    const joa::game::Unit* p = const_cast<joa::game::Mission&>(m).world().unit(const_cast<joa::game::Mission&>(m).world().player_id());
    const float a = p ? p->yaw_rad() : 0.0f;
    const glm::vec3 t = p ? to_engine(p->pos.x, p->pos.y, p->pos.z + 80) : glm::vec3(0);
    const glm::vec3 eye = p ? to_engine(p->pos.x - 400 * std::cos(a), p->pos.y - 400 * std::sin(a), p->pos.z + 200) : glm::vec3(1);
    return glm::perspective(glm::radians(45.0f), 4.0f / 3.0f, 10.0f, 20000.0f) * glm::lookAt(eye, t, {0, 1, 0});
}

// One UI frame of the bridge overlays with an optional click (released) at design point `click`.
std::vector<UiCommand> frame(Ui& ui, GameUiBridge& b, joa::game::Mission& m, HudModel& hud, float dt, glm::vec2 click = {-1, -1}) {
    std::vector<UiQuad> quads;
    UiInput in;
    in.mouse = click;
    in.mouse_released = click.x >= 0;
    ui.begin(kViewport, quads, in, dt);
    std::vector<UiCommand> out;
    b.draw(ui, m, hud, out);
    return out;
}
}  // namespace

int main() {
    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !joa::formats::Vfs(game).exists("Resource/INTERFAC.SET")) {
        std::fprintf(stderr, "SKIP: no original data\n");
        return 77;
    }
    // Native saves go to a scratch XDG_DATA_HOME, never the user's.
    const char* out_dir = std::getenv("JOA_TEST_OUT");
    const std::filesystem::path data = std::filesystem::path(out_dir ? out_dir : ".") / "ui_bridge_data";
    std::filesystem::remove_all(data);
    setenv("XDG_DATA_HOME", data.c_str(), 1);

    joa::formats::Vfs vfs(game);
    Session s(vfs);
    s.seed = 12345;
    std::string err;
    if (!s.new_game(1, 1, &err)) {
        std::fprintf(stderr, "new_game: %s\n", err.c_str());
        return 1;
    }
    joa::game::Mission& m = *s.mission();
    m.autosave = false;
    NullRenderer r;
    Ui ui(vfs, r);
    GameUiBridge b(vfs);
    b.auto_advance = true;  // headless: scripted dialogs / interfaces answer themselves
    HudModel hud;
    b.reset(m, hud);
    CHECK(m.ui().auto_advance);
    b.on_events(s.take_mission_events(), m, hud);

    bool message_seen = false, name_bar_seen = false;
    for (int t = 0; t < 300; ++t) {
        s.tick({});
        s.take_world_events();
        b.on_events(s.take_mission_events(), m, hud);
        b.fill(m, s.current(), view_proj(m), kViewport, hud);
        message_seen |= !hud.messages.empty() || !hud.message_log.empty();
        name_bar_seen |= std::any_of(hud.name_bars.begin(), hud.name_bars.end(), [](const HudUnit& u) { return u.on_screen; });
        frame(ui, b, m, hud, 0.033f);
    }
    std::fprintf(stderr, "player %s %d/%d portrait %s, party %zu, messages %zu log %zu, objectives %zu, icons %zu\n",
                 hud.player.name.c_str(), hud.player.hp_value, hud.player.hp_max, hud.player.portrait.c_str(), hud.party.size(),
                 hud.messages.size(), hud.message_log.size(), hud.objective_list.size(), hud.map_icons.size());
    CHECK(hud.player.name == "Joan of Arc");
    CHECK(hud.player.hp_value == 60 && hud.player.hp_max == 60);
    CHECK(hud.player.portrait == "JOAN1");
    CHECK(hud.mission == 1 && hud.scenario == "S1L1");
    int metz = -1;
    for (std::size_t i = 0; i < hud.party.size(); ++i)
        if (hud.party[i].name == "Jean de Metz") metz = int(i);
    CHECK(metz >= 0);
    for (const HudUnit& u : hud.party) std::fprintf(stderr, "party %s portrait '%s'\n", u.name.c_str(), u.portrait.c_str());
    CHECK(!hud.party.empty() && hud.party[0].portrait == "JOANC2");
    CHECK(metz >= 0 && hud.party[std::size_t(metz)].portrait == "JEANC1");
    CHECK(message_seen);
    for (const auto* l : {&hud.message_log, &hud.messages})
        if (!l->empty()) std::fprintf(stderr, "message %s: %s\n", l->front().icon.c_str(), l->front().text.c_str());
    CHECK(std::any_of(hud.objective_list.begin(), hud.objective_list.end(), [](const auto& l) { return l.type == 0 && !l.des.empty(); }));
    hud.show_mission_brief = true;  // the brief lists are filled only while the brief is shown
    b.fill(m, s.current(), view_proj(m), kViewport, hud);
    CHECK(!hud.brief_commanders.empty());
    hud.show_mission_brief = false;
    CHECK(name_bar_seen);
    CHECK(std::any_of(hud.map_icons.begin(), hud.map_icons.end(), [](const HudMapIcon& i) { return i.sprite == "fcastle"; }));

    // HUD HOLD/RESUME on Jean de Metz toggles his hold task.
    if (metz >= 0) {
        joa::game::Unit* jean = nullptr;
        for (auto& u : m.world().units())
            if (!u.deleted && u.name == "Jean de Metz") jean = &u;
        const bool before = jean && m.ai().is_holding(*jean);
        UiCommand hold;
        hold.type = UiCommand::Type::HoldResume;
        hold.value = metz;
        CHECK(b.handle(hold, m));
        CHECK(jean && m.ai().is_holding(*jean) != before);
    }

    // SMS line (interfac\i_sms.cpp): map items picked up under the player post SMS 3 + MessageName (0x5a6660),
    // the S7 gold bar its counter (SMS 11, Unit_auto_pickup 0x49da30); the front line slides in, holds 3 s, leaves.
    {
        joa::game::MapObjects& o = m.objects();
        const joa::game::Unit& joan = *m.world().unit(m.world().player_id());
        for (const auto& [code, qty] : {std::pair{"GOLDBAG", 1}, std::pair{"fd_bread_L", 2}, std::pair{"S7_GOLDBAR", 1}}) {
            const int r = o.create(m, o.res_by_code(code));
            CHECK(r > 0);
            if (r <= 0) continue;
            joa::game::MapItem& it = *o.item(r);  // script add_item 0x651ad0: Pickable -> floor marker
            it.on_floor = true, it.quantity = qty, it.level = o.res(it.res)->min_level, it.pos = joan.pos;
        }
        const int gold = m.state().gold;
        s.tick({});
        s.take_world_events();
        b.on_events(s.take_mission_events(), m, hud);
        frame(ui, b, m, hud, 0.05f);
        for (const auto& l : b.sms()) std::fprintf(stderr, "sms (type %d): %s\n", l.type, l.text.c_str());
        const auto has = [&](const char* text) {
            return std::any_of(b.sms().begin(), b.sms().end(), [&](const GameUiBridge::Sms& l) { return l.text == text; });
        };
        CHECK(m.state().gold == gold + 120 && m.state().gold_crates == 1);
        CHECK(b.sms().size() == 3);
        CHECK(has("You have picked up a pouch worth 120 gold."));
        CHECK(has("You have picked up two loaves of bread."));
        CHECK(has("You have collected 1 barrel of gold"));
        for (int i = 0; i < 100; ++i) frame(ui, b, m, hud, 0.05f);  // 0.67 s in + 3 s + ~0.7 s out
        CHECK(b.sms().size() == 2);
    }

    // list_saves finds a native save written by Session::save (full path, META description and mission).
    const std::filesystem::path save = joa::game::native_save_dir() / "BridgeTest.jsav";
    CHECK(s.save(save, "Bridge test", &err));
    const auto saves = list_saves(vfs);
    CHECK(!saves.empty() && saves.front().file == save.string() && saves.front().description == "Bridge test" && saves.front().mission == 1);

    // Forced win: the win message settles, "Next Mission" opens the game-end menu, CONTINUE emits ContinueCampaign.
    m.finish_objectives();
    for (int t = 0; t < 20000 && (m.finish() != 1 || m.ui().cs.active || m.ui().title.active); ++t) {
        s.tick({});
        s.take_world_events();
        b.on_events(s.take_mission_events(), m, hud);
    }
    CHECK(m.finish() == 1);
    CHECK(b.blocking(m));
    for (int i = 0; i < 40; ++i) frame(ui, b, m, hud, 0.05f);  // animation: 14 steps of 50 ms
    CHECK(frame(ui, b, m, hud, 0.05f, {512, 706}).empty());  // Next Mission
    const auto cmds = frame(ui, b, m, hud, 0.05f, {774, 679});  // CONTINUE
    CHECK(cmds.size() == 1 && cmds[0].type == UiCommand::Type::ContinueCampaign);
    return test_result();
}
