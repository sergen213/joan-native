// Every campaign mission (CampaignRunner order) for 9000 ticks on the real World with a
// deterministic autopilot driving the player unit through InputFrames: steer to the nearest
// hostile / active minimap way point, A/B combos in reach, occasional blocks, interact near
// items and units, F3 when HP is low. Checks: every builtin has a handler, no script errors,
// no exception, builtins called, the player moved and fought, same Mission::hash() twice.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "formats/script_funcs.hpp"
#include "game/campaign_runner.hpp"

using namespace joa;
using namespace joa::game;

namespace {

float dist2d(Vec3 a, Vec3 b) { return std::hypot(b.x - a.x, b.y - a.y); }

// Deterministic: reads only mission state and its own tick counter (no wall clock, no rand).
struct Autopilot {
    int tick = 0;
    Vec3 check{};
    int check_tick = 0, unstick_until = 0, unstick_dir = 0, last_heal = -1000;

    InputFrame next(Mission& m) {
        ++tick;
        InputFrame f;
        World& w = m.world();
        Unit* p = w.unit(w.player_id());
        if (!p || p->deleted || w.dead(*p)) return f;
        // Nearest live hostile.
        const Unit* foe = nullptr;
        float fd = 1e30f;
        for (const Unit& u : w.units())
            if (!u.deleted && u.hp > 0 && !w.dead(u) && w.hostile_to_player(u))
                if (const float d = dist2d(p->pos, u.pos); d < fd) fd = d, foe = &u;
        // First active minimap way point (the original's objective markers).
        const Vec3* wp = nullptr;
        Vec3 wp_pos;
        for (const auto& x : m.ui().waypoints) {
            if (x.type == 0) continue;
            const Unit* fu = x.follow ? w.unit(x.follow) : nullptr;
            wp_pos = fu && !fu->deleted ? fu->pos : Vec3{x.pos[0], x.pos[1], x.pos[2]};
            wp = &wp_pos;
            break;
        }
        // F3 when HP is low.
        if (p->hp * 100 < w.max_hp(*p) * 35 && tick - last_heal > 90) f.press(IN_HEAL_ITEM), last_heal = tick;
        // Interact near items / non-hostile units.
        if (tick % 30 == 0) {
            bool near = false;
            for (const WorldItem& it : w.items()) near |= !it.deleted && dist2d(p->pos, it.pos) < 120;
            for (const Unit& u : w.units())
                near |= !u.deleted && u.id != p->id && !w.hostile_to_player(u) && dist2d(p->pos, u.pos) < 120;
            if (near) f.press(IN_INTERACT);
        }
        const Unit* target = foe && (fd < 1500 || !wp) ? foe : nullptr;
        const Vec3 goal = target ? target->pos : wp ? *wp : Vec3{};
        if (!target && !wp) {  // nothing to go for: wander in a slow circle
            f.camera_yaw_deg = float(tick % 720) * 0.5f;
            f.held.set(IN_FORWARD);
            return f;
        }
        const float d = dist2d(p->pos, goal);
        f.camera_yaw_deg = std::atan2(goal.y - p->pos.y, goal.x - p->pos.x) / 0.017453292f;
        if (target && d < 110) {  // fight
            f.mouse_dx = w.rel_angle(*p, target->pos) / 0.18f;
            const int ph = tick % 48;
            if (is_attacking(target->action) && (tick / 48) % 3 == 1 && !is_attacking(p->action)) {
                f.held.set(IN_DEFEND);
                return f;
            }
            if (ph == 0 || ph == 6 || ph == 12) f.press(IN_ATTACK_A);
            else if (ph == 18) f.press(IN_ATTACK_B);
            else if (ph == 30) f.press(IN_ATTACK_A), f.press(IN_ATTACK_B);  // A+B energy attack
            return f;
        }
        if (d < 60) return f;  // at the way point
        // Unstick: no progress over 100 ticks -> strafe + jump for 40 ticks.
        if (tick - check_tick >= 100) {
            if (dist2d(check, p->pos) < 40) unstick_until = tick + 40, unstick_dir ^= 1;
            check = p->pos, check_tick = tick;
        }
        f.held.set(IN_FORWARD);
        if (tick < unstick_until) {
            f.held.set(unstick_dir ? IN_STRAFE_LEFT : IN_STRAFE_RIGHT);
            if (tick + 30 == unstick_until) f.press(IN_JUMP);
        }
        return f;
    }
};

struct Result {
    std::uint64_t hash = 0;
    int finish = 0, stage = 0, hits = 0, kills = 0, dead_hostile = 0, dead_friendly = 0;
    double moved = 0, ms_per_tick = 0;
    std::uint64_t calls = 0;
};

int fails = 0;

Result run_mission(GameData& gd, const formats::Vfs& vfs, int no) {
    Result r;
    CampaignRunner run(gd, vfs);
    std::string err;
    if (!run.init(&err) || !run.start(no == 6 ? 5 : no, &err)) {
        std::printf("FAIL start %d: %s\n", no, err.c_str()), ++fails;
        return r;
    }
    // Scenario 6 (S9L1) only exists as the continuation of scenario 5's world (Battle_load_world
    // skips world creation for 6): win S5L1 first, then CONTINUE.
    if (no == 6) {
        run.mission()->autosave = false;
        run.mission()->finish_objectives();
        for (int t = 0; t < 3000 && run.mission()->finish() == 0; ++t) run.mission()->tick();
        if (!run.continue_after_win(&err)) {
            std::printf("FAIL S5L1 -> S9L1: %s\n", err.c_str()), ++fails;
            return r;
        }
    }
    Mission& m = *run.mission();
    m.autosave = false;
    m.world().take_events();
    Autopilot ap;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        Vec3 last = m.world().unit(m.world().player_id()) ? m.world().unit(m.world().player_id())->pos : Vec3{};
        for (int t = 0; t < 9000; ++t) {
            const InputFrame in = ap.next(m);
            m.tick(in);
            const int pid = m.world().player_id();
            for (const GameEvent& e : m.world().take_events()) {
                r.hits += e.type == Ev::Hit && e.other == pid && pid;
                r.kills += e.type == Ev::Death && e.other == pid && pid;
                if (const Unit* v = e.type == Ev::Death ? m.world().unit(e.unit) : nullptr)
                    (m.world().hostile_to_player(*v) ? r.dead_hostile : r.dead_friendly)++;
            }
            m.take_events();
            if (const Unit* p = m.world().unit(pid)) {
                const float d = dist2d(last, p->pos);
                if (d < 200) r.moved += d;  // ignore teleports / player switches
                last = p->pos;
            }
        }
    } catch (const std::exception& e) {
        std::printf("FAIL %s exception: %s\n", m.info().stem().c_str(), e.what()), ++fails;
    }
    r.ms_per_tick = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 9000;
    r.hash = m.hash();
    r.finish = m.finish();
    r.stage = m.scenario().stage;
    r.calls = m.host().calls;
    for (const auto& [id, n] : m.host().unimplemented_calls())
        std::printf("FAIL %s unimplemented builtin %d %s (%llu calls)\n", m.info().stem().c_str(), id,
                    std::string(formats::script_funcs()[id - 1].name).c_str(), static_cast<unsigned long long>(n)),
            ++fails;
    for (const auto& e : m.vm().errors()) std::printf("FAIL %s script error: %s\n", m.info().stem().c_str(), e.c_str()), ++fails;
    if (r.calls == 0) std::printf("FAIL %s no builtin calls\n", m.info().stem().c_str()), ++fails;
    if (r.moved < 100) std::printf("FAIL %s player did not move (%.0f)\n", m.info().stem().c_str(), r.moved), ++fails;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !formats::Vfs(dir).exists("scenario/scenario_01.scr")) return 77;
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::printf("game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    const int only = argc > 1 ? std::atoi(argv[1]) : 0;  // optional: one mission number
    int hits = 0;
    std::printf("no stem finish stage hash             calls   hits kills dead(h/f) moved  ms/tick\n");
    for (int no = 1; no <= 8; ++no) {
        if (only && no != only) continue;
        const Result a = run_mission(*gd, vfs, no);
        const Result b = run_mission(*gd, vfs, no);
        static const char* stems[] = {"S1L1", "S3L1", "S2L1", "S4L1", "S5L1", "S9L1", "S7L1", "S6L1"};
        std::printf("%d  %s %d      %-5d %016llx %-7llu %-5d %-5d %d/%d %8.0f %.3f\n", no, stems[no - 1], a.finish, a.stage,
                    static_cast<unsigned long long>(a.hash), static_cast<unsigned long long>(a.calls), a.hits, a.kills,
                    a.dead_hostile, a.dead_friendly, a.moved, a.ms_per_tick);
        if (a.hash != b.hash) std::printf("FAIL %s nondeterministic (%016llx vs %016llx)\n", stems[no - 1],
                                          static_cast<unsigned long long>(a.hash), static_cast<unsigned long long>(b.hash)),
                ++fails;
        hits += a.hits;
    }
    if (hits < 1) std::printf("FAIL player never hit anything\n"), ++fails;
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
