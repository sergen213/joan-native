// Point & click controller, mode 2 (PlayerCtrl_mouse_click 0x438b30, update 0x439090 / Player_autoAttackUpdate
// 0x43acb0 / 0x43b2d0): ground click walks there, enemy click approaches and attacks, a second click pays EGATK_MP for
// the fury round, right click = energy attack target, friendly click does nothing, own gate = approach + GATE_OPEN,
// floor item = walk to it, keys = item handler only; mode 3 ignores the click fields. Flat terrain, no navmesh.
#include "sim_fixture.hpp"

#include "game/ai_system.hpp"

using namespace joa::game;

namespace {
struct Rig {
    FlatTerrain flat{0};
    World w;
    AiSystem ai;
    int joan = 0, french = 0, english = 0;
    std::vector<GameEvent> ev;
    Rig(GameData& gd, int mode) : w(gd, &flat, 0), ai(w, nullptr) {
        french = gd.nation_id("FRENCH"), english = gd.nation_id("ENGLISH");
        joan = w.spawn_unit("JOAN_OF_ARC", french, {0, 0, 0}, 0);
        w.set_player(joan);
        w.set_hostile(french, english, true), w.set_hostile(english, french, true);
        w.game_mode = mode;
        idle(5);
    }
    Unit& J() { return *w.unit(joan); }
    PlayerController& pc() { return w.controller(); }
    void tick(const InputFrame& f) {
        w.tick(f);
        for (GameEvent& e : w.take_events()) ev.push_back(e);
    }
    void idle(int n) { for (int i = 0; i < n; ++i) tick(InputFrame{}); }
    int foe(Vec3 p) {
        const int id = w.spawn_unit("ENGLISH_LIGHT_INFANTRY", english, p, 180);
        ai.give_task(*w.unit(id), task::IDLE, 100, 1, 0, 0, 0);  // stands still
        return id;
    }
    static InputFrame click(bool left, int unit = 0, int item = 0, int drop = 0) {
        InputFrame f;
        (left ? f.click_left : f.click_right) = true;
        f.pick_unit = unit, f.pick_item = item, f.pick_drop = drop;
        return f;
    }
    static InputFrame ground(float x, float y) {
        InputFrame f;
        f.click_left = f.pick_ground = true;
        f.ground_x = x, f.ground_y = y;
        return f;
    }
    float dist(Vec3 p) { return std::hypot(J().pos.x - p.x, J().pos.y - p.y); }
};
}  // namespace

std::uint64_t run(GameData& gd) {
    std::uint64_t h = 0;
    {   // Ground click: path_to (0x43a580), run, stop on arrival.
        Rig r(gd, 2);
        r.tick(Rig::ground(600, 0));
        CHECK(!r.ai.st(r.J()).path.empty() && r.pc().auto_state == 0, "ground click builds a path");
        bool ran = false;
        for (int i = 0; i < 300 && !r.ai.st(r.J()).path.empty(); ++i) r.idle(1), ran |= r.J().action == RUN;
        r.idle(10);
        CHECK(ran && r.dist({600, 0, 0}) < 60, "walked to the click: at (%.0f, %.0f)", r.J().pos.x, r.J().pos.y);
        CHECK(!is_locomotion(r.J().action), "stands at the goal: %s", action_name(r.J().action));
        h ^= r.w.hash();
    }
    {   // Enemy click: Player_autoAttackSetTarget (state 1), approach, attack within radius + 10.
        Rig r(gd, 2);
        const int e = r.foe({500, 0, 0});
        const int hp0 = r.w.unit(e)->hp;
        r.tick(Rig::click(true, e));
        CHECK(r.pc().auto_state == 1 && r.pc().auto_target == e && r.pc().auto_kind == 0, "state %d target %d",
              r.pc().auto_state, r.pc().auto_target);
        int attacks = 0, hits = 0;
        for (int i = 0; i < 300 && hits == 0; ++i) {
            r.ev.clear();
            r.idle(1);
            for (const GameEvent& g : r.ev) {
                attacks += g.type == Ev::Action && g.unit == r.joan && g.value == ATTACK;
                hits += (g.type == Ev::Hit || g.type == Ev::Blocked) && g.other == r.joan;
            }
        }
        CHECK(attacks >= 1, "Joan attacked the clicked enemy (%d ATTACK)", attacks);
        CHECK(hits >= 1 || r.w.unit(e)->hp < hp0, "the attack connected (hits %d, hp %d -> %d)", hits, hp0, r.w.unit(e)->hp);
        CHECK(r.dist(r.w.unit(e)->pos) < 120, "fought in reach (%.0f)", r.dist(r.w.unit(e)->pos));
        h ^= r.w.hash();
    }
    {   // Second click on the target while attacking with EP >= EGATK_MP: fury (state 2), EP paid.
        Rig r(gd, 2);
        const int e = r.foe({800, 0, 0});
        r.J().base_max_ep = std::max(r.J().base_max_ep, gd.c.egatk_mp + 10);   // level-1 Joan has 30 < EGATK_MP
        r.w.set_ep(r.J(), r.w.max_ep(r.J()));
        const int ep0 = r.J().ep;
        r.tick(Rig::click(true, e));
        r.tick(Rig::click(true, e));
        CHECK(r.pc().auto_state == 2 && r.J().ep == ep0 - gd.c.egatk_mp, "fury: state %d ep %d -> %d", r.pc().auto_state, ep0,
              r.J().ep);
        r.tick(Rig::click(true, e));   // refused while in fury
        CHECK(r.pc().auto_state == 2 && r.J().ep == ep0 - gd.c.egatk_mp, "fury refuses a new set");
        h ^= r.w.hash();
    }
    {   // Right click enemy: 0x43a9e0 state 3, energy attack / attack on arrival, then the state ends.
        Rig r(gd, 2);
        const int e = r.foe({400, 0, 0});
        r.tick(Rig::click(false, e));
        CHECK(r.pc().auto_state == 3, "right click: state %d", r.pc().auto_state);
        int swung = 0;
        for (int i = 0; i < 300 && r.pc().auto_state == 3; ++i) {
            r.ev.clear();
            r.idle(1);
            for (const GameEvent& g : r.ev) swung += g.type == Ev::Action && g.unit == r.joan && (g.value == ENGATK || g.value == ATTACK);
        }
        CHECK(r.pc().auto_state == 0 && swung >= 1, "arrived and attacked once (state %d, %d swings)", r.pc().auto_state, swung);
        h ^= r.w.hash();
    }
    {   // Friendly click: 0x5095e0 -> 0x43a720, nothing happens.
        Rig r(gd, 2);
        const int f = r.w.spawn_unit("FRENCH_LIGHT_INFANTRY", r.french, {300, 0, 0}, 0);
        r.tick(Rig::click(true, f));
        CHECK(f && r.pc().auto_state == 0 && r.ai.st(r.J()).path.empty(), "friendly click ignored");
    }
    {   // Own gate: 0x43a650 state 4, GATE_OPEN on reaching radius + 15, state ends; own floor item: walk to it.
        Rig r(gd, 2);
        int gate_req = 0;
        r.ai.item = [&](int id) -> std::optional<AiItem> {
            AiItem a;
            a.nation = r.french;
            a.gate = id == 7, a.on_floor = id == 8;
            a.pos = id == 7 ? Vec3{500, 0, 0} : Vec3{0, 400, 0};
            a.radius = 60;
            return a;
        };
        r.ai.on_gate_request = [&](int unit, int item, int) { gate_req += unit == r.joan && item == 7; };
        r.tick(Rig::click(true, 0, 7));
        CHECK(r.pc().auto_state == 4 && r.pc().auto_target == 7, "own gate: state %d", r.pc().auto_state);
        for (int i = 0; i < 300 && r.pc().auto_state == 4; ++i) r.idle(1);
        CHECK(gate_req == 1 && r.pc().auto_state == 0 && r.dist({500, 0, 0}) < 60 + 15 + 20, "gate request %d at %.0f", gate_req,
              r.dist({500, 0, 0}));
        r.tick(Rig::click(true, 0, 8));
        CHECK(r.pc().auto_state == 0 && !r.ai.st(r.J()).path.empty(), "floor item: walk to it (0x43abd0)");
        h ^= r.w.hash();
    }
    {   // Keys: Interact = the item handler 0x43bc50 only.
        Rig r(gd, 2);
        int item = 0, full = 0;
        r.w.interact_item = [&](Unit&) { return ++item, true; };
        r.w.interact = [&](Unit&) { return ++full, true; };
        InputFrame f;
        f.press(IN_INTERACT);
        r.tick(f);
        CHECK(item == 1 && full == 0, "interact: item handler %d, full chain %d", item, full);
    }
    {   // Keyboard + mouse (mode 3) ignores the click fields.
        Rig r(gd, 3);
        r.tick(Rig::ground(600, 0));
        r.idle(5);
        CHECK(r.ai.st(r.J()).path.empty() && r.dist({0, 0, 0}) < 1 && r.pc().auto_state == 0, "mode 3: no path");
    }
    return h;
}

int main() {
    auto gd = load_data_or_skip();
    const std::uint64_t a = run(*gd), b = run(*gd);
    CHECK(a == b, "deterministic");
    return finish();
}
