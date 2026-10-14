// XP and levels: EXPUP 0x5229a0, LEVELUP 0x522c40, set-level 0x523190, upgrade unlock 0x523240,
// upgrade apply 0x4949f0, AI auto-upgrade 0x4a95a0, manual allocation 0x50d640.
// GAMEPLAY_CHARACTER_COMBAT.md Part A §4.
#include <algorithm>

#include "game/world_sim.hpp"

namespace joa::game {

void World::add_xp(Unit& u, int xp, bool share, bool quiet) {
    if (gd_.units[u.cls].unit_class == 8) return;   // siege weapons gain nothing
    if (share) {
        if (!u.members.empty()) {
            const int s = int(float(xp) * gd_.c.mem_exp);
            const int count = int(u.members.size());
            for (int id : u.members)
                if (Unit* m = unit(id); m && m->id != u.id && !dead(*m)) add_xp(*m, int(float(s) / float(count)), false, quiet);
        } else if (Unit* l = unit(u.leader); l && l->id != u.id && !dead(*l)) {
            add_xp(*l, int(float(xp) * gd_.c.ld_exp), false, quiet);
        }
    }
    u.xp += xp;
    if (u.id == player_ && game_mode != 4) command_points = std::min(command_points + xp, gd_.c.max_com);
    emit(Ev::XpGain, u.id, 0, xp);
    level_up(u, quiet);
    if (u.id == player_) emit(Ev::Effect, u.id, 0, 0, "EXPUP");
}

int World::level_up(Unit& u, bool quiet) {
    if (dead(u)) return 0;
    const LevelRow* c = gd_.level_row(u.level);
    if (!c || !gd_.level_row(u.level + 1) || u.xp < c->exp) return 0;
    int gained = 0;
    do {   // note: LEVEL[level+1] is not re-checked inside the loop (edge case 30 -> 31 kept)
        unlock_upgrades(u, u.level);
        ++u.level;
        ++gained;
        u.level_arrow = true;
        u.ts_level = now_;
        u.attr_points += c->att;
        u.skill_points += c->ski;
        c = gd_.level_row(u.level);
    } while (c && u.xp >= c->exp);
    u.upgrade_ts = now_;
    set_hp(u, max_hp(u));
    u.hp_trail = max_hp(u);
    set_ep(u, max_ep(u));
    emit(Ev::LevelUp, u.id, 0, u.level);
    if (!quiet) emit(Ev::Effect, u.id, 0, 0, u.id == player_ ? "LEVELUP" : "flash.txt");
    for (int id : u.members)
        if (Unit* m = unit(id); m && m->id != u.id && !dead(*m)) set_level(*m, u.level);
    return gained;
}

void World::set_level(Unit& u, int level) {
    if (dead(u) || u.level >= level) return;
    if (const LevelRow* r = gd_.level_row(level - 1)) add_xp(u, r->exp - u.xp, false, true);
    ai_auto_upgrade(u);
}

void World::unlock_upgrades(Unit& u, int level) {
    auto it = gd_.upgrade_lists.find(gd_.units[u.cls].upgrade_list);
    if (it == gd_.upgrade_lists.end()) return;
    for (auto [lvl, up] : it->second) {
        if (lvl != level) continue;
        auto e = std::find_if(u.upgrades.begin(), u.upgrades.end(), [&](const UpgradeEntry& x) { return x.upitem == up; });
        if (e != u.upgrades.end()) ++e->unlocked;
        else u.upgrades.push_back({up, 1, 0, 1});
    }
}

bool World::apply_upgrade(Unit& u, int entry) {
    if (entry < 0 || entry >= int(u.upgrades.size())) return false;
    UpgradeEntry& e = u.upgrades[size_t(entry)];
    const UpItem& up = gd_.upitems[e.upitem];
    int& pts = up.skill() ? u.skill_points : u.attr_points;
    if (pts < up.cost || e.applied >= e.unlocked) return false;
    pts -= up.cost;
    ++e.applied;
    switch (up.type) {
    case 1: u.str += up.p1; u.base_max_hp += up.p1 * gd_.c.str_hp; break;
    case 2: u.dex += up.p1; break;
    case 3: u.def += up.p1; u.base_max_hp += up.p1 * gd_.c.def_hp; break;
    case 5: u.lead += up.p1; break;
    case 6:   // setter 0x45b720: raising max HP heals by the delta, then HP = max
        u.base_max_hp += up.p1;
        set_hp(u, max_hp(u));
        break;
    default: break;   // ENERGY_POINT and skill types: only marked applied here
    }
    return true;
}

void World::ai_auto_upgrade(Unit& u) {
    auto applied = [&](std::string_view combo) {
        for (const auto& e : u.upgrades)
            if (gd_.upitems[e.upitem].type == 9 && gd_.upitems[e.upitem].para1 == combo && e.applied > 0) return true;
        return false;
    };
    for (int i = int(u.upgrades.size()) - 1; i >= 0; --i) {
        const UpItem& up = gd_.upitems[u.upgrades[size_t(i)].upitem];
        if (up.skill() && !up.require.empty() && !applied(up.require)) continue;
        while (apply_upgrade(u, i)) {}
    }
    while (u.skill_points > 0) {
        std::vector<int> cand;
        for (size_t i = 0; i < u.upgrades.size(); ++i)
            if (gd_.upitems[u.upgrades[i].upitem].type == 9 && u.upgrades[i].combo_level < 10) cand.push_back(int(i));
        if (cand.empty()) break;
        ++u.upgrades[size_t(cand[size_t(rng().next(int(cand.size())))])].combo_level;
        --u.skill_points;
    }
}

bool World::allocate_point(Unit& u, int type, bool undo, int entry) {
    const Consts& c = gd_.c;
    const int s = undo ? -1 : 1;
    if (type == 8) {
        if (entry < 0 || entry >= int(u.upgrades.size()) || (!undo && u.skill_points < 1)) return false;
        u.upgrades[size_t(entry)].combo_level += s;
        u.skill_points -= s;
        return true;
    }
    if (!undo && u.attr_points < 1) return false;
    switch (type) {
    case 1: u.str += s * c.str_per; u.base_max_hp += s * c.str_per * c.str_hp; break;
    case 2: u.dex += s * c.dex_per; break;
    case 3: u.def += s * c.def_per; u.base_max_hp += s * c.def_per * c.def_hp; break;
    case 5: u.lead += s * c.lds_per; break;
    case 6: u.base_max_hp += s * c.hp_per; set_hp(u, u.hp + s * c.hp_per); break;
    case 7: u.base_max_ep += s * c.ep_per; break;
    default: return false;   // 4 CHARISMA unsupported in the original
    }
    u.attr_points -= s;
    return true;
}

}  // namespace joa::game
