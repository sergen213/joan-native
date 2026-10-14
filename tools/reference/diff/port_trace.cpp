// port_trace: lockstep driver of the port's mission World for the differential harness
// (tools/reference/diff/diffrun.py, docs/reverse-engineering/DIFFERENTIAL_VERIFICATION.md).
// stdin, one command per line; every reply ends with "end":
//   start MISSION DIFFICULTY SEED   new campaign mission, opening cut scene skipped
//   units                           U lines for every unit
//   step N HELD PRESSED MDX         N ticks (33 ms); HELD/PRESSED = comma lists of InputFrame action ids or "-";
//                                   PRESSED and MDX (mickeys) apply to the first tick only. After each tick:
//                                   "T <now ms> <camera yaw deg>", then U lines of the units within 4000 of the player.
//   set REC FIELD VALUE             test hook: hp | ep | xp | x | y | yaw
//   objects                         O recno name cls type content x y z res stage on_floor  (map items within 3000 of the player)
//   path X Y                        P x y z waypoints of the mission navmesh path from the player to (X, Y)
// U rec name nat cls hp maxhp ep maxep str dex def lead level xp action x y z yaw morale armor_level leader
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "game/campaign_runner.hpp"
#include "game/multimap_nav.hpp"

using namespace joa;
using namespace joa::game;

static void put(World& w, const Unit& u) {
    std::string name = u.name;
    for (char& c : name) if (c == ' ') c = '_';
    std::printf("U %d %s %d %d %d %d %d %d %d %d %d %d %d %d %d %.2f %.2f %.2f %.3f %d %d %d\n", u.id, name.empty() ? "-" : name.c_str(),
                u.nation, u.cls, u.hp, w.max_hp(u), u.ep, w.max_ep(u), u.str, u.dex, u.def, u.lead, u.level, u.xp, u.action,
                u.pos.x, u.pos.y, u.pos.z, u.yaw_deg, u.morale, u.armor_level, u.leader);
}

static void bits(const std::string& s, std::bitset<IN_ACTION_COUNT>& b) {
    if (s == "-") return;
    std::stringstream ss(s);
    for (std::string t; std::getline(ss, t, ',');) b.set(std::size_t(std::atoi(t.c_str())));
}

int main() {
    const std::string dir = std::getenv("JOA_GAME_DIR") ? std::getenv("JOA_GAME_DIR") : ".";
    std::string err;
    auto gd = GameData::load(dir, &err);
    if (!gd) return std::fprintf(stderr, "game data: %s\n", err.c_str()), 1;
    const formats::Vfs vfs(dir);
    std::unique_ptr<CampaignRunner> run;
    float cam_yaw = 0;
    for (std::string line; std::getline(std::cin, line);) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "start") {
            int mission = 1, diff = 1;
            unsigned seed = 1;
            in >> mission >> diff >> seed;
            run = std::make_unique<CampaignRunner>(*gd, vfs);
            run->seed = seed;
            run->profile.difficulty = diff;
            if (!run->init(&err) || !run->start(mission, &err)) return std::fprintf(stderr, "start: %s\n", err.c_str()), 1;
            Mission& m = *run->mission();
            // Escape on the opening cut scene, as the original run does (session_flow.cpp).
            for (int t = 0; t < 600 && !(m.ui().cs.active || m.ui().game_cut_scene); ++t) m.tick();
            if (m.ui().cs.active || m.ui().game_cut_scene) m.ui().skip_cut_scene(m);
            m.tick();
            if (const Unit* p = m.world().unit(m.world().player_id())) cam_yaw = p->yaw_goal;  // GameCamera::reset
            std::printf("player %d now %u\n", m.world().player_id(), m.now());
        } else if (!run) {
            std::printf("error no mission\n");
        } else if (cmd == "units") {
            World& w = run->mission()->world();
            for (const Unit& u : w.units()) if (!u.deleted) put(w, u);
        } else if (cmd == "step") {
            int n = 1;
            std::string held, pressed;
            float mdx = 0;
            in >> n >> held >> pressed >> mdx;
            Mission& m = *run->mission();
            for (int t = 0; t < n; ++t) {
                InputFrame f;
                bits(held, f.held);
                if (t == 0) {
                    bits(pressed, f.pressed);
                    f.held |= f.pressed;
                    f.key_char_event = f.pressed.any();
                    f.mouse_dx = mdx;
                }
                f.camera_yaw_deg = cam_yaw;  // GameCamera::before_tick
                m.tick(f);
                cam_yaw = m.world().controller().camera_yaw_out;  // GameCamera::after_tick
                World& w = m.world();
                w.take_events();
                std::printf("T %u %.3f\n", m.now(), cam_yaw);  // + camera 0 yaw (deg)
                const Unit* p = w.unit(w.player_id());
                for (const Unit& u : w.units())
                    if (!u.deleted && p && std::hypot(u.pos.x - p->pos.x, u.pos.y - p->pos.y) < 4000) put(w, u);
            }
        } else if (cmd == "objects") {
            Mission& m = *run->mission();
            const Unit* p = m.world().unit(m.world().player_id());
            for (int r = 1; p && r <= m.objects().size(); ++r)
                if (const MapItem* it = m.objects().item(r); it && std::hypot(it->pos.x - p->pos.x, it->pos.y - p->pos.y) < 3000)
                    std::printf("O %d %s %d %d %d %.0f %.0f %.0f %d %d %d\n", r, it->name.empty() ? "-" : it->name.c_str(), it->cls, it->type,
                                it->content, it->pos.x, it->pos.y, it->pos.z, it->res, it->stage, int(it->on_floor));
        } else if (cmd == "path") {
            Mission& m = *run->mission();
            float x = 0, y = 0;
            in >> x >> y;
            const Unit* p = m.world().unit(m.world().player_id());
            if (NavWorld* nav = m.nav(); nav && p) {
                PathQuery q;
                q.from = {p->pos.x, p->pos.y, p->pos.z};
                q.to = {x, y, p->pos.z};
                q.max_expand = 4000;  // as the campaign bot: a player knows the roads
                for (const NavPt& n : nav->find_path(q).points) std::printf("P %.0f %.0f %.0f\n", n.x, n.y, n.z);
            }
        } else if (cmd == "set") {
            int rec = 0;
            std::string field;
            float v = 0;
            in >> rec >> field >> v;
            World& w = run->mission()->world();
            if (Unit* u = w.unit(rec)) {
                if (field == "hp") u->hp = int(v);
                else if (field == "ep") u->ep = int(v);
                else if (field == "xp") u->xp = int(v);
                else if (field == "x") u->pos.x = v;
                else if (field == "y") u->pos.y = v;
                else if (field == "yaw") u->yaw_deg = u->yaw_goal = v;
            }
        } else {
            std::printf("error %s\n", cmd.c_str());
        }
        std::printf("end\n");
        std::fflush(stdout);
    }
    return 0;
}
